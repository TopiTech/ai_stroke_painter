/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_DELIBERATE_STROKE_H
#define KIS_AI_DELIBERATE_STROKE_H

#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiStrokeProgram.h"

/**
 * V4 Deliberate Stroke Engine.
 *
 * Human painters work stroke by stroke: plan order, clean the line,
 * check it, then commit ink. This engine gives the same discipline to
 * generated programs without trusting the LLM with fine geometry.
 *
 * All helpers are deterministic (no wall-clock randomness) and operate
 * in normalized [0,1] coordinates unless stated otherwise.
 */
struct KRITAUI_EXPORT KisAiStrokeLintReport {
    bool drop = false;
    bool needsRepair = false;
    QStringList reasons;
    qreal lengthPx = 0.0;
    qreal maxCurvature = 0.0;
    int selfIntersections = 0;
    qreal inkCoverage = 0.0;
};

struct KRITAUI_EXPORT KisAiStrokeCommitReview {
    bool committed = true;
    QRectF dirtyRect;
    qreal inkCoverage = 0.0; // normalized area estimate in [0,1]
    QStringList notes;
};

class KRITAUI_EXPORT KisAiDeliberateStroke
{
public:
    /**
     * Clean one stroke: RDP jitter removal (1.2px) + equidistant
     * resampling (3px). Closed flags are honoured. Deterministic.
     */
    static QVector<KisAiStrokePoint> stabilizeStroke(
        const QVector<KisAiStrokePoint> &points,
        const QSize &canvasSize,
        bool closed,
        quint32 seed = 42);

    /**
     * V5 R7-1: ink dynamics — velocity-linked pressure modulation.
     * Slow sample spacing = ink pooling (pressure boosted, up to +15%),
     * fast spacing = dry fade (pressure eased toward @p fadeFloor).
     * Pure and deterministic; runs after stabilizeStroke so LLM points and
     * resampled points both receive the same pen physics.
     */
    static QVector<KisAiStrokePoint> applyInkDynamics(
        const QVector<KisAiStrokePoint> &points,
        const QSize &canvasSize,
        qreal poolingBoost = 0.15,
        qreal fadeFloor = 0.55);

    /**
     * Inspect one operation before any ink is committed.
     * Never paints; only classifies drop / repair / keep.
     */
    static KisAiStrokeLintReport lintStroke(
        const KisAiStrokeOperation &op,
        const QSize &canvasSize);

    /**
     * Human-like paint order: layer order, large masses first,
     * thin washes before opaque strokes, facial details last.
     * Returns permutation indices into ops.
     */
    static QVector<int> planStrokeOrder(
        const QVector<KisAiStrokeOperation> &ops,
        const QSize &canvasSize);

    /** Sorted copy helper used directly by the renderer buckets. */
    static QVector<KisAiStrokeOperation> orderOperationsForRendering(
        const QVector<KisAiStrokeOperation> &ops,
        const QSize &canvasSize);

    /** Face-detail ids are painted last (eyes, brows, mouth, nose...). */
    static bool isFaceDetail(const QString &id);

    /**
     * Meaning-aware supersampling: faces/eyes deserve 3x on small
     * canvases while backgrounds stay at 1x. Memory-bounded.
     */
    static int adaptiveSupersampleScale(
        const QVector<KisAiStrokeOperation> &ops,
        const QSize &canvasSize);

    /**
     * Shared envelope builder (bowtie-clamped) so Path and Ribbon
     * rendering stop diverging with private normal math.
     */
    static QPolygonF buildEnvelopePolygon(
        const QVector<KisAiStrokePoint> &normalizedPoints,
        const KisAiStrokeBrush &brush,
        const QSize &canvasSizePx,
        bool closed,
        int supersampleScale = 1);

    /**
     * Meaning-group critique for an eye pair. Returns warnings;
     * empty means the pair is symmetric and committable.
     */
    static QStringList eyePairSymmetryWarnings(
        const QVector<KisAiStrokeOperation> &ops);

    /** Cheap ink-coverage + dirty-rect estimate for post-stroke review. */
    static KisAiStrokeCommitReview reviewStroke(
        const KisAiStrokeOperation &op,
        const QSize &canvasSize);
};

#endif // KIS_AI_DELIBERATE_STROKE_H
