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
[[maybe_unused]] qreal clamp01Local(qreal v)
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

    const qreal shadowAngleDeg = std::atan2(-lightDir.y(), -lightDir.x()) * 180.0 / M_PI;
    const QPointF perpT(-lightDir.y(), lightDir.x());

    int shadowIndex = 0;
    for (const KisAiStrokeOperation &op : flatsOps) {
        if (op.kind != KisAiStrokeOperation::Kind::Fill && op.kind != KisAiStrokeOperation::Kind::GradientFill)
            continue;
        if (op.polygon.size() < 3)
            continue;
        if (polygonAreaLocal(op.polygon) < 2.0e-4)
            continue; // micro patches earn no shadow; keeps noise down

        const QString lowerId = op.id.toLower();
        // Fringe bangs are anatomically shadowed by hair_cast_shadow
        if (lowerId.contains(QLatin1String("fringe")) || lowerId.contains(QLatin1String("bangs"))) {
            continue;
        }

        const bool isFaceSkin = lowerId.contains(QLatin1String("skin")) || lowerId.contains(QLatin1String("face")) ||
                                lowerId.contains(QLatin1String("ear"));

        const QRectF b = op.polygon.boundingRect();
        const QPointF center = b.center();
        const qreal r = qMax(b.width(), b.height()) * 1.5;

        // Construct 3D curvature terminator plane:
        // Positioned across the center of the form and oriented opposite the key light direction,
        // producing a natural half-tone terminator falloff instead of artificial shifted paper silhouettes.
        const qreal termOffset = isFaceSkin ? (r * 0.15) : (r * 0.08);
        const QPointF cTerm = center + (lightDir * termOffset);

        QPolygonF shadowHalfPlane;
        shadowHalfPlane << (cTerm - perpT * r * 2.0)
                        << (cTerm + perpT * r * 2.0)
                        << (cTerm + perpT * r * 2.0 - lightDir * r * 3.0)
                        << (cTerm - perpT * r * 2.0 - lightDir * r * 3.0);

        QPainterPath original;
        original.addPolygon(op.polygon);
        QPainterPath planePath;
        planePath.addPolygon(shadowHalfPlane);
        QPainterPath shadowPath = original.intersected(planePath);

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
        shadow.brush.opacity = isFaceSkin ? 0.22 : 0.38;
        shadow.brush.size = 0.03;
        shadow.polygon = coreShadow;
        shadow.fillStyle = QStringLiteral("directional");
        shadow.angleDeg = shadowAngleDeg;
        shadow.blendMode = QStringLiteral("multiply");
        shadow.clipToId = op.id;
        shading.append(shadow);
        ++shadowIndex;
        if (shadowIndex >= 24)
            break;
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
            rim.blendMode = QStringLiteral("color_dodge");
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
        ao.blendMode = QStringLiteral("multiply");
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
        neckAo.blendMode = QStringLiteral("multiply");
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
        hairCast.blendMode = QStringLiteral("multiply");
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

// ========================================================================
// V5 R7-3/R7-4: time-of-day LUT + 4-layer shading completion
// ========================================================================
KisAiLightRig::TimeOfDayLut KisAiLightRig::timeOfDayLut(const QString &timeOfDay)
{
    TimeOfDayLut lut;
    if (timeOfDay == QLatin1String("night")) {
        lut.keyTint = QColor(214, 226, 255);      // cool moon key
        lut.fillTint = QColor(30, 42, 88);        // deep blue fill
        lut.ambientTint = QColor(24, 34, 76);     // night atmosphere
        lut.sssTint = QColor(150, 160, 220);      // cool SSS, subdued
        lut.bounceTint = QColor(70, 90, 150);     // moon bounce off ground
        lut.skyTop = QColor(8, 12, 34);
        lut.skyMid = QColor(22, 32, 72);
        lut.skyBottom = QColor(48, 58, 104);
    } else if (timeOfDay == QLatin1String("sunset")) {
        lut.keyTint = QColor(255, 176, 108);      // orange key
        lut.fillTint = QColor(88, 62, 118);       // violet fill
        lut.ambientTint = QColor(180, 110, 90);   // warm dusk atmosphere
        lut.sssTint = QColor(255, 138, 118);      // amplified warm SSS
        lut.bounceTint = QColor(230, 150, 100);   // warm ground bounce
        lut.skyTop = QColor(64, 52, 120);
        lut.skyMid = QColor(214, 118, 96);
        lut.skyBottom = QColor(252, 206, 150);
    } else { // day
        lut.keyTint = QColor(255, 252, 240);
        lut.fillTint = QColor(52, 64, 104);
        lut.ambientTint = QColor(190, 208, 235);
        lut.sssTint = QColor(255, 154, 138);      // classic skin SSS coral
        lut.bounceTint = QColor(226, 214, 196);   // neutral warm bounce
        lut.skyTop = QColor(96, 156, 232);
        lut.skyMid = QColor(164, 208, 244);
        lut.skyBottom = QColor(226, 240, 252);
    }
    return lut;
}

QVector<KisAiStrokeOperation> KisAiLightRig::synthesizeFormShading(
    const QVector<KisAiStrokeOperation> &flatsOps,
    const KisAiLightSettings &rig,
    const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> form;
    const QPointF lightDir = normalizedDirection(rig.direction);
    const QPointF perpT(-lightDir.y(), lightDir.x());

    int emitted = 0;
    for (const KisAiStrokeOperation &op : flatsOps) {
        if (op.kind != KisAiStrokeOperation::Kind::Fill && op.kind != KisAiStrokeOperation::Kind::GradientFill)
            continue;
        if (op.polygon.size() < 3)
            continue;
        if (polygonAreaLocal(op.polygon) < 2.0e-4)
            continue;

        const QRectF b = op.polygon.boundingRect();
        const QPointF center = b.center();
        const qreal r = qMax(b.width(), b.height()) * 1.5;

        // Softer, wider terminator than the core pass (2x offset).
        const QPointF cTerm = center + (lightDir * r * 0.16);
        QPolygonF shadowHalfPlane;
        shadowHalfPlane << (cTerm - perpT * r * 2.0)
                        << (cTerm + perpT * r * 2.0)
                        << (cTerm + perpT * r * 2.0 - lightDir * r * 3.0)
                        << (cTerm - perpT * r * 2.0 - lightDir * r * 3.0);

        QPainterPath original;
        original.addPolygon(op.polygon);
        QPainterPath planePath;
        planePath.addPolygon(shadowHalfPlane);
        QPainterPath formPath = original.intersected(planePath);
        const QPolygonF formPoly = formPath.toFillPolygon();
        if (formPoly.size() < 3 || polygonAreaLocal(formPoly) < 1.0e-4)
            continue;

        KisAiStrokeOperation soft;
        soft.kind = KisAiStrokeOperation::Kind::Fill;
        soft.id = QStringLiteral("%1_form_shadow").arg(op.id);
        soft.layer = QStringLiteral("Shading");
        soft.brush.profile = QStringLiteral("watercolor");
        soft.brush.color = shadowColor(op.brush.color, rig);
        // About half the core opacity: a whisper, not a statement.
        soft.brush.opacity = 0.14;
        soft.brush.size = 0.03;
        soft.polygon = formPoly;
        soft.fillStyle = QStringLiteral("directional");
        soft.angleDeg = std::atan2(-lightDir.y(), -lightDir.x()) * 180.0 / M_PI;
        soft.blendMode = QStringLiteral("multiply");
        soft.clipToId = op.id;
        form.append(soft);

        if (++emitted >= 24)
            break;
    }
    return form;
}

QVector<KisAiStrokeOperation> KisAiLightRig::synthesizeBounceLight(
    const QVector<KisAiStrokeOperation> &flatsOps,
    const KisAiLightSettings &rig,
    const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> bounce;
    const TimeOfDayLut lut = timeOfDayLut(rig.timeOfDay);

    int emitted = 0;
    for (const KisAiStrokeOperation &op : flatsOps) {
        if (op.kind != KisAiStrokeOperation::Kind::Fill && op.kind != KisAiStrokeOperation::Kind::GradientFill)
            continue;
        if (op.polygon.size() < 3)
            continue;
        const qreal area = polygonAreaLocal(op.polygon);
        if (area < 1.0e-3)
            continue; // bounce only matters on real masses

        // Lower-third intersection of the mass (light bounces up from below).
        const QRectF b = op.polygon.boundingRect();
        const QRectF lowerThird(b.left(), b.top() + b.height() * 2.0 / 3.0,
                                b.width(), b.height() / 3.0);
        QPainterPath original;
        original.addPolygon(op.polygon);
        QPainterPath bandPath;
        bandPath.addRect(lowerThird);
        const QPolygonF bandPoly = original.intersected(bandPath).toFillPolygon();
        if (bandPoly.size() < 3 || polygonAreaLocal(bandPoly) < 1.0e-4)
            continue;

        KisAiStrokeOperation wash;
        wash.kind = KisAiStrokeOperation::Kind::Fill;
        wash.id = QStringLiteral("%1_bounce_light").arg(op.id);
        wash.layer = QStringLiteral("Shading");
        wash.brush.profile = QStringLiteral("airbrush");
        // Bounce color derives from the LUT, brightened toward the base color.
        const QColor base = op.brush.color;
        wash.brush.color = QColor(
            qBound(0, (base.red() + lut.bounceTint.red()) / 2 + 22, 255),
            qBound(0, (base.green() + lut.bounceTint.green()) / 2 + 22, 255),
            qBound(0, (base.blue() + lut.bounceTint.blue()) / 2 + 22, 255),
            255);
        wash.brush.opacity = 0.16;
        wash.brush.size = 0.03;
        wash.polygon = bandPoly;
        wash.fillStyle = QStringLiteral("wash");
        wash.blendMode = QStringLiteral("screen");
        wash.clipToId = op.id;
        bounce.append(wash);

        if (++emitted >= 12)
            break;
    }
    return bounce;
}
