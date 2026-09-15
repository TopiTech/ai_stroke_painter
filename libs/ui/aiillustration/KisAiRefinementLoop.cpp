/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiRefinementLoop.h"

#include "KisAiLayoutEngine.h"
#include "KisAiVisionCritic.h"

KisAiNBestResult KisAiRefinementLoop::selectBestSpecFromBodies(const QString &prompt,
                                                               const QSize &canvasSize,
                                                               const QVector<QByteArray> &bodies)
{
    KisAiNBestResult result;
    QVector<KisAiSceneSpec> candidates;
    int parsed = 0;
    for (const QByteArray &body : bodies) {
        KisAiSceneSpec spec;
        QString err;
        if (KisAiSceneSpecCodec::parseSceneSpec(body, &spec, &err)) {
            if (spec.prompt.trimmed().isEmpty())
                spec.prompt = prompt;
            spec.canvasSize = canvasSize.isValid() ? canvasSize : spec.canvasSize;
            candidates.append(spec);
            ++parsed;
        }
    }
    result.candidateCount = bodies.size();
    result.spec = KisAiSceneSpecCodec::selectBestSpec(prompt, canvasSize, candidates);
    result.score = KisAiSceneSpecCodec::scoreSceneSpec(result.spec);
    result.logLines.append(QStringLiteral("parsed=%1/of=%2 score=%3")
                               .arg(parsed)
                               .arg(bodies.size())
                               .arg(QString::number(result.score.total, 'f', 3)));
    return result;
}

KisAiRefinementLoop::ResolvedSampling KisAiRefinementLoop::samplingFor(KisAiModelRouter::Stage stage,
                                                                       const QString &preferredModel,
                                                                       const QString &endpoint,
                                                                       qreal configuredTemperature,
                                                                       qreal configuredTopP,
                                                                       int configuredMaxTokens,
                                                                       const QString &configuredReasoningEffort,
                                                                       bool forceJsonObjectOnly)
{
    ResolvedSampling out;
    const KisAiModelRouter::StagePlan plan = KisAiModelRouter::planFor(stage, preferredModel);
    out.temperature = plan.temperature;
    out.topP = plan.topP;
    out.reasoningEffort =
        !configuredReasoningEffort.trimmed().isEmpty() ? configuredReasoningEffort : plan.reasoningEffort;
    // User pins still win for the base knobs; the Router owns stage deltas.
    if (stage == KisAiModelRouter::Stage::SceneSpec || stage == KisAiModelRouter::Stage::GoalStep) {
        out.temperature = qBound<qreal>(0.0, configuredTemperature, 2.0);
        out.topP = configuredTopP;
    }
    out.maxTokens = configuredMaxTokens;
    const QString strategy = forceJsonObjectOnly
        ? QStringLiteral("json_object")
        : KisAiModelRouter::structuredStrategy(plan.model.isEmpty() ? preferredModel : plan.model, endpoint);
    out.useJsonSchema = (strategy == QLatin1String("json_schema")) && plan.useStructuredOutput;
    out.useJsonObject =
        (strategy == QLatin1String("json_object")) || (plan.useJsonFormat && strategy != QLatin1String("none"));
    return out;
}

KisAiStrokeProgram KisAiRefinementLoop::applyRigPatchesAndRelayout(const KisAiStrokeProgram &base,
                                                                   const KisAiSceneSpec &baseSpec,
                                                                   const QSize &canvasSize,
                                                                   const QVector<KisAiProgramPatch> &patches,
                                                                   KisAiSceneRigOverrides *rigDelta,
                                                                   QStringList *rejected)
{
    KisAiSceneRigOverrides delta;
    KisAiStrokeProgram patched = KisAiProgramPatchCodec::applyPatches(base, patches, &delta, rejected);
    if (rigDelta)
        *rigDelta = delta;

    // Re-layout only when a rig key actually moved: structural ops stay
    // frozen so good regions survive (patch-only Goal steps).
    const bool rigMoved = (delta.eyeAperture != KisAiSceneRigOverrides().eyeAperture)
        || (delta.irisRatio != KisAiSceneRigOverrides().irisRatio) || (!delta.eyeHighlight.isEmpty())
        || (delta.hairHighlightBands != KisAiSceneRigOverrides().hairHighlightBands)
        || (delta.mouthWidthScale != KisAiSceneRigOverrides().mouthWidthScale)
        || (delta.hairStrandDensity != KisAiSceneRigOverrides().hairStrandDensity)
        || (delta.hairFlyaway != KisAiSceneRigOverrides().hairFlyaway);
    if (!rigMoved)
        return patched;

    KisAiSceneSpec relaid = baseSpec;
    relaid.rig.eyeAperture = delta.eyeAperture;
    relaid.rig.irisRatio = delta.irisRatio;
    if (!delta.eyeHighlight.isEmpty())
        relaid.rig.eyeHighlight = delta.eyeHighlight;
    relaid.rig.doubleLid = delta.doubleLid;
    relaid.rig.hairStrandDensity = delta.hairStrandDensity;
    relaid.rig.hairFlyaway = delta.hairFlyaway;
    relaid.rig.hairHighlightBands = delta.hairHighlightBands;
    relaid.rig.mouthWidthScale = delta.mouthWidthScale;
    relaid.rig.hasBrows = delta.hasBrows;

    const KisAiStrokeProgram fresh =
        KisAiLayoutEngine::generateProgram(relaid, canvasSize.isValid() ? canvasSize : base.canvasSize);
    // Merge: fresh rig-driven face + hair bands replace their counterparts;
    // every other op (background, clothing, shading) keeps the patched base.
    KisAiStrokeProgram merged = patched;
    auto isRigFaceId = [](const QString &id) {
        const QString l = id.toLower();
        return l.startsWith(QLatin1String("rig_eye_")) || l.startsWith(QLatin1String("rig_brow_"))
            || l.startsWith(QLatin1String("rig_nose_")) || l == QLatin1String("rig_mouth")
            || l.startsWith(QLatin1String("rig_hair_band_")) || l.startsWith(QLatin1String("eyelid_shade_"))
            || l.startsWith(QLatin1String("tear_trough_"));
    };
    for (int i = merged.operations.size() - 1; i >= 0; --i) {
        if (isRigFaceId(merged.operations.at(i).id))
            merged.operations.removeAt(i);
    }
    for (const KisAiStrokeOperation &op : fresh.operations) {
        if (isRigFaceId(op.id))
            merged.operations.append(op);
    }
    KisAiStrokeQualityReport report;
    KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(merged, &report);
    refined.prompt = base.prompt;
    return refined;
}

QJsonObject KisAiRefinementLoop::rigStateSnapshot(const KisAiSceneSpec &spec)
{
    QJsonObject rig;
    rig.insert(QStringLiteral("eye_aperture"), spec.rig.eyeAperture);
    rig.insert(QStringLiteral("iris_ratio"), spec.rig.irisRatio);
    rig.insert(QStringLiteral("eye_highlight"), spec.rig.eyeHighlight);
    rig.insert(QStringLiteral("double_lid"), spec.rig.doubleLid);
    rig.insert(QStringLiteral("hair_strand_density"), spec.rig.hairStrandDensity);
    rig.insert(QStringLiteral("hair_flyaway"), spec.rig.hairFlyaway);
    rig.insert(QStringLiteral("hair_highlight_bands"), spec.rig.hairHighlightBands);
    rig.insert(QStringLiteral("mouth_width_scale"), spec.rig.mouthWidthScale);
    rig.insert(QStringLiteral("has_brows"), spec.rig.hasBrows);
    return rig;
}

QJsonArray KisAiRefinementLoop::regionsToJson(const QVector<KisAiCritiqueRegion> &regions)
{
    QJsonArray arr;
    for (const KisAiCritiqueRegion &r : regions) {
        QJsonObject o;
        o.insert(QStringLiteral("area"), r.area);
        o.insert(QStringLiteral("issue"), r.issue);
        o.insert(QStringLiteral("action"), r.action);
        o.insert(QStringLiteral("priority"), r.priority);
        arr.append(o);
    }
    return arr;
}

QString KisAiRefinementLoop::formatTelemetry(const QString &category, const QString &summary)
{
    return QStringLiteral("[%1] %2").arg(category, summary);
}

bool KisAiRefinementLoop::shouldContinue(qreal psnrBefore,
                                         qreal psnrAfter,
                                         int roundsDone,
                                         int roundBudget,
                                         qreal minImprovementDb)
{
    if (roundsDone >= roundBudget)
        return false;
    return !KisAiVisionCritic::hasConverged(psnrBefore, psnrAfter, minImprovementDb);
}
