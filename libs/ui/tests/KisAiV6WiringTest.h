/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_V6_WIRING_TEST_H
#define KIS_AI_V6_WIRING_TEST_H

#include <QObject>

/**
 * V6 wiring tests: Rig-driven character faces (W1), LightRig LUT single
 * source of truth (W2), Docker-side offline helpers (W3/W6), renderer
 * idempotence + envelope unification (W4), Spec v2 few-shot (W5).
 */
class KisAiV6WiringTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // W1: Rig wiring
    void testCharacterUsesRigEyePair();
    void testRigBrowsNoseMouthWired();
    void testDetailLevelScalesOrnaments();
    void testCameraFocalTiltShiftsHead();
    void testCharacterBackdropWeatherFaceGuard();
    void testFacingCompensation();

    // W2: light / color single source of truth
    void testBackgroundUsesLut();
    void testFourLayerShadingBothPaths();
    void testNarrativeTimeResolves();
    void testColorScriptBlend();

    // W4: renderer unification
    void testNoDoubleBlushOrSss();
    void testPresetHintsAssigned();
    void testEnvelopeUnification();
    void testPreviewParity();

    // W3/W6: refinement loop helpers
    void testNBestSelectsBest();
    void testPatchWhitelistRejectsStructural();
    void testCriticLoopConverges();
    void testGoalStep2UsesPatchOnly();
    void testOfflineFallbackPaints();
    void testGoldenKpiGate();
    void testRenderBudgetDegrade();

    // W5: spec finishing
    void testCanonicalExampleMentionsV2();
    void testArtStyleReachesSpec();

    // Regression tests for code review fixes
    void testGoalStepPatchDoesNotDoubleOperations();
    void testSceneSpecParseRespectsInitialCanvasSize();
    void testFinishingPostProcessParity();
    void testEnvelopeClampedVertexPreserved();
    void testApplyRigPatchesPreservesUnpatchedRigFields();
    void testApplyRigPatchesDoubleLidAndBrows();
    void testEnvelopeSupersampleScaleInPxMode();
    void testSceneSpecParseNaNFloatSafety();
    void testVisionCriticImageToDataUrlTransparency();
    void testDraperyFoldIdsAreUnique();
    void testSamplingForClampsTopPAndMaxTokens();
    void testGoalStepAdvanceLimitIncludesConfiguredFinalStep();
    void testRigClampedCoversPoseFields();
    void testLandscapeWaterMeadowSelection();
    void testSceneSpecNarrativeTimeLengthCapped();
};

#endif // KIS_AI_V6_WIRING_TEST_H
