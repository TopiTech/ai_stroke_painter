/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiDeliberateStroke.h"
#include "KisAiStrokeQualityUtils.h"

#include <QLineF>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace
{
qreal normalizedPolyArea(const QPolygonF &poly)
{
    qreal twice = 0.0;
    const int n = poly.size();
    for (int i = 0; i < n; ++i) {
        const QPointF &a = poly.at(i);
        const QPointF &b = poly.at((i + 1) % n);
        twice += a.x() * b.y() - b.x() * a.y();
    }
    return qAbs(twice) * 0.5;
}

qreal pathLengthPx(const QVector<KisAiStrokePoint> &pts, const QSize &canvas, bool closed)
{
    if (pts.size() < 2)
        return 0.0;
    qreal len = 0.0;
    const int n = pts.size();
    const int segs = closed ? n : (n - 1);
    for (int i = 0; i < segs; ++i) {
        const QPointF a(pts.at(i).pos.x() * canvas.width(),
                        pts.at(i).pos.y() * canvas.height());
        const QPointF b(pts.at((i + 1) % n).pos.x() * canvas.width(),
                        pts.at((i + 1) % n).pos.y() * canvas.height());
        len += std::hypot(b.x() - a.x(), b.y() - a.y());
    }
    return len;
}

bool allPointsOutside(const QVector<KisAiStrokePoint> &pts, qreal margin = 0.05)
{
    if (pts.isEmpty())
        return true;

    const QRectF canvasBox(-margin, -margin, 1.0 + 2.0 * margin, 1.0 + 2.0 * margin);
    for (const KisAiStrokePoint &p : pts) {
        if (canvasBox.contains(p.pos)) {
            return false;
        }
    }

    const qreal boxMin = -margin;
    const qreal boxMax = 1.0 + margin;

    qreal minX = pts[0].pos.x(), maxX = minX;
    qreal minY = pts[0].pos.y(), maxY = minY;
    for (int i = 1; i < pts.size(); ++i) {
        minX = qMin(minX, pts[i].pos.x());
        maxX = qMax(maxX, pts[i].pos.x());
        minY = qMin(minY, pts[i].pos.y());
        maxY = qMax(maxY, pts[i].pos.y());
    }
    if (maxX < boxMin || minX > boxMax || maxY < boxMin || minY > boxMax) {
        return true;
    }

    // Check if any segment crosses the [0, 1]x[0, 1] canvas
    for (int i = 0; i + 1 < pts.size(); ++i) {
        const QLineF seg(pts[i].pos, pts[i + 1].pos);
        QPointF isect;
        if (seg.intersects(QLineF(0.0, 0.0, 1.0, 0.0), &isect) == QLineF::BoundedIntersection ||
            seg.intersects(QLineF(1.0, 0.0, 1.0, 1.0), &isect) == QLineF::BoundedIntersection ||
            seg.intersects(QLineF(1.0, 1.0, 0.0, 1.0), &isect) == QLineF::BoundedIntersection ||
            seg.intersects(QLineF(0.0, 1.0, 0.0, 0.0), &isect) == QLineF::BoundedIntersection) {
            return false;
        }
    }
    return true;
}

bool allPolyOutside(const QPolygonF &poly, qreal margin = 0.05)
{
    if (poly.size() < 3)
        return true;

    const QRectF canvasBox(-margin, -margin, 1.0 + 2.0 * margin, 1.0 + 2.0 * margin);
    for (const QPointF &p : poly) {
        if (canvasBox.contains(p)) {
            return false;
        }
    }

    const qreal boxMin = -margin;
    const qreal boxMax = 1.0 + margin;

    qreal minX = poly[0].x(), maxX = minX;
    qreal minY = poly[0].y(), maxY = minY;
    for (int i = 1; i < poly.size(); ++i) {
        minX = qMin(minX, poly[i].x());
        maxX = qMax(maxX, poly[i].x());
        minY = qMin(minY, poly[i].y());
        maxY = qMax(maxY, poly[i].y());
    }
    if (maxX < boxMin || minX > boxMax || maxY < boxMin || minY > boxMax) {
        return true;
    }

    // Full-bleed or large fill enclosing canvas center
    if (poly.containsPoint(QPointF(0.5, 0.5), Qt::OddEvenFill)) {
        return false;
    }

    const int n = poly.size();
    for (int i = 0; i < n; ++i) {
        const QLineF edge(poly.at(i), poly.at((i + 1) % n));
        QPointF isect;
        if (edge.intersects(QLineF(0.0, 0.0, 1.0, 0.0), &isect) == QLineF::BoundedIntersection ||
            edge.intersects(QLineF(1.0, 0.0, 1.0, 1.0), &isect) == QLineF::BoundedIntersection ||
            edge.intersects(QLineF(1.0, 1.0, 0.0, 1.0), &isect) == QLineF::BoundedIntersection ||
            edge.intersects(QLineF(0.0, 1.0, 0.0, 0.0), &isect) == QLineF::BoundedIntersection) {
            return false;
        }
    }
    return true;
}

int countSelfIntersectionsPx(const QVector<QPointF> &px, bool closed)
{
    const int n = px.size();
    if (n < 4)
        return 0;
    // Bound work: check at most ~120 samples.
    const int step = qMax(1, n / 120);
    int hits = 0;
    const int segs = closed ? n : (n - 1);
    for (int i = 0; i < segs; i += step) {
        const QPointF a1 = px.at(i);
        const QPointF a2 = px.at((i + 1) % n);
        for (int j = i + 2; j < segs; j += step) {
            if (closed && i == 0 && j == segs - 1)
                continue; // shared closure vertex
            const QPointF b1 = px.at(j);
            const QPointF b2 = px.at((j + 1) % n);
            QPointF isect;
            if (QLineF(a1, a2).intersects(QLineF(b1, b2), &isect) == QLineF::BoundedIntersection)
                ++hits;
            if (hits >= 8)
                return hits;
        }
    }
    return hits;
}

int layerRank(const QString &layer)
{
    const QString l = KisAiStrokeProgramCodec::normalizeLayerName(layer);
    if (l == QLatin1String("Background"))
        return 0;
    if (l == QLatin1String("Flats"))
        return 1;
    if (l == QLatin1String("Shading"))
        return 2;
    if (l == QLatin1String("Lineart"))
        return 3;
    if (l == QLatin1String("Highlights"))
        return 4;
    if (l == QLatin1String("FX"))
        return 5;
    return 6;
}

qreal opMassEstimate(const KisAiStrokeOperation &op, const QSize &canvas)
{
    switch (op.kind) {
    case KisAiStrokeOperation::Kind::Fill:
    case KisAiStrokeOperation::Kind::GradientFill:
    case KisAiStrokeOperation::Kind::Hatch:
        return normalizedPolyArea(op.polygon);
    case KisAiStrokeOperation::Kind::Path: {
        const qreal lenPx = pathLengthPx(op.points, canvas, op.closed);
        const qreal base = qMin(canvas.width(), canvas.height());
        const qreal wPx = op.brush.sizeMode == QLatin1String("px")
            ? op.brush.size
            : op.brush.size * base;
        return (lenPx * qMax<qreal>(1.0, wPx)) / qMax<qreal>(1.0, base * base);
    }
    case KisAiStrokeOperation::Kind::Ribbon: {
        QVector<KisAiStrokePoint> spinePts;
        spinePts.reserve(op.spine.size());
        for (const QPointF &p : op.spine)
            spinePts.append(KisAiStrokePoint(p.x(), p.y(), 0.8));
        const qreal lenPx = pathLengthPx(spinePts, canvas, false);
        const qreal base = qMin(canvas.width(), canvas.height());
        const qreal wPx = ((op.widthStart + op.widthMid + op.widthEnd) / 3.0) * base;
        return (lenPx * qMax<qreal>(1.0, wPx)) / qMax<qreal>(1.0, base * base);
    }
    case KisAiStrokeOperation::Kind::AnimeEye:
        return op.eyeSize.width() * op.eyeSize.height() * 4.0;
    case KisAiStrokeOperation::Kind::Particles:
        return 1.0e-5 * qMax(0, op.particleCount);
    case KisAiStrokeOperation::Kind::MangaLines:
        return 1.0e-4 * qMax(0, op.density);
    default:
        return 0.0;
    }
}
} // namespace

QVector<KisAiStrokePoint> KisAiDeliberateStroke::stabilizeStroke(
    const QVector<KisAiStrokePoint> &points,
    const QSize &canvasSize,
    bool closed,
    quint32 seed)
{
    Q_UNUSED(seed);
    if (points.size() < 3)
        return points;
    const qreal minDim = qMax<qreal>(64.0, qMin(canvasSize.width(), canvasSize.height()));

    const qreal rawLenPx = pathLengthPx(points, canvasSize, closed);

    // Adaptive step & RDP: short strokes (< 45px, such as eyelashes, hair tips, micro hatches)
    // must not be decimated to 2-3 straight segments by a coarse 3px step.
    qreal stepPx = 3.0;
    qreal epsPx = 1.2;
    if (rawLenPx < 15.0) {
        stepPx = qBound<qreal>(0.75, rawLenPx / 8.0, 1.2);
        epsPx = 0.4;
    } else if (rawLenPx < 45.0) {
        stepPx = 1.8;
        epsPx = 0.8;
    }

    // 1. RDP jitter removal in normalized units.
    QVector<QPointF> positions;
    positions.reserve(points.size());
    for (const KisAiStrokePoint &p : points)
        positions.append(p.pos);
    const qreal epsNorm = epsPx / minDim;
    QVector<QPointF> simplified = KisAiStrokeQualityUtils::simplifyRDP(positions, epsNorm);
    if (simplified.size() < 2)
        return points;

    // Reattach pressures by nearest original vertex (deterministic).
    QVector<KisAiStrokePoint> kept;
    kept.reserve(simplified.size());
    for (const QPointF &sp : simplified) {
        int best = 0;
        qreal bestD = 1e18;
        for (int i = 0; i < points.size(); ++i) {
            const QPointF d = points.at(i).pos - sp;
            const qreal dist = d.x() * d.x() + d.y() * d.y();
            if (dist < bestD) {
                bestD = dist;
                best = i;
            }
        }
        const KisAiStrokePoint &src = points.at(best);
        kept.append(KisAiStrokePoint(sp.x(), sp.y(), src.pressure, src.timeMs));
    }
    if (kept.size() < 2)
        return kept;

    // 2. Equidistant resampling (adaptive step) for uniform ink density without erasing details.
    const qreal stepNorm = stepPx / minDim;
    QVector<KisAiStrokePoint> resampled =
        KisAiStrokeQualityUtils::resampleEquidistant(kept, stepNorm, closed);
    if (resampled.size() < 2)
        return kept;
    return resampled;
}

KisAiStrokeLintReport KisAiDeliberateStroke::lintStroke(
    const KisAiStrokeOperation &op,
    const QSize &canvasSize)
{
    KisAiStrokeLintReport rep;
    const QSize canvas = canvasSize.isValid() ? canvasSize : QSize(1024, 1024);

    auto finitePoint = [](const QPointF &p) {
        return std::isfinite(p.x()) && std::isfinite(p.y());
    };

    switch (op.kind) {
    case KisAiStrokeOperation::Kind::Path: {
        if (op.points.isEmpty()) {
            rep.drop = true;
            rep.reasons << QStringLiteral("empty-path");
            return rep;
        }
        for (const KisAiStrokePoint &p : op.points) {
            if (!finitePoint(p.pos) || !std::isfinite(p.pressure)) {
                // Bail out: NaN compares false against every threshold below, so
                // continuing would leave drop==false and push the bad vertex into
                // pathLength/curvature/self-intersection diagnostics and on to QPainter.
                rep.drop = true;
                rep.needsRepair = true;
                rep.reasons << QStringLiteral("non-finite-point");
                return rep;
            }
        }
        if (allPointsOutside(op.points)) {
            rep.drop = true;
            rep.reasons << QStringLiteral("off-canvas");
            return rep;
        }
        rep.lengthPx = pathLengthPx(op.points, canvas, op.closed);
        if (op.points.size() == 1) {
            // Single dab: keep unless fully transparent.
            if (op.brush.opacity <= 0.01) {
                rep.drop = true;
                rep.reasons << QStringLiteral("invisible-dab");
            }
            return rep;
        }
        // Adaptive micro-path threshold: fine linework, stippling, facial details,
        // eyelashes, double eyelids, and hair strands tolerate delicate lengths down to 0.45px.
        const QString prof = op.brush.profile.toLower();
        const bool isFine = prof == QLatin1String("fineliner") || prof == QLatin1String("maru_pen")
            || prof == QLatin1String("feathering") || prof == QLatin1String("stipple")
            || prof == QLatin1String("pencil")
            || isFaceDetail(op.id) || op.id.contains(QLatin1String("strand"))
            || op.id.contains(QLatin1String("hatch")) || op.id.contains(QLatin1String("wrinkle"))
            || op.id.contains(QLatin1String("trim")) || op.id.contains(QLatin1String("eyelash"))
            || op.id.contains(QLatin1String("lash")) || op.id.contains(QLatin1String("lid"))
            || op.id.contains(QLatin1String("catchlight")) || op.id.contains(QLatin1String("pupil"));
        const qreal minLen = isFine ? 0.45 : 1.2;
        if (rep.lengthPx < minLen) {
            rep.drop = true;
            rep.reasons << QStringLiteral("micro-path");
            return rep;
        }
        if (!op.brush.color.isValid() || !(op.brush.size > 0.0)) {
            rep.needsRepair = true;
            rep.reasons << QStringLiteral("invalid-brush");
        }
        // Curvature + self-intersection diagnostics.
        QVector<QPointF> px;
        px.reserve(op.points.size());
        for (const KisAiStrokePoint &p : op.points)
            px.append(QPointF(p.pos.x() * canvas.width(), p.pos.y() * canvas.height()));
        const QVector<qreal> curves = KisAiStrokeQualityUtils::computeCurvatures(px);
        for (qreal c : curves)
            rep.maxCurvature = qMax(rep.maxCurvature, qAbs(c));
        rep.selfIntersections = countSelfIntersectionsPx(px, op.closed);
        if (rep.selfIntersections >= 4) {
            rep.needsRepair = true;
            rep.reasons << QStringLiteral("self-intersecting");
        }
        if (rep.maxCurvature > 1.5 && op.points.size() > 24) {
            rep.needsRepair = true;
            rep.reasons << QStringLiteral("high-curvature-jitter");
        }
        return rep;
    }
    case KisAiStrokeOperation::Kind::Ribbon: {
        QVector<QPointF> spine = op.spine;
        if (spine.isEmpty() && !op.points.isEmpty()) {
            spine.reserve(op.points.size());
            for (const KisAiStrokePoint &p : op.points)
                spine.append(p.pos);
        }
        if (spine.size() < 2) {
            rep.drop = true;
            rep.reasons << QStringLiteral("empty-spine");
            return rep;
        }
        QVector<KisAiStrokePoint> spinePts;
        spinePts.reserve(spine.size());
        for (const QPointF &p : spine) {
            if (!finitePoint(p)) {
                rep.drop = true;
                rep.needsRepair = true;
                rep.reasons << QStringLiteral("non-finite-spine");
                return rep;
            }
            spinePts.append(KisAiStrokePoint(p.x(), p.y(), 0.8));
        }
        rep.lengthPx = pathLengthPx(spinePts, canvas, false);
        if (rep.lengthPx < 2.0) {
            rep.drop = true;
            rep.reasons << QStringLiteral("micro-spine");
            return rep;
        }
        if (allPointsOutside(spinePts)) {
            rep.drop = true;
            rep.reasons << QStringLiteral("off-canvas-spine");
            return rep;
        }
        if (!(op.widthStart > 0.0) || !(op.widthMid > 0.0) || !(op.widthEnd > 0.0)) {
            rep.needsRepair = true;
            rep.reasons << QStringLiteral("invalid-ribbon-width");
        }
        return rep;
    }
    case KisAiStrokeOperation::Kind::Fill:
    case KisAiStrokeOperation::Kind::GradientFill:
    case KisAiStrokeOperation::Kind::Hatch: {
        if (op.kind == KisAiStrokeOperation::Kind::GradientFill && op.polygon.isEmpty()) {
            // Empty gradient geometry deliberately means full canvas (see refineForRendering).
            rep.inkCoverage = qBound<qreal>(0.0, op.brush.opacity, 1.0);
            return rep;
        }
        if (op.polygon.size() < 3) {
            rep.drop = true;
            rep.reasons << QStringLiteral("degenerate-polygon");
            return rep;
        }
        for (const QPointF &p : op.polygon) {
            if (!finitePoint(p)) {
                rep.drop = true;
                rep.needsRepair = true;
                rep.reasons << QStringLiteral("non-finite-polygon");
                return rep;
            }
        }
        if (allPolyOutside(op.polygon)) {
            rep.drop = true;
            rep.reasons << QStringLiteral("off-canvas-fill");
            return rep;
        }
        const qreal area = normalizedPolyArea(op.polygon);
        rep.inkCoverage = area;
        // ~1px at 1024 (1e-6 normalized) is dust; drop it.
        if (area < 1.0e-6) {
            rep.drop = true;
            rep.reasons << QStringLiteral("micro-fill");
            return rep;
        }
        return rep;
    }
    case KisAiStrokeOperation::Kind::Particles: {
        if (op.particleCount <= 0) {
            rep.drop = true;
            rep.reasons << QStringLiteral("zero-particles");
            return rep;
        }
        return rep;
    }
    case KisAiStrokeOperation::Kind::MangaLines: {
        if (op.density <= 0) {
            rep.drop = true;
            rep.reasons << QStringLiteral("zero-density");
            return rep;
        }
        return rep;
    }
    case KisAiStrokeOperation::Kind::AnimeEye: {
        const qreal wPx = op.eyeSize.width() * canvas.width();
        const qreal hPx = op.eyeSize.height() * canvas.height();
        rep.lengthPx = qMax(wPx, hPx);
        if (wPx < 4.0 || hPx < 4.0) {
            rep.drop = true;
            rep.reasons << QStringLiteral("micro-eye");
            return rep;
        }
        if (!std::isfinite(op.eyeCenter.x()) || !std::isfinite(op.eyeCenter.y())) {
            rep.drop = true;
            rep.needsRepair = true;
            rep.reasons << QStringLiteral("non-finite-eye-center");
        }
        return rep;
    }
    default: {
        rep.drop = true;
        rep.reasons << QStringLiteral("unknown-kind");
        return rep;
    }
    }
}

bool KisAiDeliberateStroke::isFaceDetail(const QString &id)
{
    const QString l = id.toLower();
    return l.contains(QLatin1String("eye")) || l.contains(QLatin1String("lash"))
        || l.contains(QLatin1String("brow")) || l.contains(QLatin1String("mouth"))
        || l.contains(QLatin1String("lip")) || l.contains(QLatin1String("nose"))
        || l.contains(QLatin1String("tear")) || l.contains(QLatin1String("crease"))
        || l.contains(QLatin1String("pupil")) || l.contains(QLatin1String("iris"));
}

QVector<int> KisAiDeliberateStroke::planStrokeOrder(
    const QVector<KisAiStrokeOperation> &ops,
    const QSize &canvasSize)
{
    struct Item {
        int index;
        int rank;
        qreal mass;
        qreal opacity;
        bool face;
        QString id;
    };
    QVector<Item> items;
    items.reserve(ops.size());
    for (int i = 0; i < ops.size(); ++i) {
        const KisAiStrokeOperation &op = ops.at(i);
        items.append({i, layerRank(op.layer), opMassEstimate(op, canvasSize),
                      op.brush.opacity, isFaceDetail(op.id), op.id});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.face != b.face)
            return !a.face && b.face; // non-face first, details last
        if (!qFuzzyCompare(a.mass + 1.0, b.mass + 1.0))
            return a.mass > b.mass; // large masses first
        if (!qFuzzyCompare(a.opacity + 1.0, b.opacity + 1.0))
            return a.opacity < b.opacity; // thin washes first
        return a.index < b.index;
    });
    QVector<int> order;
    order.reserve(items.size());
    for (const Item &it : items)
        order.append(it.index);
    return order;
}

QVector<KisAiStrokeOperation> KisAiDeliberateStroke::orderOperationsForRendering(
    const QVector<KisAiStrokeOperation> &ops,
    const QSize &canvasSize)
{
    if (ops.size() < 2)
        return ops;
    const QVector<int> order = planStrokeOrder(ops, canvasSize);
    QVector<KisAiStrokeOperation> sorted;
    sorted.reserve(ops.size());
    for (int idx : order)
        sorted.append(ops.at(idx));
    return sorted;
}

int KisAiDeliberateStroke::adaptiveSupersampleScale(
    const QVector<KisAiStrokeOperation> &ops,
    const QSize &canvasSize)
{
    const int maxEdge = qMax(canvasSize.width(), canvasSize.height());
    bool hasFaceWork = false;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye || isFaceDetail(op.id)
            || op.id.contains(QLatin1String("face_contour"))) {
            hasFaceWork = true;
            break;
        }
    }
    // Faces on modest canvases deserve 3x; keep memory bounded at 4096px.
    if (hasFaceWork && maxEdge <= 1024 && maxEdge * 3 <= 4096)
        return 3;
    if (maxEdge <= 1536)
        return 2;
    return 1;
}

QPolygonF KisAiDeliberateStroke::buildEnvelopePolygon(
    const QVector<KisAiStrokePoint> &normalizedPoints,
    const KisAiStrokeBrush &brush,
    const QSize &canvasSizePx,
    bool closed)
{
    QPolygonF poly;
    if (normalizedPoints.size() < 2)
        return poly;
    const QVector<KisAiStrokeQualityUtils::StrokeEnvelopeSegment> segs =
        KisAiStrokeQualityUtils::generateStrokeEnvelope(normalizedPoints, brush, canvasSizePx, closed);
    if (segs.isEmpty())
        return poly;
    poly.reserve(segs.size() * 2 + 2);
    for (const auto &s : segs)
        poly.append(s.leftStart);
    poly.append(segs.last().leftEnd);
    poly.append(segs.last().rightEnd);
    for (int i = segs.size() - 1; i >= 0; --i)
        poly.append(segs.at(i).rightStart);
    return poly;
}

QStringList KisAiDeliberateStroke::eyePairSymmetryWarnings(
    const QVector<KisAiStrokeOperation> &ops)
{
    QStringList warnings;
    QVector<const KisAiStrokeOperation *> eyes;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye)
            eyes.append(&op);
    }
    if (eyes.size() == 1) {
        warnings << QStringLiteral("single-eye-only");
        return warnings;
    }
    if (eyes.size() != 2)
        return warnings;
    const KisAiStrokeOperation *a = eyes.at(0);
    const KisAiStrokeOperation *b = eyes.at(1);
    if (qAbs(a->eyeCenter.y() - b->eyeCenter.y()) > 0.02)
        warnings << QStringLiteral("eye-height-mismatch");
    const qreal avgW = (a->eyeSize.width() + b->eyeSize.width()) * 0.5;
    const qreal avgH = (a->eyeSize.height() + b->eyeSize.height()) * 0.5;
    if (avgW > 1e-6 && qAbs(a->eyeSize.width() - b->eyeSize.width()) / avgW > 0.15)
        warnings << QStringLiteral("eye-width-mismatch");
    if (avgH > 1e-6 && qAbs(a->eyeSize.height() - b->eyeSize.height()) / avgH > 0.15)
        warnings << QStringLiteral("eye-height-size-mismatch");
    return warnings;
}

KisAiStrokeCommitReview KisAiDeliberateStroke::reviewStroke(
    const KisAiStrokeOperation &op,
    const QSize &canvasSize)
{
    KisAiStrokeCommitReview rev;
    const QSize canvas = canvasSize.isValid() ? canvasSize : QSize(1024, 1024);
    switch (op.kind) {
    case KisAiStrokeOperation::Kind::Fill:
    case KisAiStrokeOperation::Kind::GradientFill:
    case KisAiStrokeOperation::Kind::Hatch: {
        if (op.kind == KisAiStrokeOperation::Kind::GradientFill && op.polygon.isEmpty()) {
            rev.dirtyRect = QRectF(0, 0, canvas.width(), canvas.height());
            rev.inkCoverage = qBound<qreal>(0.0, op.brush.opacity, 1.0);
            break;
        }
        QPolygonF px;
        px.reserve(op.polygon.size());
        for (const QPointF &p : op.polygon)
            px.append(QPointF(p.x() * canvas.width(), p.y() * canvas.height()));
        rev.dirtyRect = px.boundingRect();
        rev.inkCoverage = normalizedPolyArea(op.polygon) * qBound<qreal>(0.0, op.brush.opacity, 1.0);
        break;
    }
    case KisAiStrokeOperation::Kind::Path: {
        QPolygonF px;
        px.reserve(op.points.size());
        for (const KisAiStrokePoint &p : op.points)
            px.append(QPointF(p.pos.x() * canvas.width(), p.pos.y() * canvas.height()));
        rev.dirtyRect = px.boundingRect();
        rev.inkCoverage = opMassEstimate(op, canvas);
        break;
    }
    case KisAiStrokeOperation::Kind::Ribbon: {
        QPolygonF px;
        px.reserve(op.spine.size());
        for (const QPointF &p : op.spine)
            px.append(QPointF(p.x() * canvas.width(), p.y() * canvas.height()));
        rev.dirtyRect = px.boundingRect();
        rev.inkCoverage = opMassEstimate(op, canvas);
        break;
    }
    case KisAiStrokeOperation::Kind::AnimeEye: {
        const QPointF c(op.eyeCenter.x() * canvas.width(), op.eyeCenter.y() * canvas.height());
        const qreal w = op.eyeSize.width() * canvas.width();
        const qreal h = op.eyeSize.height() * canvas.height();
        rev.dirtyRect = QRectF(c.x() - w, c.y() - h, w * 2.0, h * 2.0);
        rev.inkCoverage = op.eyeSize.width() * op.eyeSize.height();
        break;
    }
    default:
        rev.inkCoverage = opMassEstimate(op, canvas);
        break;
    }
    if (!(rev.inkCoverage > 1.0e-7)) {
        rev.committed = false;
        rev.notes << QStringLiteral("zero-coverage-skip");
    }
    return rev;
}
