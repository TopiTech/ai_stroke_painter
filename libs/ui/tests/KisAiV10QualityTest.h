/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_V10_QUALITY_TEST_H
#define KIS_AI_V10_QUALITY_TEST_H

#include <QObject>

/**
 * V10 Masterwork Quality & LLM Orchestration Tests:
 * - Hierarchical 3D hair strands (flows, tapers, flyaways, AO)
 * - Curvature-following jagged angel halo highlight
 * - Multi-layered anime eye assembly (cornea refraction, multi-catchlights, caustics)
 * - Occlusion and lighting line weight modulation
 * - Corner inking fillet generation
 * - Optical diffusion bloom and atmospheric finish
 * - SceneSpec v3 schema validation and parsing
 * - Semantic PromptAnalyzer heuristics
 */
class KisAiV10QualityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testHierarchicalHairStrandsGeneration();
    void testJaggedHairHaloHighlight();
    void testDetailedAnimeEyeStructure();
    void testOcclusionAndLightingLineWeight();
    void testCornerInkingFillets();
    void testDiffusionBloomAndAtmosphericFinish();
    void testSceneSpecV3SchemaAndParsing();
    void testPromptAnalyzerV10Heuristics();
    void testFloatingAngelHaloTorus();
    void testSmoothCubicHairClumpsAndDrapery();
};

#endif // KIS_AI_V10_QUALITY_TEST_H
