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
    void testStrictStructuredOutputsAndJsonSchema();
    void testCompositionPlanPayloadAndParsing();
    void testHueShiftedShadowCalculation();
    void testRevisedQualityScoreAndLinting();
    void testIntentAdherenceCheck();
    void testTrimOperationsToBudget();
    void testGoalModeGeometryDigest();
    void testPromptFirstPriorityBlock();
    void testCompositionPlanOpenAiChoicesUnwrapping();
    void testTrimOperationsPreservesRibbonAndParticles();
    void testQualityScoreWithHatch();
    void testTrimOperationsPreservesOriginalOrderWithinLayers();
    void testExtractOperationsQualityReportPassthrough();
    void testTypeCheckerParticleAndMangaLinesAliases();
    void testAnimeEyeParsingAndRefinement();
    void testDotNoiseSuppression();
    void testParticleAccumulationBlockedInMerge();
    void testParticlesOperationCapInRefine();
    void testEyePairSymmetryLint();
    void testSceneSpecSchemaStrict();
    void testSceneSpecParsingAndDefault();
    void testHeadRigSymmetryAndHairMass();
    void testLayoutEngineGeneratesProgram();
    void testLightRigConsistency();
    void testFourLayerShadingPresent();
    void testLineartHierarchy();
    void testBrushPresetMapping();
    void testStructuredCritiqueParsing();
    void testFindFieldDoesNotHijackShortKeys();
    void testSseAccumulatedContentIsBounded();
    void testSseChunkWithManyLinesIsLinear();
    void testCanvasSizeClampedAtParseTime();
    void testReasoningModelFamilyPrefixMatching();
    void testExtractOperationsDiagnosticNotFabricated();
    void testSceneSpecPayloadReasoningModelOmitsTemperature();
    void testSceneSpecPayloadStreamingAndJsonSchema();
    void testJsonModeForcedJsonObjectHandling();
    void testColorClauseDeduplication();
    void testEyeKindWinsOverLineSubstring();
    void testNumericStringExponentNotMangled();
    void testLiteralsMaskingBudgetIsBounded();
    void testCritiqueRegionsCountIsCapped();
    void testHatchSpacingClampedBeforeRescue();
    void testCompositionPlanRejectsOversizedBody();
    void testExtractOperationsSchemaVersionGate();
    void testSceneSpecRejectsOversizedBody();
    void testStructuredOutputsJsonSchemaCompleteness();
    void testLenientParsingCasingAndAliases();
    void testResampleEquidistantClosesTruncatedClosedCurve();
    void testResampleEquidistantStaysBounded();
    void testSchemaVersionCoercionRejectsHugeStringValue();
    void testCharacterDomainArtDirectionSubstitutions();
    void testNeutralSchemaExampleNoSpecificAnatomy();
    void testAnimeMouthParsingAndValidation();
    void testLandscapeRigsAndMultiTierComposition();
    void testRichOperationsLimitExpanded();
    void testTypeCheckerBleedAndCrossingCoordinatesPreserved();
    void testReferenceImagePayloadMultimodal();
    void testGoalModeAutonomousRefinementContinuation();
    void testFlagshipDirectivesAndTokenScaling();
    void testParseResponseApiErrorWithoutErrorMessagePointer();
    void testAnimeMouthSurvivesFullParsePipeline();
    void testPixelCoordinatesNormalizeThroughFullPipeline();
    void testSceneSpecArtStyleEnumMapping();
    void testMacroPrimitivesExpansion();
    void testParseResponseArrayFormContent();
    void testParseResponseToolCallsFunctionArguments();
    void testParseCompositionPlanArrayFormContent();
    void testParseSseStreamChunkArrayFormDelta();
};

#endif // KIS_AI_STROKE_PROGRAM_TEST_H
