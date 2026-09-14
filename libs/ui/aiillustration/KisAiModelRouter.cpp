/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiModelRouter.h"

#include "KisAiStrokeProgram.h"

namespace
{
KisAiModelRouter::QualityMode s_qualityMode = KisAiModelRouter::QualityMode::Quality;

bool isKnownFlagship(const QString &model)
{
    // Best-effort classification of current-generation flagship families.
    // Unknown models keep the user's explicit selection unchanged.
    const QString m = model.toLower();
    if (m.isEmpty())
        return false;
    return m.startsWith(QLatin1String("gpt-5"))
        || m.startsWith(QLatin1String("o3")) || m.startsWith(QLatin1String("o4"))
        || m.contains(QLatin1String("opus")) || m.contains(QLatin1String("sonnet"))
        || m.contains(QLatin1String("gemini-2.5")) || m.contains(QLatin1String("gemini-3"))
        || m.contains(QLatin1String("deepseek-v3")) || m.contains(QLatin1String("qwen3-max"));
}

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

void KisAiModelRouter::setQualityMode(QualityMode mode)
{
    s_qualityMode = mode;
}

KisAiModelRouter::QualityMode KisAiModelRouter::qualityMode()
{
    return s_qualityMode;
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
    plan.useJsonFormat = !plan.useStructuredOutput;
    return plan;
}

int KisAiModelRouter::specCandidateCount()
{
    switch (s_qualityMode) {
    case QualityMode::Fast:
        return 1;
    case QualityMode::Quality:
        return 3;
    case QualityMode::Max:
        return 5;
    }
    return 3;
}

int KisAiModelRouter::critiqueRoundBudget()
{
    switch (s_qualityMode) {
    case QualityMode::Fast:
        return 1;
    case QualityMode::Quality:
        return 2;
    case QualityMode::Max:
        return 3;
    }
    return 2;
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

    switch (stage) {
    case Stage::SceneSpec:
    case Stage::VisionCritique:
    case Stage::PatchProposal:
    case Stage::GoalStep:
    case Stage::PromptExpansion:
        break;
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
