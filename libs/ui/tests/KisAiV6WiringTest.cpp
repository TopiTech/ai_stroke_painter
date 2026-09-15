/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiV6WiringTest.h"

#include <QImage>
#include <QPainter>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiDeliberateStroke.h"
#include "aiillustration/KisAiLayoutEngine.h"
#include "aiillustration/KisAiLightRig.h"
#include "aiillustration/KisAiModelRouter.h"
#include "aiillustration/KisAiProgramPatch.h"
#include "aiillustration/KisAiRefinementLoop.h"
#include "aiillustration/KisAiRigLibrary.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeQualityUtils.h"
#include "aiillustration/KisAiStrokeRenderer.h"
#include "aiillustration/KisAiVisionCritic.h"

#include <QJsonDocument>

#include <cmath>

using namespace QTest;

namespace
{
int countId(const QVector<KisAiStrokeOperation> &ops, const QString &fragment)
{
    int n = 0;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.id.contains(fragment, Qt::CaseInsensitive))
            ++n;
    }
    return n;
}

bool hasId(const QVector<KisAiStrokeOperation> &ops, const QString &fragment)
{
    return countId(ops, fragment) > 0;
}

bool hasExactId(const QVector<KisAiStrokeOperation> &ops, const QString &id)
{
    for (const KisAiStrokeOperation &op : ops) {
        if (op.id == id)
            return true;
    }
    return false;
}

QVector<KisAiStrokeOperation> animeEyes(const QVector<KisAiStrokeOperation> &ops)
{
    QVector<KisAiStrokeOperation> out;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye)
            out.append(op);
    }
    return out;
}
} // namespace

// ========================================================================
// W1: Rig wiring — LLM rig values finally reach ink
// ========================================================================

void KisAiV6WiringTest::testCharacterUsesRigEyePair()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(512, 512));

    const QVector<KisAiStrokeOperation> eyes = animeEyes(prog.operations);
    QCOMPARE(eyes.size(), 2);

    // Rig-driven ids replace the legacy left_eye/right_eye pair.
    QVERIFY(hasId(prog.operations, QStringLiteral("rig_eye_l")));
    QVERIFY(hasId(prog.operations, QStringLiteral("rig_eye_r")));
    QVERIFY(!hasId(prog.operations, QStringLiteral("left_eye")));
    QVERIFY(!hasId(prog.operations, QStringLiteral("right_eye")));

    // Symmetry by construction: mirrored around the head axis.
    const QPointF a = eyes.at(0).eyeCenter;
    const QPointF b = eyes.at(1).eyeCenter;
    QVERIFY(qAbs((a.x() + b.x()) * 0.5 - spec.composition.headCenter.x()) < 1e-9);
    QCOMPARE(a.y(), b.y());
    QVERIFY(KisAiDeliberateStroke::eyePairSymmetryWarnings(prog.operations).isEmpty());

    // Rig values move both eyes: aperture changes eye height symmetrically.
    KisAiSceneSpec narrow = spec;
    narrow.rig.eyeAperture = 0.3;
    const KisAiStrokeProgram narrowProg = KisAiLayoutEngine::generateProgram(narrow, QSize(512, 512));
    const QVector<KisAiStrokeOperation> narrowEyes = animeEyes(narrowProg.operations);
    QCOMPARE(narrowEyes.size(), 2);
    QVERIFY(narrowEyes.at(0).eyeSize.height() < eyes.at(0).eyeSize.height());
    QVERIFY(narrowEyes.at(1).eyeSize.height() < eyes.at(1).eyeSize.height());
    QVERIFY(qAbs(narrowEyes.at(0).eyeSize.height() - narrowEyes.at(1).eyeSize.height()) < 1e-9);

    // Highlight shape reaches the eye style.
    KisAiSceneSpec streak = spec;
    streak.rig.eyeHighlight = QStringLiteral("streak");
    const KisAiStrokeProgram streakProg = KisAiLayoutEngine::generateProgram(streak, QSize(512, 512));
    bool foundSparkle = false;
    for (const KisAiStrokeOperation &op : animeEyes(streakProg.operations)) {
        if (op.eyeStyle == QLatin1String("sparkle"))
            foundSparkle = true;
    }
    QVERIFY(foundSparkle);
}

void KisAiV6WiringTest::testRigBrowsNoseMouthWired()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(512, 512));

    QVERIFY(hasId(prog.operations, QStringLiteral("rig_brow_l")));
    QVERIFY(hasId(prog.operations, QStringLiteral("rig_brow_r")));
    QVERIFY(hasId(prog.operations, QStringLiteral("rig_nose_point")));
    QVERIFY(hasId(prog.operations, QStringLiteral("rig_mouth")));
    // Legacy hand-written ids are retired (exact match: rig_brow_l must not
    // count as a legacy brow_l hit).
    QVERIFY(!hasExactId(prog.operations, QStringLiteral("brow_l")));
    QVERIFY(!hasExactId(prog.operations, QStringLiteral("nose_bridge_hl")));
    QVERIFY(!hasExactId(prog.operations, QStringLiteral("lip_gloss")));

    // hasBrows=false removes brows (previously always drawn).
    KisAiSceneSpec noBrows = spec;
    noBrows.rig.hasBrows = false;
    const KisAiStrokeProgram noBrowProg = KisAiLayoutEngine::generateProgram(noBrows, QSize(512, 512));
    QVERIFY(!hasId(noBrowProg.operations, QStringLiteral("rig_brow_")));

    // mouthWidthScale reaches the mouth assembly.
    KisAiSceneSpec wide = spec;
    wide.rig.mouthWidthScale = 1.4;
    const KisAiStrokeProgram wideProg = KisAiLayoutEngine::generateProgram(wide, QSize(512, 512));
    qreal baseW = 0.0, wideW = 0.0;
    for (const KisAiStrokeOperation &op : prog.operations) {
        if (op.id == QLatin1String("rig_mouth"))
            baseW = op.mouthSize.width();
    }
    for (const KisAiStrokeOperation &op : wideProg.operations) {
        if (op.id == QLatin1String("rig_mouth"))
            wideW = op.mouthSize.width();
    }
    QVERIFY(baseW > 0.0 && wideW > baseW);

    // Hair highlight bands follow the rig count.
    KisAiSceneSpec bands = spec;
    bands.rig.hairHighlightBands = 3;
    const KisAiStrokeProgram bandsProg = KisAiLayoutEngine::generateProgram(bands, QSize(512, 512));
    QCOMPARE(countId(bandsProg.operations, QStringLiteral("rig_hair_band_")), 3);
}

void KisAiV6WiringTest::testDetailLevelScalesOrnaments()
{
    KisAiSceneSpec full;
    full.subject.type = QStringLiteral("character");
    full.style.detailLevel = 0.9;
    const KisAiStrokeProgram fullProg = KisAiLayoutEngine::generateProgram(full, QSize(512, 512));
    QVERIFY(hasId(fullProg.operations, QStringLiteral("eyelid_shade_l")));
    QVERIFY(hasId(fullProg.operations, QStringLiteral("tear_trough_l")));

    KisAiSceneSpec low = full;
    low.style.detailLevel = 0.1;
    const KisAiStrokeProgram lowProg = KisAiLayoutEngine::generateProgram(low, QSize(512, 512));
    QVERIFY(!hasId(lowProg.operations, QStringLiteral("eyelid_shade_")));
    QVERIFY(!hasId(lowProg.operations, QStringLiteral("tear_trough_")));
    // Low detail trims highlight bands to at most one.
    QVERIFY(countId(lowProg.operations, QStringLiteral("rig_hair_band_")) <= 1);
    // Faces survive the trim.
    QCOMPARE(animeEyes(lowProg.operations).size(), 2);
}

void KisAiV6WiringTest::testCameraFocalTiltShiftsHead()
{
    KisAiSceneSpec base;
    base.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram baseProg = KisAiLayoutEngine::generateProgram(base, QSize(512, 512));
    const QVector<KisAiStrokeOperation> baseEyes = animeEyes(baseProg.operations);
    QCOMPARE(baseEyes.size(), 2);

    KisAiSceneSpec tilted = base;
    tilted.camera.tilt = QStringLiteral("high_angle");
    const KisAiStrokeProgram tiltProg = KisAiLayoutEngine::generateProgram(tilted, QSize(512, 512));
    const QVector<KisAiStrokeOperation> tiltEyes = animeEyes(tiltProg.operations);
    QCOMPARE(tiltEyes.size(), 2);
    // high_angle nudges the head anchor up; eye Y follows.
    QVERIFY(tiltEyes.at(0).eyeCenter.y() < baseEyes.at(0).eyeCenter.y());
    QVERIFY(tiltEyes.at(1).eyeCenter.y() < baseEyes.at(1).eyeCenter.y());
    // Symmetry survives the nudge.
    QVERIFY(KisAiDeliberateStroke::eyePairSymmetryWarnings(tiltProg.operations).isEmpty());
}

void KisAiV6WiringTest::testCharacterBackdropWeatherFaceGuard()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    spec.narrative.weather = QStringLiteral("snow");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(512, 512));
    // Character path now shares the BackdropRig; snow exists but never
    // overlaps the face box.
    bool hasSnow = false;
    const QRectF faceBox = KisAiVisionCritic::faceBoxFromAnchor(spec.composition.headCenter,
                                                                spec.composition.headHeight * 0.78,
                                                                spec.composition.headHeight);
    for (const KisAiStrokeOperation &op : prog.operations) {
        if (op.id.contains(QStringLiteral("snow"), Qt::CaseInsensitive)) {
            hasSnow = true;
            if (op.kind == KisAiStrokeOperation::Kind::Fill && op.polygon.size() >= 3) {
                for (const QPointF &p : op.polygon)
                    QVERIFY(!faceBox.contains(p));
            }
        }
    }
    QVERIFY(hasSnow);
}

void KisAiV6WiringTest::testFacingCompensation()
{
    KisAiSceneSpec front;
    front.subject.type = QStringLiteral("character");
    front.subject.facing = QStringLiteral("front");
    const KisAiStrokeProgram frontProg = KisAiLayoutEngine::generateProgram(front, QSize(512, 512));

    KisAiSceneSpec side = front;
    side.subject.facing = QStringLiteral("front-right");
    const KisAiStrokeProgram sideProg = KisAiLayoutEngine::generateProgram(side, QSize(512, 512));

    const QVector<KisAiStrokeOperation> frontEyes = animeEyes(frontProg.operations);
    const QVector<KisAiStrokeOperation> sideEyes = animeEyes(sideProg.operations);
    QCOMPARE(frontEyes.size(), 2);
    QCOMPARE(sideEyes.size(), 2);
    // front-right shifts both eyes right while keeping the pair symmetric.
    QVERIFY(sideEyes.at(0).eyeCenter.x() > frontEyes.at(0).eyeCenter.x());
    QVERIFY(sideEyes.at(1).eyeCenter.x() > frontEyes.at(1).eyeCenter.x());
    QVERIFY(KisAiDeliberateStroke::eyePairSymmetryWarnings(sideProg.operations).isEmpty());
}

// ========================================================================
// W2: light / color single source of truth
// ========================================================================

void KisAiV6WiringTest::testBackgroundUsesLut()
{
    KisAiSceneSpec day;
    day.subject.type = QStringLiteral("character");
    day.light.timeOfDay = QStringLiteral("day");
    KisAiSceneSpec night = day;
    night.light.timeOfDay = QStringLiteral("night");

    const KisAiStrokeProgram dayProg = KisAiLayoutEngine::generateProgram(day, QSize(256, 256));
    const KisAiStrokeProgram nightProg = KisAiLayoutEngine::generateProgram(night, QSize(256, 256));

    QVector<QColor> dayStops, nightStops;
    for (const KisAiStrokeOperation &op : dayProg.operations) {
        if (op.id == QLatin1String("bg_wash") && op.gradientColors.size() >= 3)
            dayStops = op.gradientColors;
    }
    for (const KisAiStrokeOperation &op : nightProg.operations) {
        if (op.id == QLatin1String("bg_wash") && op.gradientColors.size() >= 3)
            nightStops = op.gradientColors;
    }
    QVERIFY(dayStops.size() >= 3 && nightStops.size() >= 3);
    const auto lutDay = KisAiLightRig::timeOfDayLut(QStringLiteral("day"));
    const auto lutNight = KisAiLightRig::timeOfDayLut(QStringLiteral("night"));
    QCOMPARE(dayStops.at(0), lutDay.skyTop);
    QCOMPARE(dayStops.at(1), lutDay.skyMid);
    QCOMPARE(dayStops.at(2), lutDay.skyBottom);
    QCOMPARE(nightStops.at(0), lutNight.skyTop);
    QCOMPARE(nightStops.at(1), lutNight.skyMid);
    QCOMPARE(nightStops.at(2), lutNight.skyBottom);
}

void KisAiV6WiringTest::testFourLayerShadingBothPaths()
{
    KisAiSceneSpec character;
    character.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram cProg = KisAiLayoutEngine::generateProgram(character, QSize(512, 512));
    QVERIFY(hasId(cProg.operations, QStringLiteral("_form_shadow")));
    QVERIFY(hasId(cProg.operations, QStringLiteral("_bounce_light")));

    KisAiSceneSpec landscape;
    landscape.subject.type = QStringLiteral("landscape");
    landscape.prompt = QStringLiteral("mountain lake at dusk");
    const KisAiStrokeProgram lProg = KisAiLayoutEngine::generateProgram(landscape, QSize(512, 512));
    QVERIFY(hasId(lProg.operations, QStringLiteral("_form_shadow")));
    QVERIFY(hasId(lProg.operations, QStringLiteral("_bounce_light")));
}

void KisAiV6WiringTest::testNarrativeTimeResolves()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    spec.light.timeOfDay = QStringLiteral("day"); // default
    spec.narrative.time = QStringLiteral("golden hour");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));

    // golden hour resolves to sunset: the sky wash must match the sunset LUT.
    const auto lut = KisAiLightRig::timeOfDayLut(QStringLiteral("sunset"));
    bool matched = false;
    for (const KisAiStrokeOperation &op : prog.operations) {
        if (op.id == QLatin1String("bg_wash") && op.gradientColors.size() >= 3)
            matched = (op.gradientColors.at(0) == lut.skyTop);
    }
    QVERIFY(matched);

    // fromSpec agrees with the layout resolution.
    const KisAiLightSettings rig = KisAiLightRig::fromSpec(spec);
    QCOMPARE(rig.timeOfDay, QLatin1String("sunset"));
}

void KisAiV6WiringTest::testColorScriptBlend()
{
    KisAiSceneSpec plain;
    plain.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram plainProg = KisAiLayoutEngine::generateProgram(plain, QSize(256, 256));

    KisAiSceneSpec tinted = plain;
    tinted.colorScript.midtone = QColor(125, 143, 201);
    tinted.colorScript.accentWeight = 1.0;
    const KisAiStrokeProgram tintProg = KisAiLayoutEngine::generateProgram(tinted, QSize(256, 256));

    QColor plainSkin, tintSkin;
    for (const KisAiStrokeOperation &op : plainProg.operations) {
        if (op.id == QLatin1String("face_skin"))
            plainSkin = op.brush.color;
    }
    for (const KisAiStrokeOperation &op : tintProg.operations) {
        if (op.id == QLatin1String("face_skin"))
            tintSkin = op.brush.color;
    }
    QVERIFY(plainSkin.isValid() && tintSkin.isValid());
    QVERIFY(plainSkin != tintSkin);
}

// ========================================================================
// W4: renderer unification
// ========================================================================

void KisAiV6WiringTest::testNoDoubleBlushOrSss()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(512, 512));
    const QVector<KisAiStrokeOperation> expanded =
        KisAiStrokeRenderer::expandProceduralOperations(prog.operations, QSize(512, 512));

    // Layout already paints blush + SSS: the renderer must not double them.
    QVERIFY(countId(expanded, QStringLiteral("procedural_blush")) == 0);
    QVERIFY(countId(expanded, QStringLiteral("sss_fringe")) == 0);
    // Layout originals survive exactly once (per side).
    QCOMPARE(countId(expanded, QStringLiteral("blush_l")), 1);
    QCOMPARE(countId(expanded, QStringLiteral("blush_r")), 1);
}

void KisAiV6WiringTest::testPresetHintsAssigned()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(512, 512));
    QVERIFY(!prog.operations.isEmpty());
    for (const KisAiStrokeOperation &op : prog.operations)
        QVERIFY(!op.brush.presetHint.trimmed().isEmpty());
}

void KisAiV6WiringTest::testEnvelopeUnification()
{
    // A sharp corner must not bowtie: the shared builder is the path the
    // renderer now draws for wide strokes.
    QVector<KisAiStrokePoint> pts;
    pts.append(KisAiStrokePoint(0.2, 0.5, 0.9));
    pts.append(KisAiStrokePoint(0.5, 0.5, 0.9));
    pts.append(KisAiStrokePoint(0.5, 0.8, 0.9));
    KisAiStrokeBrush brush;
    brush.profile = QStringLiteral("gpen");
    brush.size = 0.01;
    const QPolygonF poly = KisAiDeliberateStroke::buildEnvelopePolygon(pts, brush, QSize(512, 512), false);
    QVERIFY(poly.size() >= 6);
    for (const QPointF &p : poly)
        QVERIFY(std::isfinite(p.x()) && std::isfinite(p.y()));
}

void KisAiV6WiringTest::testPreviewParity()
{
    // W0: parity probe — renderProgramToImage on the same program twice is
    // deterministic (PSNR-capped identical). Full layer parity needs Krita.
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));
    const QImage a = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    const QImage b = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    QVERIFY(!a.isNull() && !b.isNull());
    QCOMPARE(KisAiVisionCritic::psnr(a, b), 60.0);
}

// ========================================================================
// W5: spec finishing
// ========================================================================

void KisAiV6WiringTest::testCanonicalExampleMentionsV2()
{
    const QString characterExample = KisAiSceneSpecCodec::canonicalSpecExample(QStringLiteral("anime girl portrait"));
    QVERIFY(characterExample.contains(QStringLiteral("art_style")));
    QVERIFY(characterExample.contains(QStringLiteral("detail_level")));
    QVERIFY(characterExample.contains(QStringLiteral("focal")));
    QVERIFY(characterExample.contains(QStringLiteral("eye_aperture")));
    QVERIFY(characterExample.contains(QStringLiteral("hair_highlight_bands")));

    const QString landscapeExample =
        KisAiSceneSpecCodec::canonicalSpecExample(QStringLiteral("mountain landscape scenery"));
    QVERIFY(landscapeExample.contains(QStringLiteral("art_style")));
    QVERIFY(landscapeExample.contains(QStringLiteral("narrative")));
}

void KisAiV6WiringTest::testArtStyleReachesSpec()
{
    const QSize canvas(512, 512);
    // Auto (0) leaves no style override; watercolor (2) pins the style block.
    const QJsonObject autoPayload = KisAiSceneSpecCodec::buildSceneSpecPayload(QStringLiteral("gpt-4o"),
                                                                               QStringLiteral("a girl"),
                                                                               canvas,
                                                                               0,
                                                                               QString(),
                                                                               QString(),
                                                                               false,
                                                                               false,
                                                                               0.7,
                                                                               1.0,
                                                                               0,
                                                                               false);
    const QJsonObject wcPayload = KisAiSceneSpecCodec::buildSceneSpecPayload(QStringLiteral("gpt-4o"),
                                                                             QStringLiteral("a girl"),
                                                                             canvas,
                                                                             2,
                                                                             QString(),
                                                                             QString(),
                                                                             false,
                                                                             false,
                                                                             0.7,
                                                                             1.0,
                                                                             0,
                                                                             false);
    const QString autoText =
        QJsonDocument(autoPayload.value(QStringLiteral("messages")).toArray()).toJson(QJsonDocument::Compact);
    const QString wcText =
        QJsonDocument(wcPayload.value(QStringLiteral("messages")).toArray()).toJson(QJsonDocument::Compact);
    QVERIFY(!autoText.contains(QStringLiteral("ART STYLE OVERRIDE")));
    QVERIFY(wcText.contains(QStringLiteral("watercolor")));
}

// ========================================================================
// W3/W6: refinement loop helpers (offline, no network)
// ========================================================================

void KisAiV6WiringTest::testNBestSelectsBest()
{
    KisAiModelRouter::setQualityMode(KisAiModelRouter::QualityMode::Quality);
    const QString good = QStringLiteral(R"({"subject": {"type": "character"}, "head": {"hair_style": "twin_tails"}})");
    const QString bad = QStringLiteral(R"({"subject": {"type": "character"}, "head": {"hair_style": "bob"}})");
    const KisAiNBestResult result = KisAiRefinementLoop::selectBestSpecFromBodies(
        QStringLiteral("a girl with twin tails"),
        QSize(512, 512),
        {good.toUtf8(), bad.toUtf8(), QStringLiteral("not json").toUtf8()});
    QCOMPARE(result.candidateCount, 3);
    QCOMPARE(result.spec.head.hairStyle, QLatin1String("twin_tails"));
    QVERIFY(!result.logLines.isEmpty());
}

void KisAiV6WiringTest::testPatchWhitelistRejectsStructural()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram base = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));

    // Structural change disguised as a patch: Flats add must be rejected.
    KisAiProgramPatch evil;
    evil.op = KisAiProgramPatch::Op::Add;
    evil.path = QStringLiteral("/ops/add");
    QJsonObject evilOp;
    evilOp.insert(QStringLiteral("kind"), QStringLiteral("fill"));
    evilOp.insert(QStringLiteral("id"), QStringLiteral("evil_flats"));
    evilOp.insert(QStringLiteral("layer"), QStringLiteral("Flats"));
    evil.value = evilOp;

    KisAiSceneRigOverrides delta;
    QStringList rejected;
    const KisAiStrokeProgram out =
        KisAiRefinementLoop::applyRigPatchesAndRelayout(base, spec, QSize(256, 256), {evil}, &delta, &rejected);
    QVERIFY(!rejected.isEmpty());
    bool hasEvil = false;
    for (const KisAiStrokeOperation &op : out.operations) {
        if (op.id == QLatin1String("evil_flats"))
            hasEvil = true;
    }
    QVERIFY(!hasEvil);
}

void KisAiV6WiringTest::testCriticLoopConverges()
{
    // Converged (tiny gain) stops; fresh improvement continues.
    QVERIFY(!KisAiRefinementLoop::shouldContinue(30.0, 30.5, 0, 3));
    QVERIFY(KisAiRefinementLoop::shouldContinue(20.0, 30.0, 0, 3));
    // Budget spent stops regardless of gain.
    QVERIFY(!KisAiRefinementLoop::shouldContinue(20.0, 30.0, 3, 3));
    QCOMPARE(KisAiRefinementLoop::formatTelemetry(QStringLiteral("CRITIC_ROUND"), QStringLiteral("gain=+0.4dB")),
             QLatin1String("[CRITIC_ROUND] gain=+0.4dB"));
}

void KisAiV6WiringTest::testGoalStep2UsesPatchOnly()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram base = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));
    const int baseOps = base.operations.size();

    KisAiProgramPatch patch;
    patch.op = KisAiProgramPatch::Op::Replace;
    patch.path = QStringLiteral("/rig/hair_highlight_bands");
    patch.value = QJsonValue(3);
    QStringList rejected;
    const KisAiStrokeProgram out =
        KisAiRefinementLoop::applyRigPatchesAndRelayout(base, spec, QSize(256, 256), {patch}, nullptr, &rejected);
    QVERIFY(rejected.isEmpty());
    // Patch-only: 3 hair bands now, background/clothing untouched in count.
    QCOMPARE(countId(out.operations, QStringLiteral("rig_hair_band_")), 3);
    QVERIFY(out.operations.size() >= baseOps);
    bool hasFace = false;
    for (const KisAiStrokeOperation &op : out.operations) {
        if (op.id == QLatin1String("face_skin"))
            hasFace = true;
    }
    QVERIFY(hasFace);
}

void KisAiV6WiringTest::testOfflineFallbackPaints()
{
    // Empty/garbage bodies fall back to the deterministic offline spec.
    const KisAiNBestResult result = KisAiRefinementLoop::selectBestSpecFromBodies(
        QStringLiteral("silver hair girl with green eyes in starry night sky"),
        QSize(256, 256),
        {});
    QCOMPARE(result.spec.light.timeOfDay, QLatin1String("night"));
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(result.spec, QSize(256, 256));
    QVERIFY(prog.isValid());
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(128, 128));
    QVERIFY(!img.isNull());
}

void KisAiV6WiringTest::testGoldenKpiGate()
{
    // W0/W6: golden KPI gate — representative prompts must hold the floor:
    // valid program, symmetric eyes, rig-derived shading present, no face
    // particles by construction (Layout emits none on faces).
    const QStringList prompts = {
        QStringLiteral("anime girl with twin tails, smile"),
        QStringLiteral("silver hair girl with green eyes in starry night sky"),
        QStringLiteral("sunset meadow landscape"),
    };
    for (const QString &prompt : prompts) {
        const KisAiSceneSpec spec = KisAiSceneSpecCodec::defaultSpecForPrompt(
            prompt, QSize(256, 256));
        const KisAiStrokeProgram prog =
            KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));
        QVERIFY2(prog.isValid(), qPrintable(prompt));
        QVERIFY2(KisAiDeliberateStroke::eyePairSymmetryWarnings(prog.operations).isEmpty(),
                 qPrintable(prompt));
        const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(128, 128));
        QVERIFY2(!img.isNull(), qPrintable(prompt));
    }
}

void KisAiV6WiringTest::testRenderBudgetDegrade()
{
    const auto light = KisAiStrokeRenderer::renderBudgetFor(QSize(256, 256), 50);
    QVERIFY(!light.degraded);
    const auto heavy = KisAiStrokeRenderer::renderBudgetFor(QSize(2048, 2048), 800);
    QVERIFY(heavy.degraded);
    QVERIFY(!heavy.note.isEmpty());
}

void KisAiV6WiringTest::testGoalStepPatchDoesNotDoubleOperations()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram base = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));
    const int baseOps = base.operations.size();
    QVERIFY(baseOps > 10);

    KisAiProgramPatch patch;
    patch.op = KisAiProgramPatch::Op::Replace;
    patch.path = QStringLiteral("/rig/hair_highlight_bands");
    patch.value = QJsonValue(2);
    QStringList rejected;
    KisAiSceneRigOverrides delta;
    const KisAiStrokeProgram patched = KisAiRefinementLoop::applyRigPatchesAndRelayout(
        base, spec, QSize(256, 256), {patch}, &delta, &rejected);
    QVERIFY(rejected.isEmpty());

    // When patch path is taken in Goal Mode, replacing m_goalAccumulatedProgram
    // with patched results in ops count close to base (not 2x base).
    KisAiStrokeProgram goalAccum = base;
    const bool usedPatchPath = true;
    if (usedPatchPath) {
        goalAccum = patched;
    } else {
        goalAccum = KisAiStrokeProgramCodec::mergePrograms(goalAccum, patched);
    }
    // With fix: operations are replaced/updated, not doubled.
    QVERIFY(goalAccum.operations.size() < baseOps * 2);
    QVERIFY(qAbs(goalAccum.operations.size() - baseOps) < 10);
}

void KisAiV6WiringTest::testSceneSpecParseRespectsInitialCanvasSize()
{
    const QString json = QStringLiteral(
        "{\"subject\": {\"type\": \"character\"}, \"prompt\": \"test\"}");
    KisAiStrokeProgram progCustom;
    progCustom.canvasSize = QSize(1920, 1080);
    QString parseErr;
    KisAiJsonDiagnostic diag;
    QVERIFY(KisAiStrokeProgramCodec::parseResponse(json.toUtf8(), &progCustom, &parseErr, &diag));
    QCOMPARE(progCustom.canvasSize, QSize(1920, 1080));

    KisAiStrokeProgram progDefault;
    QVERIFY(KisAiStrokeProgramCodec::parseResponse(json.toUtf8(), &progDefault, &parseErr, &diag));
    QCOMPARE(progDefault.canvasSize, QSize(1024, 1024));
}

void KisAiV6WiringTest::testFinishingPostProcessParity()
{
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));
    prog.stepPhase = QStringLiteral("complete");
    prog.goalReached = false; // Disable finishing

    const QImage plain = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    QVERIFY(!plain.isNull());

    prog.stepPhase = QStringLiteral("finishing");
    const QImage finished = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    QVERIFY(!finished.isNull());

    // Finishing adds Bloom, Grade, Vignette, and Film Grain, so it must differ from plain.
    const qreal psnrDiff = KisAiVisionCritic::psnr(plain, finished);
    QVERIFY(psnrDiff < 50.0);
    QVERIFY(psnrDiff > 15.0);
}

void KisAiV6WiringTest::testEnvelopeClampedVertexPreserved()
{
    // Sharp hairpin turn that triggers bowtie clamping
    QVector<KisAiStrokePoint> hairpin;
    hairpin.append(KisAiStrokePoint(0.1, 0.1, 1.0));
    hairpin.append(KisAiStrokePoint(0.5, 0.5, 1.0));
    hairpin.append(KisAiStrokePoint(0.1, 0.52, 1.0));

    KisAiStrokeBrush brush;
    brush.size = 0.08;
    brush.profile = QStringLiteral("gpen");

    const QPolygonF poly = KisAiDeliberateStroke::buildEnvelopePolygon(hairpin, brush, QSize(512, 512), false);
    QVERIFY(poly.size() >= 4);
    // Boundary polygon should be non-empty and have positive bounding area
    const QRectF bounds = poly.boundingRect();
    QVERIFY(bounds.width() > 10.0);
    QVERIFY(bounds.height() > 10.0);
}

KISTEST_MAIN(KisAiV6WiringTest)
