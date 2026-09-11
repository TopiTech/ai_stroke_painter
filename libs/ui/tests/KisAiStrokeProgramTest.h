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
    void testParticleCountClamping();
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
    void testAcceptedResponseContentType();
    void testGoalModeCompletionInvariant();
    void testSchemaVersionValidation();
    void testUserCorruptedJsonRepair();
    void testJsonSyntaxRepairVariousCases();
    void testExtractOperationsFromTruncatedEnvelope();
    void testSupportsJsonFormat();
    void testTypeCheckerValidationAndCoercion();
    void testTestUtilsMockAndCorruptions();
    void testSpikeNoiseSuppressionAndLayerSorting();
    void testJsonDiagnosticReporting();
    void testGoalModePayloadReasoningEffortAndSamplingParams();
    void testPythonLiteralsAndMissingCommasRepair();
    void testNestedEnvelopeUnwrapping();
    void testGoalModePayloadMaxTokensOverride();
    void testNewBrushProfilesNormalization();
    void testSignedLeadingDotAndTypeCheckerEdgeCases();
    void testSchemaAliasesAndGoalModeArtStyle();
    void testAgentCritiqueAndReadinessParsing();
    void testSanitizeUnescapedControlCharsInStrings();
    void testRefineBoundsHostileCanvasSizeAndAngle();
    void testGradientColorCountIsCapped();
    void testNumericOverflowFieldsFallBackToDefaults();
    void testParseColorAlphaOverflowIsSafe();
    void testSseCarryOverBufferIsBounded();
    void testRepairJsonSyntaxPreservesManyLiterals();
};

#endif // KIS_AI_STROKE_PROGRAM_TEST_H
