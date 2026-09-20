/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPrimitiveExpander.h"

#include "KisAiStrokeGraph.h"

#include <QRandomGenerator>
#include <QtMath>

#include <cmath>

namespace
{
constexpr qreal PI = 3.14159265358979323846;

QVector<KisAiStrokePoint>
sampleQuad(const QPointF &a, const QPointF &ctrl, const QPointF &b, int steps, qreal p0, qreal p1)
{
    QVector<KisAiStrokePoint> pts;
    pts.reserve(steps + 1);
    for (int i = 0; i <= steps; ++i) {
        const qreal t = qreal(i) / qreal(steps);
        const qreal it = 1.0 - t;
        const QPointF p = it * it * a + 2.0 * it * t * ctrl + t * t * b;
        const qreal pr = p0 + (p1 - p0) * t;
        pts.append(KisAiStrokePoint(p.x(), p.y(), pr));
    }
    return pts;
}

QPolygonF ellipsePoly(const QPointF &c, qreal rx, qreal ry, int steps = 16)
{
    QPolygonF poly;
    poly.reserve(steps);
    for (int i = 0; i < steps; ++i) {
        const qreal a = 2.0 * PI * qreal(i) / qreal(steps);
        poly.append(QPointF(c.x() + std::cos(a) * rx, c.y() + std::sin(a) * ry));
    }
    return poly;
}

QString sideTag(const KisAiStrokeOperation &op)
{
    if (op.eyeIsRight || op.id.contains(QLatin1String("_r"), Qt::CaseInsensitive) || op.id.endsWith(QLatin1String("r")))
        return QStringLiteral("r");
    return QStringLiteral("l");
}
} // namespace

bool KisAiPrimitiveExpander::isCompositeKind(KisAiStrokeOperation::Kind kind)
{
    return kind == KisAiStrokeOperation::Kind::AnimeEye || kind == KisAiStrokeOperation::Kind::AnimeMouth
        || kind == KisAiStrokeOperation::Kind::Hatch || kind == KisAiStrokeOperation::Kind::MangaLines
        || kind == KisAiStrokeOperation::Kind::Particles;
}

int KisAiPrimitiveExpander::atomicPathCount(const QVector<KisAiStrokeOperation> &ops)
{
    int n = 0;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.kind == KisAiStrokeOperation::Kind::Path)
            ++n;
    }
    return n;
}

int KisAiPrimitiveExpander::leftoverCompositeCount(const QVector<KisAiStrokeOperation> &ops)
{
    int n = 0;
    for (const KisAiStrokeOperation &op : ops) {
        if (isCompositeKind(op.kind))
            ++n;
    }
    return n;
}

KisAiStrokeOperation KisAiPrimitiveExpander::makePath(const QString &id,
                                                      const QString &groupId,
                                                      const QString &layer,
                                                      const QVector<KisAiStrokePoint> &pts,
                                                      const QColor &color,
                                                      const QString &profile,
                                                      qreal size,
                                                      qreal opacity,
                                                      const QString &role,
                                                      const QString &parentId)
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.id = id;
    op.groupId = groupId;
    op.parentId = parentId;
    op.role = role;
    op.layer = layer;
    op.points = pts;
    op.smooth = true;
    op.closed = false;
    op.brush.profile = profile;
    op.brush.color = color;
    op.brush.size = size;
    op.brush.opacity = opacity;
    return op;
}

KisAiStrokeOperation KisAiPrimitiveExpander::makeFill(const QString &id,
                                                      const QString &groupId,
                                                      const QString &layer,
                                                      const QPolygonF &poly,
                                                      const QColor &color,
                                                      qreal opacity,
                                                      const QString &role,
                                                      const QString &clipToId)
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.id = id;
    op.groupId = groupId;
    op.role = role;
    op.layer = layer;
    op.polygon = poly;
    op.brush.color = color;
    op.brush.opacity = opacity;
    op.brush.profile = QStringLiteral("auto");
    op.clipToId = clipToId;
    op.fillStyle = QStringLiteral("contour");
    return op;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandEye(const KisAiStrokeOperation &op, const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    const QString side = sideTag(op);
    const QString group = op.groupId.isEmpty() ? QStringLiteral("eye_%1").arg(side) : op.groupId;
    const QPointF c = op.eyeCenter;
    const qreal w = op.eyeSize.width();
    const qreal h = op.eyeSize.height();
    if (w < 0.004 || h < 0.004)
        return out;

    const qreal outerSign = op.eyeIsRight ? 1.0 : -1.0;
    const qreal innerSign = -outerSign;
    const QColor iris = op.eyeIrisColor.isValid() ? op.eyeIrisColor : QColor(60, 120, 240);
    QColor dark = iris.darker(300).darker(140);
    dark.setAlpha(255);
    const QString scleraId = QStringLiteral("%1_sclera").arg(group);

    out.append(makeFill(scleraId,
                        group,
                        QStringLiteral("Flats"),
                        ellipsePoly(c, w * 0.50, h * 0.48),
                        QColor(252, 252, 255),
                        1.0,
                        QStringLiteral("mass")));

    const QString irisId = QStringLiteral("%1_iris").arg(group);
    const QPointF irisC(c.x(), c.y() - h * 0.02);
    out.append(makeFill(irisId,
                        group,
                        QStringLiteral("Flats"),
                        ellipsePoly(irisC, w * 0.31, h * 0.41),
                        iris,
                        1.0,
                        QStringLiteral("mass"),
                        scleraId));

    const QColor glow = op.eyeSecondaryColor.isValid() ? op.eyeSecondaryColor : iris.lighter(150);
    out.append(makeFill(QStringLiteral("%1_iris_glow").arg(group),
                        group,
                        QStringLiteral("Flats"),
                        ellipsePoly(QPointF(c.x(), c.y() + h * 0.10), w * 0.22, h * 0.18),
                        glow,
                        0.85,
                        QStringLiteral("mass"),
                        irisId));

    // Geometric eye center stays on iris body; pupil sits above it.
    out.append(makeFill(QStringLiteral("%1_pupil").arg(group),
                        group,
                        QStringLiteral("Flats"),
                        ellipsePoly(QPointF(c.x(), c.y() - h * 0.14), w * 0.07, h * 0.10),
                        dark,
                        1.0,
                        QStringLiteral("mass"),
                        irisId));
    // Upper limbal arc only. A full-ellipse Path envelope would flood the iris.
    QVector<KisAiStrokePoint> limbal;
    for (int i = 0; i <= 8; ++i) {
        const qreal t = PI * (0.15 + 0.70 * qreal(i) / 8.0);
        limbal.append(KisAiStrokePoint(irisC.x() + std::cos(t) * w * 0.31, irisC.y() - std::sin(t) * h * 0.41, 0.7));
    }
    KisAiStrokeOperation limbalOp = makePath(QStringLiteral("%1_limbal").arg(group),
                                             group,
                                             QStringLiteral("Lineart"),
                                             limbal,
                                             dark,
                                             QStringLiteral("fineliner"),
                                             0.0016,
                                             0.85,
                                             QStringLiteral("internal"),
                                             irisId);
    limbalOp.closed = false;
    out.append(limbalOp);
    const int striations = 8;
    for (int i = 0; i < striations; ++i) {
        const qreal angle = (PI * 0.15) + (PI * 0.70) * (qreal(i) / qreal(striations - 1));
        QVector<KisAiStrokePoint> st;
        st.append(
            KisAiStrokePoint(c.x() + std::cos(angle) * w * 0.08, c.y() - h * 0.08 + std::sin(angle) * h * 0.10, 0.3));
        st.append(
            KisAiStrokePoint(c.x() + std::cos(angle) * w * 0.22, c.y() - h * 0.04 + std::sin(angle) * h * 0.28, 0.6));
        out.append(makePath(QStringLiteral("%1_striation_%2").arg(group).arg(i),
                            group,
                            QStringLiteral("Highlights"),
                            st,
                            iris.lighter(140),
                            QStringLiteral("fineliner"),
                            0.0009,
                            0.45,
                            QStringLiteral("internal"),
                            irisId));
    }

    out.append(makeFill(QStringLiteral("%1_catch_main").arg(group),
                        group,
                        QStringLiteral("Highlights"),
                        ellipsePoly(QPointF(c.x() - w * 0.12, c.y() - h * 0.16), w * 0.045, h * 0.05),
                        QColor(255, 255, 255),
                        1.0,
                        QStringLiteral("accent"),
                        irisId));
    out.append(makeFill(QStringLiteral("%1_catch_sub").arg(group),
                        group,
                        QStringLiteral("Highlights"),
                        ellipsePoly(QPointF(c.x() + w * 0.10, c.y() + h * 0.08), w * 0.022, h * 0.024),
                        QColor(255, 255, 255),
                        0.85,
                        QStringLiteral("accent"),
                        irisId));

    const QPointF lashStart(c.x() + innerSign * w * 0.44, c.y() + h * 0.02);
    const QPointF lashCtrl(c.x() + outerSign * w * 0.05, c.y() - h * 0.56);
    const QPointF lashEnd(c.x() + outerSign * w * 0.46, c.y() - h * 0.12);
    QVector<KisAiStrokePoint> upper = sampleQuad(lashStart, lashCtrl, lashEnd, 10, 0.55, 0.95);
    const QPointF flickCtrl(c.x() + outerSign * w * 0.54, c.y() - h * 0.22);
    const QPointF flickEnd(c.x() + outerSign * w * 0.58, c.y() - h * 0.30);
    upper.append(sampleQuad(lashEnd, flickCtrl, flickEnd, 4, 0.90, 0.25));
    out.append(makePath(QStringLiteral("%1_lash_upper").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        upper,
                        dark,
                        QStringLiteral("gpen"),
                        qMax<qreal>(0.0045, h * 0.095),
                        1.0,
                        QStringLiteral("contour")));

    QVector<KisAiStrokePoint> clump1 = sampleQuad(QPointF(c.x() + outerSign * w * 0.38, c.y() - h * 0.35),
                                                  QPointF(c.x() + outerSign * w * 0.48, c.y() - h * 0.46),
                                                  QPointF(c.x() + outerSign * w * 0.54, c.y() - h * 0.50),
                                                  5,
                                                  0.7,
                                                  0.2);
    out.append(makePath(QStringLiteral("%1_lash_clump_1").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        clump1,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0018,
                        0.95,
                        QStringLiteral("contour"),
                        QStringLiteral("%1_lash_upper").arg(group)));

    QVector<KisAiStrokePoint> clump2 = sampleQuad(QPointF(c.x() + outerSign * w * 0.46, c.y() - h * 0.20),
                                                  QPointF(c.x() + outerSign * w * 0.56, c.y() - h * 0.32),
                                                  QPointF(c.x() + outerSign * w * 0.62, c.y() - h * 0.34),
                                                  5,
                                                  0.65,
                                                  0.2);
    out.append(makePath(QStringLiteral("%1_lash_clump_2").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        clump2,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0015,
                        0.90,
                        QStringLiteral("contour"),
                        QStringLiteral("%1_lash_upper").arg(group)));

    QVector<KisAiStrokePoint> crease = sampleQuad(QPointF(c.x() + innerSign * w * 0.28, c.y() - h * 0.60),
                                                  QPointF(c.x() + outerSign * w * 0.05, c.y() - h * 0.68),
                                                  QPointF(c.x() + outerSign * w * 0.36, c.y() - h * 0.54),
                                                  6,
                                                  0.4,
                                                  0.55);
    out.append(makePath(QStringLiteral("%1_crease").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        crease,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0014,
                        0.75,
                        QStringLiteral("internal")));

    QVector<KisAiStrokePoint> lower1 = sampleQuad(QPointF(c.x() + outerSign * w * 0.16, c.y() + h * 0.46),
                                                  QPointF(c.x() + outerSign * w * 0.28, c.y() + h * 0.48),
                                                  QPointF(c.x() + outerSign * w * 0.38, c.y() + h * 0.38),
                                                  5,
                                                  0.5,
                                                  0.2);
    out.append(makePath(QStringLiteral("%1_lash_lower_1").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        lower1,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0014,
                        0.70,
                        QStringLiteral("contour"),
                        scleraId));
    QVector<KisAiStrokePoint> lower2 = sampleQuad(QPointF(c.x() + outerSign * w * 0.32, c.y() + h * 0.44),
                                                  QPointF(c.x() + outerSign * w * 0.40, c.y() + h * 0.48),
                                                  QPointF(c.x() + outerSign * w * 0.44, c.y() + h * 0.52),
                                                  4,
                                                  0.45,
                                                  0.18);
    out.append(makePath(QStringLiteral("%1_lash_lower_2").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        lower2,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0012,
                        0.65,
                        QStringLiteral("contour"),
                        scleraId));
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandMouth(const KisAiStrokeOperation &op,
                                                                  const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    const QString group = op.groupId.isEmpty() ? QStringLiteral("mouth") : op.groupId;
    const QPointF c = op.mouthCenter;
    const qreal w = op.mouthSize.width();
    const qreal h = op.mouthSize.height();
    if (w < 0.002 || h < 0.001)
        return out;

    const qreal halfW = w * 0.5;
    const QColor lip = op.mouthLipColor.isValid() ? op.mouthLipColor : QColor(225, 115, 125);
    const QColor ink = lip.darker(220);
    const QString expr = op.mouthExpression.toLower();
    const bool isOpen =
        (expr == QLatin1String("open_smile") || expr == QLatin1String("small_open") || expr == QLatin1String("open"));
    const bool isSmile = (expr == QLatin1String("smile") || expr == QLatin1String("open_smile"));
    const bool isCat = (expr == QLatin1String("cat_mouth"));

    if (isOpen) {
        QPolygonF cavity;
        cavity.append(QPointF(c.x() - halfW * 0.85, c.y()));
        cavity.append(QPointF(c.x(), c.y() - h * 0.15));
        cavity.append(QPointF(c.x() + halfW * 0.85, c.y()));
        cavity.append(QPointF(c.x(), c.y() + h * 0.85));
        out.append(makeFill(QStringLiteral("%1_cavity").arg(group),
                            group,
                            QStringLiteral("Flats"),
                            cavity,
                            lip.darker(280),
                            1.0,
                            QStringLiteral("mass")));
    }

    QVector<KisAiStrokePoint> upper;
    if (isCat) {
        upper = sampleQuad(QPointF(c.x() - halfW, c.y()),
                           QPointF(c.x() - halfW * 0.5, c.y() - h * 0.4),
                           QPointF(c.x(), c.y()),
                           5,
                           0.4,
                           0.8);
        upper.append(sampleQuad(QPointF(c.x(), c.y()),
                                QPointF(c.x() + halfW * 0.5, c.y() - h * 0.4),
                                QPointF(c.x() + halfW, c.y()),
                                5,
                                0.8,
                                0.4));
    } else {
        const qreal arch = isSmile ? -h * 0.35 : (isOpen ? -h * 0.15 : 0.0);
        const QPointF left(c.x() - halfW, c.y() + (isSmile ? -h * 0.1 : 0.0));
        const QPointF right(c.x() + halfW, c.y() + (isSmile ? -h * 0.1 : 0.0));
        upper = sampleQuad(left, QPointF(c.x(), c.y() + arch), right, 10, 0.35, 0.35);
        if (!upper.isEmpty())
            upper[upper.size() / 2].pressure = 0.9;
    }
    const QString upperId = QStringLiteral("%1_upper_lip").arg(group);
    out.append(makePath(upperId,
                        group,
                        QStringLiteral("Lineart"),
                        upper,
                        ink,
                        QStringLiteral("gpen"),
                        0.0028,
                        1.0,
                        QStringLiteral("contour")));

    if (!upper.isEmpty()) {
        QVector<KisAiStrokePoint> cornerL;
        cornerL.append(KisAiStrokePoint(upper.first().pos.x(), upper.first().pos.y(), 1.0));
        out.append(makePath(QStringLiteral("%1_corner_l").arg(group),
                            group,
                            QStringLiteral("Lineart"),
                            cornerL,
                            ink,
                            QStringLiteral("gpen"),
                            0.0024,
                            1.0,
                            QStringLiteral("accent"),
                            upperId));
        QVector<KisAiStrokePoint> cornerR;
        cornerR.append(KisAiStrokePoint(upper.last().pos.x(), upper.last().pos.y(), 1.0));
        out.append(makePath(QStringLiteral("%1_corner_r").arg(group),
                            group,
                            QStringLiteral("Lineart"),
                            cornerR,
                            ink,
                            QStringLiteral("gpen"),
                            0.0024,
                            1.0,
                            QStringLiteral("accent"),
                            upperId));
    }

    if (!isOpen) {
        QVector<KisAiStrokePoint> lower = sampleQuad(QPointF(c.x() - halfW * 0.32, c.y() + h * 0.45),
                                                     QPointF(c.x(), c.y() + h * 0.55),
                                                     QPointF(c.x() + halfW * 0.32, c.y() + h * 0.45),
                                                     6,
                                                     0.4,
                                                     0.4);
        out.append(makePath(QStringLiteral("%1_lower_lip").arg(group),
                            group,
                            QStringLiteral("Lineart"),
                            lower,
                            lip.darker(150),
                            QStringLiteral("fineliner"),
                            0.0020,
                            0.70,
                            QStringLiteral("contour"),
                            upperId));
    }

    if (op.mouthHasHighlight) {
        const QPointF gloss(c.x() + halfW * 0.12, c.y() + (isOpen ? h * 0.75 : h * 0.32));
        out.append(makeFill(QStringLiteral("%1_gloss").arg(group),
                            group,
                            QStringLiteral("Highlights"),
                            ellipsePoly(gloss, w * 0.035, h * 0.07, 10),
                            QColor(255, 255, 255),
                            0.80,
                            QStringLiteral("accent")));
    }
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandHatch(const KisAiStrokeOperation &op,
                                                                  const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> out;
    if (op.polygon.size() < 3 || canvasSize.width() <= 0)
        return out;
    const QString group = op.groupId.isEmpty() ? (op.id.isEmpty() ? QStringLiteral("hatch") : op.id) : op.groupId;
    const QRectF b = op.polygon.boundingRect();
    const qreal minDim = qMax<qreal>(1.0, qMin(canvasSize.width(), canvasSize.height()));
    const qreal spacing = qMax<qreal>(0.004, op.spacing);
    const qreal radius = std::hypot(b.width(), b.height()) * 0.55;
    const int rawLines = qRound(radius * 2.0 / qMax<qreal>(0.002, spacing));
    const int numLines = qBound(2, rawLines, 48);
    const qreal effective = numLines > 0 ? qMax(spacing, radius * 2.0 / numLines) : spacing;
    const QPointF center = b.center();
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/hatch")));
    const qreal lineSize = (op.brush.sizeMode == QLatin1String("px")) ? qMax<qreal>(0.5, op.brush.size * 0.20)
                                                                      : qMax<qreal>(0.001, op.brush.size * 0.20);
    Q_UNUSED(minDim);

    auto emitPass = [&](qreal angleDeg, const QString &suffix) {
        const qreal rad = angleDeg * PI / 180.0;
        const QPointF dir(std::cos(rad), std::sin(rad));
        const QPointF norm(-std::sin(rad), std::cos(rad));
        int emitted = 0;
        for (int i = -numLines; i <= numLines && emitted < 64; ++i) {
            const qreal wobble = (rng.generateDouble() - 0.5) * effective * 0.15;
            const QPointF mid = center + norm * (i * effective);
            const QPointF p1 = mid - dir * radius + norm * wobble;
            const QPointF p2 = mid + dir * radius - norm * wobble;
            QVector<KisAiStrokePoint> pts;
            pts.append(KisAiStrokePoint(p1.x(), p1.y(), 0.50));
            pts.append(KisAiStrokePoint(p2.x(), p2.y(), 0.50));
            KisAiStrokeOperation line = makePath(QStringLiteral("%1_line_%2_%3").arg(group, suffix).arg(emitted),
                                                 group,
                                                 op.layer.isEmpty() ? QStringLiteral("Shading") : op.layer,
                                                 pts,
                                                 op.brush.color,
                                                 QStringLiteral("fineliner"),
                                                 lineSize,
                                                 qBound<qreal>(0.05, op.brush.opacity * 0.70, 1.0),
                                                 QStringLiteral("internal"));
            line.clipToId = op.id;
            line.polygon = op.polygon;
            line.brush.isEraser = op.brush.isEraser;
            line.brush.sizeMode = op.brush.sizeMode;
            out.append(line);
            ++emitted;
        }
    };

    emitPass(op.angleDeg, QStringLiteral("a"));
    if (op.crossHatch)
        emitPass(op.angleDeg + 90.0, QStringLiteral("b"));
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandMangaLines(const KisAiStrokeOperation &op,
                                                                       const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    const QString group = op.groupId.isEmpty() ? (op.id.isEmpty() ? QStringLiteral("manga") : op.id) : op.groupId;
    const QPointF center = op.gradientCenter;
    const int count = qBound(4, op.density, 64);
    const qreal jitter = qBound<qreal>(0.0, op.lineLengthJitter, 0.8);
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/manga")));
    for (int i = 0; i < count; ++i) {
        const qreal baseAngle = (2.0 * PI * i) / count;
        const qreal angle = baseAngle + (rng.generateDouble() - 0.5) * (2.0 * PI / count) * 0.4;
        const qreal cosA = std::cos(angle);
        const qreal sinA = std::sin(angle);
        const qreal innerDist = op.innerRadius * (1.0 + (rng.generateDouble() - 0.5) * jitter * 0.8);
        const qreal outerDist = op.outerRadius * (1.0 + (rng.generateDouble() - 0.5) * jitter);
        QVector<KisAiStrokePoint> pts;
        pts.append(KisAiStrokePoint(center.x() + cosA * innerDist, center.y() + sinA * innerDist, 0.25));
        pts.append(KisAiStrokePoint(center.x() + cosA * outerDist, center.y() + sinA * outerDist, 0.85));
        out.append(makePath(QStringLiteral("%1_ray_%2").arg(group).arg(i),
                            group,
                            op.layer.isEmpty() ? QStringLiteral("FX") : op.layer,
                            pts,
                            op.brush.color,
                            QStringLiteral("gpen"),
                            qMax<qreal>(0.0015, op.brush.size),
                            op.brush.opacity,
                            QStringLiteral("accent")));
    }
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandParticles(const KisAiStrokeOperation &op,
                                                                      const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    if (op.particleCount <= 0)
        return out;
    const QString group = op.groupId.isEmpty() ? (op.id.isEmpty() ? QStringLiteral("particles") : op.id) : op.groupId;
    const QRectF bounds = op.bounds.isValid() ? op.bounds : QRectF(0.0, 0.0, 1.0, 1.0);
    const int count = qBound(1, op.particleCount, 60);
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id.isEmpty() ? QStringLiteral("particles") : op.id));
    for (int i = 0; i < count; ++i) {
        const qreal x = bounds.left() + rng.generateDouble() * bounds.width();
        const qreal y = bounds.top() + rng.generateDouble() * bounds.height();
        const qreal r = 0.004 + rng.generateDouble() * 0.006;
        out.append(makeFill(QStringLiteral("%1_dot_%2").arg(group).arg(i),
                            group,
                            op.layer.isEmpty() ? QStringLiteral("FX") : op.layer,
                            ellipsePoly(QPointF(x, y), r, r, 8),
                            op.brush.color,
                            qBound<qreal>(0.05, op.brush.opacity * 0.85, 0.90),
                            QStringLiteral("accent")));
    }
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expand(const KisAiStrokeOperation &op, const QSize &canvasSize)
{
    switch (op.kind) {
    case KisAiStrokeOperation::Kind::AnimeEye:
        return expandEye(op, canvasSize);
    case KisAiStrokeOperation::Kind::AnimeMouth:
        return expandMouth(op, canvasSize);
    case KisAiStrokeOperation::Kind::Hatch:
        return expandHatch(op, canvasSize);
    case KisAiStrokeOperation::Kind::MangaLines:
        return expandMangaLines(op, canvasSize);
    case KisAiStrokeOperation::Kind::Particles:
        return expandParticles(op, canvasSize);
    default: {
        KisAiStrokeOperation annotated = op;
        if (annotated.groupId.isEmpty())
            annotated.groupId = KisAiStrokeGraph::inferGroupId(op);
        if (annotated.parentId.isEmpty())
            annotated.parentId = KisAiStrokeGraph::inferParentId(op);
        if (annotated.role.isEmpty() || annotated.role == QLatin1String("auto"))
            annotated.role = KisAiStrokeGraph::inferRole(op);
        return {annotated};
    }
    }
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandAll(const QVector<KisAiStrokeOperation> &ops,
                                                                const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> out;
    out.reserve(ops.size() * 4);
    for (const KisAiStrokeOperation &op : ops)
        out.append(expand(op, canvasSize));
    return out;
}
