/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_VISION_CRITIC_H
#define KIS_AI_VISION_CRITIC_H

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiProgramPatch.h"
#include "KisAiLightRig.h"
#include "KisAiStrokeProgram.h"

/**
 * V5 R4: Vision Critic engine.
 *
 * Vision becomes an inspection instrument instead of a passive attachment:
 * the critic receives the full canvas plus a deterministic selection of
 * zoomed crops (face box, edge-dense regions, previously flagged regions),
 * answers a fixed painter's checklist as structured critique regions, and
 * proposes whitelisted patches. Code validates everything before any ink
 * changes.
 */
struct KRITAUI_EXPORT KisAiCriticCrop
{
    QRectF region;        // normalized [0,1] source rect
    QString reason;       // face, edge_density, prior_critique, face_hair_boundary
    QImage image;         // upscaled crop ready for base64 encoding
};

struct KRITAUI_EXPORT KisAiCriticRound
{
    int round {1};
    QVector<KisAiCritiqueRegion> regions;
    qreal readinessScore {1.0};
    QVector<KisAiProgramPatch> acceptedPatches;
    QStringList rejectedPatchReasons;
    qreal psnrBefore {0.0};
    qreal psnrAfter {0.0};
};

class KRITAUI_EXPORT KisAiVisionCritic
{
public:
    /**
     * Deterministic crop selection (max @p maxCrops):
     * 1. Face box (from head anchor) when a character is present.
     * 2. Highest edge-density tile among a fixed 4x4 grid, excluding the face.
     * 3. Regions flagged priority >= 4 in the previous round.
     * Crops are upscaled to at least 384 px on the short side.
     */
    static QVector<KisAiCriticCrop> selectCrops(
        const QImage &canvas,
        const QRectF &faceBox,
        const QVector<KisAiCritiqueRegion> &previousRegions,
        int maxCrops = 4);

    /**
     * Build the vision critique Chat Completions payload: checklist contract,
     * full canvas image and all crop images attached.
     */
    static QJsonObject buildCritiquePayload(
        const QString &model,
        const QString &prompt,
        const QImage &canvas,
        const QVector<KisAiCriticCrop> &crops,
        const KisAiLightSettings &rig,
        const QJsonArray &previousRegions = QJsonArray());

    /** The fixed painter's checklist system prompt. */
    static QString buildCritiqueSystemPrompt(const KisAiLightSettings &rig);

    /**
     * Parse the critique response into regions + readiness score.
     * Regions missing mandatory checklist fields are repaired or dropped.
     */
    static bool parseCritiqueResponse(
        const QByteArray &responseBytes,
        QVector<KisAiCritiqueRegion> *outRegions,
        qreal *readinessScore,
        QString *errorMessage = nullptr);

    /**
     * Extract patch proposals from a critique response (regions may embed
     * "suggestion_patches"). Only whitelisted patches survive validation.
     */
    static QVector<KisAiProgramPatch> extractSuggestedPatches(
        const QByteArray &responseBytes,
        QStringList *rejected = nullptr);

    /**
     * Convergence test for the critique loop: stop when the image stopped
     * improving (PSNR gain < threshold dB) or the budget is spent.
     */
    static bool hasConverged(qreal psnrBefore, qreal psnrAfter, qreal minImprovementDb = 1.5);

    /** Peak signal-to-noise ratio between two same-size images (dB, capped at 60). */
    static qreal psnr(const QImage &a, const QImage &b);

    /**
     * Merge a new critique round into the accumulated region list,
     * keeping the highest priority per (area, issue) key.
     */
    static QVector<KisAiCritiqueRegion> mergeRegions(
        const QVector<KisAiCritiqueRegion> &accumulated,
        const QVector<KisAiCritiqueRegion> &incoming);

    /** Face box helper shared with the crop selector (normalized rect). */
    static QRectF faceBoxFromAnchor(const QPointF &headCenter, qreal headWidth, qreal headHeight);
};

#endif // KIS_AI_VISION_CRITIC_H
