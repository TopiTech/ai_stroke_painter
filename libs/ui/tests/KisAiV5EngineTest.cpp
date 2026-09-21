/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiV5EngineTest.h"

#include "aiillustration/KisAiModelRouter.h"
#include "aiillustration/KisAiProgramPatch.h"
#include "aiillustration/KisAiRigLibrary.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiVisionCritic.h"
#include "aiillustration/KisAiDeliberateStroke.h"
#include "aiillustration/KisAiLightRig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTest>

using namespace QTest;

namespace
{
KisAiSceneSpec specFromJson(const QString &json)
{
    KisAiSceneSpec spec;
    QString err;
    QStringList warnings;
    const bool ok = KisAiSceneSpecCodec::parseSceneSpec(json.toUtf8(), &spec, &err, &warnings);
    Q_ASSERT(ok);
    Q_UNUSED(ok);
    Q_UNUSED(err);
    Q_UNUSED(warnings);
    return spec;
}

KisAiCritiqueRegion makeRegion(const QString &area, const QString &issue, int priority)
{
    KisAiCritiqueRegion r;
    r.area = area;
    r.issue = issue;
    r.action = QStringLiteral("repaint");
    r.priority = priority;
    return r;
}
} // namespace

// ========================================================================
// F0: Model Router
// ========================================================================

void KisAiV5EngineTest::testModelRouterStagePlans()
{
    KisAiModelRouter::setQualityMode(KisAiModelRouter::QualityMode::Quality);

    const auto spec = KisAiModelRouter::planFor(KisAiModelRouter::Stage::SceneSpec,
                                                QStringLiteral("gpt-5"));
    QVERIFY(spec.useStructuredOutput);
    QVERIFY(!spec.model.isEmpty());
    QVERIFY(spec.nBestCandidates >= 2);

    const auto critique = KisAiModelRouter::planFor(KisAiModelRouter::Stage::VisionCritique,
                                                    QStringLiteral("gpt-5"));
    QVERIFY(critique.includeVision);
    QVERIFY(critique.temperature <= 0.3); // inspectors stay cold
    QVERIFY(critique.visionDetail == QLatin1String("high"));

    const auto patch = KisAiModelRouter::planFor(KisAiModelRouter::Stage::PatchProposal,
                                                 QString());
    QVERIFY(patch.useStructuredOutput);
    QVERIFY(patch.temperature <= 0.5);

    const auto expansion = KisAiModelRouter::planFor(KisAiModelRouter::Stage::PromptExpansion,
                                                     QStringLiteral("gpt-5"));
    QVERIFY(!expansion.useStructuredOutput);
    QVERIFY(!expansion.useJsonFormat);
    QVERIFY(!expansion.model.isEmpty());

    // Empty model must resolve to a sendable flagship model instead of "".
    const auto emptyExpansion = KisAiModelRouter::planFor(KisAiModelRouter::Stage::PromptExpansion, QString());
    QVERIFY(!emptyExpansion.model.isEmpty());
    const auto emptyGoal = KisAiModelRouter::planFor(KisAiModelRouter::Stage::GoalStep, QStringLiteral("  "));
    QVERIFY(!emptyGoal.model.isEmpty());
}

void KisAiV5EngineTest::testModelRouterQualityModes()
{
    KisAiModelRouter::setQualityMode(KisAiModelRouter::QualityMode::Fast);
    QCOMPARE(KisAiModelRouter::specCandidateCount(), 1);
    QCOMPARE(KisAiModelRouter::critiqueRoundBudget(), 1);

    KisAiModelRouter::setQualityMode(KisAiModelRouter::QualityMode::Quality);
    QCOMPARE(KisAiModelRouter::specCandidateCount(), 3);
    QCOMPARE(KisAiModelRouter::critiqueRoundBudget(), 2);

    KisAiModelRouter::setQualityMode(KisAiModelRouter::QualityMode::Max);
    QCOMPARE(KisAiModelRouter::specCandidateCount(), 5);
    QCOMPARE(KisAiModelRouter::critiqueRoundBudget(), 3);

    // Fast mode hands out a mid-tier default model when nothing is pinned.
    const auto fastPlan = KisAiModelRouter::planFor(KisAiModelRouter::Stage::SceneSpec, QString());
    QVERIFY(!fastPlan.model.isEmpty());

    KisAiModelRouter::setQualityMode(KisAiModelRouter::QualityMode::Quality);
}

void KisAiV5EngineTest::testModelRouterFallbackChainAndStrategy()
{
    const QStringList chain = KisAiModelRouter::modelFallbackChain(
        KisAiModelRouter::Stage::SceneSpec, QStringLiteral("my-model"));
    // Preferred model first, sentinel empty entry last (offline path).
    QCOMPARE(chain.first(), QLatin1String("my-model"));
    QVERIFY(chain.last().isEmpty());
    QVERIFY(chain.size() >= 3);

    // Structured-output strategy detection uses the legacy probes.
    const QString strategy = KisAiModelRouter::structuredStrategy(QStringLiteral("gpt-5"));
    QVERIFY(strategy == QLatin1String("json_schema") || strategy == QLatin1String("json_object"));

    QVERIFY(KisAiModelRouter::stageUsesVision(KisAiModelRouter::Stage::VisionCritique));
    QVERIFY(!KisAiModelRouter::stageUsesVision(KisAiModelRouter::Stage::SceneSpec));
}

// ========================================================================
// F1: SceneSpec v2
// ========================================================================

void KisAiV5EngineTest::testSceneSpecV2Parsing()
{
    const QString json = QStringLiteral(R"({
        "subject": {"type": "character", "pose_id": "three_quarter_bust", "facing": "front"},
        "head": {"expression": "smile_open", "hair_style": "bob", "hair_color": "#2b3a67"},
        "style": {"art_style": "watercolor", "custom_tags": ["windy", "school_festival"],
                   "line_weight": "delicate", "detail_level": 0.9},
        "camera": {"focal": "short", "tilt": "low_angle"},
        "color_script": {"shadow": "#243055", "midtone": "#7d8fc9", "highlight": "#f3f6ff",
                          "accent_weight": 0.4},
        "narrative": {"time": "golden hour", "weather": "rain", "props": ["lamp", "stars"]},
        "rig": {"eye_aperture": 0.95, "iris_ratio": 0.7, "eye_highlight": "streak",
                 "double_lid": false, "hair_strand_density": 0.8, "hair_flyaway": 0.5,
                 "hair_highlight_bands": 3, "mouth_width_scale": 1.2, "has_brows": true}
    })");

    const KisAiSceneSpec spec = specFromJson(json);
    QCOMPARE(spec.style.artStyleId, QLatin1String("watercolor"));
    QCOMPARE(spec.style.customTags.size(), 2);
    QCOMPARE(spec.style.lineWeight, QLatin1String("delicate"));
    QVERIFY(spec.style.detailLevel > 0.85);
    QCOMPARE(spec.camera.tilt, QLatin1String("low_angle"));
    QCOMPARE(spec.colorScript.midtone, QColor(125, 143, 201));
    QCOMPARE(spec.narrative.weather, QLatin1String("rain"));
    QVERIFY(spec.narrative.props.contains(QLatin1String("lamp")));

    // Rig overrides parsed and clamped into range.
    QVERIFY(spec.rig.eyeAperture > 0.9);
    QVERIFY(spec.rig.irisRatio > 0.65);
    QCOMPARE(spec.rig.eyeHighlight, QLatin1String("streak"));
    QVERIFY(!spec.rig.doubleLid);
    QCOMPARE(spec.rig.hairHighlightBands, 3);

    // Narrative time mapping (golden hour -> sunset).
    QCOMPARE(KisAiRigLibrary::narrativeTimeToTimeOfDay(spec.narrative.time, QStringLiteral("day")),
             QLatin1String("sunset"));
}

void KisAiV5EngineTest::testSceneSpecV2BackwardCompatible()
{
    // A pure v3 spec (no v2 blocks) must parse identically to before.
    const QString json = QStringLiteral(R"({
        "subject": {"type": "character", "pose_id": "front_bust", "facing": "front"},
        "head": {"expression": "neutral", "hair_style": "long_hime"}
    })");
    const KisAiSceneSpec spec = specFromJson(json);
    QCOMPARE(spec.head.hairStyle, QLatin1String("long_hime"));
    QCOMPARE(spec.style.artStyleId, QLatin1String("anime_cel")); // default
    QCOMPARE(spec.camera.focal, QLatin1String("normal"));        // default
    QCOMPARE(spec.rig.hairHighlightBands, 1);                    // default
    QVERIFY(spec.narrative.props.isEmpty());

    // Invalid v2 values fall back to safe defaults instead of failing.
    const QString dirty = QStringLiteral(R"({
        "style": {"art_style": "photorealistic", "detail_level": 7.5},
        "camera": {"focal": "fisheye"},
        "rig": {"eye_aperture": 12.0, "hair_highlight_bands": 99}
    })");
    const KisAiSceneSpec dirtySpec = specFromJson(dirty);
    QCOMPARE(dirtySpec.style.artStyleId, QLatin1String("anime_cel"));
    QCOMPARE(dirtySpec.camera.focal, QLatin1String("normal"));
    QCOMPARE(dirtySpec.rig.eyeAperture, 1.0);   // clamped, not rejected
    QCOMPARE(dirtySpec.rig.hairHighlightBands, 3);
}

void KisAiV5EngineTest::testSceneSpecNBestSelection()
{
    KisAiSceneSpec good = specFromJson(QStringLiteral(
        R"({"subject": {"type": "character"},
            "head": {"hair_style": "twin_tails", "expression": "smile_open"},
            "light": {"time": "day"},
            "palette": {"key": "#64748b", "accents": ["#f59e0b"]},
            "rig": {"eye_aperture": 0.85}})"));
    good.prompt = QStringLiteral("a girl with twin tails in a school uniform");

    KisAiSceneSpec bad = good;
    bad.head.hairStyle = QStringLiteral("bob");               // prompt asks twin tails
    bad.clothing.style = QStringLiteral("dress");             // prompt asks uniform
    bad.light.timeOfDay = QStringLiteral("night");            // prompt asks day
    bad.palette.accents.clear();
    bad.rig.eyeAperture = 1.0;
    bad.rig.irisRatio = 0.85;
    bad.rig.mouthWidthScale = 1.4;

    const QVector<KisAiSceneSpec> candidates = {bad, good, bad};
    const KisAiSceneSpec chosen = KisAiSceneSpecCodec::selectBestSpec(
        good.prompt, QSize(1024, 1024), candidates);
    QCOMPARE(chosen.head.hairStyle, QLatin1String("twin_tails"));

    // Empty candidate list falls back to the offline deterministic spec.
    const KisAiSceneSpec fallback = KisAiSceneSpecCodec::selectBestSpec(
        QStringLiteral("a night city scene"), QSize(1024, 1024), {});
    QCOMPARE(fallback.light.timeOfDay, QLatin1String("night"));
}

// ========================================================================
// F2: Rig DSL
// ========================================================================

void KisAiV5EngineTest::testRigParameterClamping()
{
    KisAiSceneSpec spec;
    spec.rig.eyeAperture = 42.0;
    spec.rig.irisRatio = -5.0;
    spec.rig.hairHighlightBands = 99;
    spec.rig.mouthWidthScale = 0.01;
    spec.rig.hairStrandDensity = 7.5;

    const KisAiRigParameterSet params = KisAiRigLibrary::parametersFromSpec(spec);
    QCOMPARE(params.eyeLeft.aperture, 1.0);
    QCOMPARE(params.eyeRight.aperture, 1.0);
    QCOMPARE(params.eyeLeft.irisRatio, 0.35);
    QCOMPARE(params.hair.highlightBands, 3);
    QCOMPARE(params.mouth.widthScale, 0.6);
    QCOMPARE(params.hair.strandDensity, 1.0);

    // Clamping is idempotent.
    const KisAiRigParameterSet again = KisAiRigLibrary::clamped(params);
    QCOMPARE(again.eyeLeft.aperture, params.eyeLeft.aperture);
    QCOMPARE(again.mouth.widthScale, params.mouth.widthScale);
}

void KisAiV5EngineTest::testRigEyePairSymmetryByConstruction()
{
    KisAiSceneSpec spec;
    spec.rig.eyeAperture = 0.9;
    const KisAiRigParameterSet params = KisAiRigLibrary::parametersFromSpec(spec);
    const QVector<KisAiStrokeOperation> ops = KisAiRigLibrary::eyePairOps(params);

    int eyeCount = 0;
    QPointF leftCenter, rightCenter;
    QSizeF leftSize, rightSize;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
            if (op.eyeIsRight) {
                rightCenter = op.eyeCenter;
                rightSize = op.eyeSize;
            } else {
                leftCenter = op.eyeCenter;
                leftSize = op.eyeSize;
            }
            ++eyeCount;
        }
    }
    QCOMPARE(eyeCount, 2);

    // Mirrored around the head axis: x-center average equals head center,
    // identical sizes, same height.
    const qreal midX = (leftCenter.x() + rightCenter.x()) / 2.0;
    QVERIFY(qAbs(midX - params.headCenter.x()) < 1e-9);
    QCOMPARE(leftCenter.y(), rightCenter.y());
    QVERIFY(qAbs(leftSize.width() - rightSize.width()) < 1e-9);
    QVERIFY(qAbs(leftSize.height() - rightSize.height()) < 1e-9);

    // Both eyes inside the head box.
    const KisAiVisionCritic critic; // unused; keep include honest
    Q_UNUSED(critic);
    QVERIFY(leftCenter.x() > params.headCenter.x() - params.headWidth);
    QVERIFY(rightCenter.x() < params.headCenter.x() + params.headWidth);
}

void KisAiV5EngineTest::testRigFacePartsGenerated()
{
    KisAiSceneSpec spec;
    const KisAiRigParameterSet params = KisAiRigLibrary::parametersFromSpec(spec);

    const QVector<KisAiStrokeOperation> brows = KisAiRigLibrary::browOps(params);
    QCOMPARE(brows.size(), 2); // mirrored pair
    QVERIFY(brows.first().id.startsWith(QLatin1String("rig_brow_")));

    const QVector<KisAiStrokeOperation> nose = KisAiRigLibrary::noseOps(params);
    QVERIFY(!nose.isEmpty());
    // Nose colors derive from skin: never pure black (lint contract).
    for (const KisAiStrokeOperation &op : nose) {
        QVERIFY(op.brush.color.value() > 60);
    }

    const QVector<KisAiStrokeOperation> mouth = KisAiRigLibrary::mouthOps(params);
    QCOMPARE(mouth.size(), 1);
    QCOMPARE(mouth.first().kind, KisAiStrokeOperation::Kind::AnimeMouth);

    // Disabling brows yields none.
    KisAiRigParameterSet noBrows = params;
    noBrows.brow.enabled = false;
    QVERIFY(KisAiRigLibrary::browOps(noBrows).isEmpty());

    // Hair highlight bands follow the band count.
    KisAiRigParameterSet threeBands = params;
    threeBands.hair.highlightBands = 3;
    QCOMPARE(KisAiRigLibrary::hairHighlightOps(threeBands).size(), 3);
    KisAiRigParameterSet zeroBands = params;
    zeroBands.hair.highlightBands = 0;
    QVERIFY(KisAiRigLibrary::hairHighlightOps(zeroBands).isEmpty());

    // Double-lid creases appear when enabled and vanish when disabled.
    KisAiRigParameterSet lids = params;
    lids.eyeLeft.doubleLid = true;
    lids.eyeRight.doubleLid = true;
    QCOMPARE(KisAiRigLibrary::doubleLidOps(lids).size(), 2);
    // Asymmetric lid test: left only enabled
    lids.eyeLeft.doubleLid = true;
    lids.eyeRight.doubleLid = false;
    const QVector<KisAiStrokeOperation> leftOnly = KisAiRigLibrary::doubleLidOps(lids);
    QCOMPARE(leftOnly.size(), 1);
    QCOMPARE(leftOnly.first().id, QStringLiteral("rig_eye_l_lid"));
    lids.eyeLeft.doubleLid = false;
    lids.eyeRight.doubleLid = false;
    QVERIFY(KisAiRigLibrary::doubleLidOps(lids).isEmpty());
}

void KisAiV5EngineTest::testRigBackdropWeatherFaceGuard()
{
    KisAiSceneSpec spec;
    spec.narrative.weather = QStringLiteral("snow");
    spec.narrative.props = QStringList{QStringLiteral("stars"), QStringLiteral("lamp")};
    const KisAiRigParameterSet params = KisAiRigLibrary::parametersFromSpec(spec);

    const QVector<KisAiStrokeOperation> ops =
        KisAiRigLibrary::backdropWeatherOps(params, QSize(1024, 1024));

    bool hasSnow = false;
    bool hasStars = false;
    const QRectF faceBox = KisAiVisionCritic::faceBoxFromAnchor(
        params.headCenter, params.headWidth, params.headHeight);

    for (const KisAiStrokeOperation &op : ops) {
        if (op.id == QLatin1String("rig_weather_snow"))
            hasSnow = true;
        if (op.id == QLatin1String("rig_prop_stars")) {
            hasStars = true;
            // Sky band only.
            QVERIFY(op.bounds.bottom() <= 0.42 + 1e-9);
        }
        // Particle effects never intersect the face box.
        if (op.kind == KisAiStrokeOperation::Kind::Particles && op.bounds.width() > 0) {
            QVERIFY2(!op.bounds.intersects(faceBox),
                     qPrintable(QStringLiteral("particles overlap face: %1").arg(op.id)));
        }
    }
    QVERIFY(hasSnow);
    QVERIFY(hasStars);
}

// ========================================================================
// F3a: Program Patch
// ========================================================================

void KisAiV5EngineTest::testPatchParseAndApplyRig()
{
    const QByteArray body = QByteArrayLiteral(
        "{\"patches\": ["
        " {\"op\": \"replace\", \"path\": \"/rig/eye_aperture\", \"value\": 0.5},"
        " {\"op\": \"replace\", \"path\": \"/rig/hair_highlight_bands\", \"value\": 2},"
        " {\"op\": \"replace\", \"path\": \"/rig/unknown_key\", \"value\": 1},"
        " {\"op\": \"replace\", \"path\": \"/lineart\", \"value\": true}"
        " ]}");

    QVector<KisAiProgramPatch> patches;
    QStringList rejected;
    QString err;
    QVERIFY(KisAiProgramPatchCodec::parsePatches(body, &patches, &rejected, &err));
    QCOMPARE(patches.size(), 2);     // unknown path + structural path rejected
    QCOMPARE(rejected.size(), 2);

    // Bare array format tolerated
    const QByteArray bareArrayBody = QByteArrayLiteral(
        "["
        " {\"op\": \"replace\", \"path\": \"/rig/eye_aperture\", \"value\": 0.5}"
        "]");
    QVector<KisAiProgramPatch> barePatches;
    QString bareErr;
    QVERIFY(KisAiProgramPatchCodec::parsePatches(bareArrayBody, &barePatches, nullptr, &bareErr));
    QCOMPARE(barePatches.size(), 1);
    QCOMPARE(barePatches.first().path, QStringLiteral("/rig/eye_aperture"));

    KisAiStrokeProgram program;
    KisAiStrokeOperation base;
    base.kind = KisAiStrokeOperation::Kind::Path;
    base.id = QStringLiteral("line_1");
    base.layer = QStringLiteral("Lineart");
    program.operations.append(base);

    KisAiSceneRigOverrides rigDelta;
    const KisAiStrokeProgram applied =
        KisAiProgramPatchCodec::applyPatches(program, patches, &rigDelta, &rejected);
    QCOMPARE(rigDelta.eyeAperture, 0.5);
    QCOMPARE(rigDelta.hairHighlightBands, 2);
    QCOMPARE(applied.operations.size(), 1); // structure untouched
}

void KisAiV5EngineTest::testPatchWhitelistRejectsStructureChanges()
{
    KisAiStrokeProgram program;
    KisAiStrokeOperation flat;
    flat.kind = KisAiStrokeOperation::Kind::Fill;
    flat.id = QStringLiteral("skin_flats");
    flat.layer = QStringLiteral("Flats");
    flat.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1) << QPointF(0.5, 0.9);
    program.operations.append(flat);

    QVector<KisAiProgramPatch> patches;
    KisAiProgramPatch remove;
    remove.op = KisAiProgramPatch::Op::Remove;
    remove.path = QStringLiteral("/ops/skin_flats/remove");
    patches.append(remove);

    KisAiProgramPatch shrink;
    shrink.op = KisAiProgramPatch::Op::Replace;
    shrink.path = QStringLiteral("/ops/skin_flats/brush/size");
    shrink.value = 0.001;
    patches.append(shrink);

    QStringList rejected;
    const KisAiStrokeProgram applied =
        KisAiProgramPatchCodec::applyPatches(program, patches, nullptr, &rejected);
    // Structural Flats op survives: geometry is frozen.
    QCOMPARE(applied.operations.size(), 1);
    QVERIFY(!rejected.isEmpty());

    // Brush value adjustments on existing ops are allowed.
    QVector<KisAiProgramPatch> opacityPatch;
    KisAiProgramPatch p;
    p.op = KisAiProgramPatch::Op::Replace;
    p.path = QStringLiteral("/ops/skin_flats/brush/opacity");
    p.value = 0.5;
    opacityPatch.append(p);
    const KisAiStrokeProgram toned =
        KisAiProgramPatchCodec::applyPatches(program, opacityPatch, nullptr, nullptr);
    QCOMPARE(toned.operations.first().brush.opacity, 0.5);
}

void KisAiV5EngineTest::testPatchDecorativeAddRemove()
{
    KisAiStrokeProgram program;
    const QByteArray body = QByteArrayLiteral(
        "{\"patches\": ["
        " {\"op\": \"add\", \"path\": \"/ops/add\", \"value\": {\"kind\": \"particles\", \"id\": \"fx_sparkle\","
        "   \"layer\": \"FX\", \"particle_shape\": \"sparkle\", \"count\": 10,"
        "   \"bounds\": {\"x\": 0.6, \"y\": 0.05, \"w\": 0.3, \"h\": 0.25}, \"color\": \"#ffe9a8\", \"opacity\": 0.6}},"
        " {\"op\": \"add\", \"path\": \"/ops/add\", \"value\": {\"kind\": \"path\", \"id\": \"rogue_line\","
        "   \"layer\": \"Lineart\"}},"
        " {\"op\": \"add\", \"path\": \"/ops/add\", \"value\": {\"kind\": \"particles\", \"id\": \"evil_flats\","
        "   \"layer\": \"Flats\", \"bounds\": {\"x\": 0, \"y\": 0, \"w\": 1, \"h\": 1}}}"
        " ]}");

    QVector<KisAiProgramPatch> patches;
    QStringList rejected;
    QVERIFY(KisAiProgramPatchCodec::parsePatches(body, &patches, &rejected));
    // Parse-level validation accepts the three /ops/add shapes; kind/layer
    // whitelisting happens at apply time where the full op is inspected.
    QCOMPARE(patches.size(), 3);

    const KisAiStrokeProgram applied =
        KisAiProgramPatchCodec::applyPatches(program, patches, nullptr, &rejected);
    // Only the FX particles op survives: path kind + Flats layer rejected.
    QCOMPARE(applied.operations.size(), 1);
    QCOMPARE(applied.operations.first().id, QLatin1String("fx_sparkle"));
    QCOMPARE(applied.operations.first().particleCount, 10);
    QCOMPARE(rejected.size(), 2);

    // Round trip through JSON (all parsed patches serialize).
    const QJsonArray arr = KisAiProgramPatchCodec::patchesToJson(patches);
    QCOMPARE(arr.size(), 3);

    // Remove the decorative op again.
    QVector<KisAiProgramPatch> removePatches;
    KisAiProgramPatch rm;
    rm.op = KisAiProgramPatch::Op::Remove;
    rm.path = QStringLiteral("/ops/fx_sparkle/remove");
    removePatches.append(rm);
    const KisAiStrokeProgram cleaned =
        KisAiProgramPatchCodec::applyPatches(applied, removePatches, nullptr, nullptr);
    QVERIFY(cleaned.operations.isEmpty());
}

// ========================================================================
// F3b: Vision Critic
// ========================================================================

void KisAiV5EngineTest::testCriticCropSelectionDeterministic()
{
    QImage canvas(256, 256, QImage::Format_ARGB32);
    canvas.fill(QColor(240, 240, 245));
    // High-contrast stripe in the bottom-right tile.
    QPainter p(&canvas);
    p.fillRect(200, 200, 56, 56, QColor(20, 20, 30));
    p.end();

    const QRectF faceBox(0.35, 0.2, 0.3, 0.4);
    QVector<KisAiCritiqueRegion> prior;
    prior.append(makeRegion(QStringLiteral("hair"), QStringLiteral("banding"), 5));

    const QVector<KisAiCriticCrop> crops1 =
        KisAiVisionCritic::selectCrops(canvas, faceBox, prior, 4);
    const QVector<KisAiCriticCrop> crops2 =
        KisAiVisionCritic::selectCrops(canvas, faceBox, prior, 4);
    // Deterministic across calls.
    QCOMPARE(crops1.size(), crops2.size());
    QVERIFY(crops1.size() >= 2); // face + detail at minimum
    for (int i = 0; i < crops1.size(); ++i) {
        QCOMPARE(crops1.at(i).region, crops2.at(i).region);
        QCOMPARE(crops1.at(i).reason, crops2.at(i).reason);
    }
    QCOMPARE(crops1.first().reason, QLatin1String("face"));
    QVERIFY(!crops1.first().image.isNull());

    // Crop budget respected.
    QCOMPARE(KisAiVisionCritic::selectCrops(canvas, faceBox, prior, 2).size(), 2);

    // Full-canvas critique images are downscaled before JPEG/base64 encoding so a
    // 4k canvas does not become multi-MB per round.
    QImage bigCanvas(2048, 1536, QImage::Format_ARGB32);
    bigCanvas.fill(QColor(200, 210, 225));
    const QJsonObject bigPayload = KisAiVisionCritic::buildCritiquePayload(
        QStringLiteral("gpt-4o"), QStringLiteral("lake"), bigCanvas, {}, KisAiLightSettings());
    const QJsonArray bigMessages = bigPayload.value(QStringLiteral("messages")).toArray();
    QVERIFY(bigMessages.size() == 2);
    const QJsonArray bigContent = bigMessages.at(1).toObject().value(QStringLiteral("content")).toArray();
    QVERIFY(!bigContent.isEmpty());
    const QString bigUrl = bigContent.at(1).toObject().value(QStringLiteral("image_url")).toObject().value(
        QStringLiteral("url")).toString();
    QVERIFY(bigUrl.startsWith(QLatin1String("data:image/jpeg;base64,")));
    // 2048px wide at JPEG82 is ~100-300KB raw; allow generous headroom but fail
    // if the encoder ever ships near-full-resolution megabytes again.
    QVERIFY2(bigUrl.size() < 900000, qPrintable(QString::number(bigUrl.size())));

    // Safety regression tests: null canvas and non-32-bit image formats
    QVERIFY(KisAiVisionCritic::selectCrops(QImage(), faceBox, prior, 4).isEmpty());
    const QImage rgb888Canvas = canvas.convertToFormat(QImage::Format_RGB888);
    const QVector<KisAiCriticCrop> rgb888Crops = KisAiVisionCritic::selectCrops(rgb888Canvas, faceBox, prior, 4);
    QCOMPARE(rgb888Crops.size(), crops1.size());

    const QImage monoCanvas = canvas.convertToFormat(QImage::Format_Mono);
    const QVector<KisAiCriticCrop> monoCrops = KisAiVisionCritic::selectCrops(monoCanvas, faceBox, prior, 4);
    QCOMPARE(monoCrops.size(), crops1.size());
}

void KisAiV5EngineTest::testCritiqueParseAndMerge()
{
    const QByteArray body = QByteArrayLiteral(
        "{\"readiness_score\": 0.7, \"regions\": ["
        " {\"area\": \"left_eye\", \"issue\": \"iris clipped by lid\", \"action\": \"repaint\", \"priority\": 4},"
        " {\"area\": \"skin\", \"issue\": \"smudge on cheek\", \"action\": \"remove\", \"priority\": 3},"
        " {\"area\": \"made_up_area\", \"issue\": \"hallucinated\", \"action\": \"keep\", \"priority\": 5},"
        " {\"area\": \"hair\", \"issue\": \"\", \"action\": \"soften\", \"priority\": 2}"
        " ]}");

    QVector<KisAiCritiqueRegion> regions;
    qreal readiness = 1.0;
    QString err;
    QVERIFY(KisAiVisionCritic::parseCritiqueResponse(body, &regions, &readiness, &err));
    QCOMPARE(readiness, 0.7);
    QCOMPARE(regions.size(), 2); // alias repair keeps "skin"; unknown area and empty issue dropped
    QCOMPARE(regions.first().area, QLatin1String("left_eye"));
    QCOMPARE(regions.last().area, QLatin1String("face_skin"));

    // Merge keeps highest priority per (area, issue).
    QVector<KisAiCritiqueRegion> accumulated;
    accumulated.append(makeRegion(QStringLiteral("hair"), QStringLiteral("banding"), 2));
    const QVector<KisAiCritiqueRegion> merged = KisAiVisionCritic::mergeRegions(
        accumulated, QVector<KisAiCritiqueRegion>{
            makeRegion(QStringLiteral("hair"), QStringLiteral("banding"), 4),
            makeRegion(QStringLiteral("fx"), QStringLiteral("particles on face"), 5)});
    QCOMPARE(merged.size(), 2);
    QVERIFY(merged.first().priority == 4);
}

void KisAiV5EngineTest::testPsnrAndConvergence()
{
    QImage a(64, 64, QImage::Format_ARGB32);
    a.fill(QColor(100, 120, 160));
    QImage b = a.copy();
    QVERIFY(KisAiVisionCritic::psnr(a, b) >= 59.0); // identical

    QImage c = a.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&c);
    p.fillRect(0, 0, 32, 32, QColor(250, 240, 220));
    p.end();
    const qreal noisy = KisAiVisionCritic::psnr(a, c);
    QVERIFY(noisy > 0.0 && noisy < 60.0);

    QVERIFY(!KisAiVisionCritic::hasConverged(10.0, 15.0)); // improving
    QVERIFY(KisAiVisionCritic::hasConverged(10.0, 11.0));  // stalled

    // Alpha channel sensitivity: transparent canvas vs solid black must differ significantly
    QImage transparentImg(64, 64, QImage::Format_ARGB32);
    transparentImg.fill(QColor(0, 0, 0, 0));
    QImage solidBlackImg(64, 64, QImage::Format_ARGB32);
    solidBlackImg.fill(QColor(0, 0, 0, 255));
    const qreal alphaDiffPsnr = KisAiVisionCritic::psnr(transparentImg, solidBlackImg);
    QVERIFY(alphaDiffPsnr < 15.0); // Major difference detected via alpha channel, not falsely 60.0 dB
}

// ========================================================================
// F4: LightRig LUT + ink dynamics
// ========================================================================

void KisAiV5EngineTest::testTimeOfDayLutConsistency()
{
    const auto day = KisAiLightRig::timeOfDayLut(QStringLiteral("day"));
    const auto sunset = KisAiLightRig::timeOfDayLut(QStringLiteral("sunset"));
    const auto night = KisAiLightRig::timeOfDayLut(QStringLiteral("night"));

    // Night sky must be darker than day sky at every stop.
    QVERIFY(night.skyTop.value() < day.skyTop.value());
    QVERIFY(night.skyMid.value() < day.skyMid.value());
    QVERIFY(night.skyBottom.value() < day.skyBottom.value());
    // Sunset sky is warmer than day sky (red channel dominance).
    QVERIFY(sunset.skyMid.red() > day.skyMid.red());

    // Fill tint derives per rig: night fill is cool (blue dominant).
    QVERIFY(night.fillTint.blue() > night.fillTint.red());

    // Invalid input falls back to day.
    const auto fallback = KisAiLightRig::timeOfDayLut(QStringLiteral("golden_hour"));
    QCOMPARE(fallback.skyTop, day.skyTop);
}

void KisAiV5EngineTest::testFormAndBounceLayers()
{
    KisAiSceneSpec spec;
    const KisAiLightSettings rig = KisAiLightRig::fromSpec(spec);

    KisAiStrokeOperation mass;
    mass.kind = KisAiStrokeOperation::Kind::Fill;
    mass.id = QStringLiteral("torso_flats");
    mass.layer = QStringLiteral("Flats");
    mass.brush.color = QColor(64, 72, 90);
    mass.polygon = QPolygonF() << QPointF(0.2, 0.5) << QPointF(0.8, 0.5)
                               << QPointF(0.8, 0.95) << QPointF(0.2, 0.95);

    const QVector<KisAiStrokeOperation> form =
        KisAiLightRig::synthesizeFormShading({mass}, rig, QSize(1024, 1024));
    QVERIFY(!form.isEmpty());
    for (const KisAiStrokeOperation &op : form) {
        QCOMPARE(op.layer, QLatin1String("Shading"));
        QCOMPARE(op.blendMode, QLatin1String("multiply"));
        QCOMPARE(op.clipToId, QLatin1String("torso_flats"));
        QVERIFY(op.id.endsWith(QLatin1String("_form_shadow")));
        // Softer than the core pass (0.22/0.38): a whisper.
        QVERIFY(op.brush.opacity <= 0.2);
    }

    const QVector<KisAiStrokeOperation> bounce =
        KisAiLightRig::synthesizeBounceLight({mass}, rig, QSize(1024, 1024));
    QVERIFY(!bounce.isEmpty());
    for (const KisAiStrokeOperation &op : bounce) {
        QCOMPARE(op.blendMode, QLatin1String("screen"));
        QVERIFY(op.id.endsWith(QLatin1String("_bounce_light")));
        QVERIFY(op.brush.opacity <= 0.25);
    }

    // Tiny masses earn neither layer.
    KisAiStrokeOperation micro = mass;
    micro.id = QStringLiteral("micro");
    micro.polygon = QPolygonF() << QPointF(0.4, 0.4) << QPointF(0.401, 0.4) << QPointF(0.4, 0.401);
    QVERIFY(KisAiLightRig::synthesizeFormShading({micro}, rig, QSize(1024, 1024)).isEmpty());
}

void KisAiV5EngineTest::testInkDynamicsPoolingAndFade()
{
    const QSize canvas(1024, 1024);

    // A slow pass: dense samples over a short span (small spacing).
    QVector<KisAiStrokePoint> slow;
    for (int i = 0; i <= 30; ++i)
        slow.append(KisAiStrokePoint(0.3 + 0.02 * i / 30.0, 0.5, 0.8));
    const QVector<KisAiStrokePoint> slowOut =
        KisAiDeliberateStroke::applyInkDynamics(slow, canvas);
    QCOMPARE(slowOut.size(), slow.size());
    // Midpoint pressure pooled above the input 0.8.
    QVERIFY(slowOut.at(15).pressure > 0.8);

    // A fast pass: sparse samples over a long span (large spacing) —
    // the pen flew, so ink dries out (fades toward the floor).
    QVector<KisAiStrokePoint> fast;
    fast.append(KisAiStrokePoint(0.1, 0.5, 0.8));
    fast.append(KisAiStrokePoint(0.5, 0.5, 0.8));
    fast.append(KisAiStrokePoint(0.9, 0.5, 0.8));
    const QVector<KisAiStrokePoint> fastOut =
        KisAiDeliberateStroke::applyInkDynamics(fast, canvas);
    QCOMPARE(fastOut.size(), fast.size());
    QVERIFY(fastOut.at(1).pressure < 0.8);        // faded
    QVERIFY(fastOut.at(1).pressure >= 0.55);      // floor respected

    // Neutral zone: spacing near the renderer cadence (~4px) keeps pressure.
    QVector<KisAiStrokePoint> neutral;
    for (int i = 0; i <= 20; ++i)
        neutral.append(KisAiStrokePoint(0.2 + 4.0 * i / 1024.0, 0.5, 0.8));
    const QVector<KisAiStrokePoint> neutralOut =
        KisAiDeliberateStroke::applyInkDynamics(neutral, canvas);
    QCOMPARE(neutralOut.at(10).pressure, 0.8);

    // Mixed: slow head, fast tail -> pressure gradient across the stroke.
    QVector<KisAiStrokePoint> mixed;
    for (int i = 0; i <= 20; ++i)
        mixed.append(KisAiStrokePoint(0.1 + 0.05 * i / 20.0, 0.5, 0.8));
    for (int i = 0; i < 5; ++i)
        mixed.append(KisAiStrokePoint(0.15 + 0.2 * (i + 1), 0.5, 0.8));
    const QVector<KisAiStrokePoint> mixedOut =
        KisAiDeliberateStroke::applyInkDynamics(mixed, canvas);
    QVERIFY(mixedOut.first().pressure >= mixedOut.last().pressure);

    // Degenerate input passes through untouched.
    QVector<KisAiStrokePoint> single;
    single.append(KisAiStrokePoint(0.5, 0.5, 0.8));
    QCOMPARE(KisAiDeliberateStroke::applyInkDynamics(single, canvas).size(), 1);
}

QTEST_MAIN(KisAiV5EngineTest)
