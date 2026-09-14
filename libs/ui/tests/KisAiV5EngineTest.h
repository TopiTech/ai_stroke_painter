/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_V5_ENGINE_TEST_H
#define KIS_AI_V5_ENGINE_TEST_H

#include <QObject>

/**
 * V5 engine test suite: Model Router (F0), SceneSpec v2 (F1),
 * Rig DSL (F2), Program Patch (F3a), Vision Critic (F3b),
 * LightRig 4-layer (F4) and ink dynamics (F4b).
 */
class KisAiV5EngineTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // F0: Model Router
    void testModelRouterStagePlans();
    void testModelRouterQualityModes();
    void testModelRouterFallbackChainAndStrategy();

    // F1: SceneSpec v2
    void testSceneSpecV2Parsing();
    void testSceneSpecV2BackwardCompatible();
    void testSceneSpecNBestSelection();

    // F2: Rig DSL
    void testRigParameterClamping();
    void testRigEyePairSymmetryByConstruction();
    void testRigFacePartsGenerated();
    void testRigBackdropWeatherFaceGuard();

    // F3a: Program Patch
    void testPatchParseAndApplyRig();
    void testPatchWhitelistRejectsStructureChanges();
    void testPatchDecorativeAddRemove();

    // F3b: Vision Critic
    void testCriticCropSelectionDeterministic();
    void testCritiqueParseAndMerge();
    void testPsnrAndConvergence();

    // F4: LightRig + ink dynamics
    void testTimeOfDayLutConsistency();
    void testFormAndBounceLayers();
    void testInkDynamicsPoolingAndFade();
};

#endif // KIS_AI_V5_ENGINE_TEST_H
