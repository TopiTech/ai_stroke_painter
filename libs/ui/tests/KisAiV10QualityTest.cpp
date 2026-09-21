/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiV10QualityTest.h"

#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiLayoutEngine.h"
#include "aiillustration/KisAiLightRig.h"
#include "aiillustration/KisAiPromptAnalyzer.h"
#include "aiillustration/KisAiRigLibrary.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeQualityUtils.h"
#include "aiillustration/KisAiStrokeRenderer.h"

#include <cmath>

using namespace QTest;

void KisAiV10QualityTest::testHierarchicalHairStrandsGeneration()
{
    KisAiStrokeOperation baseClump;
    baseClump.id = QStringLiteral("clump_0");
    baseClump.kind = KisAiStrokeOperation::Kind::Fill;
    baseClump.layer = QStringLiteral("Flats");
    baseClump.brush.color = QColor(45, 55, 95);
    baseClump.polygon << QPointF(0.45, 0.20)
                      << QPointF(0.42, 0.35)
                      << QPointF(0.48, 0.48)
                      << QPointF(0.54, 0.35)
                      << QPointF(0.52, 0.20);

    const auto strands = KisAiStrokeQualityUtils::generateHierarchicalHairStrands(
        baseClump, QPointF(0.5, 0.38), QSize(1024, 1024), 42);

    QVERIFY(!strands.isEmpty());

    bool hasFlow = false;
    bool hasFlyaway = false;
    bool hasAo = false;

    for (const auto &op : strands) {
        if (op.id.contains(QLatin1String("hair_flow_"))) {
            hasFlow = true;
            QCOMPARE(op.layer, QStringLiteral("Lineart"));
            QVERIFY(op.points.size() >= 3);
        } else if (op.id.contains(QLatin1String("hair_flyaway_"))) {
            hasFlyaway = true;
            QCOMPARE(op.layer, QStringLiteral("Lineart"));
            QCOMPARE(op.brush.profile, QStringLiteral("fineliner"));
        } else if (op.id.contains(QLatin1String("hair_ao_"))) {
            hasAo = true;
            QCOMPARE(op.layer, QStringLiteral("Shading"));
            QCOMPARE(op.kind, KisAiStrokeOperation::Kind::Fill);
        }
    }

    QVERIFY2(hasFlow, "Hierarchical hair must emit S-curve flow strands");
    QVERIFY2(hasFlyaway, "Hierarchical hair must emit micro flyaways");
    QVERIFY2(hasAo, "Hierarchical hair must emit valley ambient occlusion");
}

void KisAiV10QualityTest::testJaggedHairHaloHighlight()
{
    const QPointF headCenter(0.5, 0.38);
    const qreal headWidth = 0.33;
    const qreal headHeight = 0.42;
    const QColor hairColor(35, 40, 60);

    const auto haloOps = KisAiStrokeQualityUtils::generateJaggedHairHalo(
        headCenter, headWidth, headHeight, hairColor, QSize(1024, 1024), -0.10, 2, 42);

    QCOMPARE(haloOps.size(), 2);

    for (const auto &op : haloOps) {
        QCOMPARE(op.kind, KisAiStrokeOperation::Kind::Fill);
        QCOMPARE(op.layer, QStringLiteral("Highlights"));
        QCOMPARE(op.blendMode, QStringLiteral("screen"));
        QVERIFY(op.polygon.size() >= 12);

        // Check that y coordinates have notches (not a flat horizontal band)
        qreal minY = 999.0, maxY = -999.0;
        for (const auto &pt : op.polygon) {
            minY = qMin(minY, pt.y());
            maxY = qMax(maxY, pt.y());
        }
        QVERIFY((maxY - minY) > 0.015);
    }
}

void KisAiV10QualityTest::testDetailedAnimeEyeStructure()
{
    const QPointF eyeCenter(0.42, 0.38);
    const QSizeF eyeSize(0.08, 0.10);
    const QColor irisColor(40, 110, 230);
    KisAiStrokeBrush brush;
    brush.color = QColor(25, 20, 35);

    const auto eyeOps = KisAiStrokeQualityUtils::generateDetailedAnimeEyeOps(
        eyeCenter, eyeSize, irisColor, QStringLiteral("radiant_sparkle"), false, QStringLiteral("open"),
        QSize(1024, 1024), brush, 42);

    QVERIFY(eyeOps.size() >= 7);

    QStringList ids;
    for (const auto &op : eyeOps) {
        ids.append(op.id);
    }

    QVERIFY(ids.contains(QStringLiteral("eye_l_sclera")));
    QVERIFY(ids.contains(QStringLiteral("eye_l_sclera_shade")));
    QVERIFY(ids.contains(QStringLiteral("eye_l_iris_base")));
    QVERIFY(ids.contains(QStringLiteral("eye_l_pupil")));
    QVERIFY(ids.contains(QStringLiteral("eye_l_catch_main")));
    QVERIFY(ids.contains(QStringLiteral("eye_l_catch_sub")));
    QVERIFY(ids.contains(QStringLiteral("eye_l_lash_upper")));
    QVERIFY(ids.contains(QStringLiteral("eye_l_crease")));
}

void KisAiV10QualityTest::testOcclusionAndLightingLineWeight()
{
    QVector<KisAiStrokeOperation> ops;

    // Stroke 1: horizontal line pointing right, normal pointing up (facing key light at -0.5, -0.7)
    KisAiStrokeOperation opSun;
    opSun.kind = KisAiStrokeOperation::Kind::Path;
    opSun.layer = QStringLiteral("Lineart");
    opSun.points.append(KisAiStrokePoint(0.30, 0.20, 0.50));
    opSun.points.append(KisAiStrokePoint(0.50, 0.20, 0.50));
    opSun.points.append(KisAiStrokePoint(0.70, 0.20, 0.50));
    ops.append(opSun);

    // Stroke 2: horizontal line pointing left, normal pointing down (away from key light)
    KisAiStrokeOperation opShade;
    opShade.kind = KisAiStrokeOperation::Kind::Path;
    opShade.layer = QStringLiteral("Lineart");
    opShade.points.append(KisAiStrokePoint(0.70, 0.60, 0.50));
    opShade.points.append(KisAiStrokePoint(0.50, 0.60, 0.50));
    opShade.points.append(KisAiStrokePoint(0.30, 0.60, 0.50));
    ops.append(opShade);

    KisAiStrokeQualityUtils::applyOcclusionAndLightingLineWeight(ops, QPointF(0.0, -1.0));

    // Mid points comparison
    const qreal sunPressure = ops.at(0).points.at(1).pressure;
    const qreal shadePressure = ops.at(1).points.at(1).pressure;

    QVERIFY2(sunPressure < 0.50, "Sunlit line segment must be tapered down");
    QVERIFY2(shadePressure > 0.50, "Shadow line segment must be weighted up");
}

void KisAiV10QualityTest::testCornerInkingFillets()
{
    QVector<KisAiStrokeOperation> ops;

    // Acute corner stroke (90 degrees turn)
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.layer = QStringLiteral("Lineart");
    op.brush.size = 0.008;
    op.brush.color = QColor(20, 20, 30);
    op.points.append(KisAiStrokePoint(0.30, 0.30, 0.8));
    op.points.append(KisAiStrokePoint(0.50, 0.50, 0.8));
    op.points.append(KisAiStrokePoint(0.70, 0.30, 0.8));
    ops.append(op);

    const auto fillets = KisAiStrokeQualityUtils::applyCornerInkingFillets(ops, QSize(1024, 1024), 120.0);
    QCOMPARE(fillets.size(), 1);
    QCOMPARE(fillets.at(0).layer, QStringLiteral("Lineart"));
    QCOMPARE(fillets.at(0).kind, KisAiStrokeOperation::Kind::Fill);
    QVERIFY(fillets.at(0).polygon.size() >= 3);
}

void KisAiV10QualityTest::testDiffusionBloomAndAtmosphericFinish()
{
    QImage testImg(128, 128, QImage::Format_ARGB32_Premultiplied);
    testImg.fill(QColor(20, 20, 30));

    // Paint a bright specular spot in the center
    {
        QPainter p(&testImg);
        p.setBrush(QColor(255, 255, 255));
        p.drawEllipse(QPoint(64, 64), 10, 10);
    }

    const QRgb bgBefore = testImg.pixel(64, 50); // 4 pixels above specular spot boundary (64 - 10 = 54)
    QVERIFY(qRed(bgBefore) <= 30);

    KisAiStrokeQualityUtils::applyDiffusionBloom(testImg, 0.70, 0.50, 16);

    const QRgb bgAfter = testImg.pixel(64, 50);
    QVERIFY2(qRed(bgAfter) > qRed(bgBefore), "Bloom must softly glow into adjacent pixels");

    const QImage finished = KisAiStrokeQualityUtils::applyAtmosphericFinish(testImg, 0.20, 0.15, 0.05, 42);
    QVERIFY(!finished.isNull());
    QCOMPARE(finished.size(), testImg.size());
}

void KisAiV10QualityTest::testSceneSpecV3SchemaAndParsing()
{
    const QJsonObject schema = KisAiSceneSpecCodec::sceneSpecJsonSchema();
    QVERIFY(!schema.isEmpty());

    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    QVERIFY(props.contains(QStringLiteral("head")));
    QVERIFY(props.contains(QStringLiteral("light")));
    QVERIFY(props.contains(QStringLiteral("finish")));

    // Test parsing a full SceneSpec v3 JSON
    const QString jsonText = QStringLiteral(
        "{\n"
        "  \"prompt\": \"A twin-tailed girl winking in dramatic backlight\",\n"
        "  \"head\": {\n"
        "    \"expression\": \"wink_left\",\n"
        "    \"hair_style\": \"twin_tails\",\n"
        "    \"hair_bangs\": \"see_through\",\n"
        "    \"hair_volume\": 0.75,\n"
        "    \"hair_flyaway\": 0.40,\n"
        "    \"blush_intensity\": 0.80,\n"
        "    \"eye_highlight_style\": \"radiant_sparkle\"\n"
        "  },\n"
        "  \"light\": {\n"
        "    \"lighting_style\": \"dramatic_backlight\",\n"
        "    \"rim_intensity\": 0.70,\n"
        "    \"sss_strength\": 0.65\n"
        "  },\n"
        "  \"finish\": {\n"
        "    \"bloom_strength\": 0.30,\n"
        "    \"grain_intensity\": 0.08,\n"
        "    \"tone_mood\": \"cinematic_warm\"\n"
        "  }\n"
        "}");

    KisAiSceneSpec spec;
    QString err;
    QStringList warnings;
    const bool ok = KisAiSceneSpecCodec::parseSceneSpec(jsonText.toUtf8(), &spec, &err, &warnings);
    QVERIFY2(ok, qPrintable(err));

    QCOMPARE(spec.head.expression, QStringLiteral("wink_left"));
    QCOMPARE(spec.head.hairStyle, QStringLiteral("twin_tails"));
    QCOMPARE(spec.head.hairBangs, QStringLiteral("see_through"));
    QCOMPARE(spec.head.hairVolume, 0.75);
    QCOMPARE(spec.head.hairFlyaway, 0.40);
    QCOMPARE(spec.head.blushIntensity, 0.80);
    QCOMPARE(spec.head.eyeHighlightStyle, QStringLiteral("radiant_sparkle"));

    QCOMPARE(spec.light.lightingStyle, QStringLiteral("dramatic_backlight"));
    QCOMPARE(spec.light.rimIntensity, 0.70);
    QCOMPARE(spec.light.sssStrength, 0.65);

    QCOMPARE(spec.finish.bloomStrength, 0.30);
    QCOMPARE(spec.finish.grainIntensity, 0.08);
    QCOMPARE(spec.finish.toneMood, QStringLiteral("cinematic_warm"));
}

void KisAiV10QualityTest::testPromptAnalyzerV10Heuristics()
{
    const QString prompt = QStringLiteral("A beautiful girl with ponytail, winking shyly under dappled komorebi light, dramatic backlight");
    const KisAiSceneSpec spec = KisAiSceneSpecCodec::defaultSpecForPrompt(prompt, QSize(1024, 1024));

    QCOMPARE(spec.head.hairStyle, QStringLiteral("pony_tail"));
    QCOMPARE(spec.head.expression, QStringLiteral("wink_left"));
    QCOMPARE(spec.light.lightingStyle, QStringLiteral("dramatic_backlight"));
    QVERIFY(spec.light.rimIntensity >= 0.70);
}

KISTEST_MAIN(KisAiV10QualityTest)
