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
     */
    struct HeadAnchor {
        QPointF headCenter {0.5, 0.38};
        qreal headHeight {0.42};
        qreal headWidth {0.33};
    };

    static QVector<KisAiStrokeOperation> synthesizeShading(
        const QVector<KisAiStrokeOperation> &flatsOps,
        const KisAiLightSettings &rig,
        const QSize &canvasSize,
        const HeadAnchor *headAnchor = nullptr
    );
};

#endif // KIS_AI_LIGHT_RIG_H
