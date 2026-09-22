/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiModelRouter.h"

#include "KisAiStrokeProgram.h"

#include <atomic>

namespace
{
std::atomic<KisAiModelRouter::QualityMode> s_qualityMode{KisAiModelRouter::QualityMode::Quality};
std::atomic<bool> s_forceAdvancedStrokeLogic{true}; // Enabled by default for all models

QString flagshipFallbackModel()
{
    // Stable, widely available flagship used when the user did not pin one.
    return QStringLiteral("gpt-5");
}

QString midTierModel()
{
    return QStringLiteral("gpt-4.1-mini");
}
} // namespace

bool KisAiModelRouter::isKnownFlagship(const QString &model)
{
    // Comprehensive classification of current and frontier flagship families (2025-2026+).
    const QString modelLower = model.trimmed().toLower();
    if (modelLower.isEmpty())
        return false;
    return modelLower.startsWith(QLatin1String("gpt-5"))
        || modelLower.startsWith(QLatin1String("gpt-6"))
        || modelLower.contains(QLatin1String("gpt-4.5"))
        || modelLower.contains(QLatin1String("gpt-4o"))
        || modelLower.startsWith(QLatin1String("o1"))
        || modelLower.startsWith(QLatin1String("o3"))
        || modelLower.startsWith(QLatin1String("o4"))
        || modelLower.contains(QLatin1String("opus"))
        || modelLower.contains(QLatin1String("sonnet"))
        || modelLower.contains(QLatin1String("claude-3-7"))
        || modelLower.contains(QLatin1String("claude-3.7"))
        || modelLower.contains(QLatin1String("claude-5"))
        || modelLower.contains(QLatin1String("fable"))
        || modelLower.contains(QLatin1String("gemini-2.0-pro"))
        || modelLower.contains(QLatin1String("gemini-2.5"))
        || modelLower.contains(QLatin1String("gemini-3"))
        || modelLower.contains(QLatin1String("deepseek-v3"))
        || modelLower.contains(QLatin1String("deepseek-v4"))
        || modelLower.contains(QLatin1String("deepseek-r1"))
        || modelLower.contains(QLatin1String("qwen-3"))
        || modelLower.contains(QLatin1String("qwen3"))
        || modelLower.contains(QLatin1String("qwen2.5-coder"));
}

bool KisAiModelRouter::shouldUseAdvancedStrokeLogic(const QString &model, QualityMode mode)
{
    // Operates for ANY model (even custom or local fine-tunes) when forced or in Quality/Max mode,
    // or when the model is recognized as a flagship.
    if (s_forceAdvancedStrokeLogic.load(std::memory_order_relaxed)) {
        return true;
    }
    if (mode == QualityMode::Quality || mode == QualityMode::Max) {
        return true;
    }
    return isKnownFlagship(model);
}

void KisAiModelRouter::setForceAdvancedStrokeLogic(bool force)
{
    s_forceAdvancedStrokeLogic.store(force, std::memory_order_relaxed);
}

bool KisAiModelRouter::forceAdvancedStrokeLogic()
{
    return s_forceAdvancedStrokeLogic.load(std::memory_order_relaxed);
}

void KisAiModelRouter::setQualityMode(QualityMode mode)
{
    s_qualityMode.store(mode, std::memory_order_relaxed);
}

KisAiModelRouter::QualityMode KisAiModelRouter::qualityMode()
{
    return s_qualityMode.load(std::memory_order_relaxed);
}

KisAiModelRouter::StagePlan KisAiModelRouter::planFor(Stage stage, const QString &preferredModel)
{
    StagePlan plan;
    plan.critiqueRounds = critiqueRoundBudget();
    plan.nBestCandidates = specCandidateCount();

    const bool flagship = isKnownFlagship(preferredModel);
    QString model = preferredModel;

    switch (stage) {
    case Stage::PromptExpansion:
        plan.temperature = 0.8;
        plan.reasoningEffort = QStringLiteral("medium");
        if (model.trimmed().isEmpty())
            model = flagshipFallbackModel();
        break;

    case Stage::SceneSpec:
        // Structured, meaning-only direction. N-best handled by the caller.
        plan.temperature = 0.7;
        plan.useStructuredOutput = true;
        plan.reasoningEffort = flagship ? QStringLiteral("medium") : QStringLiteral("low");
        if (model.isEmpty())
            model = flagshipFallbackModel();
        break;

    case Stage::VisionCritique:
        plan.temperature = 0.2;
        plan.useStructuredOutput = true;
        plan.includeVision = true;
        plan.visionDetail = QStringLiteral("high");
        plan.reasoningEffort = flagship ? QStringLiteral("high") : QStringLiteral("medium");
        if (model.isEmpty())
            model = flagshipFallbackModel();
        break;

    case Stage::PatchProposal:
        plan.temperature = 0.3;
        plan.useStructuredOutput = true;
        plan.includeVision = true;
        plan.visionDetail = QStringLiteral("high");
        plan.reasoningEffort = QStringLiteral("medium");
        if (model.isEmpty())
            model = flagshipFallbackModel();
        break;

    case Stage::GoalStep:
        plan.temperature = 0.5;
        plan.useStructuredOutput = true;
        plan.includeVision = true;
        plan.visionDetail = QStringLiteral("auto");
        plan.reasoningEffort = flagship ? QStringLiteral("medium") : QString();
        if (model.trimmed().isEmpty())
            model = flagshipFallbackModel();
        break;
    }

    if (s_qualityMode == QualityMode::Fast) {
        // Fast mode trades a tier down for latency; the user-pinned model
        // still wins unless they left the field empty.
        if (model.isEmpty())
            model = midTierModel();
        if (stage == Stage::VisionCritique || stage == Stage::PatchProposal) {
            plan.critiqueRounds = 1;
            plan.nBestCandidates = 1;
            plan.visionDetail = QStringLiteral("auto");
        }
    }

    plan.model = model;
    plan.useJsonFormat = (stage != Stage::PromptExpansion) && !plan.useStructuredOutput;
    return plan;
}

int KisAiModelRouter::specCandidateCount()
{
    switch (s_qualityMode.load(std::memory_order_relaxed)) {
    case QualityMode::Fast:
        return 1;
    case QualityMode::Quality:
        return 3;
    case QualityMode::Max:
        return 5;
    default:
        return 3;
    }
}

int KisAiModelRouter::critiqueRoundBudget()
{
    switch (s_qualityMode.load(std::memory_order_relaxed)) {
    case QualityMode::Fast:
        return 1;
    case QualityMode::Quality:
        return 2;
    case QualityMode::Max:
        return 3;
    default:
        return 2;
    }
}

QString KisAiModelRouter::structuredStrategy(const QString &model, const QString &endpoint)
{
    if (KisAiStrokeProgramCodec::supportsJsonSchema(model))
        return QStringLiteral("json_schema");
    if (endpoint.isEmpty() || KisAiStrokeProgramCodec::supportsJsonFormat(endpoint))
        return QStringLiteral("json_object");
    return QStringLiteral("none");
}

QStringList KisAiModelRouter::modelFallbackChain(Stage stage, const QString &preferredModel)
{
    Q_UNUSED(stage);
    QStringList chain;
    if (!preferredModel.trimmed().isEmpty())
        chain.append(preferredModel.trimmed());

    const QString flagship = flagshipFallbackModel();
    if (!chain.contains(flagship))
        chain.append(flagship);

    // Fast mode stops the ladder one tier earlier but always ends with the
    // empty string sentinel meaning "offline deterministic path".
    if (s_qualityMode != QualityMode::Fast) {
        const QString mid = midTierModel();
        if (!chain.contains(mid))
            chain.append(mid);
    }

    chain.append(QString());
    return chain;
}

bool KisAiModelRouter::stageUsesVision(Stage stage)
{
    return stage == Stage::VisionCritique
        || stage == Stage::PatchProposal
        || stage == Stage::GoalStep;
}

QString KisAiModelRouter::stageName(Stage stage)
{
    switch (stage) {
    case Stage::PromptExpansion:
        return QStringLiteral("prompt_expansion");
    case Stage::SceneSpec:
        return QStringLiteral("scene_spec");
    case Stage::VisionCritique:
        return QStringLiteral("vision_critique");
    case Stage::PatchProposal:
        return QStringLiteral("patch_proposal");
    case Stage::GoalStep:
        return QStringLiteral("goal_step");
    }
    return QStringLiteral("unknown");
}
