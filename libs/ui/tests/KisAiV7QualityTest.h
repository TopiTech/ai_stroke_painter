/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_V7_QUALITY_TEST_H
#define KIS_AI_V7_QUALITY_TEST_H

#include <QObject>

/**
 * V7 Quality & Stroke Expressiveness Tests:
 * - Bezier head contour curvature & tilt rotation
 * - Hierarchical 3D hair clumps (ribbon flows, tapered tips & cast shadows)
 * - Volumetric pseudo-normal shading & material optics (Half-Lambert, SSS, sheen)
 * - Art style shader pipeline adaptation (watercolor, impasto, manga, cyber_neon)
 * - Dynamic stroke beautification (jitter removal, natural taper curves)
 * - Corner inking fillets & occlusion weight modulation
 */
class KisAiV7QualityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testBezierHeadCurvature();
    void testHierarchicalHairClumpStructure();
    void testVolumetricShadingConsistency();
    void testVolumetricShadingEmitCap();
    void testCornerInkingDotsSegmentCap();
    void testMaterialOpticsSssAndSheen();
    void testArtStylePipelineSwitching();
    void testStrokeBeautifierTaperAndSmoothing();
    void testCornerInkingFillet();
    void testLineartOcclusionWeighting();
    void testDraperyFoldsSynthesis();
};

#endif // KIS_AI_V7_QUALITY_TEST_H
