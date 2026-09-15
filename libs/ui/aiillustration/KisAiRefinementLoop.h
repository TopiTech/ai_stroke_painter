/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_REFINEMENT_LOOP_H
#define KIS_AI_REFINEMENT_LOOP_H

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiModelRouter.h"
#include "KisAiProgramPatch.h"
#include "KisAiSceneSpec.h"
#include "KisAiStrokeProgram.h"

struct KRITAUI_EXPORT KisAiNBestResult {
    KisAiSceneSpec spec;
    KisAiSceneSpecScore score;
    int candidateCount{0};
    QStringList logLines; // telemetry-safe (no prompt text)
};

/**
 * V6 W3: offline refinement glue.
 *
 * The Docker owns the network; this class owns everything deterministic
 * around it so the loop is unit-testable without HTTP:
 * - N-best candidate selection (parse several SceneSpec bodies, pick best)
 * - Router-resolved request parameters per stage
 * - Patch application + rig-delta re-layout (patch-only Goal steps)
 * - Critic convergence bookkeeping + telemetry line formatting
 */
class KRITAUI_EXPORT KisAiRefinementLoop
{
public:
    /**
     * Parse several SceneSpec response bodies and return the best by
     * deterministic score. Unparseable bodies are skipped; when none
     * parse, the offline default spec wins (code-side floor guarantee).
     */
    static KisAiNBestResult
    selectBestSpecFromBodies(const QString &prompt, const QSize &canvasSize, const QVector<QByteArray> &bodies);

    /** Resolve temperature/topP/tokens for a stage (Router single source). */
    struct ResolvedSampling {
        qreal temperature{0.7};
        qreal topP{1.0};
        int maxTokens{0};
        QString reasoningEffort;
        bool useJsonSchema{false};
        bool useJsonObject{false};
    };
    static ResolvedSampling samplingFor(KisAiModelRouter::Stage stage,
                                        const QString &preferredModel,
                                        const QString &endpoint,
                                        qreal configuredTemperature,
                                        qreal configuredTopP,
                                        int configuredMaxTokens,
                                        const QString &configuredReasoningEffort,
                                        bool forceJsonObjectOnly);

    /**
     * Apply rig patches to a spec, then re-lay-out the affected program.
     * Returns the re-laid-out program; rigDelta/rejected report the outcome.
     * Structural (non-rig) patches are applied directly to base ops.
     */
    static KisAiStrokeProgram applyRigPatchesAndRelayout(const KisAiStrokeProgram &base,
                                                         const KisAiSceneSpec &baseSpec,
                                                         const QSize &canvasSize,
                                                         const QVector<KisAiProgramPatch> &patches,
                                                         KisAiSceneRigOverrides *rigDelta = nullptr,
                                                         QStringList *rejected = nullptr);

    /** Serialize a rig parameter snapshot for patch-request payloads. */
    static QJsonObject rigStateSnapshot(const KisAiSceneSpec &spec);

    /** Critique regions to the JSON array the patch payload expects. */
    static QJsonArray regionsToJson(const QVector<KisAiCritiqueRegion> &regions);

    /**
     * Telemetry-safe log line for one loop round (no prompt/API content).
     * Categories: SPEC_NBEST, PATCH_APPLY, CRITIC_ROUND, DEGRADE.
     */
    static QString formatTelemetry(const QString &category, const QString &summary);

    /**
     * Decide whether to keep refining: false when the PSNR gain converged
     * or the round budget is spent.
     */
    static bool
    shouldContinue(qreal psnrBefore, qreal psnrAfter, int roundsDone, int roundBudget, qreal minImprovementDb = 1.5);
};

#endif // KIS_AI_REFINEMENT_LOOP_H
