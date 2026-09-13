/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiLightRig.h"
#include "KisAiLayoutEngine.h"
#include "KisAiStrokeQualityUtils.h"

#include <QPainterPath>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace
{
qreal clamp01Local(qreal v)
{
    if (!std::isfinite(v))
        return 0.0;
    return qMax<qreal>(0.0, qMin<qreal>(1.0, v));
}

QPointF normalizedDirection(const QPointF &d)
{
    const qreal len = std::hypot(d.x(), d.y());
    if (len < 1.0e-6 || !std::isfinite(len))
        return QPointF(-0.5, -0.7);
    return QPointF(d.x() / len, d.y() / len);
}

qreal polygonAreaLocal(const QPolygonF &polygon)
{
    qreal twiceArea = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon.at(i);
        const QPointF &b = polygon.at((i + 1) % polygon.size());
        twiceArea += a.x() * b.y() - b.x() * a.y();
    }
    return qAbs(twiceArea) * 0.5;
}
} // namespace

KisAiLightSettings KisAiLightRig::fromSpec(const KisAiSceneSpec &spec)
{
    KisAiLightSettings rig;
    rig.direction = normalizedDirection(spec.light.direction);
    rig.warmth = spec.light.warmth;
    rig.timeOfDay = spec.light.timeOfDay;
    rig.keyTint = keyTintFor(rig);
    rig.fillTint = fillTintFor(rig);
    return rig;
}

QColor KisAiLightRig::keyTintFor(const KisAiLightSettings &rig)
{
    if (rig.timeOfDay == QLatin1String("night"))
        return QColor(255, 214, 150); // warm practicals against cool night
    if (rig.timeOfDay == QLatin1String("sunset"))
        return QColor(255, 170, 110);
    if (rig.warmth == QLatin1String("cool_key_warm_fill"))
        return QColor(210, 226, 255);
    return QColor(255, 252, 240);
}

QColor KisAiLightRig::fillTintFor(const KisAiLightSettings &rig)
{
    if (rig.timeOfDay == QLatin1String("night"))
        return QColor(40, 55, 110);
    if (rig.timeOfDay == QLatin1String("sunset"))
        return QColor(70, 60, 110);
    if (rig.warmth == QLatin1String("cool_key_warm_fill"))
        return QColor(90, 70, 60);
    return QColor(35, 40, 65);
}

QColor KisAiLightRig::shadowColor(const QColor &base, const KisAiLightSettings &rig)
{
    // Hue-shifted shadow anchored on the rig fill tint; never dirty black.
    QColor shadow = KisAiStrokeQualityUtils::calculateHueShiftedShadow(base, rig.fillTint, 0.38);
    if (shadow.value() < 30) {
        shadow = QColor::fromHsv((shadow.hue() + 360) % 360, qMax(40, shadow.saturation()), 42, shadow.alpha());
    }
    return shadow;
}

QColor KisAiLightRig::highlightColor(const QColor &base, const KisAiLightSettings &rig)
{
    return KisAiStrokeQualityUtils::calculateHueShiftedHighlight(base, rig.keyTint, 0.5);
}

QVector<KisAiStrokeOperation> KisAiLightRig::synthesizeShading(
    const QVector<KisAiStrokeOperation> &flatsOps,
    const KisAiLightSettings &rig,
    const QSize &canvasSize,
    const HeadAnchor *headAnchor)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> shading;
    const QPointF lightDir = normalizedDirection(rig.direction);
    // Shadow mass sits opposite the key light.
    const QPointF shadowOffset(-lightDir.x() * 0.035, -lightDir.y() * 0.035);

    const KisAiStrokeOperation *largestMass = nullptr;
    qreal largestArea = 0.0;
    for (const KisAiStrokeOperation &op : flatsOps) {
        if (op.kind != KisAiStrokeOperation::Kind::Fill && op.kind != KisAiStrokeOperation::Kind::GradientFill)
            continue;
        if (op.polygon.size() < 3)
            continue;
        const qreal area = polygonAreaLocal(op.polygon);
        if (area > largestArea) {
            largestArea = area;
            largestMass = &op;
        }
    }

    const KisAiStrokeOperation *fringeOp = nullptr;
    for (const KisAiStrokeOperation &op : flatsOps) {
        const QString lowerId = op.id.toLower();
        if (lowerId.contains(QLatin1String("fringe")) || lowerId.contains(QLatin1String("bangs"))) {
            fringeOp = &op;
            break;
        }
    }

    int shadowIndex = 0;
    for (const KisAiStrokeOperation &op : flatsOps) {
        if (op.kind != KisAiStrokeOperation::Kind::Fill && op.kind != KisAiStrokeOperation::Kind::GradientFill)
            continue;
        if (op.polygon.size() < 3)
            continue;
        if (polygonAreaLocal(op.polygon) < 2.0e-4)
            continue; // micro patches earn no shadow; keeps noise down

        // Exclude facial skin, neck, and front hair fringe from coarse shifted core shadows.
        // Facial and fringe shadows are handled anatomically by neck_shadow, eyelid_shade, hair_cast_shadow, etc.
        const QString lowerId = op.id.toLower();
        if (lowerId.contains(QLatin1String("skin")) || lowerId.contains(QLatin1String("face")) ||
            lowerId.contains(QLatin1String("ear")) || lowerId.contains(QLatin1String("neck")) ||
            lowerId.contains(QLatin1String("fringe")) || lowerId.contains(QLatin1String("bangs"))) {
            continue;
        }

        QPainterPath original;
        original.addPolygon(op.polygon);
        QPainterPath shifted;
        QPolygonF shiftedPoly;
        shiftedPoly.reserve(op.polygon.size());
        for (const QPointF &pt : op.polygon)
            shiftedPoly.append(QPointF(clamp01Local(pt.x() + shadowOffset.x()), clamp01Local(pt.y() + shadowOffset.y())));
        shifted.addPolygon(shiftedPoly);
        QPainterPath shadowPath = original.intersected(shifted);

        // Subtract head/face anchor so back hair shadows never cast over the front of the face!
        if (headAnchor && (lowerId.contains(QLatin1String("hair")) || lowerId.contains(QLatin1String("back")))) {
            QPainterPath facePath;
            facePath.addPolygon(KisAiLayoutEngine::headOutlinePolygon(headAnchor->headCenter, headAnchor->headWidth, headAnchor->headHeight));
            shadowPath = shadowPath.subtracted(facePath);
        }

        const QPolygonF coreShadow = shadowPath.toFillPolygon();
        if (coreShadow.size() < 3 || polygonAreaLocal(coreShadow) < 1.0e-4)
            continue;

        KisAiStrokeOperation shadow;
        shadow.kind = KisAiStrokeOperation::Kind::Fill;
        shadow.id = QStringLiteral("%1_core_shadow").arg(op.id);
        shadow.layer = QStringLiteral("Shading");
        shadow.brush.profile = QStringLiteral("watercolor");
        shadow.brush.color = shadowColor(op.brush.color, rig);
        shadow.brush.opacity = 0.35;
        shadow.brush.size = 0.03;
        shadow.polygon = coreShadow;
        shadow.fillStyle = QStringLiteral("wash");
        shading.append(shadow);
        ++shadowIndex;
        if (shadowIndex >= 12)
            break; // budget: shading hints, not wallpaper
    }

    // Rim light along the light-facing edge of the largest mass.
    if (largestMass && largestMass->polygon.size() >= 4) {
        const QPolygonF &poly = largestMass->polygon;
        const int n = poly.size();

        // Find the best contiguous edge run facing the light direction.
        QVector<qreal> scores(n);
        for (int i = 0; i < n; ++i) {
            scores[i] = poly[i].x() * lightDir.x() + poly[i].y() * lightDir.y();
        }

        int bestEdge = 0;
        qreal bestEdgeScore = -1e9;
        for (int i = 0; i < n; ++i) {
            const int next = (i + 1) % n;
            const qreal dist = QLineF(poly[i], poly[next]).length();
            if (dist > 0.35) continue;
            const qreal edgeScore = (scores[i] + scores[next]) * 0.5;
            if (edgeScore > bestEdgeScore) {
                bestEdgeScore = edgeScore;
                bestEdge = i;
            }
        }

        QVector<int> runIndices;
        runIndices.append(bestEdge);
        runIndices.append((bestEdge + 1) % n);

        const int prev = (bestEdge - 1 + n) % n;
        if (QLineF(poly[prev], poly[bestEdge]).length() < 0.25 && scores[prev] > bestEdgeScore * 0.6) {
            runIndices.prepend(prev);
        }
        const int next2 = (bestEdge + 2) % n;
        const int currEnd = runIndices.last();
        if (QLineF(poly[currEnd], poly[next2]).length() < 0.25 && scores[next2] > bestEdgeScore * 0.6) {
            runIndices.append(next2);
        }

        if (runIndices.size() >= 2) {
            KisAiStrokeOperation rim;
            rim.kind = KisAiStrokeOperation::Kind::Path;
            rim.id = QStringLiteral("%1_rim_light").arg(largestMass->id);
            rim.layer = QStringLiteral("Highlights");
            rim.brush.profile = QStringLiteral("airbrush");
            rim.brush.color = highlightColor(largestMass->brush.color, rig);
            rim.brush.size = 0.0055;
            rim.brush.opacity = 0.70;

            for (int idx : runIndices) {
                rim.points.append(KisAiStrokePoint(poly[idx].x(), poly[idx].y(), 0.7));
            }
            rim.closed = false;
            rim.smooth = true;
            shading.append(rim);
        }
    }

    // Character head anchor: chin AO + forehead hair-cast band.
    if (headAnchor) {
        const QPointF hc = headAnchor->headCenter;
        const qreal hw = headAnchor->headWidth;
        const qreal hh = headAnchor->headHeight;

        KisAiStrokeOperation ao;
        ao.kind = KisAiStrokeOperation::Kind::Fill;
        ao.id = QStringLiteral("chin_ao");
        ao.layer = QStringLiteral("Shading");
        ao.brush.profile = QStringLiteral("watercolor");
        ao.brush.color = shadowColor(QColor(255, 224, 192), rig);
        ao.brush.opacity = 0.20;
        ao.brush.size = 0.02;
        ao.fillStyle = QStringLiteral("wash");
        const qreal aoW = hw * 0.25, aoH = hh * 0.05;
        const QPointF aoC(hc.x(), hc.y() + (hh * 0.44));
        for (int i = 0; i <= 12; ++i) {
            const qreal t = 2.0 * M_PI * i / 12.0;
            ao.polygon.append(QPointF(aoC.x() + (aoW * std::cos(t)), aoC.y() + (aoH * std::sin(t))));
        }
        shading.append(ao);

        // Contact AO: Neck base / collar junction (接触影)
        KisAiStrokeOperation neckAo;
        neckAo.kind = KisAiStrokeOperation::Kind::Fill;
        neckAo.id = QStringLiteral("neck_collar_ao");
        neckAo.layer = QStringLiteral("Shading");
        neckAo.brush.profile = QStringLiteral("watercolor");
        neckAo.brush.color = shadowColor(QColor(255, 224, 192), rig);
        neckAo.brush.opacity = 0.24;
        neckAo.brush.size = 0.02;
        neckAo.fillStyle = QStringLiteral("wash");
        const qreal nAoW = hw * 0.32, nAoH = hh * 0.04;
        const QPointF nAoC(hc.x(), hc.y() + (hh * 0.72));
        for (int i = 0; i <= 12; ++i) {
            const qreal t = 2.0 * M_PI * i / 12.0;
            neckAo.polygon.append(QPointF(nAoC.x() + (nAoW * std::cos(t)), nAoC.y() + (nAoH * std::sin(t))));
        }
        shading.append(neckAo);

        KisAiStrokeOperation hairCast;
        hairCast.kind = KisAiStrokeOperation::Kind::Fill;
        hairCast.id = QStringLiteral("hair_cast_shadow");
        hairCast.layer = QStringLiteral("Shading");
        hairCast.brush.profile = QStringLiteral("watercolor");
        hairCast.brush.color = shadowColor(QColor(255, 224, 192), rig);
        hairCast.brush.opacity = 0.18;
        hairCast.brush.size = 0.02;
        hairCast.fillStyle = QStringLiteral("wash");

        if (fringeOp && fringeOp->polygon.size() >= 3) {
            // Project shadow downward from fringe clump tips onto forehead skin
            const QPointF castOffset(-lightDir.x() * 0.015, std::abs(lightDir.y()) * 0.025 + 0.012);
            QPolygonF shiftedFringe;
            shiftedFringe.reserve(fringeOp->polygon.size());
            for (const QPointF &pt : fringeOp->polygon) {
                shiftedFringe.append(pt + castOffset);
            }
            QPainterPath fringePath;
            fringePath.addPolygon(fringeOp->polygon);
            QPainterPath castPath;
            castPath.addPolygon(shiftedFringe);
            // Crucial: Subtract the front hair fringe itself so the cast shadow is
            // ONLY rendered onto the exposed forehead skin and NEVER covers the bangs!
            castPath = castPath.subtracted(fringePath);

            // Also clip to face skin contour so it stays strictly on the forehead
            QPainterPath facePath;
            facePath.addPolygon(KisAiLayoutEngine::headOutlinePolygon(hc, hw, hh));
            castPath = castPath.intersected(facePath);

            hairCast.polygon = castPath.toFillPolygon();
            if (hairCast.polygon.size() < 3) {
                const qreal bandW = hw * 0.38;
                const qreal bandTop = hc.y() + (hh * 0.02);
                const qreal bandBottom = hc.y() + (hh * 0.05);
                hairCast.polygon = QPolygonF{
                    QPointF(hc.x() - bandW, bandTop), QPointF(hc.x() + bandW, bandTop),
                    QPointF(hc.x() + (bandW * 0.90), bandBottom), QPointF(hc.x() - (bandW * 0.90), bandBottom)};
            }
        } else {
            const qreal bandW = hw * 0.38;
            const qreal bandTop = hc.y() + (hh * 0.02);
            const qreal bandBottom = hc.y() + (hh * 0.05);
            hairCast.polygon = QPolygonF{
                QPointF(hc.x() - bandW, bandTop), QPointF(hc.x() + bandW, bandTop),
                QPointF(hc.x() + (bandW * 0.90), bandBottom), QPointF(hc.x() - (bandW * 0.90), bandBottom)};
        }
        shading.append(hairCast);
    }

    return shading;
}
