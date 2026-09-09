/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeQualityUtils.h"

#include <QColor>
#include <QPainterPath>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace
{
constexpr qreal PI = 3.14159265358979323846;
constexpr qreal DEG2RAD = PI / 180.0;
constexpr qreal RAD2DEG = 180.0 / PI;

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

    const qreal step = qMax<qreal>(0.5, stepPx);
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
    result.reserve(qCeil(totalLength / step) + 2);
    result.append(points.first());

    qreal currentTargetDist = step;
    qreal accumulatedDist = 0.0;
    int segIdx = 0;

    while (currentTargetDist <= totalLength && segIdx < segmentCount) {
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

    if (!closed && (result.isEmpty() || pointDistance(result.last().pos, points.last().pos) > step * 0.25)) {
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

    painter.translate(bounds.center());
    painter.rotate(angleDeg);

    const qreal diag = std::hypot(bounds.width(), bounds.height());
    const int steps = qCeil(diag / spacing);

    if (lineScreen) {
        QPen pen(color, radius * 2.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (int i = -steps; i <= steps; ++i) {
            const qreal y = i * spacing;
            painter.drawLine(QPointF(-diag, y), QPointF(diag, y));
        }
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int iy = -steps; iy <= steps; ++iy) {
            for (int ix = -steps; ix <= steps; ++ix) {
                const QPointF pt(ix * spacing, iy * spacing);
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

    // Warm colors (red, orange, yellow, skin tones: H < 0.18 or H > 0.85) shift toward cool blue/violet
    // Cool colors (blue, cyan: 0.45 < H < 0.75) deepen toward rich indigo/navy
    float targetHue = h;
    if (h >= 0.0f && h < 0.18f) {
        // Red-orange-yellow -> shift towards purple-blue (approx 0.70 - 0.78)
        targetHue = h + 0.08f;
    } else if (h >= 0.85f && h <= 1.0f) {
        targetHue = h - 0.08f;
    } else if (h >= 0.45f && h < 0.65f) {
        // Cyan-blue -> shift deeper toward violet
        targetHue = h + 0.05f;
    }

    if (targetHue < 0.0f) targetHue += 1.0f;
    if (targetHue > 1.0f) targetHue -= 1.0f;

    const float depth = static_cast<float>(qBound<qreal>(0.1, shadowDepth, 0.8));
    const float newL = qMax<float>(0.05f, l * (1.0f - depth * 0.65f));
    const float newS = qBound<float>(0.1f, s * 1.15f, 1.0f); // Maintain rich chroma in shadows

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

    // Shift toward warm sunlight (yellow/cream: approx 0.12 - 0.15)
    float targetHue = h;
    if (h > 0.15f && h < 0.50f) {
        targetHue = h - 0.05f; // Greens shift toward warm yellow
    } else if (h >= 0.50f && h < 0.80f) {
        targetHue = h - 0.06f; // Blues shift toward turquoise highlight
    }

    if (targetHue < 0.0f) targetHue += 1.0f;
    if (targetHue > 1.0f) targetHue -= 1.0f;

    const float boost = static_cast<float>(qBound<qreal>(0.1, intensity, 0.9));
    const float newL = qMin<float>(0.98f, l + (1.0f - l) * boost);
    const float newS = qMax<float>(0.15f, s * (1.0f - boost * 0.4f));

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

    const QSize canvasSize = program.canvasSize.isValid() ? program.canvasSize : QSize(1024, 1024);
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
