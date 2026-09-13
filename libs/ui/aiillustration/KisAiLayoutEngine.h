/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_LAYOUT_ENGINE_H
#define KIS_AI_LAYOUT_ENGINE_H

#include <QPair>
#include <QPolygonF>
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
 * V3 Phase 1: Deterministic painter. Converts a meaning-only SceneSpec into a
 * KisAiStrokeProgram whose geometry is generated from canonical anime rigs
 * (HeadRig / EyePair / HairMass / background slots) with guaranteed symmetry.
 * The LLM never emits coordinates through this path.
 */
class KRITAUI_EXPORT KisAiLayoutEngine
{
public:
    static KisAiStrokeProgram generateProgram(
        const KisAiSceneSpec &spec,
        const QSize &canvasSize
    );

    // Rig helpers (exposed for unit tests).
    static QPolygonF headOutlinePolygon(const QPointF &center, qreal width, qreal height);
    static QPair<QPointF, QPointF> eyePairCenters(const QPointF &headCenter, qreal headWidth, qreal headHeight, const QString &facing);
    static QVector<KisAiStrokeOperation> hairMassForStyle(
        const KisAiSceneSpec &spec,
        const QPointF &headCenter,
        qreal headWidth,
        qreal headHeight
    );
    static QVector<KisAiStrokeOperation> hairBackMassForStyle(
        const KisAiSceneSpec &spec,
        const QPointF &headCenter,
        qreal headWidth,
        qreal headHeight
    );
    static QVector<KisAiStrokeOperation> hairFrontMassForStyle(
        const KisAiSceneSpec &spec,
        const QPointF &headCenter,
        qreal headWidth,
        qreal headHeight
    );
    static QVector<KisAiStrokeOperation> backgroundForSpec(
        const KisAiSceneSpec &spec,
        const QSize &canvasSize
    );
    static QVector<KisAiStrokeOperation> clothingForSpec(
        const KisAiSceneSpec &spec,
        const QPointF &headCenter,
        qreal headWidth,
        qreal headHeight,
        const QSize &canvasSize
    );

private:
    static QVector<KisAiStrokeOperation> characterProgram(
        const KisAiSceneSpec &spec,
        const QSize &canvasSize
    );
    static QVector<KisAiStrokeOperation> landscapeProgram(
        const KisAiSceneSpec &spec,
        const QSize &canvasSize
    );
    static KisAiStrokeOperation makeFill(
        const QString &id, const QString &layer,
        const QPolygonF &polygon, const QColor &color,
        const QString &profile, qreal opacity, const QString &style
    );
    static KisAiStrokeOperation makePath(
        const QString &id, const QString &layer,
        const QVector<KisAiStrokePoint> &points,
        const QColor &color, const QString &profile, qreal size, qreal opacity
    );
};

#endif // KIS_AI_LAYOUT_ENGINE_H
