/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiLightRig.h"
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

    int shadowIndex = 0;
    for (const KisAiStrokeOperation &op : flatsOps) {
        if (op.kind != KisAiStrokeOperation::Kind::Fill && op.kind != KisAiStrokeOperation::Kind::GradientFill)
            continue;
        if (op.polygon.size() < 3)
            continue;
        if (polygonAreaLocal(op.polygon) < 2.0e-4)
            continue; // micro patches earn no shadow; keeps noise down
        QPainterPath original;
        original.addPolygon(op.polygon);
        QPainterPath shifted;
        QPolygonF shiftedPoly;
        shiftedPoly.reserve(op.polygon.size());
        for (const QPointF &pt : op.polygon)
            shiftedPoly.append(QPointF(clamp01Local(pt.x() + shadowOffset.x()), clamp01Local(pt.y() + shadowOffset.y())));
        shifted.addPolygon(shiftedPoly);
        const QPolygonF coreShadow = original.intersected(shifted).toFillPolygon();
        if (coreShadow.size() < 3 || polygonAreaLocal(coreShadow) < 1.0e-4)
            continue;

        KisAiStrokeOperation shadow;
        shadow.kind = KisAiStrokeOperation::Kind::Fill;
        shadow.id = QStringLiteral("%1_core_shadow").arg(op.id);
        shadow.layer = QStringLiteral("Shading");
        shadow.brush.profile = QStringLiteral("watercolor");
        shadow.brush.color = shadowColor(op.brush.color, rig);
        shadow.brush.opacity = 0.55;
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
        struct ScoredPoint { int index; QPointF pt; qreal score; };
        QVector<ScoredPoint> scored;
        scored.reserve(largestMass->polygon.size());
        for (int pi = 0; pi < largestMass->polygon.size(); ++pi) {
            const QPointF &pt = largestMass->polygon.at(pi);
            scored.append({pi, pt, pt.x() * lightDir.x() + pt.y() * lightDir.y()});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const ScoredPoint &a, const ScoredPoint &b) { return a.score > b.score; });
        const int rimCount = qMin(6, scored.size());
        if (rimCount >= 2) {
            KisAiStrokeOperation rim;
            rim.kind = KisAiStrokeOperation::Kind::Path;
            rim.id = QStringLiteral("%1_rim_light").arg(largestMass->id);
            rim.layer = QStringLiteral("Highlights");
            rim.brush.profile = QStringLiteral("gpen");
            rim.brush.color = highlightColor(largestMass->brush.color, rig);
            rim.brush.size = 0.0035;
            rim.brush.opacity = 0.85;
            // Order rim points along the polygon so the path stays coherent.
            // Carry the original index instead of re-looking points up by
            // value: duplicate coordinates would otherwise all resolve to the
            // first index and zig-zag the rim path.
            QVector<ScoredPoint> top;
            for (int i = 0; i < rimCount; ++i)
                top.append(scored.at(i));
            std::sort(top.begin(), top.end(),
                      [](const ScoredPoint &a, const ScoredPoint &b) { return a.index < b.index; });
            for (const ScoredPoint &sp : top)
                rim.points.append(KisAiStrokePoint(sp.pt.x(), sp.pt.y(), 0.7));
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
        ao.brush.opacity = 0.35;
        ao.brush.size = 0.02;
        ao.fillStyle = QStringLiteral("wash");
        const qreal aoW = hw * 0.30, aoH = hh * 0.06;
        const QPointF aoC(hc.x(), hc.y() + hh * 0.44);
        for (int i = 0; i <= 12; ++i) {
            const qreal t = 2.0 * M_PI * i / 12.0;
            ao.polygon.append(QPointF(aoC.x() + aoW * std::cos(t), aoC.y() + aoH * std::sin(t)));
        }
        shading.append(ao);

        KisAiStrokeOperation hairCast;
        hairCast.kind = KisAiStrokeOperation::Kind::Fill;
        hairCast.id = QStringLiteral("hair_cast_shadow");
        hairCast.layer = QStringLiteral("Shading");
        hairCast.brush.profile = QStringLiteral("watercolor");
        hairCast.brush.color = shadowColor(QColor(255, 224, 192), rig);
        hairCast.brush.opacity = 0.30;
        hairCast.brush.size = 0.02;
        hairCast.fillStyle = QStringLiteral("wash");
        const qreal bandW = hw * 0.46, bandTop = hc.y() - hh * 0.34, bandBottom = hc.y() - hh * 0.20;
        hairCast.polygon = QPolygonF{
            QPointF(hc.x() - bandW, bandTop), QPointF(hc.x() + bandW, bandTop),
            QPointF(hc.x() + bandW * 0.92, bandBottom), QPointF(hc.x() - bandW * 0.92, bandBottom)};
        shading.append(hairCast);
    }

    return shading;
}
