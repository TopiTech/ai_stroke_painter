/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeQualityUtils.h"

#include <QColor>
#include <QImage>
#include <QLinearGradient>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTransform>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace
{
constexpr qreal PI = 3.14159265358979323846;
constexpr qreal DEG2RAD = PI / 180.0;

qreal clamp01(qreal v)
{
    if (!std::isfinite(v))
        return 0.0;
    return qMax<qreal>(0.0, qMin<qreal>(1.0, v));
}

qreal dotProduct(const QPointF &a, const QPointF &b)
{
    return a.x() * b.x() + a.y() * b.y();
}

qreal pointDistance(const QPointF &a, const QPointF &b)
{
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

QPointF normalizeVector(const QPointF &v)
{
    const qreal len = std::hypot(v.x(), v.y());
    if (len < 1.0e-7) {
        return QPointF(0.0, 0.0);
    }
    return QPointF(v.x() / len, v.y() / len);
}

QPointF scalePoint(const QPointF &normPt, const QSize &canvasSize)
{
    return QPointF(normPt.x() * canvasSize.width(), normPt.y() * canvasSize.height());
}

QPolygonF scalePolygon(const QPolygonF &normPoly, const QSize &canvasSize)
{
    QPolygonF res;
    res.reserve(normPoly.size());
    for (const QPointF &pt : normPoly) {
        res.append(scalePoint(pt, canvasSize));
    }
    return res;
}

qreal effectiveWidthPx(const KisAiStrokeBrush &brush, qreal pressure, const QSize &canvasSize)
{
    const qreal baseDim = qMin(canvasSize.width(), canvasSize.height());
    qreal sz = 8.0;
    if (brush.sizeMode == QLatin1String("px")) {
        sz = brush.size;
    } else {
        sz = brush.size * baseDim;
    }
    sz = qMax<qreal>(1.0, sz * qBound<qreal>(0.05, pressure, 1.0));
    return sz;
}

QPointF catmullRomPoint(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t)
{
    const qreal t2 = t * t;
    const qreal t3 = t2 * t;

    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

} // namespace

QVector<KisAiStrokePoint> KisAiStrokeQualityUtils::resampleEquidistant(
    const QVector<KisAiStrokePoint> &points,
    qreal stepPx,
    bool closed)
{
    if (points.size() < 2) {
        return points;
    }

    const qreal step = qMax<qreal>(1.0e-5, stepPx);
    const int n = points.size();

    QVector<qreal> segmentLengths;
    segmentLengths.reserve(n);
    qreal totalLength = 0.0;

    const int segmentCount = closed ? n : (n - 1);
    for (int i = 0; i < segmentCount; ++i) {
        const QPointF &p1 = points.at(i).pos;
        const QPointF &p2 = points.at((i + 1) % n).pos;
        const qreal len = pointDistance(p1, p2);
        segmentLengths.append(len);
        totalLength += len;
    }

    if (totalLength <= step) {
        QVector<KisAiStrokePoint> shortRes;
        shortRes.append(points.first());
        if (!closed && points.size() > 1) {
            shortRes.append(points.last());
        }
        return shortRes;
    }

    QVector<KisAiStrokePoint> result;
    // The output count is totalLength/step, so a caller passing a very small
    // normalised step could enqueue hundreds of thousands of points (and the
    // reserve() below would commit that memory up front). Bound both.
    constexpr int kMaxResampledPoints = 33000;
    const int estimatedPoints = qBound(0, qCeil(totalLength / step) + 2, kMaxResampledPoints);
    result.reserve(estimatedPoints);
    result.append(points.first());

    qreal currentTargetDist = step;
    qreal accumulatedDist = 0.0;
    int segIdx = 0;

    while (currentTargetDist <= totalLength && segIdx < segmentCount) {
        if (result.size() >= kMaxResampledPoints) {
            break;
        }
        const qreal segLen = segmentLengths.at(segIdx);
        if (accumulatedDist + segLen >= currentTargetDist && segLen > 1.0e-7) {
            const qreal localDist = currentTargetDist - accumulatedDist;
            const qreal t = qBound<qreal>(0.0, localDist / segLen, 1.0);

            const KisAiStrokePoint &ptA = points.at(segIdx);
            const KisAiStrokePoint &ptB = points.at((segIdx + 1) % n);

            const QPointF pos = ptA.pos + (ptB.pos - ptA.pos) * t;
            const qreal pressure = ptA.pressure + (ptB.pressure - ptA.pressure) * t;
            const qint64 timeMs = ptA.timeMs + qRound64((ptB.timeMs - ptA.timeMs) * t);

            result.append(KisAiStrokePoint(pos.x(), pos.y(), pressure, timeMs));
            currentTargetDist += step;
        } else {
            accumulatedDist += segLen;
            ++segIdx;
        }
    }

    // A closed curve that hit the cap must still be sealed: the loop walks up to
    // totalLength but stops early when the bound is reached, so re-append the
    // first vertex to close the ring. Without this the stroke renders with a
    // long straight chord between the last emitted vertex and the start.
    if (closed) {
        if (pointDistance(result.last().pos, points.first().pos) > step * 0.25) {
            result.append(points.first());
        }
    } else if (result.isEmpty() || pointDistance(result.last().pos, points.last().pos) > step * 0.25) {
        result.append(points.last());
    }

    return result;
}

namespace
{
void rdpRecursive(const QVector<QPointF> &points, int start, int end, qreal epsilonSq, QVector<bool> *keep)
{
    if (end <= start + 1) {
        return;
    }

    const QPointF &pA = points.at(start);
    const QPointF &pB = points.at(end);
    const QPointF lineVec = pB - pA;
    const qreal lineLenSq = lineVec.x() * lineVec.x() + lineVec.y() * lineVec.y();

    qreal maxDistSq = 0.0;
    int maxIdx = start;

    for (int i = start + 1; i < end; ++i) {
        const QPointF &pP = points.at(i);
        qreal distSq = 0.0;

        if (lineLenSq < 1.0e-8) {
            distSq = pointDistance(pP, pA);
            distSq = distSq * distSq;
        } else {
            const qreal t = qBound<qreal>(0.0, dotProduct(pP - pA, lineVec) / lineLenSq, 1.0);
            const QPointF proj = pA + lineVec * t;
            const QPointF diff = pP - proj;
            distSq = diff.x() * diff.x() + diff.y() * diff.y();
        }

        if (distSq > maxDistSq) {
            maxDistSq = distSq;
            maxIdx = i;
        }
    }

    if (maxDistSq > epsilonSq) {
        (*keep)[maxIdx] = true;
        rdpRecursive(points, start, maxIdx, epsilonSq, keep);
        rdpRecursive(points, maxIdx, end, epsilonSq, keep);
    }
}
} // namespace

QVector<QPointF> KisAiStrokeQualityUtils::simplifyRDP(const QVector<QPointF> &points, qreal epsilonPx)
{
    if (points.size() <= 2) {
        return points;
    }

    const qreal epsilonSq = epsilonPx * epsilonPx;
    QVector<bool> keep(points.size(), false);
    keep[0] = true;
    keep[points.size() - 1] = true;

    rdpRecursive(points, 0, points.size() - 1, epsilonSq, &keep);

    QVector<QPointF> simplified;
    simplified.reserve(points.size());
    for (int i = 0; i < points.size(); ++i) {
        if (keep.at(i)) {
            simplified.append(points.at(i));
        }
    }

    return simplified;
}

QPolygonF KisAiStrokeQualityUtils::smoothPolygonCornerPreserving(
    const QPolygonF &polygon,
    qreal cornerAngleThresholdDeg,
    int subdivisions)
{
    const int n = polygon.size();
    if (n < 3) {
        return polygon;
    }

    const qreal cosThreshold = std::cos(cornerAngleThresholdDeg * DEG2RAD);

    // Identify sharp corners
    QVector<bool> isCorner(n, false);
    for (int i = 0; i < n; ++i) {
        const QPointF &prev = polygon.at((i - 1 + n) % n);
        const QPointF &curr = polygon.at(i);
        const QPointF &next = polygon.at((i + 1) % n);

        const QPointF v1 = normalizeVector(curr - prev);
        const QPointF v2 = normalizeVector(next - curr);

        const qreal dot = dotProduct(v1, v2);
        // dot < cosThreshold means sharp turn
        if (dot < cosThreshold) {
            isCorner[i] = true;
        }
    }

    const int steps = qBound(2, subdivisions, 12);
    QPolygonF smoothed;
    smoothed.reserve(n * steps);

    for (int i = 0; i < n; ++i) {
        const int idx0 = (i - 1 + n) % n;
        const int idx1 = i;
        const int idx2 = (i + 1) % n;
        const int idx3 = (i + 2) % n;

        const QPointF &p1 = polygon.at(idx1);
        const QPointF &p2 = polygon.at(idx2);

        if (isCorner.at(idx1) && isCorner.at(idx2)) {
            // Both endpoints are sharp corners: keep straight segment
            smoothed.append(p1);
            continue;
        }

        const QPointF &p0 = isCorner.at(idx1) ? p1 : polygon.at(idx0);
        const QPointF &p3 = isCorner.at(idx2) ? p2 : polygon.at(idx3);

        for (int step = 0; step < steps; ++step) {
            const qreal t = qreal(step) / steps;
            smoothed.append(catmullRomPoint(p0, p1, p2, p3, t));
        }
        // Include t = 1 (== p2) so each span reaches its end knot; the next
        // span starts at p1 == this p2, so the polygon stays connected without
        // per-span gaps.
        smoothed.append(p2);
    }

    return smoothed;
}

QPolygonF KisAiStrokeQualityUtils::offsetPolygon(const QPolygonF &polygon, qreal distancePx)
{
    const int n = polygon.size();
    if (n < 3 || qAbs(distancePx) < 1.0e-5) {
        return polygon;
    }

    // Determine polygon winding (shoelace signed area)
    qreal signedArea = 0.0;
    for (int i = 0; i < n; ++i) {
        const QPointF &p1 = polygon.at(i);
        const QPointF &p2 = polygon.at((i + 1) % n);
        signedArea += p1.x() * p2.y() - p2.x() * p1.y();
    }
    // A zero-area (collinear or duplicated) polygon has no meaningful winding:
    // signedArea > 0.0 would be false and every outward normal would be flipped,
    // turning trapping dilation into a contraction. Leave it untouched instead.
    if (qAbs(signedArea) < 1.0e-9) {
        return polygon;
    }
    const bool isCCW = signedArea > 0.0;
    const qreal sign = isCCW ? 1.0 : -1.0;

    // Calculate edge normals (pointing outward)
    QVector<QPointF> edgeNormals;
    edgeNormals.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QPointF &p1 = polygon.at(i);
        const QPointF &p2 = polygon.at((i + 1) % n);
        const QPointF edge = p2 - p1;
        const qreal len = std::hypot(edge.x(), edge.y());
        if (len < 1.0e-6) {
            edgeNormals.append(QPointF(0.0, 0.0));
        } else {
            // Normal perpendicular to edge, outward
            const QPointF normal(edge.y() / len * sign, -edge.x() / len * sign);
            edgeNormals.append(normal);
        }
    }

    QPolygonF offsetPoly;
    offsetPoly.reserve(n);
    constexpr qreal MITER_LIMIT = 2.5;

    for (int i = 0; i < n; ++i) {
        const QPointF &nPrev = edgeNormals.at((i - 1 + n) % n);
        const QPointF &nCurr = edgeNormals.at(i);

        QPointF bisector = nPrev + nCurr;
        const qreal bLen = std::hypot(bisector.x(), bisector.y());

        if (bLen < 1.0e-6) {
            offsetPoly.append(polygon.at(i) + nCurr * distancePx);
            continue;
        }

        bisector = bisector / bLen;
        const qreal cosAngle = dotProduct(bisector, nCurr);
        qreal miterScale = 1.0;
        if (cosAngle > 1.0e-4) {
            miterScale = qMin<qreal>(MITER_LIMIT, 1.0 / cosAngle);
        }

        const QPointF offsetPt = polygon.at(i) + bisector * (distancePx * miterScale);
        offsetPoly.append(offsetPt);
    }

    return offsetPoly;
}

QVector<qreal> KisAiStrokeQualityUtils::computeCurvatures(const QVector<QPointF> &points)
{
    const int n = points.size();
    QVector<qreal> curvatures(n, 0.0);
    if (n < 3) {
        return curvatures;
    }

    for (int i = 1; i + 1 < n; ++i) {
        const QPointF &a = points.at(i - 1);
        const QPointF &b = points.at(i);
        const QPointF &c = points.at(i + 1);

        const qreal ab = pointDistance(a, b);
        const qreal bc = pointDistance(b, c);
        const qreal ac = pointDistance(a, c);

        const qreal s = (ab + bc + ac) * 0.5;
        const qreal area = std::sqrt(qMax<qreal>(0.0, s * (s - ab) * (s - bc) * (s - ac)));

        if (ab * bc * ac > 1.0e-7) {
            // Curvature = 4 * Area / (a * b * c) = 1 / R
            curvatures[i] = (4.0 * area) / (ab * bc * ac);
        }
    }

    curvatures[0] = curvatures[1];
    curvatures[n - 1] = curvatures[n - 2];
    return curvatures;
}

qreal KisAiStrokeQualityUtils::calculateTaper(qreal globalT, const QString &profile, bool isClosed)
{
    if (isClosed) {
        return 1.0;
    }

    const qreal t = clamp01(globalT);
    const QString p = profile.toLower();

    if (p == QLatin1String("gpen")) {
        // Crisp comic dip-pen: snappy in (5%), delicate extended tapering out (25%)
        constexpr qreal IN_LEN = 0.05;
        constexpr qreal OUT_LEN = 0.25;
        if (t < IN_LEN) {
            return 0.10 + 0.90 * std::sin((t / IN_LEN) * (PI * 0.5));
        } else if (t > (1.0 - OUT_LEN)) {
            const qreal progress = (1.0 - t) / OUT_LEN;
            return 0.02 + 0.98 * std::sin(progress * (PI * 0.5));
        }
        return 1.0;
    }

    if (p == QLatin1String("pencil")) {
        // Natural graphite drag: gentle symmetrical start and end
        constexpr qreal TAPER_LEN = 0.10;
        if (t < TAPER_LEN) {
            return 0.35 + 0.65 * std::sin((t / TAPER_LEN) * (PI * 0.5));
        } else if (t > (1.0 - TAPER_LEN)) {
            return 0.35 + 0.65 * std::sin(((1.0 - t) / TAPER_LEN) * (PI * 0.5));
        }
        return 1.0;
    }

    if (p == QLatin1String("marker")) {
        // Chisel alcohol marker: blunt edge, minimal taper
        constexpr qreal TAPER_LEN = 0.03;
        if (t < TAPER_LEN) {
            return 0.70 + 0.30 * (t / TAPER_LEN);
        } else if (t > (1.0 - TAPER_LEN)) {
            return 0.70 + 0.30 * ((1.0 - t) / TAPER_LEN);
        }
        return 1.0;
    }

    if (p == QLatin1String("calligraphy")) {
        // Flat nib: firm blunt landing and release
        constexpr qreal TAPER_LEN = 0.04;
        if (t < TAPER_LEN) {
            return 0.50 + 0.50 * (t / TAPER_LEN);
        } else if (t > (1.0 - TAPER_LEN)) {
            return 0.50 + 0.50 * ((1.0 - t) / TAPER_LEN);
        }
        return 1.0;
    }

    if (p == QLatin1String("brush")) {
        // Calligraphic hair brush: responsive flex
        constexpr qreal IN_LEN = 0.08;
        constexpr qreal OUT_LEN = 0.18;
        if (t < IN_LEN) {
            return 0.15 + 0.85 * std::sin((t / IN_LEN) * (PI * 0.5));
        } else if (t > (1.0 - OUT_LEN)) {
            return 0.05 + 0.95 * std::sin(((1.0 - t) / OUT_LEN) * (PI * 0.5));
        }
        return 1.0;
    }

    // Default universal smooth taper
    constexpr qreal TAPER_LEN = 0.12;
    if (t < TAPER_LEN) {
        return 0.15 + 0.85 * std::sin((t / TAPER_LEN) * (PI * 0.5));
    } else if (t > (1.0 - TAPER_LEN)) {
        return 0.15 + 0.85 * std::sin(((1.0 - t) / TAPER_LEN) * (PI * 0.5));
    }
    return 1.0;
}

QVector<KisAiStrokeQualityUtils::StrokeEnvelopeSegment> KisAiStrokeQualityUtils::generateStrokeEnvelope(
    const QVector<KisAiStrokePoint> &points,
    const KisAiStrokeBrush &brush,
    const QSize &canvasSize,
    bool closed)
{
    QVector<StrokeEnvelopeSegment> segments;
    if (points.size() < 2) {
        return segments;
    }

    const int n = points.size();

    // Convert normalized points to canvas coordinates
    QVector<QPointF> canvasPts;
    QVector<qreal> pressures;
    canvasPts.reserve(n);
    pressures.reserve(n);

    for (const KisAiStrokePoint &pt : points) {
        canvasPts.append(QPointF(pt.pos.x() * canvasSize.width(), pt.pos.y() * canvasSize.height()));
        pressures.append(qBound<qreal>(0.05, pt.pressure, 1.0));
    }

    // Compute effective widths along spine
    QVector<qreal> widths;
    widths.reserve(n);
    const int segCount = closed ? n : (n - 1);

    for (int i = 0; i < n; ++i) {
        const qreal globalT = qreal(i) / qMax(1, segCount);
        const qreal taper = calculateTaper(globalT, brush.profile, closed);
        const qreal w = effectiveWidthPx(brush, pressures.at(i) * taper, canvasSize);
        widths.append(w);
    }

    // Compute unit tangents and normals at each point
    QVector<QPointF> normals;
    normals.reserve(n);

    for (int i = 0; i < n; ++i) {
        QPointF tangent;
        if (closed) {
            const QPointF &prev = canvasPts.at((i - 1 + n) % n);
            const QPointF &next = canvasPts.at((i + 1) % n);
            tangent = next - prev;
        } else if (i == 0) {
            tangent = canvasPts.at(1) - canvasPts.at(0);
        } else if (i == n - 1) {
            tangent = canvasPts.at(n - 1) - canvasPts.at(n - 2);
        } else {
            tangent = canvasPts.at(i + 1) - canvasPts.at(i - 1);
        }

        const QPointF unitT = normalizeVector(tangent);
        normals.append(QPointF(-unitT.y(), unitT.x()));
    }

    // Build segments with self-intersection clamping
    segments.reserve(segCount);
    for (int i = 0; i < segCount; ++i) {
        const int idx1 = i;
        const int idx2 = (i + 1) % n;

        const QPointF &p1 = canvasPts.at(idx1);
        const QPointF &p2 = canvasPts.at(idx2);
        const QPointF &n1 = normals.at(idx1);
        const QPointF &n2 = normals.at(idx2);

        const qreal w1 = widths.at(idx1) * 0.5;
        const qreal w2 = widths.at(idx2) * 0.5;

        StrokeEnvelopeSegment seg;
        seg.centerStart = p1;
        seg.centerEnd = p2;
        seg.widthStart = widths.at(idx1);
        seg.widthEnd = widths.at(idx2);

        seg.leftStart = p1 + n1 * w1;
        seg.leftEnd = p2 + n2 * w2;
        seg.rightStart = p1 - n1 * w1;
        seg.rightEnd = p2 - n2 * w2;

        // Prevent bowtie twists on sharp turns:
        // If leftStart->leftEnd crosses rightStart->rightEnd, resolve by clamping
        const QPointF leftEdge = seg.leftEnd - seg.leftStart;
        const QPointF rightEdge = seg.rightEnd - seg.rightStart;
        const qreal leftDotRight = dotProduct(normalizeVector(leftEdge), normalizeVector(rightEdge));

        if (leftDotRight < -0.2) {
            // Severe twist: anchor mid-join to prevent self-crossing
            const QPointF midL = (seg.leftStart + seg.leftEnd) * 0.5;
            const QPointF midR = (seg.rightStart + seg.rightEnd) * 0.5;
            seg.leftEnd = midL;
            seg.rightEnd = midR;
        }

        segments.append(seg);
    }

    return segments;
}

QVector<QVector<QPointF>> KisAiStrokeQualityUtils::generateBristleStrands(
    const QVector<KisAiStrokePoint> &spine,
    int strandCount,
    qreal maxSpreadPx,
    quint32 seed)
{
    QVector<QVector<QPointF>> strands;
    if (spine.size() < 2 || strandCount <= 0) {
        return strands;
    }

    const int count = qBound(2, strandCount, 16);
    strands.resize(count);

    QRandomGenerator rng(seed);

    // Compute spine normals
    const int n = spine.size();
    QVector<QPointF> normals;
    normals.reserve(n);

    for (int i = 0; i < n; ++i) {
        QPointF tangent;
        if (i == 0) {
            tangent = spine.at(1).pos - spine.at(0).pos;
        } else if (i == n - 1) {
            tangent = spine.at(n - 1).pos - spine.at(n - 2).pos;
        } else {
            tangent = spine.at(i + 1).pos - spine.at(i - 1).pos;
        }
        const QPointF unitT = normalizeVector(tangent);
        normals.append(QPointF(-unitT.y(), unitT.x()));
    }

    // Generate individual hair strand trajectories
    for (int k = 0; k < count; ++k) {
        const qreal baseOffsetRatio = (count > 1) ? ((qreal(k) / (count - 1)) * 2.0 - 1.0) : 0.0;
        const qreal hairJitter = (rng.generateDouble() - 0.5) * 0.35;
        const qreal strandOffset = (baseOffsetRatio + hairJitter) * maxSpreadPx;

        QVector<QPointF> &strandPath = strands[k];
        strandPath.reserve(n);

        for (int i = 0; i < n; ++i) {
            const qreal progress = qreal(i) / (n - 1);
            // Strands bunch at the start, spread slightly in mid, and converge/fray at tip
            const qreal spreadScale = 0.5 + 0.5 * std::sin(progress * PI);
            const QPointF pt = spine.at(i).pos + normals.at(i) * (strandOffset * spreadScale);
            strandPath.append(pt);
        }
    }

    return strands;
}

qreal KisAiStrokeQualityUtils::calculateCalligraphyWidth(
    const QPointF &tangent,
    qreal baseWidthPx,
    qreal nibAngleDeg,
    qreal thinRatio)
{
    const qreal nibRad = nibAngleDeg * DEG2RAD;
    const QPointF nibNormal(-std::sin(nibRad), std::cos(nibRad));
    const QPointF unitT = normalizeVector(tangent);

    // Movement perpendicular to the nib creates the thickest stroke;
    // movement parallel to the nib creates the thinnest stroke.
    const qreal projection = qAbs(dotProduct(unitT, nibNormal));
    const qreal minW = baseWidthPx * qBound<qreal>(0.05, thinRatio, 0.50);

    return minW + (baseWidthPx - minW) * projection;
}

void KisAiStrokeQualityUtils::drawHalftonePattern(
    QPainter &painter,
    const QPolygonF &polygon,
    const QColor &color,
    qreal dotSpacingPx,
    qreal dotRadiusPx,
    qreal angleDeg,
    bool lineScreen)
{
    if (polygon.size() < 3) {
        return;
    }

    const QRectF bounds = polygon.boundingRect();
    if (bounds.isEmpty()) {
        return;
    }

    painter.save();
    QPainterPath clip;
    clip.addPolygon(polygon);
    painter.setClipPath(clip);

    const qreal spacing = qMax<qreal>(3.0, dotSpacingPx);
    const qreal radius = qMax<qreal>(0.5, dotRadiusPx);

    // Work in the painter's rotated frame so only the dots that can actually
    // intersect the polygon are drawn. Iterating a square around the bounding
    // diagonal instead issued millions of painter calls for a large fill.
    QTransform screenTransform;
    screenTransform.translate(bounds.center().x(), bounds.center().y());
    screenTransform.rotate(angleDeg);
    bool invertible = false;
    const QTransform inverseTransform = screenTransform.inverted(&invertible);
    if (!invertible) {
        painter.restore();
        return;
    }
    // Dots are emitted at (ix * spacing, iy * spacing) in the rotated frame, so the
    // polygon has to be brought into that same frame to find the covered range.
    const QRectF screenBounds = inverseTransform.map(polygon).boundingRect();
    if (!screenBounds.isValid() || !std::isfinite(screenBounds.width()) || !std::isfinite(screenBounds.height())) {
        painter.restore();
        return;
    }

    // Keep one operation from monopolizing the UI thread. The spacing is only ever
    // widened, never narrowed, so the whole polygon stays covered rather than
    // rendering just a central patch.
    constexpr qreal MAX_HALFTONE_DOTS = 400000.0;
    constexpr qreal MAX_HALFTONE_LINES = 40000.0;
    qreal effectiveSpacing = spacing;
    const qreal screenArea = qMax<qreal>(0.0, screenBounds.width()) * qMax<qreal>(0.0, screenBounds.height());
    const qreal estimatedDots = screenArea / (effectiveSpacing * effectiveSpacing);
    if (estimatedDots > MAX_HALFTONE_DOTS) {
        effectiveSpacing = std::sqrt(screenArea / MAX_HALFTONE_DOTS);
    }
    if (lineScreen) {
        // Lines are one per row, so the row count — not the dot count — is the
        // unbounded quantity; apply the same widening policy to it.
        const qreal estimatedLines = qMax<qreal>(0.0, screenBounds.height()) / effectiveSpacing;
        if (estimatedLines > MAX_HALFTONE_LINES) {
            effectiveSpacing = qMax<qreal>(1.0, screenBounds.height() / MAX_HALFTONE_LINES);
        }
    }
    if (!std::isfinite(effectiveSpacing) || effectiveSpacing <= 0.0) {
        painter.restore();
        return;
    }

    const int ixStart = qFloor(screenBounds.left() / effectiveSpacing);
    const int ixEnd = qCeil(screenBounds.right() / effectiveSpacing);
    const int iyStart = qFloor(screenBounds.top() / effectiveSpacing);
    const int iyEnd = qCeil(screenBounds.bottom() / effectiveSpacing);

    painter.translate(bounds.center());
    painter.rotate(angleDeg);

    if (lineScreen) {
        QPen pen(color, radius * 2.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (int i = iyStart; i <= iyEnd; ++i) {
            const qreal y = i * effectiveSpacing;
            painter.drawLine(QPointF(screenBounds.left(), y), QPointF(screenBounds.right(), y));
        }
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int iy = iyStart; iy <= iyEnd; ++iy) {
            for (int ix = ixStart; ix <= ixEnd; ++ix) {
                const QPointF pt(ix * effectiveSpacing, iy * effectiveSpacing);
                painter.drawEllipse(pt, radius, radius);
            }
        }
    }

    painter.restore();
}

QColor KisAiStrokeQualityUtils::calculateHueShiftedShadow(
    const QColor &baseColor,
    const QColor &ambientShadowTint,
    qreal shadowDepth)
{
    if (!baseColor.isValid()) {
        return QColor(30, 30, 45);
    }

    float h = 0.0f, s = 0.0f, l = 0.0f, a = 1.0f;
    baseColor.getHslF(&h, &s, &l, &a);

    const bool isAchromatic = (h < 0.0f || s < 0.02f);
    float targetHue = h;

    if (isAchromatic) {
        if (ambientShadowTint.isValid()) {
            float ah = 0.0f, as = 0.0f, al = 0.0f;
            ambientShadowTint.getHslF(&ah, &as, &al);
            targetHue = (ah >= 0.0f) ? ah : 0.65f;
        } else {
            targetHue = 0.65f;
        }
    } else {
        // Warm colors (red, orange, yellow, skin tones: H < 0.18 or H > 0.85) shift toward cool blue/violet
        // Cool colors (blue, cyan: 0.45 < H < 0.75) deepen toward rich indigo/navy
        if (h >= 0.0f && h < 0.18f) {
            // Red-orange-yellow -> shift towards purple-blue (approx 0.70 - 0.78 / wrap into violet)
            targetHue = h - 0.08f;
        } else if (h >= 0.85f && h <= 1.0f) {
            targetHue = h - 0.08f;
        } else if (h >= 0.45f && h < 0.65f) {
            // Cyan-blue -> shift deeper toward violet
            targetHue = h + 0.05f;
        }

        if (targetHue < 0.0f) targetHue += 1.0f;
        if (targetHue >= 1.0f) targetHue -= 1.0f;
        targetHue = qBound(0.0f, targetHue, 0.9999f);
    }

    const float depth = static_cast<float>(qBound<qreal>(0.1, shadowDepth, 0.8));
    const float newL = qMax<float>(0.05f, l * (1.0f - depth * 0.65f));
    const float newS = isAchromatic ? (ambientShadowTint.isValid() ? 0.04f : 0.0f) : qBound<float>(0.1f, s * 1.15f, 1.0f); // Maintain rich chroma in shadows, neutral for achromatic

    QColor shifted;
    shifted.setHslF(targetHue, newS, newL, a);

    // Ambient light subtle tint integration
    if (ambientShadowTint.isValid()) {
        const qreal blend = 0.22;
        const int r = qRound(shifted.red() * (1.0 - blend) + ambientShadowTint.red() * blend);
        const int g = qRound(shifted.green() * (1.0 - blend) + ambientShadowTint.green() * blend);
        const int b = qRound(shifted.blue() * (1.0 - blend) + ambientShadowTint.blue() * blend);
        shifted.setRgb(r, g, b, shifted.alpha());
    }

    return shifted;
}

QColor KisAiStrokeQualityUtils::calculateHueShiftedHighlight(
    const QColor &baseColor,
    const QColor &keyLightTint,
    qreal intensity)
{
    if (!baseColor.isValid()) {
        return QColor(255, 255, 255);
    }

    float h = 0.0f, s = 0.0f, l = 0.0f, a = 1.0f;
    baseColor.getHslF(&h, &s, &l, &a);

    const bool isAchromatic = (h < 0.0f || s < 0.02f);
    float targetHue = h;

    if (isAchromatic) {
        if (keyLightTint.isValid()) {
            float kh = 0.0f, ks = 0.0f, kl = 0.0f;
            keyLightTint.getHslF(&kh, &ks, &kl);
            targetHue = (kh >= 0.0f) ? kh : 0.12f;
        } else {
            targetHue = 0.12f;
        }
    } else {
        // Shift toward warm sunlight (yellow/cream: approx 0.12 - 0.15)
        if (h > 0.15f && h < 0.50f) {
            targetHue = h - 0.05f; // Greens shift toward warm yellow
        } else if (h >= 0.50f && h < 0.80f) {
            targetHue = h - 0.06f; // Blues shift toward turquoise highlight
        }

        if (targetHue < 0.0f) targetHue += 1.0f;
        if (targetHue >= 1.0f) targetHue -= 1.0f;
        targetHue = qBound(0.0f, targetHue, 0.9999f);
    }

    const float boost = static_cast<float>(qBound<qreal>(0.1, intensity, 0.9));
    const float newL = qMin<float>(0.98f, l + (1.0f - l) * boost);
    const float newS = isAchromatic ? (keyLightTint.isValid() ? 0.03f : 0.0f) : qMax<float>(0.15f, s * (1.0f - boost * 0.4f));

    QColor highlight;
    highlight.setHslF(targetHue, newS, newL, a);

    if (keyLightTint.isValid()) {
        const qreal blend = 0.28;
        const int r = qRound(highlight.red() * (1.0 - blend) + keyLightTint.red() * blend);
        const int g = qRound(highlight.green() * (1.0 - blend) + keyLightTint.green() * blend);
        const int b = qRound(highlight.blue() * (1.0 - blend) + keyLightTint.blue() * blend);
        highlight.setRgb(r, g, b, highlight.alpha());
    }

    return highlight;
}

KisAiStrokeProgram KisAiStrokeQualityUtils::applyTrapping(
    const KisAiStrokeProgram &program,
    qreal trappingPx)
{
    if (qAbs(trappingPx) < 1.0e-4 || program.operations.isEmpty()) {
        return program;
    }

    // QSize::isValid() is true for (0, 0), which would then be used as a divisor.
    const QSize canvasSize = (program.canvasSize.width() > 0 && program.canvasSize.height() > 0)
        ? program.canvasSize
        : QSize(1024, 1024);
    KisAiStrokeProgram trapped = program;

    for (KisAiStrokeOperation &op : trapped.operations) {
        const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
        if (lName == QLatin1String("Flats") && (op.kind == KisAiStrokeOperation::Kind::Fill ||
                                                op.kind == KisAiStrokeOperation::Kind::GradientFill)) {
            if (op.polygon.size() >= 3) {
                // Scale polygon to pixels
                QPolygonF pixelPoly;
                pixelPoly.reserve(op.polygon.size());
                for (const QPointF &pt : op.polygon) {
                    pixelPoly.append(QPointF(pt.x() * canvasSize.width(), pt.y() * canvasSize.height()));
                }

                // Apply outward offset (trapping)
                const QPolygonF dilated = offsetPolygon(pixelPoly, trappingPx);

                // Convert back to normalized [0.0, 1.0]
                QPolygonF normPoly;
                normPoly.reserve(dilated.size());
                for (const QPointF &pt : dilated) {
                    normPoly.append(QPointF(clamp01(pt.x() / canvasSize.width()),
                                            clamp01(pt.y() / canvasSize.height())));
                }
                op.polygon = normPoly;
            }
        }
    }

    return trapped;
}

namespace {
bool isFrontHairOp(const KisAiStrokeOperation &op)
{
    const QString id = op.id.toLower();
    return id.contains(QLatin1String("fringe")) ||
           id.contains(QLatin1String("bangs")) ||
           id.contains(QLatin1String("front")) ||
           id.contains(QLatin1String("side_lock"));
}
} // namespace

QVector<KisAiStrokeOperation> KisAiStrokeQualityUtils::uniteOverlappingHairFlats(
    const QVector<KisAiStrokeOperation> &operations)
{
    QVector<KisAiStrokeOperation> others;
    QVector<KisAiStrokeOperation> hairCandidates;
    others.reserve(operations.size());
    for (const KisAiStrokeOperation &op : operations) {
        const bool isHairFill = op.kind == KisAiStrokeOperation::Kind::Fill
            && KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Flats")
            && op.id.contains(QLatin1String("hair"), Qt::CaseInsensitive)
            && op.polygon.size() >= 3;
        if (isHairFill)
            hairCandidates.append(op);
        else
            others.append(op);
    }
    if (hairCandidates.size() < 2)
        return operations;

    // Greedy boolean union: merge intersecting silhouettes, but NEVER merge
    // Front Hair (fringe/bangs) with Back Hair (behind face skin) across Z-depth!
    QVector<QPainterPath> masses;
    QVector<KisAiStrokeOperation> massOwners;
    QVector<int> candidateMassIndex(hairCandidates.size(), -1);
    masses.reserve(hairCandidates.size());

    for (int ci = 0; ci < hairCandidates.size(); ++ci) {
        const KisAiStrokeOperation &op = hairCandidates.at(ci);
        const bool opIsFront = isFrontHairOp(op);
        QPainterPath path;
        path.addPolygon(op.polygon);
        const QRectF opBounds = op.polygon.boundingRect();
        bool absorbed = false;
        for (int i = 0; i < masses.size(); ++i) {
            // Front hair only merges with front hair; back hair only with back hair
            if (opIsFront != isFrontHairOp(massOwners.at(i)))
                continue;

            // Reject disjoint candidates with a cheap bounds test: Qt's boolean
            // path ops are far more expensive and this loop is quadratic in the
            // number of hair silhouettes (routinely 20-40 from the model).
            if (!opBounds.intersects(masses.at(i).boundingRect())) {
                continue;
            }

            if (masses.at(i).intersects(path)) {
                masses[i] = masses.at(i).united(path);
                // Keep the visual identity of the larger contributor.
                if (op.polygon.boundingRect().width() * op.polygon.boundingRect().height()
                    > massOwners.at(i).polygon.boundingRect().width()
                        * massOwners.at(i).polygon.boundingRect().height()) {
                    KisAiStrokeOperation owner = op;
                    owner.polygon = masses.at(i).toFillPolygon();
                    massOwners[i] = owner;
                } else {
                    massOwners[i].polygon = masses.at(i).toFillPolygon();
                }
                candidateMassIndex[ci] = i;
                absorbed = true;
                break;
            }
        }
        if (!absorbed) {
            candidateMassIndex[ci] = masses.size();
            masses.append(path);
            massOwners.append(op);
        }
    }

    QVector<KisAiStrokeOperation> result;
    result.reserve(others.size() + massOwners.size());
    // Preserve exact Z-order: each hair mass is emitted when its FIRST contributing
    // candidate is encountered, keeping front hair strictly over face skin!
    QSet<int> emittedMasses;
    int hairCandidateCursor = 0;

    for (const KisAiStrokeOperation &op : operations) {
        const bool isHairFill = op.kind == KisAiStrokeOperation::Kind::Fill
            && KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Flats")
            && op.id.contains(QLatin1String("hair"), Qt::CaseInsensitive)
            && op.polygon.size() >= 3;
        if (isHairFill) {
            const int massIdx = candidateMassIndex.value(hairCandidateCursor++, -1);
            if (massIdx >= 0 && !emittedMasses.contains(massIdx)) {
                emittedMasses.insert(massIdx);
                result.append(massOwners.at(massIdx));
            }
            continue;
        }
        result.append(op);
    }
    return result;
}

int KisAiStrokeQualityUtils::applyLineartHierarchy(
    QVector<KisAiStrokeOperation> &operations)
{
    int adjusted = 0;
    for (KisAiStrokeOperation &op : operations) {
        if (op.kind != KisAiStrokeOperation::Kind::Path)
            continue;
        if (KisAiStrokeProgramCodec::normalizeLayerName(op.layer) != QLatin1String("Lineart"))
            continue;
        if (op.brush.sizeMode.compare(QLatin1String("px"), Qt::CaseInsensitive) == 0)
            continue; // absolute sizes are authorial intent
        if (op.points.size() < 2)
            continue;

        const QString prof = op.brush.profile.toLower();
        const bool isDelicate = prof == QLatin1String("fineliner") || prof == QLatin1String("maru_pen")
            || prof == QLatin1String("feathering") || prof == QLatin1String("stipple")
            || op.id.contains(QLatin1String("strand")) || op.id.contains(QLatin1String("hatch"))
            || op.id.contains(QLatin1String("eyelash")) || op.id.contains(QLatin1String("double_eyelid"))
            || op.id.contains(QLatin1String("wrinkle")) || op.id.contains(QLatin1String("trim"))
            || op.id.contains(QLatin1String("iris"));

        // If authorial / rig intent explicitly specified a delicate line <= 0.0025, do not inflate it
        if (op.brush.size > 0.0 && op.brush.size <= 0.0025 && isDelicate) {
            continue;
        }

        qreal length = 0.0;
        for (int i = 1; i < op.points.size(); ++i) {
            const QPointF d = op.points.at(i).pos - op.points.at(i - 1).pos;
            length += std::hypot(d.x(), d.y());
        }

        qreal tier = 0.003;
        if (isDelicate || length < 0.12) {
            // Tier 0: Micro details, hair strands, delicate hatches, eyelashes
            tier = 0.0015;
        } else if (length >= 1.0) {
            // Tier 3: Major outer silhouette contours
            tier = 0.008;
        } else if (length >= 0.35) {
            // Tier 2: Structural outlines
            tier = 0.005;
        } else {
            // Tier 1: Intermediate contours
            tier = 0.003;
        }

        if (op.closed)
            tier = qMin<qreal>(0.009, tier + 0.001);
        if (qAbs(op.brush.size - tier) > 1.0e-6) {
            op.brush.size = tier;
            ++adjusted;
        }
    }
    return adjusted;
}

QString KisAiStrokeQualityUtils::brushPresetName(const QString &profile)
{
    const QString p = profile.trimmed().toLower();
    if (p == QLatin1String("fineliner") || p == QLatin1String("maru_pen"))
        return QStringLiteral("Ink_Fineliner");
    if (p == QLatin1String("gpen") || p == QLatin1String("pencil") || p == QLatin1String("feathering"))
        return QStringLiteral("Pencil-2");
    if (p == QLatin1String("airbrush"))
        return QStringLiteral("Airbrush Soft");
    if (p == QLatin1String("crayon") || p == QLatin1String("charcoal") || p == QLatin1String("splatter") || p == QLatin1String("stipple"))
        return QStringLiteral("Chalk Soft");
    if (p == QLatin1String("watercolor"))
        return QStringLiteral("Watercolor Soft");
    return QStringLiteral("Basic-5 Size");
}

int KisAiStrokeQualityUtils::assignBrushPresetHints(
    KisAiStrokeProgram &program)
{
    int assigned = 0;
    for (KisAiStrokeOperation &op : program.operations) {
        if (!op.brush.presetHint.trimmed().isEmpty())
            continue;
        op.brush.presetHint = brushPresetName(op.brush.profile);
        ++assigned;
    }
    return assigned;
}

KisAiStrokeQualityUtils::HairClumpSynthesis KisAiStrokeQualityUtils::synthesizeHairClump(
    const KisAiStrokeOperation &ribbonOp,
    const QSize &canvasSize,
    quint32 seed)
{
    HairClumpSynthesis out;
    out.mainMass = ribbonOp;
    // Ensure the foundational hair mass is opaque and solidly blocks background/skin
    out.mainMass.layer = QStringLiteral("Flats");
    out.mainMass.brush.opacity = qMax<qreal>(0.92, ribbonOp.brush.opacity);
    out.mainMass.widthStart = qMax<qreal>(0.015, ribbonOp.widthStart);
    out.mainMass.widthMid = qMax<qreal>(0.018, ribbonOp.widthMid);

    const QVector<QPointF> &spine = ribbonOp.spine;
    if (spine.size() < 2 || canvasSize.width() <= 0 || canvasSize.height() <= 0) {
        return out;
    }

    QRandomGenerator rng(seed);
    const qreal baseDim = qMax<qreal>(1.0, qMin(canvasSize.width(), canvasSize.height()));
    const qreal avgWidth = ((out.mainMass.widthStart + out.mainMass.widthMid + out.mainMass.widthEnd) / 3.0) * baseDim;

    // 1. Generate internal flowing hair strands (Lineart / Shading)
    // Keep strand count balanced (4 to 5) to prevent dense parallel barcode artifact
    const int strandCount = qBound(4, qRound(avgWidth * 0.10) + 3, 5);
    const QColor strandColor = calculateHueShiftedShadow(ribbonOp.brush.color, QColor(30, 25, 45), 0.40);

    for (int s = 0; s < strandCount; ++s) {
        const qreal lateralOffset = ((qreal(s) / qMax(1, strandCount - 1)) - 0.5) * 1.5; // [-0.75, 0.75]
        const qreal baseOffsetPx = lateralOffset * (avgWidth * 0.40);

        KisAiStrokeOperation strandOp;
        strandOp.kind = KisAiStrokeOperation::Kind::Path;
        strandOp.id = QStringLiteral("%1_strand_%2").arg(ribbonOp.id).arg(s);
        strandOp.layer = QStringLiteral("Lineart");
        strandOp.brush.profile = QStringLiteral("gpen");
        strandOp.brush.color = strandColor;
        strandOp.brush.size = qMax<qreal>(0.0012, (avgWidth * 0.10) / baseDim);
        strandOp.brush.opacity = 0.65 + rng.generateDouble() * 0.25;

        strandOp.points.reserve(spine.size());
        for (int i = 0; i < spine.size(); ++i) {
            const QPointF curr = scalePoint(spine.at(i), canvasSize);
            QPointF tangent;
            if (i == 0) {
                tangent = scalePoint(spine.at(1), canvasSize) - curr;
            } else if (i == spine.size() - 1) {
                tangent = curr - scalePoint(spine.at(i - 1), canvasSize);
            } else {
                tangent = scalePoint(spine.at(i + 1), canvasSize) - scalePoint(spine.at(i - 1), canvasSize);
            }
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            const QPointF normal = (tLen > 1.0e-5) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);

            const qreal progress = qreal(i) / qMax(1, spine.size() - 1);
            // Converge smoothly toward the lock tip (0.25 offset at tip vs 1.0 at root) to eliminate parallel sukkari lines
            const qreal convergenceFactor = 1.0 - progress * 0.70;
            // Elegant S-curve wave along strand
            const qreal wave = std::sin(qreal(i) * 1.1 + s * 1.4) * (avgWidth * 0.07) * (1.0 - progress * 0.4);
            const QPointF displacedPx = curr + normal * (baseOffsetPx * convergenceFactor + wave);

            const qreal normX = clamp01(displacedPx.x() / canvasSize.width());
            const qreal normY = clamp01(displacedPx.y() / canvasSize.height());
            const qreal pressure = std::sin(progress * PI) * 0.80 + 0.20;
            strandOp.points.append(KisAiStrokePoint(normX, normY, pressure));
        }

        if (strandOp.points.size() >= 2) {
            out.strands.append(strandOp);
        }
    }

    // 2. Generate delicate flyaway hairs branching off the clump
    const int flyawayCount = qBound(1, qRound(avgWidth * 0.08), 3);
    for (int f = 0; f < flyawayCount; ++f) {
        KisAiStrokeOperation flyOp;
        flyOp.kind = KisAiStrokeOperation::Kind::Path;
        flyOp.id = QStringLiteral("%1_flyaway_%2").arg(ribbonOp.id).arg(f);
        flyOp.layer = QStringLiteral("Lineart");
        flyOp.brush.profile = QStringLiteral("gpen");
        flyOp.brush.color = ribbonOp.brush.color;
        flyOp.brush.size = qMax<qreal>(0.0010, (avgWidth * 0.08) / baseDim);
        flyOp.brush.opacity = 0.50;

        const int startIdx = qBound(0, qRound(spine.size() * (0.3 + f * 0.25)), spine.size() - 2);
        const qreal dirSign = (f % 2 == 0) ? 1.0 : -1.0;

        for (int i = startIdx; i < spine.size(); ++i) {
            const QPointF curr = scalePoint(spine.at(i), canvasSize);
            const qreal progress = qreal(i - startIdx) / qMax(1, spine.size() - 1 - startIdx);
            const qreal flarePx = dirSign * (avgWidth * 0.55) * progress * (1.0 + rng.generateDouble() * 0.3);

            QPointF tangent = (i < spine.size() - 1)
                ? (scalePoint(spine.at(i + 1), canvasSize) - curr)
                : (curr - scalePoint(spine.at(i - 1), canvasSize));
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            const QPointF normal = (tLen > 1.0e-5) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);

            const QPointF displacedPx = curr + normal * flarePx;
            const qreal normX = clamp01(displacedPx.x() / canvasSize.width());
            const qreal normY = clamp01(displacedPx.y() / canvasSize.height());
            const qreal pressure = (1.0 - progress) * 0.6 + 0.1;
            flyOp.points.append(KisAiStrokePoint(normX, normY, pressure));
        }

        if (flyOp.points.size() >= 2) {
            out.flyaways.append(flyOp);
        }
    }

    // 3. Angel Halo highlight arc across the mid-upper ridge
    if (spine.size() >= 3) {
        out.highlightHalo.kind = KisAiStrokeOperation::Kind::Path;
        out.highlightHalo.id = QStringLiteral("%1_halo").arg(ribbonOp.id);
        out.highlightHalo.layer = QStringLiteral("Highlights");
        out.highlightHalo.brush.profile = QStringLiteral("airbrush");
        out.highlightHalo.brush.color = calculateHueShiftedHighlight(ribbonOp.brush.color, QColor(255, 252, 240), 0.70);
        out.highlightHalo.brush.size = qMax<qreal>(0.003, (avgWidth * 0.20) / baseDim);
        out.highlightHalo.brush.opacity = 0.22;

        int hStart = qMax(0, qRound(spine.size() * 0.25));
        int hEnd = qMin(spine.size() - 1, qRound(spine.size() * 0.55));
        if (hEnd <= hStart && hStart + 1 < spine.size()) {
            hEnd = hStart + 1;
        }
        for (int i = hStart; i <= hEnd; ++i) {
            out.highlightHalo.points.append(KisAiStrokePoint(spine.at(i).x(), spine.at(i).y(), 0.9));
        }
    }

    return out;
}

QVector<KisAiStrokeOperation> KisAiStrokeQualityUtils::synthesizeFoliageClusters(
    const KisAiStrokeOperation &fillOp,
    const QSize &canvasSize,
    quint32 seed)
{
    QVector<KisAiStrokeOperation> out;
    if (fillOp.polygon.size() < 3 || canvasSize.width() <= 0 || canvasSize.height() <= 0) {
        out.append(fillOp);
        return out;
    }

    QRandomGenerator rng(seed);
    const QPolygonF pixelPoly = scalePolygon(fillOp.polygon, canvasSize);
    const QRectF b = pixelPoly.boundingRect();
    if (b.width() < 10 || b.height() < 10) {
        out.append(fillOp);
        return out;
    }

    // 1. Soft atmospheric base wash
    KisAiStrokeOperation baseWash = fillOp;
    baseWash.id = fillOp.id + QStringLiteral("_base_wash");
    baseWash.fillStyle = QStringLiteral("wash");
    baseWash.brush.opacity = qBound<qreal>(0.0, fillOp.brush.opacity * 0.85, 1.0);
    out.append(baseWash);

    // 2. Clustered billowing petal/leaf masses inside the canopy
    const int clusterCount = qBound(4, qRound(std::hypot(b.width(), b.height()) * 0.08), 16);
    const QColor baseColor = fillOp.brush.color;
    const QColor shadowColor = calculateHueShiftedShadow(baseColor, QColor(40, 25, 55), 0.35);
    const QColor highlightColor = calculateHueShiftedHighlight(baseColor, QColor(255, 250, 245), 0.45);

    for (int c = 0; c < clusterCount; ++c) {
        QPointF centerPx;
        // QPointF::isNull() is also true for a legitimate (0,0) hit, so an
        // explicit flag is needed to distinguish "not found" from "found at the
        // origin" (the renderer guards gradientCenter the same way).
        bool centerFound = false;
        for (int attempt = 0; attempt < 15; ++attempt) {
            const qreal cx = b.left() + (0.15 + rng.generateDouble() * 0.70) * b.width();
            const qreal cy = b.top() + (0.15 + rng.generateDouble() * 0.70) * b.height();
            const QPointF cand(cx, cy);
            if (pixelPoly.containsPoint(cand, Qt::OddEvenFill)) {
                centerPx = cand;
                centerFound = true;
                break;
            }
        }
        if (!centerFound) {
            centerPx = b.center();
        }

        const qreal clusterRadiusPx = (b.width() * 0.12 + rng.generateDouble() * b.width() * 0.18);
        const bool isLowerShadow = (centerPx.y() > b.center().y());

        QPolygonF clusterNormPoly;
        const int petalVerts = 10;
        for (int v = 0; v < petalVerts; ++v) {
            const qreal angle = (2.0 * PI * v) / petalVerts;
            const qreal rPerturb = 0.80 + rng.generateDouble() * 0.40;
            const qreal px = centerPx.x() + std::cos(angle) * clusterRadiusPx * rPerturb;
            const qreal py = centerPx.y() + std::sin(angle) * (clusterRadiusPx * 0.75) * rPerturb;
            clusterNormPoly.append(QPointF(clamp01(px / canvasSize.width()),
                                          clamp01(py / canvasSize.height())));
        }

        KisAiStrokeOperation clusterOp;
        clusterOp.kind = KisAiStrokeOperation::Kind::Fill;
        clusterOp.id = QStringLiteral("%1_cluster_%2").arg(fillOp.id).arg(c);
        clusterOp.layer = isLowerShadow ? QStringLiteral("Shading") : fillOp.layer;
        clusterOp.polygon = clusterNormPoly;
        clusterOp.smooth = true;
        clusterOp.fillStyle = QStringLiteral("wash");
        clusterOp.brush.profile = QStringLiteral("watercolor");
        clusterOp.brush.color = isLowerShadow ? shadowColor : (c % 2 == 0 ? highlightColor : baseColor);
        clusterOp.brush.opacity = 0.65 + rng.generateDouble() * 0.30;
        out.append(clusterOp);
    }

    // 3. Edge-drifting petal particles
    KisAiStrokeOperation petalParticles;
    petalParticles.kind = KisAiStrokeOperation::Kind::Particles;
    petalParticles.id = fillOp.id + QStringLiteral("_drifting_petals");
    petalParticles.layer = QStringLiteral("FX");
    petalParticles.bounds = QRectF(clamp01((b.left() - b.width() * 0.1) / canvasSize.width()),
                                   clamp01((b.top() - b.height() * 0.05) / canvasSize.height()),
                                   clamp01((b.width() * 1.3) / canvasSize.width()),
                                   clamp01((b.height() * 1.3) / canvasSize.height()));
    petalParticles.particleShape = QStringLiteral("petal");
    petalParticles.particleCount = qBound(8, qRound(clusterCount * 1.8), 35);
    petalParticles.brush.color = highlightColor;
    petalParticles.brush.size = 0.006;
    petalParticles.brush.opacity = 0.85;
    out.append(petalParticles);

    return out;
}

bool KisAiStrokeQualityUtils::isCastShadow(
    const QPolygonF &polygon,
    const QSize &canvasSize)
{
    if (polygon.size() < 3 || canvasSize.width() <= 0 || canvasSize.height() <= 0) return false;
    const QPolygonF pixelPoly = scalePolygon(polygon, canvasSize);
    const QRectF b = pixelPoly.boundingRect();
    if (b.width() <= 0.0 || b.height() <= 0.0) return false;
    const qreal area = b.width() * b.height();
    const qreal canvasArea = qreal(canvasSize.width()) * canvasSize.height();
    if (canvasArea <= 0.0) return false;

    const qreal aspect = b.height() > 0 ? b.width() / b.height() : 1.0;
    return (area < canvasArea * 0.015 || aspect > 4.0 || aspect < 0.25);
}

// =========================================================================
// 7. Advanced Artistic Fidelity & Inking Utilities (V5)
// =========================================================================

QVector<KisAiStrokeOperation> KisAiStrokeQualityUtils::generateCornerInkingDots(
    const QVector<KisAiStrokeOperation> &operations,
    const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> dots;
    if (canvasSize.width() <= 0 || canvasSize.height() <= 0) return dots;

    struct LineSegment {
        QPointF p1;
        QPointF p2;
        QColor color;
        qreal width;
    };
    QVector<LineSegment> segments;

    for (const auto &op : operations) {
        if (op.kind != KisAiStrokeOperation::Kind::Path || op.points.size() < 2) continue;
        if (op.layer.compare(QLatin1String("Lineart"), Qt::CaseInsensitive) != 0) continue;

        const qreal baseW = op.brush.size > 0.0 ? op.brush.size : 0.004;
        for (int i = 0; i < op.points.size() - 1; ++i) {
            segments.append({op.points.at(i).pos, op.points.at(i + 1).pos, op.brush.color, baseW});
        }
    }

    const qreal minDistSq = 0.015 * 0.015;
    QVector<QPointF> dotCenters;

    for (int i = 0; i < segments.size(); ++i) {
        for (int j = i + 1; j < segments.size(); ++j) {
            const auto &s1 = segments.at(i);
            const auto &s2 = segments.at(j);

            const qreal d1 = pointDistance(s1.p1, s2.p1);
            const qreal d2 = pointDistance(s1.p1, s2.p2);
            const qreal d3 = pointDistance(s1.p2, s2.p1);
            const qreal d4 = pointDistance(s1.p2, s2.p2);

            QPointF candidate;
            bool found = false;
            if (d1 < 0.008) { candidate = (s1.p1 + s2.p1) * 0.5; found = true; }
            else if (d2 < 0.008) { candidate = (s1.p1 + s2.p2) * 0.5; found = true; }
            else if (d3 < 0.008) { candidate = (s1.p2 + s2.p1) * 0.5; found = true; }
            else if (d4 < 0.008) { candidate = (s1.p2 + s2.p2) * 0.5; found = true; }

            if (found) {
                bool tooClose = false;
                for (const auto &c : dotCenters) {
                    const qreal dx = c.x() - candidate.x();
                    const qreal dy = c.y() - candidate.y();
                    if (dx * dx + dy * dy < minDistSq) {
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose && dotCenters.size() < 32) {
                    dotCenters.append(candidate);
                    KisAiStrokeOperation dotOp;
                    dotOp.kind = KisAiStrokeOperation::Kind::Path;
                    dotOp.id = QStringLiteral("corner_ink_%1").arg(dotCenters.size());
                    dotOp.layer = QStringLiteral("Lineart");
                    dotOp.points.append(KisAiStrokePoint(candidate.x(), candidate.y(), 1.0));
                    dotOp.brush.color = s1.color;
                    dotOp.brush.size = qBound<qreal>(0.003, s1.width * 1.5, 0.008);
                    dotOp.brush.opacity = 0.85;
                    dots.append(dotOp);
                }
            }
        }
    }

    return dots;
}

QColor KisAiStrokeQualityUtils::calculateHarmonicLineColor(
    const QColor &baseInkColor,
    const QColor &underlyingFlatsColor,
    bool isSkin)
{
    if (!underlyingFlatsColor.isValid() || underlyingFlatsColor.alpha() < 30) {
        return baseInkColor;
    }

    int fH, fS, fV;
    underlyingFlatsColor.getHsv(&fH, &fS, &fV);

    int bH, bS, bV, bA;
    baseInkColor.getHsv(&bH, &bS, &bV, &bA);

    if (isSkin) {
        const int outH = (fH >= 0 && fH <= 45) ? qBound(5, fH - 5, 20) : 10;
        const int outS = qBound(90, qRound(fS * 1.6), 180);
        const int outV = qBound(45, qRound(bV * 1.25), 85);
        return QColor::fromHsv(outH, outS, outV, bA);
    } else {
        const int outH = fH >= 0 ? fH : bH;
        const int outS = qBound(60, qRound(fS * 1.3), 200);
        const int outV = qBound(35, qRound(fV * 0.30), 75);
        return QColor::fromHsv(outH, outS, outV, bA);
    }
}

QVector<KisAiStrokeOperation> KisAiStrokeQualityUtils::generateSkinSssFringe(
    const KisAiStrokeOperation &shadingOp,
    const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    if (shadingOp.kind != KisAiStrokeOperation::Kind::Fill || shadingOp.polygon.size() < 3)
        return out;

    const QString lowerId = shadingOp.id.toLower();
    const bool isSkin = lowerId.contains(QLatin1String("skin")) || lowerId.contains(QLatin1String("face")) || lowerId.contains(QLatin1String("body"));
    if (!isSkin) return out;

    const qreal fringeWidth = 0.0035;
    KisAiStrokeOperation fringeOp;
    fringeOp.kind = KisAiStrokeOperation::Kind::Path;
    fringeOp.id = shadingOp.id + QStringLiteral("_sss_fringe");
    fringeOp.layer = QStringLiteral("Shading");
    fringeOp.smooth = true;
    fringeOp.closed = true;

    for (const auto &pt : shadingOp.polygon) {
        fringeOp.points.append(KisAiStrokePoint(pt.x(), pt.y(), 0.75));
    }

    fringeOp.brush.color = QColor(255, 95, 110);
    fringeOp.brush.size = fringeWidth;
    fringeOp.brush.opacity = 0.38;
    fringeOp.blendMode = QStringLiteral("overlay");

    out.append(fringeOp);
    return out;
}

QVector<KisAiStrokePoint> KisAiStrokeQualityUtils::beautifyFacialContour(
    const QVector<KisAiStrokePoint> &rawPoints,
    const QSize &canvasSize)
{
    if (rawPoints.size() < 7 || canvasSize.isEmpty()) {
        return rawPoints;
    }

    return resampleEquidistant(rawPoints, qMax<qreal>(4.0, canvasSize.width() * 0.008), false);
}

QVector<KisAiStrokeOperation> KisAiStrokeQualityUtils::generateRimLightStrokes(
    const QVector<KisAiStrokeOperation> &operations,
    const QSize &canvasSize,
    const QPointF &lightDir)
{
    QVector<KisAiStrokeOperation> out;
    if (canvasSize.width() <= 0 || canvasSize.height() <= 0) return out;

    const QPointF normLight = normalizeVector(lightDir.isNull() ? QPointF(0.707, -0.707) : lightDir);

    for (const auto &op : operations) {
        if (op.kind != KisAiStrokeOperation::Kind::Fill || op.polygon.size() < 4) continue;
        if (op.layer.compare(QLatin1String("Flats"), Qt::CaseInsensitive) != 0) continue;

        const auto &poly = op.polygon;
        QVector<KisAiStrokePoint> rimPoints;
        for (int i = 0; i < poly.size(); ++i) {
            const QPointF p1 = poly.at(i);
            const QPointF p2 = poly.at((i + 1) % poly.size());
            const QPointF edge = p2 - p1;
            const QPointF normal(-edge.y(), edge.x());
            if (dotProduct(normalizeVector(normal), normLight) > 0.45) {
                rimPoints.append(KisAiStrokePoint(p1.x(), p1.y(), 0.8));
            } else if (!rimPoints.isEmpty()) {
                if (rimPoints.size() >= 3) {
                    KisAiStrokeOperation rimOp;
                    rimOp.kind = KisAiStrokeOperation::Kind::Path;
                    rimOp.id = QStringLiteral("rim_light_%1_%2").arg(op.id).arg(out.size());
                    rimOp.layer = QStringLiteral("Highlights");
                    rimOp.points = rimPoints;
                    rimOp.smooth = true;
                    rimOp.brush.profile = QStringLiteral("airbrush");
                    rimOp.brush.color = QColor(220, 245, 255);
                    rimOp.brush.size = 0.0035;
                    rimOp.brush.opacity = 0.70;
                    rimOp.blendMode = QStringLiteral("screen");
                    out.append(rimOp);
                }
                rimPoints.clear();
            }
        }
        if (rimPoints.size() >= 3 && out.size() < 12) {
            KisAiStrokeOperation rimOp;
            rimOp.kind = KisAiStrokeOperation::Kind::Path;
            rimOp.id = QStringLiteral("rim_light_%1_%2").arg(op.id).arg(out.size());
            rimOp.layer = QStringLiteral("Highlights");
            rimOp.points = rimPoints;
            rimOp.smooth = true;
            rimOp.brush.profile = QStringLiteral("airbrush");
            rimOp.brush.color = QColor(220, 245, 255);
            rimOp.brush.size = 0.0035;
            rimOp.brush.opacity = 0.70;
            rimOp.blendMode = QStringLiteral("screen");
            out.append(rimOp);
        }
    }

    return out;
}

QVector<KisAiStrokeOperation> KisAiStrokeQualityUtils::generateProceduralBlush(
    const QVector<KisAiStrokeOperation> &operations,
    const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> out;
    if (canvasSize.width() <= 0 || canvasSize.height() <= 0) return out;

    QVector<QPointF> eyeCenters;
    qreal avgEyeWidth = 0.08;
    for (const auto &op : operations) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
            eyeCenters.append(op.eyeCenter);
            avgEyeWidth = qBound(0.04, op.eyeSize.width(), 0.16);
        }
    }

    if (eyeCenters.isEmpty()) return out;

    int idx = 0;
    for (const auto &eyePt : eyeCenters) {
        const qreal xOffset = (eyePt.x() < 0.5 ? -0.015 : 0.015);
        const QPointF cheekCenter(eyePt.x() + xOffset, eyePt.y() + avgEyeWidth * 0.75);

        KisAiStrokeOperation blushOp;
        blushOp.kind = KisAiStrokeOperation::Kind::GradientFill;
        blushOp.id = QStringLiteral("procedural_blush_%1").arg(++idx);
        blushOp.layer = QStringLiteral("Shading");
        blushOp.isRadial = true;
        blushOp.gradientCenter = cheekCenter;
        blushOp.gradientRadius = avgEyeWidth * 0.65;
        blushOp.gradientColors = {QColor(255, 125, 145, 95), QColor(255, 125, 145, 0)};
        blushOp.blendMode = QStringLiteral("normal");

        const int verts = 12;
        const qreal r = blushOp.gradientRadius;
        for (int v = 0; v < verts; ++v) {
            const qreal a = (2.0 * PI * v) / verts;
            blushOp.polygon.append(QPointF(cheekCenter.x() + std::cos(a) * r,
                                          cheekCenter.y() + std::sin(a) * (r * 0.75)));
        }

        out.append(blushOp);
    }

    return out;
}

QImage KisAiStrokeQualityUtils::generateFilmGrain(
    const QSize &size,
    qreal intensity,
    quint32 seed)
{
    if (size.isEmpty()) return QImage();
    QImage grain(size, QImage::Format_ARGB32_Premultiplied);
    QRandomGenerator rng(seed);

    const int alpha = qRound(qBound<qreal>(0.02, intensity, 0.25) * 255.0);
    for (int y = 0; y < size.height(); ++y) {
        auto *scanLine = reinterpret_cast<QRgb*>(grain.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int noise = rng.bounded(256);
            const int premul = (noise * alpha + 127) / 255;
            scanLine[x] = qRgba(premul, premul, premul, alpha);
        }
    }

    return grain;
}

QImage KisAiStrokeQualityUtils::generateVignetteImage(
    const QSize &size,
    qreal strength)
{
    if (size.isEmpty()) return QImage();
    QImage vignette(size, QImage::Format_ARGB32_Premultiplied);
    vignette.fill(Qt::transparent);

    QPainter p(&vignette);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QPointF center(size.width() * 0.5, size.height() * 0.5);
    const qreal radius = std::hypot(center.x(), center.y());

    QRadialGradient grad(center, radius);
    grad.setColorAt(0.0, QColor(0, 0, 0, 0));
    grad.setColorAt(0.55, QColor(0, 0, 0, 0));
    const int maxAlpha = qRound(qBound<qreal>(0.05, strength, 0.40) * 255.0);
    grad.setColorAt(1.0, QColor(0, 0, 0, maxAlpha));

    p.setBrush(grad);
    p.setPen(Qt::NoPen);
    p.drawRect(QRect(QPoint(0, 0), size));

    return vignette;
}

QImage KisAiStrokeQualityUtils::generateAmbientOverlay(
    const QSize &size,
    int artStyle,
    int timeOfDay)
{
    if (size.isEmpty()) return QImage();
    QImage overlay(size, QImage::Format_ARGB32_Premultiplied);
    overlay.fill(Qt::transparent);

    QPainter p(&overlay);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient grad(0, 0, 0, size.height());
    if (timeOfDay == 1) { // Sunset
        grad.setColorAt(0.0, QColor(255, 140, 60, 45));
        grad.setColorAt(0.6, QColor(220, 80, 120, 35));
        grad.setColorAt(1.0, QColor(60, 30, 80, 50));
    } else if (timeOfDay == 2 || artStyle == 5) { // Night / CyberNeon
        grad.setColorAt(0.0, QColor(20, 60, 120, 40));
        grad.setColorAt(0.7, QColor(80, 30, 90, 30));
        grad.setColorAt(1.0, QColor(10, 10, 30, 50));
    } else { // Day / Anime standard
        grad.setColorAt(0.0, QColor(255, 248, 220, 35));
        grad.setColorAt(0.5, QColor(255, 255, 255, 0));
        grad.setColorAt(1.0, QColor(70, 90, 140, 25));
    }

    p.setBrush(grad);
    p.setPen(Qt::NoPen);
    p.drawRect(QRect(QPoint(0, 0), size));

    return overlay;
}
