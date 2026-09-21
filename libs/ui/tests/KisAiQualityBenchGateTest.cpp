/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiQualityBenchGateTest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSize>
#include <QStandardPaths>
#include <QTextStream>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiAbstractOntology.h"
#include "aiillustration/KisAiDeliberateStroke.h"
#include "aiillustration/KisAiLayoutEngine.h"
#include "aiillustration/KisAiPerceptualRepairer.h"
#include "aiillustration/KisAiPhysicalRenderer.h"
#include "aiillustration/KisAiQualityVector.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeCommitter.h"
#include "aiillustration/KisAiStrokeProgram.h"

using namespace KisAi;

namespace
{

QString findGoldenSetPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {appDir + QStringLiteral("/../../tools/ai_quality_bench/golden_set.json"),
                                    appDir + QStringLiteral("/../../../tools/ai_quality_bench/golden_set.json"),
                                    appDir + QStringLiteral("/../tools/ai_quality_bench/golden_set.json"),
                                    QStringLiteral("tools/ai_quality_bench/golden_set.json"),
                                    QStringLiteral("../tools/ai_quality_bench/golden_set.json")};

    for (const QString &path : candidates) {
        if (QFile::exists(path)) {
            return QDir::cleanPath(path);
        }
    }
    return QString();
}

QJsonArray loadGoldenSetArray()
{
    const QString path = findGoldenSetPath();
    if (path.isEmpty()) {
        return QJsonArray();
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QJsonArray();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.object().value(QStringLiteral("prompts")).toArray();
}

bool evaluatePromptQuality(const QString &prompt, const QString &profileName, qreal minScore, qreal *outScore = nullptr)
{
    KisAiSceneSpec spec;
    OntologyApplier::apply(prompt, &spec);

    const KisAiStrokeProgram prog = KisAiStrokeProgramCodec::createDeterministicProgram(prompt, QSize(256, 256));
    if (prog.operations.isEmpty()) {
        return false;
    }

    const QImage rendered = KisAiPhysicalRenderer::renderProgramToPhysicalImage(prog, QSize(256, 256), true, -1.0, 1);

    QualityVector qv = QualityVectorEvaluator::evaluate(prog, rendered, &spec);
    // 指定されたプロファイルの重みを適用
    const QualityVector profileVec = QualityProfile::forName(profileName);
    qv.weights = profileVec.weights;

    const qreal agg = qv.aggregate();
    if (outScore) {
        *outScore = agg;
    }

    return agg >= minScore;
}

} // namespace

void KisAiQualityBenchGateTest::testGoldenSetJsonValid()
{
    const QJsonArray arr = loadGoldenSetArray();
    QVERIFY2(!arr.isEmpty(), "golden_set.json not found or empty");
    QCOMPARE(arr.size(), 32);

    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject p = arr.at(i).toObject();
        QVERIFY(p.contains(QStringLiteral("id")));
        QVERIFY(p.contains(QStringLiteral("category")));
        QVERIFY(p.contains(QStringLiteral("prompt")));
        QVERIFY(p.contains(QStringLiteral("expected_profile")));
        QVERIFY(p.contains(QStringLiteral("min_aggregate_score")));
    }
}

void KisAiQualityBenchGateTest::testBenchmarkEvaluationExistingPrompts()
{
    // 代表的な existing プロンプトの品質検証
    qreal score = 0.0;
    const bool ok = evaluatePromptQuality(QStringLiteral("黒髪ショートボブの少女、窓辺で微笑む"),
                                          QStringLiteral("anime_lineart_heavy"),
                                          0.45,
                                          &score);
    QVERIFY2(ok, qPrintable(QString("Score below threshold: %1").arg(score)));
}

void KisAiQualityBenchGateTest::testBenchmarkEvaluationAbstractPrompts()
{
    qreal score = 0.0;
    const bool ok = evaluatePromptQuality(QStringLiteral("jazzy な街角、雨上がりのネオンが水たまりに踊る"),
                                          QStringLiteral("ink_sketch_bold"),
                                          0.40,
                                          &score);
    QVERIFY2(ok, qPrintable(QString("Abstract prompt score: %1").arg(score)));
}

void KisAiQualityBenchGateTest::testBenchmarkEvaluationAsymmetryPrompts()
{
    qreal score = 0.0;
    const bool ok = evaluatePromptQuality(QStringLiteral("右側だけに強いサイド光が当たる劇的な陰影の肖像画"),
                                          QStringLiteral("photorealistic"),
                                          0.40,
                                          &score);
    QVERIFY2(ok, qPrintable(QString("Asymmetry prompt score: %1").arg(score)));
}

void KisAiQualityBenchGateTest::testBenchmarkEvaluationComplexPrompts()
{
    qreal score = 0.0;
    const bool ok = evaluatePromptQuality(QStringLiteral("アニメ風銀髪赤眼のエルフ騎士、月光に照らされる"),
                                          QStringLiteral("anime_lineart_heavy"),
                                          0.45,
                                          &score);
    QVERIFY2(ok, qPrintable(QString("Complex prompt score: %1").arg(score)));
}

void KisAiQualityBenchGateTest::testBenchmarkEvaluationBoundaryPrompts()
{
    qreal score = 0.0;
    const bool ok = evaluatePromptQuality(QStringLiteral("漆黒の背景に舞う一匹の白い蝶"),
                                          QStringLiteral("ink_sketch_bold"),
                                          0.40,
                                          &score);
    QVERIFY2(ok, qPrintable(QString("Boundary prompt score: %1").arg(score)));
}

void KisAiQualityBenchGateTest::testBenchmarkFullGatePass()
{
    const QJsonArray arr = loadGoldenSetArray();
    if (arr.isEmpty()) {
        QSKIP("golden_set.json not available in current test run path");
    }

    int passedCount = 0;
    QString failures;
    const QString diagPath =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("bench_gate_debug.txt"));
    QFile diagOut(diagPath);
    QStringList diagLines;
    const bool diagReady = diagOut.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream diag(&diagOut);
    // 32 本すべてをベンチマーク実行
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject p = arr.at(i).toObject();
        const QString prompt = p.value(QStringLiteral("prompt")).toString();
        const QString profile = p.value(QStringLiteral("expected_profile")).toString();
        const qreal minScore = p.value(QStringLiteral("min_aggregate_score")).toDouble(0.40);

        qreal score = 0.0;
        if (evaluatePromptQuality(prompt, profile, minScore, &score)) {
            ++passedCount;
        } else {
            failures += QStringLiteral("#%1 %2: score=%3 < min=%4\n")
                            .arg(p.value(QStringLiteral("id")).toInt())
                            .arg(profile)
                            .arg(score, 0, 'f', 3)
                            .arg(minScore, 0, 'f', 3);
        }
        diagLines << QStringLiteral("bench %1 score=%2 min=%3 %4")
                          .arg(p.value(QStringLiteral("id")).toInt())
                          .arg(score, 0, 'f', 3)
                          .arg(minScore, 0, 'f', 3)
                          .arg(score >= minScore ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }
    if (diagReady) {
        for (const QString &line : diagLines)
            diag << line << "\n";
        diag << "bench failures:\n" << failures << "\n";
        diagOut.close();
    }

    // 計画書要件: 95% 以上のプロンプトで合格 (32 本中 30 本以上)。
    // golden_set.json の min_aggregate_score をそのまま適用する
    // (旧実装は *0.85 の割引と 90% 判定でゲートを緩めていた)。
    const qreal passRate = static_cast<qreal>(passedCount) / arr.size();
    QVERIFY2(passRate >= 0.95,
             qPrintable(QString("Pass rate %1 (%2/%3) is below 95% target\n%4")
                            .arg(passRate)
                            .arg(passedCount)
                            .arg(arr.size())
                            .arg(failures)));
}

void KisAiQualityBenchGateTest::testAtomicInkRatioOnPortrait()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("黒髪ショートボブの少女、窓辺で微笑む");
    spec.subject.type = QStringLiteral("character");
    spec.composition.headCenter = QPointF(0.5, 0.4);
    spec.composition.headHeight = 0.40;
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));
    const QVector<KisAiStrokeOperation> atoms =
        KisAiStrokeCommitter::prepareAtomicOps(prog.operations, QSize(256, 256));
    QVERIFY2(KisAiStrokeCommitter::atomicStrokeRatio(atoms) >= 0.999,
             qPrintable(QStringLiteral("atomic ratio=%1").arg(KisAiStrokeCommitter::atomicStrokeRatio(atoms))));
}

void KisAiQualityBenchGateTest::testEyeSymmetryWarningsOnPortrait()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("黒髪ショートボブの少女、窓辺で微笑む");
    spec.subject.type = QStringLiteral("character");
    spec.composition.headCenter = QPointF(0.5, 0.4);
    spec.composition.headHeight = 0.40;
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(256, 256));
    QVERIFY(KisAiDeliberateStroke::eyePairSymmetryWarnings(prog.operations).isEmpty());
}

KISTEST_MAIN(KisAiQualityBenchGateTest)
