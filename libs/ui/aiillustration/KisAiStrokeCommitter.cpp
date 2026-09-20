/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeCommitter.h"

#include "KisAiDeliberateStroke.h"
#include "KisAiPrimitiveExpander.h"
#include "KisAiStrokeGraph.h"
#include "KisAiStrokeQualityUtils.h"
#include "KisAiStrokeRenderer.h"

#include <QPainter>
#include <QtMath>

#include <cmath>

KisAiStrokeCommitLog KisAiStrokeCommitter::s_lastLog;

namespace
{
QRect dirtyRectPx(const KisAiStrokeOperation &op, const QSize &workingSize)
{
    const KisAiStrokeCommitReview rev = KisAiDeliberateStroke::reviewStroke(op, workingSize);
    QRect r = rev.dirtyRect.toAlignedRect().adjusted(-4, -4, 4, 4);
    return r.intersected(QRect(0, 0, workingSize.width(), workingSize.height()));
}

int opaqueDelta(const QImage &before, const QImage &after, const QRect &region)
{
    if (before.isNull() || after.isNull() || region.isEmpty())
        return 0;
    const QRect r = region.intersected(before.rect()).intersected(after.rect());
    int gained = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            const int a0 = qAlpha(before.pixel(x, y));
            const int a1 = qAlpha(after.pixel(x, y));
            if (a1 > a0 + 8)
                ++gained;
        }
    }
    return gained;
}

QPointF scalePoint(const QPointF &pt, const QSize &size)
{
    return QPointF(pt.x() * size.width(), pt.y() * size.height());
}

QPolygonF scalePolygon(const QPolygonF &poly, const QSize &size)
{
    QPolygonF res;
    res.reserve(poly.size());
    for (const QPointF &p : poly)
        res.append(scalePoint(p, size));
    return res;
}
} // namespace

QVector<KisAiStrokeOperation> KisAiStrokeCommitter::prepareAtomicOps(const QVector<KisAiStrokeOperation> &operations,
                                                                     const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> expanded = KisAiPrimitiveExpander::expandAll(operations, canvasSize);
    KisAiStrokeGraph::snapTStops(expanded, canvasSize, 1.2);
    return KisAiStrokeGraph::orderForCommit(expanded, canvasSize);
}

KisAiStrokeOperation KisAiStrokeCommitter::stabilizeOperation(const KisAiStrokeOperation &op, const QSize &canvasSize)
{
    KisAiStrokeOperation out = op;
    if (op.kind == KisAiStrokeOperation::Kind::Path && op.points.size() >= 3) {
        out.points = KisAiDeliberateStroke::applyInkDynamics(
            KisAiDeliberateStroke::stabilizeStroke(op.points,
                                                   canvasSize,
                                                   op.closed,
                                                   KisAiStrokeProgramCodec::stableSeed(op.id)),
            canvasSize);
    } else if (op.kind == KisAiStrokeOperation::Kind::Ribbon) {
        QVector<KisAiStrokePoint> spinePts;
        QVector<QPointF> raw = op.spine;
        if (raw.isEmpty()) {
            raw.reserve(op.points.size());
            for (const KisAiStrokePoint &p : op.points)
                raw.append(p.pos);
        }
        spinePts.reserve(raw.size());
        for (const QPointF &p : raw)
            spinePts.append(KisAiStrokePoint(p.x(), p.y(), 0.8));
        const QVector<KisAiStrokePoint> stable =
            KisAiDeliberateStroke::stabilizeStroke(spinePts,
                                                   canvasSize,
                                                   false,
                                                   KisAiStrokeProgramCodec::stableSeed(op.id));
        out.spine.clear();
        out.points.clear();
        for (const KisAiStrokePoint &p : stable) {
            out.spine.append(p.pos);
            out.points.append(p);
        }
    }
    return out;
}

KisAiStrokeOperation KisAiStrokeCommitter::repairOperation(const KisAiStrokeOperation &op,
                                                           const KisAiStrokeLintReport &lint,
                                                           const QSize &canvasSize)
{
    KisAiStrokeOperation out = op;
    if (!lint.needsRepair)
        return out;

    if (lint.reasons.contains(QLatin1String("invalid-brush"))) {
        if (!out.brush.color.isValid())
            out.brush.color = QColor(20, 16, 28);
        if (!(out.brush.size > 0.0))
            out.brush.size = 0.004;
        if (!(out.brush.opacity > 0.0))
            out.brush.opacity = 1.0;
    }
    if (lint.reasons.contains(QLatin1String("invalid-ribbon-width"))) {
        out.widthStart = qMax<qreal>(0.002, out.widthStart);
        out.widthMid = qMax<qreal>(0.002, out.widthMid);
        out.widthEnd = qMax<qreal>(0.001, out.widthEnd);
    }
    if (out.kind == KisAiStrokeOperation::Kind::Path && out.points.size() >= 3
        && (lint.reasons.contains(QLatin1String("self-intersecting"))
            || lint.reasons.contains(QLatin1String("high-curvature-jitter")))) {
        QVector<QPointF> raw;
        raw.reserve(out.points.size());
        for (const KisAiStrokePoint &p : out.points)
            raw.append(p.pos);
        const qreal minDim = qMax<qreal>(64.0, qMin(canvasSize.width(), canvasSize.height()));
        const QVector<QPointF> simplified = KisAiStrokeQualityUtils::simplifyRDP(raw, 2.4 / minDim);
        if (simplified.size() >= 2) {
            QVector<KisAiStrokePoint> rebuilt;
            rebuilt.reserve(simplified.size());
            for (const QPointF &sp : simplified) {
                int best = 0;
                qreal bestD = 1e18;
                for (int i = 0; i < out.points.size(); ++i) {
                    const QPointF d = out.points.at(i).pos - sp;
                    const qreal dist = d.x() * d.x() + d.y() * d.y();
                    if (dist < bestD) {
                        bestD = dist;
                        best = i;
                    }
                }
                rebuilt.append(KisAiStrokePoint(sp.x(), sp.y(), out.points.at(best).pressure));
            }
            out.points = KisAiStrokeQualityUtils::resampleEquidistant(rebuilt, 3.0 / minDim, out.closed);
        }
    }
    return out;
}

KisAiStrokeCommitReview
KisAiStrokeCommitter::reviewPixels(const QImage &before, const QImage &after, const QRect &dirtyPx)
{
    KisAiStrokeCommitReview rev;
    rev.dirtyRect = QRectF(dirtyPx);
    const int gained = opaqueDelta(before, after, dirtyPx);
    const qreal area = qMax(1, dirtyPx.width() * dirtyPx.height());
    rev.inkCoverage = qreal(gained) / area;
    if (gained <= 0) {
        rev.committed = false;
        rev.notes << QStringLiteral("zero-coverage-skip");
    }
    return rev;
}

QVector<KisAiStrokeOperation> KisAiStrokeCommitter::commitToPainter(QPainter &painter,
                                                                    const QVector<KisAiStrokeOperation> &operations,
                                                                    const QSize &workingSize,
                                                                    const QSize &logicalSize,
                                                                    int supersampleScale,
                                                                    const QPainterPath &faceExclusionPath,
                                                                    const QMap<QString, QPolygonF> &globalSilhouettes,
                                                                    KisAiStrokeCommitLog *log)
{
    KisAiStrokeCommitLog local;
    QVector<KisAiStrokeOperation> kept;
    kept.reserve(operations.size());

    QMap<QString, QPolygonF> silhouettes = globalSilhouettes;
    for (const KisAiStrokeOperation &op : operations) {
        if (op.id.isEmpty())
            continue;
        if (op.polygon.size() >= 3)
            silhouettes.insert(op.id, op.polygon);
    }

    QString currentGroup;
    QVector<KisAiStrokeOperation> groupOps;
    QVector<KisAiStrokeOperation> eyeOps;

    auto flushGroup = [&]() {
        if (currentGroup.isEmpty() || groupOps.isEmpty())
            return;
        // Eyes are critiqued as a pair after both sides commit.
        if (!currentGroup.startsWith(QLatin1String("eye"))) {
            const QStringList warnings = KisAiStrokeGraph::critiqueGroup(currentGroup, groupOps);
            if (!warnings.isEmpty()) {
                local.lines << QStringLiteral("group %1: %2").arg(currentGroup, warnings.join(QLatin1Char(',')));
            }
        }
        groupOps.clear();
    };

    for (const KisAiStrokeOperation &raw : operations) {
        KisAiStrokeOperation candidate = stabilizeOperation(raw, logicalSize);
        KisAiStrokeLintReport lint = KisAiDeliberateStroke::lintStroke(candidate, logicalSize);
        int retries = 0;
        while (lint.needsRepair && retries < 2 && !lint.drop) {
            candidate = repairOperation(candidate, lint, logicalSize);
            candidate = stabilizeOperation(candidate, logicalSize);
            lint = KisAiDeliberateStroke::lintStroke(candidate, logicalSize);
            ++retries;
            ++local.repaired;
        }
        if (retries > 0)
            local.retried += retries;

        if (lint.drop) {
            ++local.skipped;
            local.lines << QStringLiteral("skip %1 (%2)").arg(raw.id, lint.reasons.join(QLatin1Char('+')));
            continue;
        }

        const KisAiStrokeCommitReview geom = KisAiDeliberateStroke::reviewStroke(candidate, logicalSize);
        if (!geom.committed) {
            ++local.skipped;
            local.lines << QStringLiteral("skip %1 (zero-coverage)").arg(raw.id);
            continue;
        }

        const QRect dirty = dirtyRectPx(candidate, workingSize);

        auto paintOne = [&](QPainter &target, const KisAiStrokeOperation &op) {
            QPolygonF clipPoly;
            if (!op.clipToId.isEmpty() && silhouettes.contains(op.clipToId))
                clipPoly = silhouettes.value(op.clipToId);
            else if (op.kind == KisAiStrokeOperation::Kind::Path && op.polygon.size() >= 3)
                clipPoly = op.polygon;
            if (clipPoly.size() >= 3) {
                target.save();
                QPainterPath clipP;
                clipP.addPolygon(scalePolygon(clipPoly, workingSize));
                target.setClipPath(clipP, Qt::IntersectClip);
                KisAiStrokeRenderer::rasterizeOperation(target, op, workingSize, supersampleScale, faceExclusionPath);
                target.restore();
                return;
            }
            KisAiStrokeRenderer::rasterizeOperation(target, op, workingSize, supersampleScale, faceExclusionPath);
        };

        bool accept = true;
        paintOne(painter, candidate);
        Q_UNUSED(dirty);

        if (!accept) {
            ++local.skipped;
            local.lines << QStringLiteral("skip %1 (pixel-review)").arg(raw.id);
            continue;
        }

        ++local.committed;
        local.lines << QStringLiteral("commit %1").arg(candidate.id);
        kept.append(candidate);

        const QString g = candidate.groupId.isEmpty() ? KisAiStrokeGraph::inferGroupId(candidate) : candidate.groupId;
        if (g != currentGroup) {
            flushGroup();
            currentGroup = g;
        }
        groupOps.append(candidate);
        if (g.startsWith(QLatin1String("eye")))
            eyeOps.append(candidate);
    }
    flushGroup();
    if (!eyeOps.isEmpty()) {
        const QStringList eyeWarn = KisAiStrokeGraph::critiqueGroup(QStringLiteral("eye"), eyeOps);
        if (!eyeWarn.isEmpty()) {
            local.lines << QStringLiteral("group eye: %1").arg(eyeWarn.join(QLatin1Char(',')));
            ++local.retried;
        }
    }

    s_lastLog = local;
    if (log)
        *log = local;
    return kept;
}

const KisAiStrokeCommitLog &KisAiStrokeCommitter::lastLog()
{
    return s_lastLog;
}

qreal KisAiStrokeCommitter::atomicStrokeRatio(const QVector<KisAiStrokeOperation> &ops)
{
    if (ops.isEmpty())
        return 1.0;
    const int leftover = KisAiPrimitiveExpander::leftoverCompositeCount(ops);
    return 1.0 - qreal(leftover) / qreal(ops.size());
}
