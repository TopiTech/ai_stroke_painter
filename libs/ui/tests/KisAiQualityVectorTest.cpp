/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiQualityVectorTest.h"

#include <QImage>
#include <QJsonObject>
#include <QPolygonF>
#include <QStringList>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiQualityVector.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeProgram.h"

using namespace KisAi;

void KisAiQualityVectorTest::testAxisNamesCountAndUniqueness()
{
    const QStringList names = QualityVector::allAxisNames();
    QCOMPARE(names.size(), 13);

    QSet<QString> uniq;
    for (int i = 0; i < names.size(); ++i) {
        uniq.insert(names.at(i));
    }
    QCOMPARE(uniq.size(), names.size());

    QVERIFY(names.contains(QStringLiteral("structural.layerCoverage")));
    QVERIFY(names.contains(QStringLiteral("perceptual.ssimAgainstReference")));
    QVERIFY(names.contains(QStringLiteral("perceptual.skinBandSmoothness")));
}

void KisAiQualityVectorTest::testAxisValueLookupByName()
{
    QualityVector v;
    v.structural.layerCoverage = 0.7;
    v.perceptual.colorEntropy = 0.3;

    QCOMPARE(v.axisValue(QStringLiteral("structural.layerCoverage")), 0.7);
    QCOMPARE(v.axisValue(QStringLiteral("perceptual.colorEntropy")), 0.3);
    QCOMPARE(v.axisValue(QStringLiteral("structural.unknown")), 0.0);
}

void KisAiQualityVectorTest::testAggregateWeightedAverage()
{
    QualityVector v;
    v.structural.layerCoverage = 1.0;
    v.structural.silhouetteContinuity = 1.0;
    v.structural.silhouetteArea = 1.0;
    v.structural.colorHarmony = 1.0;
    v.structural.strokeContinuity = 1.0;
    v.structural.intentMatch = 1.0;
    v.structural.negativeCompliance = 1.0;
    v.structural.symmetryAxisDeviation = 1.0;
    v.perceptual.ssimAgainstReference = 1.0;
    v.perceptual.colorEntropy = 1.0;
    v.perceptual.edgeDensityBalance = 1.0;
    v.perceptual.lineartThicknessStddev = 1.0;
    v.perceptual.skinBandSmoothness = 1.0;

    // デフォルト重み (1.0) → 全軸 1.0 → aggregate 1.0
    for (const QString &n : v.allAxisNames())
        v.weights[n] = 1.0;
    QCOMPARE(v.aggregate(), 1.0);

    // silhouetteContinuity を 0.0 にして重み 5.0 にしてみる
    v.structural.silhouetteContinuity = 0.0;
    v.weights[QStringLiteral("structural.silhouetteContinuity")] = 5.0;
    // 分子 = (12*1.0) + (0.0 * 5.0) = 12, 分母 = 17
    // aggregate = 12/17 ≈ 0.7059
    const qreal expected = 12.0 / 17.0;
    QVERIFY(qAbs(v.aggregate() - expected) < 1e-6);
}

void KisAiQualityVectorTest::testIsGatePassedPerAxisThreshold()
{
    QualityVector v;
    // 全軸 0.6 (デフォルトレベル)
    for (const QString &n : v.allAxisNames()) {
        v.weights[n] = 1.0;
    }
    v.structural.layerCoverage = 0.6;
    v.structural.silhouetteContinuity = 0.6;
    v.structural.silhouetteArea = 0.6;
    v.structural.colorHarmony = 0.6;
    v.structural.strokeContinuity = 0.6;
    v.structural.intentMatch = 0.6;
    v.structural.negativeCompliance = 0.6;
    v.structural.symmetryAxisDeviation = 0.6;
    v.perceptual.ssimAgainstReference = 0.6;
    v.perceptual.colorEntropy = 0.6;
    v.perceptual.edgeDensityBalance = 0.6;
    v.perceptual.lineartThicknessStddev = 0.6;
    v.perceptual.skinBandSmoothness = 0.6;

    QVERIFY(v.isGatePassed(0.5));
    QVERIFY(!v.isGatePassed(0.7));

    // 1 軸だけ 0.3 に落とす → 不合格
    v.structural.intentMatch = 0.3;
    QVERIFY(!v.isGatePassed(0.5));
}

void KisAiQualityVectorTest::testZeroWeightAxisExcludedFromGate()
{
    QualityVector v;
    for (const QString &n : v.allAxisNames()) {
        v.weights[n] = 1.0;
    }
    v.structural.layerCoverage = 0.8;
    v.structural.silhouetteContinuity = 0.8;
    v.structural.silhouetteArea = 0.8;
    v.structural.colorHarmony = 0.8;
    v.structural.strokeContinuity = 0.8;
    v.structural.intentMatch = 0.8;
    v.structural.negativeCompliance = 0.8;
    v.structural.symmetryAxisDeviation = 0.8;
    v.perceptual.ssimAgainstReference = 0.8;
    v.perceptual.colorEntropy = 0.0;
    v.perceptual.edgeDensityBalance = 0.8;
    v.perceptual.lineartThicknessStddev = 0.8;
    v.perceptual.skinBandSmoothness = 0.8;

    // colorEntropy を重み 0 に → ゲート判定から除外
    v.weights[QStringLiteral("perceptual.colorEntropy")] = 0.0;
    QVERIFY(v.isGatePassed(0.5));
}

void KisAiQualityVectorTest::testToJsonContainsAllAxesAndAggregate()
{
    QualityVector v;
    for (const QString &n : v.allAxisNames()) {
        v.weights[n] = 1.0;
    }
    v.structural.layerCoverage = 0.9;
    v.perceptual.skinBandSmoothness = 0.4;

    const QJsonObject json = v.toJson();
    QVERIFY(json.contains(QStringLiteral("structural")));
    QVERIFY(json.contains(QStringLiteral("perceptual")));
    QVERIFY(json.contains(QStringLiteral("weights")));
    QVERIFY(json.contains(QStringLiteral("aggregate")));
    QVERIFY(json.contains(QStringLiteral("gatePassed")));

    const QJsonObject s = json.value(QStringLiteral("structural")).toObject();
    QCOMPARE(s.value(QStringLiteral("layerCoverage")).toDouble(), 0.9);

    const QJsonObject p = json.value(QStringLiteral("perceptual")).toObject();
    QCOMPARE(p.value(QStringLiteral("skinBandSmoothness")).toDouble(), 0.4);
}

void KisAiQualityVectorTest::testQualityProfilesHaveDistinctWeights()
{
    const QualityVector anime = QualityProfile::animeLineartHeavy();
    const QualityVector wc = QualityProfile::watercolorSoft();
    const QualityVector pr = QualityProfile::photorealistic();
    const QualityVector ink = QualityProfile::inkSketchBold();

    // anime は perceptual.lineartThicknessStddev を 1.6 で重み付け
    QCOMPARE(anime.weights.value(QStringLiteral("perceptual.lineartThicknessStddev")), 1.6);
    // watercolor は structural.colorHarmony を 1.6
    QCOMPARE(wc.weights.value(QStringLiteral("structural.colorHarmony")), 1.6);
    // photorealistic は perceptual.ssimAgainstReference を 1.8
    QCOMPARE(pr.weights.value(QStringLiteral("perceptual.ssimAgainstReference")), 1.8);
    // ink は structural.strokeContinuity を 1.5
    QCOMPARE(ink.weights.value(QStringLiteral("structural.strokeContinuity")), 1.5);
}

void KisAiQualityVectorTest::testEvaluateStructuralOnEmptyProgram()
{
    KisAiStrokeProgram prog;
    const StructuralMetrics m = QualityVectorEvaluator::evaluateStructural(prog, nullptr);
    QCOMPARE(m.layerCoverage, 0.0);
    QCOMPARE(m.colorHarmony, 0.0);
    QCOMPARE(m.negativeCompliance, 1.0);
    QCOMPARE(m.symmetryAxisDeviation, 1.0);
}

void KisAiQualityVectorTest::testEvaluateStructuralRecognizesLayers()
{
    KisAiStrokeProgram prog;
    prog.prompt = QStringLiteral("赤髪の少女");
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.layer = QStringLiteral("Flats");
    op.brush.color = QColor(220, 60, 80); // 赤系
    op.polygon = QPolygonF() << QPointF(0.2, 0.2) << QPointF(0.8, 0.2) << QPointF(0.8, 0.8) << QPointF(0.2, 0.8);
    prog.operations.append(op);

    KisAiStrokeOperation op2 = op;
    op2.layer = QStringLiteral("Lineart");
    op2.kind = KisAiStrokeOperation::Kind::Path;
    op2.brush.color = QColor(20, 20, 30);
    KisAiStrokePoint pt;
    pt.pos = QPointF(0.5, 0.5);
    pt.pressure = 0.8;
    op2.points.append(pt);
    op2.points.append(pt);
    op2.points.append(pt);
    prog.operations.append(op2);

    const StructuralMetrics m = QualityVectorEvaluator::evaluateStructural(prog, nullptr);
    QVERIFY(m.layerCoverage >= 0.55); // Flats(0.30) + Lineart(0.25)
    QVERIFY(m.colorHarmony >= 0.0);
    QVERIFY(m.strokeContinuity >= 0.0);
}

void KisAiQualityVectorTest::testEvaluateStructuralOnSmallProgram()
{
    KisAiStrokeProgram prog;
    const StructuralMetrics m = QualityVectorEvaluator::evaluateStructural(prog, nullptr);
    QCOMPARE(m.layerCoverage, 0.0);
    // silhouetteContinuity: Flats なし → 中立 0.5
    QCOMPARE(m.silhouetteContinuity, 0.5);
    QCOMPARE(m.silhouetteArea, 0.0);
}

void KisAiQualityVectorTest::testForNameUnknownReturnsDefault()
{
    const QualityVector v = QualityProfile::forName(QStringLiteral("unknown_profile_xyz"));
    // デフォルトレベル (全軸重み 1.0)
    for (const QString &n : v.allAxisNames()) {
        QCOMPARE(v.weights.value(n), 1.0);
    }
}

void KisAiQualityVectorTest::testEvaluateStructuralSymmetryAxisDeviation()
{
    KisAiSceneSpec spec;
    spec.composition.headCenter = QPointF(0.5, 0.5);

    // Symmetric drawing around axis x = 0.5 (left eye at 0.4, right eye at 0.6)
    KisAiStrokeProgram symProg;
    KisAiStrokeOperation opSym;
    opSym.layer = QStringLiteral("Lineart");
    opSym.kind = KisAiStrokeOperation::Kind::Path;
    KisAiStrokePoint ptL, ptR;
    ptL.pos = QPointF(0.4, 0.5);
    ptR.pos = QPointF(0.6, 0.5);
    opSym.points << ptL << ptR;
    symProg.operations << opSym;

    const StructuralMetrics symM = QualityVectorEvaluator::evaluateStructural(symProg, &spec);
    // Left and right perfectly balance: score should be 1.0
    QCOMPARE(symM.symmetryAxisDeviation, 1.0);

    // Asymmetric drawing biased to one side (points at 0.7, 0.7)
    KisAiStrokeProgram asymProg;
    KisAiStrokeOperation opAsym;
    opAsym.layer = QStringLiteral("Lineart");
    opAsym.kind = KisAiStrokeOperation::Kind::Path;
    KisAiStrokePoint ptA1, ptA2;
    ptA1.pos = QPointF(0.7, 0.5);
    ptA2.pos = QPointF(0.7, 0.5);
    opAsym.points << ptA1 << ptA2;
    asymProg.operations << opAsym;

    const StructuralMetrics asymM = QualityVectorEvaluator::evaluateStructural(asymProg, &spec);
    // Skewed by 0.2 from 0.5, exceeds 0.10 threshold: score should be 0.0
    QCOMPARE(asymM.symmetryAxisDeviation, 0.0);
}

void KisAiQualityVectorTest::testEvaluatePerceptualFormatSafety()
{
    // Grayscale image should not cause memory misalignment or crash
    QImage grayImg(32, 32, QImage::Format_Grayscale8);
    grayImg.fill(128);

    KisAiSceneSpec spec;
    const PerceptualMetrics m = QualityVectorEvaluator::evaluatePerceptual(grayImg, &spec, nullptr);
    QVERIFY(m.colorEntropy >= 0.0 && m.colorEntropy <= 1.0);
    QVERIFY(m.ssimAgainstReference >= 0.0 && m.ssimAgainstReference <= 1.0);
}

KISTEST_MAIN(KisAiQualityVectorTest)