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

void KisAiQualityVectorTest::testColorEntropyUsesSampleDistribution()
{
    // 単色 (全て同じ赤) -> エントロピー 0。旧実装は分母 24 固定で
    // p = count/24 となり正規分布にならず過大評価されていた。
    KisAiStrokeProgram prog;
    for (int i = 0; i < 4; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.layer = QStringLiteral("Flats");
        op.brush.color = QColor(220, 40, 40);
        op.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1)
                                 << QPointF(0.9, 0.9) << QPointF(0.1, 0.9);
        prog.operations.append(op);
    }
    const StructuralMetrics m = QualityVectorEvaluator::evaluateStructural(prog, nullptr);
    QCOMPARE(m.colorHarmony, 0.0);

    // 無彩色のみ (白黒) -> ヒストグラム対象外 -> 0。旧実装は赤ビンに集計していた。
    KisAiStrokeProgram grayProg;
    for (int i = 0; i < 2; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.layer = QStringLiteral("Flats");
        op.brush.color = (i == 0) ? QColor(255, 255, 255) : QColor(0, 0, 0);
        op.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1)
                                 << QPointF(0.9, 0.9) << QPointF(0.1, 0.9);
        grayProg.operations.append(op);
    }
    const StructuralMetrics gm = QualityVectorEvaluator::evaluateStructural(grayProg, nullptr);
    QCOMPARE(gm.colorHarmony, 0.0);
}

void KisAiQualityVectorTest::testSilhouetteContinuityDetectsFragmentation()
{
    // 退化した Flats (面積 0) が混ざると 0.0。正常な 2 枚は 1.0。
    // 旧実装はどちらも恒等的に 1.0 だった。
    KisAiStrokeProgram okProg;
    for (int i = 0; i < 2; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.layer = QStringLiteral("Flats");
        op.brush.color = QColor(200, 150, 100);
        op.polygon = QPolygonF() << QPointF(0.2, 0.2) << QPointF(0.8, 0.2)
                                 << QPointF(0.8, 0.8) << QPointF(0.2, 0.8);
        okProg.operations.append(op);
    }
    const StructuralMetrics okM = QualityVectorEvaluator::evaluateStructural(okProg, nullptr);
    QCOMPARE(okM.silhouetteContinuity, 1.0);

    // 退化した Flats (面積 0 の線分ポリゴン) が混ざると 0.0。
    KisAiStrokeProgram fragProg;
    {
        KisAiStrokeOperation a;
        a.kind = KisAiStrokeOperation::Kind::Fill;
        a.layer = QStringLiteral("Flats");
        a.brush.color = QColor(200, 150, 100);
        a.polygon = QPolygonF() << QPointF(0.2, 0.2) << QPointF(0.8, 0.2)
                                << QPointF(0.8, 0.8) << QPointF(0.2, 0.8);
        fragProg.operations.append(a);
        KisAiStrokeOperation b = a;
        b.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.2, 0.2) << QPointF(0.1, 0.1);
        fragProg.operations.append(b);
    }
    const StructuralMetrics fragM = QualityVectorEvaluator::evaluateStructural(fragProg, nullptr);
    QCOMPARE(fragM.silhouetteContinuity, 0.0);

    // 一体のシルエット (大きな Flats + 内部に重なる 2 枚) -> 高スコア。
    // 各 bbox の最大重なり率の平均で測るため、包含関係では 1.0 に近い。
    KisAiStrokeProgram coherentProg;
    {
        KisAiStrokeOperation base;
        base.kind = KisAiStrokeOperation::Kind::Fill;
        base.layer = QStringLiteral("Flats");
        base.brush.color = QColor(200, 150, 100);
        base.polygon = QPolygonF() << QPointF(0.2, 0.2) << QPointF(0.8, 0.2)
                                   << QPointF(0.8, 0.8) << QPointF(0.2, 0.8);
        coherentProg.operations.append(base);
        KisAiStrokeOperation inner = base;
        inner.polygon = QPolygonF() << QPointF(0.3, 0.3) << QPointF(0.5, 0.3)
                                    << QPointF(0.5, 0.5) << QPointF(0.3, 0.5);
        coherentProg.operations.append(inner);
        KisAiStrokeOperation inner2 = base;
        inner2.polygon = QPolygonF() << QPointF(0.55, 0.55) << QPointF(0.7, 0.55)
                                     << QPointF(0.7, 0.7) << QPointF(0.55, 0.7);
        coherentProg.operations.append(inner2);
    }
    const StructuralMetrics coherentM = QualityVectorEvaluator::evaluateStructural(coherentProg, nullptr);
    QVERIFY(coherentM.silhouetteContinuity > 0.5);
}

void KisAiQualityVectorTest::testIntentMatchCoversSunsetAndDawn()
{
    const auto progWithWarmRed = [] {
        KisAiStrokeProgram p;
        p.prompt = QStringLiteral("sunset");
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.layer = QStringLiteral("Flats");
        op.brush.color = QColor(230, 90, 40); // 暖色 (sunset 判定に必要)
        op.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1)
                                 << QPointF(0.9, 0.9) << QPointF(0.1, 0.9);
        p.operations.append(op);
        return p;
    }();
    const StructuralMetrics sm = QualityVectorEvaluator::evaluateStructural(progWithWarmRed, nullptr);
    // 旧実装は sunset を常に不一致とし 0.0 を返していた。
    QCOMPARE(sm.intentMatch, 1.0);

    const auto progWithBright = [] {
        KisAiStrokeProgram p;
        p.prompt = QStringLiteral("dawn");
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.layer = QStringLiteral("Flats");
        op.brush.color = QColor(240, 235, 220);
        op.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1)
                                 << QPointF(0.9, 0.9) << QPointF(0.1, 0.9);
        p.operations.append(op);
        return p;
    }();
    const StructuralMetrics dm = QualityVectorEvaluator::evaluateStructural(progWithBright, nullptr);
    QCOMPARE(dm.intentMatch, 1.0);
}

void KisAiQualityVectorTest::testLineartJitterIgnoresEmptyTiles()
{
    // 疎だが均一な線画 (対角線 1 本)。旧実装は空タイルを CV に含めて
    // 常に 0.0 を返していた。修正後はインクタイルのみで評価し高スコアになる。
    KisAiStrokeProgram prog;
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.layer = QStringLiteral("Lineart");
    op.brush.color = QColor(20, 20, 30);
    op.brush.size = 0.004;
    for (int i = 0; i <= 10; ++i) {
        KisAiStrokePoint pt;
        pt.pos = QPointF(0.1 + 0.08 * i, 0.1 + 0.08 * i);
        pt.pressure = 0.8;
        op.points.append(pt);
    }
    prog.operations.append(op);

    QImage img(64, 64, QImage::Format_ARGB32);
    img.fill(0);
    const PerceptualMetrics m = QualityVectorEvaluator::evaluatePerceptual(img, nullptr, &prog);
    QVERIFY(m.lineartThicknessStddev > 0.5);
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