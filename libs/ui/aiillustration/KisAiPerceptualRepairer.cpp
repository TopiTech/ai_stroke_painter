/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPerceptualRepairer.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSet>
#include <QStack>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "KisAiSceneSpec.h"
#include "KisAiStrokeProgram.h"

namespace KisAi
{

// ---------------------------------------------------------------------------
// PerceptualRepairPlan ヘルパ
// ---------------------------------------------------------------------------

int PerceptualRepairPlan::issueCount(PerceptualIssue::Type t) const
{
    int n = 0;
    for (int i = 0; i < issues.size(); ++i) {
        if (issues.at(i).type == t)
            ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// 内部ユーティリティ
// ---------------------------------------------------------------------------

namespace
{

QString normalizeLayerLocal(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("flat") || n == QLatin1String("flats") || n == QLatin1String("base"))
        return QStringLiteral("Flats");
    if (n == QLatin1String("shade") || n == QLatin1String("shading") || n == QLatin1String("shadow")
        || n == QLatin1String("shadows"))
        return QStringLiteral("Shading");
    if (n == QLatin1String("line") || n == QLatin1String("lineart") || n == QLatin1String("lines")
        || n == QLatin1String("outline"))
        return QStringLiteral("Lineart");
    if (n == QLatin1String("highlight") || n == QLatin1String("highlights") || n == QLatin1String("specular"))
        return QStringLiteral("Highlights");
    if (n == QLatin1String("fx") || n == QLatin1String("effect") || n == QLatin1String("effects")
        || n == QLatin1String("particles"))
        return QStringLiteral("FX");
    if (n == QLatin1String("bg") || n == QLatin1String("background"))
        return QStringLiteral("Background");
    return name.trimmed().isEmpty() ? QStringLiteral("Lineart") : name.trimmed();
}

qreal polygonAreaSigned(const QPolygonF &poly)
{
    const int n = poly.size();
    if (n < 3)
        return 0.0;
    qreal area = 0.0;
    for (int i = 0; i < n; ++i) {
        const QPointF a = poly.at(i);
        const QPointF b = poly.at((i + 1) % n);
        area += a.x() * b.y() - b.x() * a.y();
    }
    return qAbs(area) * 0.5;
}

QRectF normalizedBounds(const QRectF &r, qreal imageW, qreal imageH)
{
    if (imageW <= 0.0 || imageH <= 0.0)
        return r;
    return QRectF(r.x() / imageW, r.y() / imageH, r.width() / imageW, r.height() / imageH);
}

/// Flats レイヤーを αマスク画像にラスタライズ (W x H, Format_ARGB32)。
QImage rasterizeFlatsMask(const KisAiStrokeProgram &program, int W, int H)
{
    QImage mask(W, H, QImage::Format_ARGB32);
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < program.operations.size(); ++i) {
        const KisAiStrokeOperation &op = program.operations.at(i);
        const QString layer = normalizeLayerLocal(op.layer);
        if (layer != QStringLiteral("Flats"))
            continue;
        QColor c(Qt::white);
        c.setAlphaF(qBound<qreal>(0.0, op.brush.opacity, 1.0));
        QBrush brush(c);
        painter.setBrush(brush);
        painter.setPen(Qt::NoPen);
        if (!op.polygon.isEmpty()) {
            QPolygonF scaled;
            for (int p = 0; p < op.polygon.size(); ++p) {
                const QPointF pt = op.polygon.at(p);
                scaled.append(QPointF(pt.x() * W, pt.y() * H));
            }
            painter.drawPolygon(scaled);
        } else if (op.kind == KisAiStrokeOperation::Kind::GradientFill) {
            // 空 polygon は全画面 → 描画しない (穴検出の邪魔)
        }
    }
    painter.end();
    return mask;
}

/// Lineart レイヤーを αマスク画像にラスタライズ。
QImage rasterizeLineartMask(const KisAiStrokeProgram &program, int W, int H)
{
    QImage mask(W, H, QImage::Format_ARGB32);
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < program.operations.size(); ++i) {
        const KisAiStrokeOperation &op = program.operations.at(i);
        const QString layer = normalizeLayerLocal(op.layer);
        if (layer != QStringLiteral("Lineart"))
            continue;
        if (op.points.size() < 2)
            continue;
        QColor c(Qt::white);
        c.setAlphaF(qBound<qreal>(0.0, op.brush.opacity, 1.0));
        QPen pen(c);
        const qreal sizePx = (op.brush.sizeMode == QStringLiteral("px")) ? op.brush.size : op.brush.size * qMin(W, H);
        pen.setWidthF(qMax<qreal>(1.0, sizePx));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        QVector<QPointF> pts;
        for (int p = 0; p < op.points.size(); ++p) {
            pts.append(QPointF(op.points.at(p).pos.x() * W, op.points.at(p).pos.y() * H));
        }
        painter.drawPolyline(pts);
    }
    painter.end();
    return mask;
}

/// Shading レイヤーを αマスク画像にラスタライズ (Flats 外の判定用)。
QImage rasterizeShadingMask(const KisAiStrokeProgram &program, int W, int H)
{
    QImage mask(W, H, QImage::Format_ARGB32);
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < program.operations.size(); ++i) {
        const KisAiStrokeOperation &op = program.operations.at(i);
        const QString layer = normalizeLayerLocal(op.layer);
        if (layer != QStringLiteral("Shading"))
            continue;
        QColor c(Qt::white);
        c.setAlphaF(qBound<qreal>(0.0, op.brush.opacity, 1.0));
        QBrush brush(c);
        painter.setBrush(brush);
        painter.setPen(Qt::NoPen);
        if (!op.polygon.isEmpty()) {
            QPolygonF scaled;
            for (int p = 0; p < op.polygon.size(); ++p) {
                scaled.append(QPointF(op.polygon.at(p).x() * W, op.polygon.at(p).y() * H));
            }
            painter.drawPolygon(scaled);
        } else if (!op.points.isEmpty()) {
            QPen pen(c);
            const qreal sizePx =
                (op.brush.sizeMode == QStringLiteral("px")) ? op.brush.size : op.brush.size * qMin(W, H);
            pen.setWidthF(qMax<qreal>(1.0, sizePx));
            painter.setPen(pen);
            QVector<QPointF> pts;
            for (int p = 0; p < op.points.size(); ++p) {
                pts.append(QPointF(op.points.at(p).pos.x() * W, op.points.at(p).pos.y() * H));
            }
            if (pts.size() >= 2)
                painter.drawPolyline(pts);
        }
    }
    painter.end();
    return mask;
}

/// 4-connected 連結成分ラベリング (BFS)。ラベル画像は不要、
/// 各成分の境界矩形と面積を返す。
struct ComponentInfo {
    QRect bounds; // ピクセル座標
    int area; // ピクセル数
};
QVector<ComponentInfo> connectedComponents(const QImage &mask, uchar alphaThreshold = 64)
{
    QVector<ComponentInfo> components;
    if (mask.isNull())
        return components;
    const int W = mask.width();
    const int H = mask.height();
    QImage visited(W, H, QImage::Format_ARGB32);
    visited.fill(0);

    for (int y = 0; y < H; ++y) {
        const QRgb *mRow = reinterpret_cast<const QRgb *>(mask.constScanLine(y));
        const QRgb *vRow = reinterpret_cast<const QRgb *>(visited.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            if (qAlpha(mRow[x]) < alphaThreshold)
                continue;
            if (qAlpha(vRow[x]) != 0)
                continue;

            // BFS
            ComponentInfo comp;
            comp.bounds = QRect(x, y, 1, 1);
            comp.area = 0;
            QStack<QPoint> stack;
            stack.push(QPoint(x, y));
            while (!stack.isEmpty()) {
                QPoint pt = stack.pop();
                if (pt.x() < 0 || pt.x() >= W || pt.y() < 0 || pt.y() >= H)
                    continue;
                const QRgb *vv = reinterpret_cast<const QRgb *>(visited.constScanLine(pt.y()));
                if (qAlpha(vv[pt.x()]) != 0)
                    continue;
                const QRgb *mm = reinterpret_cast<const QRgb *>(mask.constScanLine(pt.y()));
                if (qAlpha(mm[pt.x()]) < alphaThreshold)
                    continue;
                QRgb *vw = reinterpret_cast<QRgb *>(visited.scanLine(pt.y()));
                vw[pt.x()] = qRgba(255, 255, 255, 255);
                comp.bounds = comp.bounds.united(QRect(pt, QSize(1, 1)));
                ++comp.area;
                stack.push(QPoint(pt.x() + 1, pt.y()));
                stack.push(QPoint(pt.x() - 1, pt.y()));
                stack.push(QPoint(pt.x(), pt.y() + 1));
                stack.push(QPoint(pt.x(), pt.y() - 1));
            }
            if (comp.area > 0)
                components.append(comp);
        }
    }
    return components;
}

/// 顔領域 (headCenter ± headHeight/2) をピクセル矩形で返す。
QRect faceBox(const QPointF &headCenter, qreal headHeight, int imageW, int imageH)
{
    const qreal fw = headHeight * 0.40;
    const qreal fh = headHeight * 0.50;
    const int left = qMax(0, int((headCenter.x() - fw) * imageW));
    const int top = qMax(0, int((headCenter.y() - fh * 0.5) * imageH));
    const int right = qMin(imageW, int((headCenter.x() + fw) * imageW) + 1);
    const int bottom = qMin(imageH, int((headCenter.y() + fh * 0.7) * imageH) + 1);
    return QRect(left, top, qMax(1, right - left), qMax(1, bottom - top));
}

/// 矩形領域のバウンディングボックスから、外側に 8px 拡張した単純な正方形ポリゴンを生成。
QPolygonF expandBoxAsQuad(const QRectF &normBox, qreal expandNorm = 0.01)
{
    qreal l = qBound(0.0, normBox.left() - expandNorm, 1.0);
    qreal t = qBound(0.0, normBox.top() - expandNorm, 1.0);
    qreal r = qBound(0.0, normBox.right() + expandNorm, 1.0);
    qreal b = qBound(0.0, normBox.bottom() + expandNorm, 1.0);
    QPolygonF poly;
    poly.append(QPointF(l, t));
    poly.append(QPointF(r, t));
    poly.append(QPointF(r, b));
    poly.append(QPointF(l, b));
    return poly;
}

} // namespace

// ---------------------------------------------------------------------------
// 診断 (diagnose)
// ---------------------------------------------------------------------------

PerceptualRepairPlan KisAiPerceptualRepairer::diagnose(const KisAiStrokeProgram &program,
                                                       const QImage &renderedImage,
                                                       const KisAiSceneSpec *spec)
{
    PerceptualRepairPlan plan;

    if (program.operations.isEmpty())
        return plan;

    // 内部マスク解像度 (固定 512px 程度)。詳細検出は外部評価器に任せる。
    const int W = 512;
    const int H = 512;
    const QImage flatsMask = rasterizeFlatsMask(program, W, H);
    const QImage lineartMask = rasterizeLineartMask(program, W, H);
    const QImage shadingMask = rasterizeShadingMask(program, W, H);

    // ---- 1. Flats 穴検出 ----
    {
        const QVector<ComponentInfo> comps = connectedComponents(flatsMask, 32);
        if (comps.size() >= 2) {
            // 最大 component を基準に、それより小さくかつ面積比 5% 以下のものを穴と判定
            int maxArea = 0;
            for (int i = 0; i < comps.size(); ++i) {
                maxArea = qMax(maxArea, comps.at(i).area);
            }
            if (maxArea > 0) {
                for (int i = 0; i < comps.size(); ++i) {
                    const ComponentInfo &c = comps.at(i);
                    if (c.area >= maxArea)
                        continue;
                    const qreal ratio = qreal(c.area) / qreal(maxArea);
                    if (ratio < 0.05 && c.area > 0) {
                        PerceptualIssue issue;
                        issue.type = PerceptualIssue::FlatsHole;
                        issue.region = normalizedBounds(QRectF(c.bounds), W, H);
                        issue.severity = qBound<qreal>(0.0, 1.0 - ratio * 10.0, 1.0);
                        issue.description = QStringLiteral("Flats silhouette has a detached island (area %1 / %2)")
                                                .arg(c.area)
                                                .arg(maxArea);
                        issue.requiresUserConsent = false;
                        plan.issues.append(issue);

                        PerceptualFix fix;
                        fix.issue = issue;
                        fix.action = PerceptualFix::InsertPolygonFill;
                        fix.targetOpIndex = -1;
                        fix.newPolygon = expandBoxAsQuad(issue.region, 0.012);
                        // 平均色として既存 Flats の最初の op の色を採用
                        for (int j = 0; j < program.operations.size(); ++j) {
                            const KisAiStrokeOperation &op = program.operations.at(j);
                            if (normalizeLayerLocal(op.layer) == QStringLiteral("Flats") && op.brush.color.isValid()) {
                                fix.newColor = op.brush.color;
                                fix.newOpacity = op.brush.opacity;
                                break;
                            }
                        }
                        fix.description = QStringLiteral("Insert Flats Fill covering the hole bbox");
                        plan.fixes.append(fix);
                        plan.autoFixSummaries.append(fix.description);
                    }
                }
            }
        }
    }

    // ---- 2. Lineart と Flats のギャップ検出 ----
    {
        // Lineart α と Flats α の XOR でシーム画素を抽出
        const int lw = lineartMask.width();
        const int lh = lineartMask.height();
        int seamCount = 0;
        QRectF seamBounds;
        for (int y = 1; y < lh - 1; ++y) {
            const QRgb *lr = reinterpret_cast<const QRgb *>(lineartMask.constScanLine(y));
            const QRgb *fr = reinterpret_cast<const QRgb *>(flatsMask.constScanLine(y));
            for (int x = 1; x < lw - 1; ++x) {
                const bool isLineart = qAlpha(lr[x]) >= 64;
                const bool isFlats = qAlpha(fr[x]) >= 64;
                // Lineart が Flats のすぐ外にある = シーム候補
                if (isLineart) {
                    const bool neighborFlats = (qAlpha(fr[x - 1]) >= 64 || qAlpha(fr[x + 1]) >= 64
                                                || qAlpha(fr[x - lw]) >= 64 || qAlpha(fr[x + lw]) >= 64);
                    if (!neighborFlats) {
                        ++seamCount;
                        const QPointF pt(qreal(x) / lw, qreal(y) / lh);
                        if (seamCount == 1)
                            seamBounds = QRectF(pt, QSizeF(0.001, 0.001));
                        else
                            seamBounds = seamBounds.united(QRectF(pt, QSizeF(0.001, 0.001)));
                    }
                }
            }
        }
        if (seamCount > 50) {
            PerceptualIssue issue;
            issue.type = PerceptualIssue::LineartFlatsGap;
            issue.region = seamBounds.isValid() ? seamBounds : QRectF(0.3, 0.3, 0.4, 0.4);
            issue.severity = qBound<qreal>(0.0, qMin<qreal>(1.0, seamCount / 500.0), 1.0);
            issue.description = QStringLiteral("Lineart sits outside Flats in %1 pixels").arg(seamCount);
            issue.requiresUserConsent = true; // 幾何を動かすので要承認
            plan.issues.append(issue);
            plan.hasUserConsentRequired = true;
            plan.consentSummaries.append(issue.description);
        }
    }

    // ---- 3. Shading はみ出し検出 ----
    {
        // Shading α > 64 かつ Flats α < 64 の画素を抽出
        const int sw = shadingMask.width();
        const int sh = shadingMask.height();
        int spillCount = 0;
        for (int y = 0; y < sh; ++y) {
            const QRgb *sr = reinterpret_cast<const QRgb *>(shadingMask.constScanLine(y));
            const QRgb *fr = reinterpret_cast<const QRgb *>(flatsMask.constScanLine(y));
            for (int x = 0; x < sw; ++x) {
                if (qAlpha(sr[x]) >= 64 && qAlpha(fr[x]) < 32)
                    ++spillCount;
            }
        }
        if (spillCount > 100) {
            PerceptualIssue issue;
            issue.type = PerceptualIssue::ShadingOverSpill;
            issue.region = QRectF(0, 0, 1, 1);
            issue.severity = qBound<qreal>(0.0, qMin<qreal>(1.0, spillCount / 1000.0), 1.0);
            issue.description = QStringLiteral("Shading extends beyond Flats silhouette in %1 pixels").arg(spillCount);
            issue.requiresUserConsent = false;
            plan.issues.append(issue);

            // 修正案: 該当 Shading op を Flats の内側にクリップ (代表 1 op)
            for (int i = 0; i < program.operations.size(); ++i) {
                const KisAiStrokeOperation &op = program.operations.at(i);
                if (normalizeLayerLocal(op.layer) != QStringLiteral("Shading"))
                    continue;
                if (op.polygon.isEmpty())
                    continue;
                PerceptualFix fix;
                fix.issue = issue;
                fix.action = PerceptualFix::ClipPolygon;
                fix.targetOpIndex = i;
                // クリップ後のポリゴンは元の 0.95 倍に縮小 (近似)。厳密には Sutherland-Hodgman だが、
                // コスト的に代表点縮小で代用。
                QPolygonF shrunk;
                QPointF centroid;
                for (int p = 0; p < op.polygon.size(); ++p) {
                    centroid += op.polygon.at(p);
                }
                if (!op.polygon.isEmpty()) {
                    centroid /= op.polygon.size();
                    for (int p = 0; p < op.polygon.size(); ++p) {
                        const QPointF pt = op.polygon.at(p);
                        shrunk.append(centroid + (pt - centroid) * 0.95);
                    }
                }
                fix.newPolygon = shrunk;
                fix.description = QStringLiteral("Clip Shading polygon to within Flats (centroid shrink)");
                plan.fixes.append(fix);
                plan.autoFixSummaries.append(fix.description);
                break; // 代表 1 件のみ
            }
        }
    }

    // ---- 4. 顔ハッチ検出 ----
    {
        if (spec) {
            const QRect face = faceBox(spec->composition.headCenter,
                                       spec->composition.headHeight,
                                       flatsMask.width(),
                                       flatsMask.height());
            // 顔領域内の Shading / Hatch op をカウント
            int hatchOnFaceCount = 0;
            for (int i = 0; i < program.operations.size(); ++i) {
                const KisAiStrokeOperation &op = program.operations.at(i);
                const QString layer = normalizeLayerLocal(op.layer);
                if (op.kind != KisAiStrokeOperation::Kind::Hatch)
                    continue;
                // バウンスまたは polygon が顔 BBox と重なる
                QRectF polyBounds;
                if (!op.polygon.isEmpty()) {
                    polyBounds = op.polygon.boundingRect();
                    polyBounds = QRectF(polyBounds.x() * flatsMask.width(),
                                        polyBounds.y() * flatsMask.height(),
                                        polyBounds.width() * flatsMask.width(),
                                        polyBounds.height() * flatsMask.height());
                }
                if (face.intersects(polyBounds.toRect())) {
                    ++hatchOnFaceCount;
                    PerceptualIssue issue;
                    issue.type = PerceptualIssue::HatchOnFace;
                    issue.region = normalizedBounds(QRectF(face), flatsMask.width(), flatsMask.height());
                    issue.severity = 0.9;
                    issue.description = QStringLiteral("Hatch shading detected on face region");
                    issue.requiresUserConsent = false;
                    plan.issues.append(issue);

                    PerceptualFix fix;
                    fix.issue = issue;
                    fix.action = PerceptualFix::ReplaceOpKind;
                    fix.targetOpIndex = i;
                    fix.replaceKindName = QStringLiteral("Fill");
                    fix.newColor = op.brush.color;
                    fix.newOpacity = qBound<qreal>(0.05, op.brush.opacity * 0.75, 0.40);
                    fix.description = QStringLiteral("Demote face hatch to soft Fill (watercolor 0.30)");
                    plan.fixes.append(fix);
                    plan.autoFixSummaries.append(fix.description);
                }
            }
        }
    }

    // ---- 5. 左右非対称検出 (Lineart op の x 座標分布) ----
    {
        if (spec && program.operations.size() >= 2) {
            const qreal axis = spec->composition.headCenter.x();
            qreal leftCount = 0, rightCount = 0;
            for (int i = 0; i < program.operations.size(); ++i) {
                const KisAiStrokeOperation &op = program.operations.at(i);
                if (op.kind != KisAiStrokeOperation::Kind::Path)
                    continue;
                if (op.points.isEmpty())
                    continue;
                // op 中心 x
                qreal cx = 0;
                for (int p = 0; p < op.points.size(); ++p)
                    cx += op.points.at(p).pos.x();
                cx /= op.points.size();
                if (cx < axis)
                    ++leftCount;
                else
                    ++rightCount;
            }
            const qreal total = leftCount + rightCount;
            if (total > 0) {
                const qreal ratio = qAbs(leftCount - rightCount) / total;
                if (ratio > 0.30) {
                    PerceptualIssue issue;
                    issue.type = PerceptualIssue::AsymmetryEye;
                    issue.region = QRectF(0.1, 0.1, 0.8, 0.8);
                    issue.severity = qBound<qreal>(0.0, ratio, 1.0);
                    issue.description =
                        QStringLiteral("Asymmetric Path distribution: L=%1 R=%2").arg(leftCount).arg(rightCount);
                    issue.requiresUserConsent = true;
                    plan.issues.append(issue);
                    plan.hasUserConsentRequired = true;
                    plan.consentSummaries.append(issue.description);
                }
            }
        }
    }

    // ---- 6. カラーバンディング検出 (renderedImage ラプラシアン) ----
    {
        if (!renderedImage.isNull() && renderedImage.width() >= 16 && renderedImage.height() >= 16) {
            const int W2 = renderedImage.width();
            const int H2 = renderedImage.height();
            QVector<qreal> lap;
            lap.reserve((W2 - 2) * (H2 - 2));
            for (int y = 1; y < H2 - 1; ++y) {
                const QRgb *prev = reinterpret_cast<const QRgb *>(renderedImage.constScanLine(y - 1));
                const QRgb *curr = reinterpret_cast<const QRgb *>(renderedImage.constScanLine(y));
                const QRgb *next = reinterpret_cast<const QRgb *>(renderedImage.constScanLine(y + 1));
                for (int x = 1; x < W2 - 1; ++x) {
                    const qreal c = qRed(curr[x]) / 255.0;
                    const qreal l = qRed(prev[x]) / 255.0;
                    const qreal r = qRed(next[x]) / 255.0;
                    const qreal u = qRed(curr[x - 1]) / 255.0;
                    const qreal d = qRed(curr[x + 1]) / 255.0;
                    const qreal v = qAbs(4.0 * c - l - r - u - d);
                    lap.append(v);
                }
            }
            if (lap.size() > 100) {
                std::sort(lap.begin(), lap.end());
                const qreal p95 = lap.at(int(lap.size() * 0.95));
                if (p95 > 0.18) {
                    PerceptualIssue issue;
                    issue.type = PerceptualIssue::ColorBanding;
                    issue.region = QRectF(0, 0, 1, 1);
                    issue.severity = qBound<qreal>(0.0, qMin<qreal>(1.0, p95), 1.0);
                    issue.description = QStringLiteral("Color banding suspected (p95 laplacian=%1)").arg(p95);
                    issue.requiresUserConsent = false;
                    plan.issues.append(issue);

                    PerceptualFix fix;
                    fix.issue = issue;
                    fix.action = PerceptualFix::AddDither;
                    fix.targetOpIndex = -1;
                    fix.description = QStringLiteral("Add subtle dither to reduce 8-bit banding");
                    plan.fixes.append(fix);
                    plan.autoFixSummaries.append(fix.description);
                }
            }
        }
    }

    // ---- 7. 線画太さジッタ (Path op 内の隣接圧力差) ----
    {
        int jitterOps = 0;
        for (int i = 0; i < program.operations.size(); ++i) {
            const KisAiStrokeOperation &op = program.operations.at(i);
            if (op.kind != KisAiStrokeOperation::Kind::Path)
                continue;
            if (op.points.size() < 4)
                continue;
            qreal sumJitter = 0.0;
            for (int p = 1; p < op.points.size(); ++p) {
                const qreal delta = qAbs(op.points.at(p).pressure - op.points.at(p - 1).pressure);
                sumJitter += delta;
            }
            const qreal avgJitter = sumJitter / (op.points.size() - 1);
            if (avgJitter > 0.25)
                ++jitterOps;
        }
        if (jitterOps >= 1) {
            PerceptualIssue issue;
            issue.type = PerceptualIssue::LineartThicknessJitter;
            issue.region = QRectF(0, 0, 1, 1);
            issue.severity = qBound<qreal>(0.0, qMin<qreal>(1.0, qreal(jitterOps) / 5.0), 1.0);
            issue.description = QStringLiteral("Lineart pressure jitter detected in %1 path(s)").arg(jitterOps);
            issue.requiresUserConsent = false;
            plan.issues.append(issue);

            for (int i = 0; i < program.operations.size(); ++i) {
                KisAiStrokeOperation op = program.operations.at(i);
                if (op.kind != KisAiStrokeOperation::Kind::Path)
                    continue;
                if (op.points.size() < 4)
                    continue;
                PerceptualFix fix;
                fix.issue = issue;
                fix.action = PerceptualFix::SmoothControlPoints;
                fix.targetOpIndex = i;
                fix.description = QStringLiteral("Smooth pressure sequence for path '%1'").arg(op.id);
                plan.fixes.append(fix);
                plan.autoFixSummaries.append(fix.description);
            }
        }
    }

    // 期待改善量
    if (!plan.issues.isEmpty()) {
        qreal sum = 0;
        for (int i = 0; i < plan.issues.size(); ++i)
            sum += plan.issues.at(i).severity;
        plan.expectedImprovement = qBound<qreal>(0.0, sum / plan.issues.size() * 0.25, 0.5);
    }

    return plan;
}

// ---------------------------------------------------------------------------
// 適用 (apply)
// ---------------------------------------------------------------------------

KisAiStrokeProgram KisAiPerceptualRepairer::apply(const KisAiStrokeProgram &program,
                                                  const PerceptualRepairPlan &plan,
                                                  bool includeConsentFixes)
{
    KisAiStrokeProgram result = program;

    for (int f = 0; f < plan.fixes.size(); ++f) {
        const PerceptualFix &fix = plan.fixes.at(f);
        if (fix.issue.requiresUserConsent && !includeConsentFixes)
            continue;

        switch (fix.action) {
        case PerceptualFix::NoOp:
            break;
        case PerceptualFix::DropOp: {
            if (fix.targetOpIndex < 0 || fix.targetOpIndex >= result.operations.size())
                break;
            result.operations.remove(fix.targetOpIndex);
            break;
        }
        case PerceptualFix::ReplaceOpKind: {
            if (fix.targetOpIndex < 0 || fix.targetOpIndex >= result.operations.size())
                break;
            KisAiStrokeOperation op = result.operations.at(fix.targetOpIndex);
            if (fix.replaceKindName == QLatin1String("Fill"))
                op.kind = KisAiStrokeOperation::Kind::Fill;
            else if (fix.replaceKindName == QLatin1String("GradientFill"))
                op.kind = KisAiStrokeOperation::Kind::GradientFill;
            else if (fix.replaceKindName == QLatin1String("Path"))
                op.kind = KisAiStrokeOperation::Kind::Path;
            op.brush.profile = QStringLiteral("watercolor");
            if (fix.newColor.isValid())
                op.brush.color = fix.newColor;
            op.brush.opacity = fix.newOpacity;
            result.operations[fix.targetOpIndex] = op;
            break;
        }
        case PerceptualFix::InsertPolygonFill: {
            KisAiStrokeOperation op;
            op.kind = KisAiStrokeOperation::Kind::Fill;
            op.id = QStringLiteral("pr_hole_fill_%1").arg(result.operations.size());
            op.layer = QStringLiteral("Flats");
            op.polygon = fix.newPolygon;
            op.brush.color = fix.newColor.isValid() ? fix.newColor : QColor(200, 200, 200);
            op.brush.opacity = fix.newOpacity > 0 ? fix.newOpacity : 0.85;
            op.brush.profile = QStringLiteral("watercolor");
            op.fillStyle = QStringLiteral("wash");
            op.closed = true;
            op.smooth = true;
            result.operations.append(op);
            break;
        }
        case PerceptualFix::ClipPolygon: {
            if (fix.targetOpIndex < 0 || fix.targetOpIndex >= result.operations.size())
                break;
            KisAiStrokeOperation op = result.operations.at(fix.targetOpIndex);
            if (!fix.newPolygon.isEmpty())
                op.polygon = fix.newPolygon;
            result.operations[fix.targetOpIndex] = op;
            break;
        }
        case PerceptualFix::SmoothControlPoints: {
            if (fix.targetOpIndex < 0 || fix.targetOpIndex >= result.operations.size())
                break;
            KisAiStrokeOperation op = result.operations.at(fix.targetOpIndex);
            if (op.points.size() < 3)
                break;
            // 3 点移動平均で圧力を平滑化
            QVector<qreal> smoothed;
            smoothed.reserve(op.points.size());
            smoothed.append(op.points.first().pressure);
            for (int i = 1; i + 1 < op.points.size(); ++i) {
                const qreal p = op.points.at(i - 1).pressure * 0.25 + op.points.at(i).pressure * 0.50
                    + op.points.at(i + 1).pressure * 0.25;
                smoothed.append(p);
            }
            smoothed.append(op.points.last().pressure);
            for (int i = 0; i < op.points.size(); ++i) {
                op.points[i].pressure = smoothed.at(i);
            }
            result.operations[fix.targetOpIndex] = op;
            break;
        }
        case PerceptualFix::AddDither: {
            // FX レイヤーに 1px 程度の微小粒子を追加 (Flats のバンディング抑制)
            KisAiStrokeOperation op;
            op.kind = KisAiStrokeOperation::Kind::Particles;
            op.id = QStringLiteral("pr_dither_%1").arg(result.operations.size());
            op.layer = QStringLiteral("FX");
            op.bounds = QRectF(0, 0, 1, 1);
            op.particleShape = QStringLiteral("dot");
            op.particleCount = 64;
            op.brush.color = QColor(128, 128, 128);
            op.brush.opacity = 0.04;
            op.brush.profile = QStringLiteral("splatter");
            op.brush.size = 0.0015;
            result.operations.append(op);
            break;
        }
        case PerceptualFix::ResizeOp:
            break;
        }
    }

    return result;
}

KisAiStrokeProgram KisAiPerceptualRepairer::autoRepair(const KisAiStrokeProgram &program,
                                                       const QImage &renderedImage,
                                                       const KisAiSceneSpec *spec,
                                                       PerceptualRepairPlan *outPlan)
{
    PerceptualRepairPlan plan = diagnose(program, renderedImage, spec);
    if (outPlan)
        *outPlan = plan;
    return apply(program, plan, false);
}

} // namespace KisAi