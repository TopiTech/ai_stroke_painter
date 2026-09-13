/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_RENDERER_TEST_H
#define KIS_AI_STROKE_RENDERER_TEST_H

#include <QObject>

class KisAiStrokeRendererTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCatmullRomSpline();
    void testRenderProgramToImage();
    void testClippingMaskToFlats();
    void testClippingMaskFromPreviousGoalStep();
    void testRenderGradientOpacity();
    void testRenderParticleBrush();
    void testRenderAirbrushDynamics();
    void testRenderHatchOperation();
    void testRenderRadialGradient();
    void testRenderZeroDimensionsFallback();
    void testUnnormalizedLayerRendering();
    void testCentripetalSplineAvoidsUnevenPointLoop();
    void testPressureStrokeHasAntialiasedTaper();
    void testRepresentativeCompositionQualityMetrics();
    void testRenderBackgroundLayer();
    void testRenderMangaLinesOperation();
    void testRenderMangaLinesWithOriginCenter();
    void testNewBrushProfilesRendering();
    void testCaptureImageBase64();
    void testRenderGoalModeProgression();
    void testGoalModeCumulativeProgressionAndRibbon();
    void testQualityUtilsResampling();
    void testQualityUtilsRdpSimplification();
    void testQualityUtilsCornerPreservingSmoothing();
    void testQualityUtilsPolygonOffsetAndTrapping();
    void testQualityUtilsBrushTaperAndDynamics();
    void testQualityUtilsCalligraphyWidth();
    void testQualityUtilsHalftonePattern();
    void testQualityUtilsHueShiftedHarmonies();
    void testQualityUtilsProgramTrapping();
    void testRenderCalligraphyAndCharcoalBrush();
    void testSynthesizeHairClump();
    void testSynthesizeFoliageClusters();
    void testDualShadowSeparation();
    void testFinishingFiltersBloomAndChromaticAberration();
    void testFinishingFiltersVignette();
    void testRenderProgramToImageBoundsDerivedCanvasSize();
    void testHalftonePatternWorkIsBounded();
    void testGradientAngleNormalizationIsFinite();
    void testCaptureImageBase64RejectsInvalidArguments();
    void testPxBrushSizeSurvivesSupersampling();
    void testHatchErasersAreShapeBounded();
    void testTrappingWidthAndScreenBlending();
    void testSoftEdgeDiffusionRadiusBounded();
    void testRenderAnimeEye();
    void testFaceExclusionMaskSuppressesParticles();
    void testUniteOverlappingHairFlats();
    void testGoalModeSingleArtboard();
    void testHatchLineCountIsBounded();
    void testLineScreenRowBudgetMatchesDotBudget();
    void testTypeCheckerClampsHostileParticleBounds();
    void testSceneSpecHyperQualityRendering();
    void testDeliberateStabilizeRemovesJitter();
    void testDeliberateLintDropsMicroAndOffCanvas();
    void testDeliberateStrokeOrderBigToSmallFaceLast();
    void testDeliberateAdaptiveSupersampleFaceOnly();
    void testDeliberateEyePairSymmetryWarnings();
};

#endif // KIS_AI_STROKE_RENDERER_TEST_H
