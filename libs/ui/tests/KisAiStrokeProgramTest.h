/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_PROGRAM_TEST_H
#define KIS_AI_STROKE_PROGRAM_TEST_H

#include <QObject>

class KisAiStrokeProgramTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSanitizeAndExtractJson();
    void testParseValidProgram();
    void testBuildChatCompletionsPayload();
    void testStrokeProgramJsonSchema();
    void testTruncatedJsonRecovery();
    void testLayerAndKindAliases();
    void testObjectPointParsing();
    void testPixelCoordinateAutoNormalization();
    void testColorAndBrushParsing();
    void testStableSeed();
    void testPromptAnalyzerDomainClassification();
    void testHatchOperationParsing();
    void testProceduralCharacterGeneration();
    void testNormalizeLayerName();
    void testCountLayerOperationsAndFormatSummary();
    void testRefineForRenderingRepairsModelGeometry();
    void testStructuralQualityScore();
    void testParseGeometrySafetyLimits();
    void testGradientDirectionPointsParsing();
    void testMangaLinesParsingAndRefinement();
    void testGoalModePayloadAndVisionModelDetection();
    void testGoalModeProgramStepAndMerge();
    void testNewProceduralDomains();
    void testPixelCoordinateThresholdBoundary();
    void testGoalModeProgramStepDynamicTotalSteps();
    void testGoalModePayloadDynamicPhase();
    void testParsePointsNanAndInfProtection();
    void testIsReasoningModel();
    void testScalarCoordinateAutoNormalization();
    void testParseSseStreamChunk();
    void testSchemaVersionValidation();
};

#endif // KIS_AI_STROKE_PROGRAM_TEST_H
