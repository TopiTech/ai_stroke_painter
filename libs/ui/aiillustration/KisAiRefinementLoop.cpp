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
        // topP は API 仕様上 (0,1] のみ有効。UI は 0.05–1.0 に制限しているが、
        // 破損設定や直接呼び出しで範囲外が来ても API エラーにしないよう clamp する。
        out.topP = qBound<qreal>(0.01, configuredTopP, 1.0);
    }
    // 負の maxTokens は未指定扱い (0) とし、異常に大きな値は上限で抑える。
    out.maxTokens = qBound(0, configuredMaxTokens, 131072);
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
    // 拒否された rig パッチは delta に記録されずフィールドは構造体既定値のまま残る。
    // raw パッチをそのまま辿ると base の調線値が既定値に上書きされて不正な再レイアウト
    // が走るため、このラウンドの拒否理由を見てスキップする。
    QStringList localRejected;
    KisAiStrokeProgram patched = KisAiProgramPatchCodec::applyPatches(base, patches, &delta, &localRejected);
    if (rejected)
        rejected->append(localRejected);
    const auto rigKeyRejected = [&localRejected](const QString &key) {
        const QString prefix = QStringLiteral("/rig/%1:").arg(key);
        for (const QString &reason : localRejected) {
            if (reason.startsWith(prefix))
                return true;
        }
        return false;
    };

    // Re-layout only when a rig key actually moved: structural ops stay
    // frozen so good regions survive (patch-only Goal steps).
    bool rigMoved = false;
    KisAiSceneSpec relaid = baseSpec;

    for (const KisAiProgramPatch &patch : patches) {
        if (!patch.path.startsWith(QLatin1String("/rig/"))) {
            continue;
        }
        const QString key = patch.path.mid(5);
        if (rigKeyRejected(key)) {
            continue;
        }
        if (key == QLatin1String("eye_aperture")) {
            relaid.rig.eyeAperture = delta.eyeAperture;
            rigMoved = true;
        } else if (key == QLatin1String("iris_ratio")) {
            relaid.rig.irisRatio = delta.irisRatio;
            rigMoved = true;
        } else if (key == QLatin1String("eye_highlight")) {
            if (!delta.eyeHighlight.isEmpty()) {
                relaid.rig.eyeHighlight = delta.eyeHighlight;
                rigMoved = true;
            }
        } else if (key == QLatin1String("double_lid")) {
            relaid.rig.doubleLid = delta.doubleLid;
            rigMoved = true;
        } else if (key == QLatin1String("hair_strand_density")) {
            relaid.rig.hairStrandDensity = delta.hairStrandDensity;
            rigMoved = true;
        } else if (key == QLatin1String("hair_flyaway")) {
            relaid.rig.hairFlyaway = delta.hairFlyaway;
            rigMoved = true;
        } else if (key == QLatin1String("hair_highlight_bands")) {
            relaid.rig.hairHighlightBands = delta.hairHighlightBands;
            rigMoved = true;
        } else if (key == QLatin1String("mouth_width_scale")) {
            relaid.rig.mouthWidthScale = delta.mouthWidthScale;
            rigMoved = true;
        } else if (key == QLatin1String("has_brows")) {
            relaid.rig.hasBrows = delta.hasBrows;
            rigMoved = true;
        }
    }

    if (rigDelta)
        *rigDelta = relaid.rig;

    if (!rigMoved)
        return patched;

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

bool KisAiRefinementLoop::canAdvanceGoalStep(int currentStep, int totalSteps, int maxExtraSteps)
{
    return qint64(currentStep) < qint64(totalSteps) + qMax(0, maxExtraSteps);
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
