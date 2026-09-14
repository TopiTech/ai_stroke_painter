/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_MODEL_ROUTER_H
#define KIS_AI_MODEL_ROUTER_H

#include <QString>
#include <QStringList>
#include <QSize>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

/**
 * V5 R6: Model Router — single source of truth for which model, sampling
 * parameters and capability flags each pipeline stage should use.
 *
 * The router wraps the legacy capability probes (supportsJsonSchema /
 * supportsJsonFormat / isReasoningModel / isVisionModel) into one stage →
 * settings table so callers stop duplicating ad-hoc if-chains.
 *
 * Stage ordering follows the V5 pipeline:
 *   PromptExpansion → SceneSpec (N-best) → Layout (code) → Render (code)
 *   → VisionCritique → PatchProposal → repeat critique loop → Finish.
 */
class KRITAUI_EXPORT KisAiModelRouter
{
public:
    /** Pipeline stages that need model access. */
    enum class Stage {
        PromptExpansion,   ///< Rich prompt expansion (text only).
        SceneSpec,         ///< Meaning-only art direction (structured output).
        VisionCritique,    ///< Canvas + crop inspection (vision, low temperature).
        PatchProposal,     ///< Region patches from critique (vision + structured).
        GoalStep           ///< Legacy Goal Mode full-program step.
    };

    /** User selectable quality presets exposed by the Docker. */
    enum class QualityMode {
        Fast,      ///< Mid-tier model, 1 critique round, no N-best.
        Quality,   ///< Flagship model, 2 critique rounds, 3-spec N-best.
        Max        ///< Flagship model, 3 critique rounds, 5-spec N-best.
    };

    /** Resolved settings for one stage. */
    struct StagePlan {
        QString model;
        qreal temperature {0.7};
        qreal topP {1.0};
        QString reasoningEffort;      ///< Empty = provider default.
        QString visionDetail;         ///< auto / high / low (vision stages only).
        bool useStructuredOutput {false}; ///< json_schema preferred.
        bool useJsonFormat {false};       ///< json_object fallback.
        bool includeVision {false};
        int critiqueRounds {2};
        int nBestCandidates {1};
        int maxTokensOverride {0};
    };

    /** Global quality preset (persisted via Docker settings). */
    static void setQualityMode(QualityMode mode);
    static QualityMode qualityMode();

    /** Resolve the full plan for a stage. @param preferredModel user/model combo selection. */
    static StagePlan planFor(Stage stage, const QString &preferredModel = QString());

    /** Number of SceneSpec candidates to request under the current mode. */
    static int specCandidateCount();

    /** Critique rounds budget under the current mode. */
    static int critiqueRoundBudget();

    /**
     * Structured-output strategy for a model/endpoint pair.
     * Returns "json_schema" (strict), "json_object" or "none".
     */
    static QString structuredStrategy(const QString &model, const QString &endpoint = QString());

    /**
     * Fallback chain for a stage: from the preferred model down through the
     * quality-mode ladder. Callers try entries in order; the last entry is
     * always QString() meaning "use the offline deterministic path".
     */
    static QStringList modelFallbackChain(Stage stage, const QString &preferredModel = QString());

    /** True when the stage should attach canvas imagery to the request. */
    static bool stageUsesVision(Stage stage);

    /** Human-readable stage name for logs (no user data). */
    static QString stageName(Stage stage);
};

#endif // KIS_AI_MODEL_ROUTER_H
