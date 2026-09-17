/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_QUALITY_VECTOR_TEST_H
#define KIS_AI_QUALITY_VECTOR_TEST_H

#include <QObject>

/**
 * V8 Phase 1.6: KisAi::QualityVector の単体テスト。
 *
 * - 各軸が [0,1] 範囲に収まる
 * - 4 種の QualityProfile が異なる重みセットを返す
 * - 重み 0 の軸は isGatePassed() の判定から除外される
 * - aggregate() は重み付き平均を返す
 * - toJson() に 16 軸 + aggregate + gatePassed が含まれる
 */
class KisAiQualityVectorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testAxisNamesCountAndUniqueness();
    void testAxisValueLookupByName();
    void testAggregateWeightedAverage();
    void testIsGatePassedPerAxisThreshold();
    void testZeroWeightAxisExcludedFromGate();
    void testToJsonContainsAllAxesAndAggregate();
    void testQualityProfilesHaveDistinctWeights();
    void testEvaluateStructuralOnEmptyProgram();
    void testEvaluateStructuralRecognizesLayers();
    void testEvaluateStructuralOnSmallProgram();
    void testForNameUnknownReturnsDefault();
    void testEvaluateStructuralSymmetryAxisDeviation();
    void testColorEntropyUsesSampleDistribution();
    void testSilhouetteContinuityDetectsFragmentation();
    void testIntentMatchCoversSunsetAndDawn();
    void testLineartJitterIgnoresEmptyTiles();
    void testEvaluatePerceptualFormatSafety();
};

#endif // KIS_AI_QUALITY_VECTOR_TEST_H