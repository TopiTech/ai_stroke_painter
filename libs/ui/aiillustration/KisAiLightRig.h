/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_LIGHT_RIG_H
#define KIS_AI_LIGHT_RIG_H

#include <QColor>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiSceneSpec.h"
#include "KisAiStrokeProgram.h"

/**
 * V3 Phase 2: Single source of truth for light and color.
 * Every shadow/highlight/rim tone in a program must derive from one rig,
 * so the LLM can no longer scatter contradictory colors per operation.
 */
struct KRITAUI_EXPORT KisAiLightSettings
{
    QPointF direction {-0.5, -0.7}; // vector pointing TOWARD the key light
    QString warmth {QStringLiteral("warm_key_cool_fill")};
    QString timeOfDay {QStringLiteral("day")};
    QColor keyTint {QColor(255, 252, 240)};
    QColor fillTint {QColor(35, 40, 65)};
};

class KRITAUI_EXPORT KisAiLightRig
{
public:
    static KisAiLightSettings fromSpec(const KisAiSceneSpec &spec);

    static QColor keyTintFor(const KisAiLightSettings &rig);
    static QColor fillTintFor(const KisAiLightSettings &rig);

    static QColor shadowColor(const QColor &base, const KisAiLightSettings &rig);
    static QColor highlightColor(const QColor &base, const KisAiLightSettings &rig);

    /**
     * Derive shadow + rim operations from Flats silhouettes.
     * - Core shadow: silhouette translated AWAY from the light, intersected
     *   with the original (guaranteed inside, hue-shifted, never pure black).
     * - Rim light: path along the light-facing edge of the largest mass.
     * Optional head anchor adds chin AO and a forehead hair-cast band.
     */    struct HeadAnchor {
        QPointF headCenter {0.5, 0.38};
        qreal headHeight {0.42};
        qreal headWidth {0.33};
    };

    static QVector<KisAiStrokeOperation> synthesizeShading(
        const QVector<KisAiStrokeOperation> &flatsOps,
        const KisAiLightSettings &rig,
        const QSize &canvasSize,
        const HeadAnchor *headAnchor = nullptr);

    /**
     * V5 R7-3: time-of-day look-up table — the single place where
     * day/sunset/night decide key/fill/ambient/SSS/sky colors. Every layer
     * below and every background gradient should derive from this LUT so a
     * night scene can never again be painted with daylight tones.
     */
    struct TimeOfDayLut {
        QColor keyTint;      // key light color
        QColor fillTint;     // shadow-side fill color
        QColor ambientTint;  // atmosphere wash over the whole frame
        QColor sssTint;      // skin subsurface scattering tint
        QColor bounceTint;   // floor bounce light color
        QColor skyTop;       // background gradient top stop
        QColor skyMid;       // background gradient middle stop
        QColor skyBottom;    // background gradient bottom stop
    };

    static TimeOfDayLut timeOfDayLut(const QString &timeOfDay);

    /**
     * V5 R7-3: soft form-shadow layer (4-layer shading, layer 2 of 4).
     * Wider terminator offset and ~half the core opacity, hue-shifted —
     * gives volumes a soft roundness before the hard core shadow lands.
     * Clipped to the source flats like every shading op.
     */
    static QVector<KisAiStrokeOperation> synthesizeFormShading(
        const QVector<KisAiStrokeOperation> &flatsOps,
        const KisAiLightSettings &rig,
        const QSize &canvasSize);

    /**
     * V5 R7-3: floor bounce light (layer 4 supplement).
     * A subtle upward screen-blended wash on the lower third of each large
     * mass, tinted by the LUT bounce color (warm daylight bounce, cool
     * moonlight bounce). Derives only from the rig — never from raw op colors.
     */
    static QVector<KisAiStrokeOperation> synthesizeBounceLight(
        const QVector<KisAiStrokeOperation> &flatsOps,
        const KisAiLightSettings &rig,
        const QSize &canvasSize);

    /**
     * V7 Volumetric Pseudo-Normal Shading:
     * Calculates 3D surface normals (Half-Lambert lighting model) across spherical head
     * and cylindrical torso volumes to produce naturally curved terminators, 2-tone cel shadows,
     * and micro ambient occlusion (AO) in deep crevices.
     */
    static QVector<KisAiStrokeOperation> synthesizeVolumetricShading(
        const QVector<KisAiStrokeOperation> &flatsOps,
        const KisAiLightSettings &rig,
        const QSize &canvasSize,
        const HeadAnchor *headAnchor = nullptr);

    /**
     * V7 Material Optics (SSS fringe, Anisotropic hair sheen & Deep corneal highlights).
     */
    static QVector<KisAiStrokeOperation> synthesizeMaterialOptics(
        const QVector<KisAiStrokeOperation> &flatsOps,
        const KisAiLightSettings &rig,
        const QSize &canvasSize,
        const HeadAnchor *headAnchor = nullptr);
};

#endif // KIS_AI_LIGHT_RIG_H
