/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeProgramTest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiPromptAnalyzer.h"
#include "aiillustration/KisAiStrokeTypeChecker.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiLayoutEngine.h"
#include "aiillustration/KisAiLightRig.h"
#include "aiillustration/KisAiRigLibrary.h"
#include "aiillustration/KisAiStrokeQualityUtils.h"
#include "KisAiTestUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>

void KisAiStrokeProgramTest::testSanitizeAndExtractJson()
{
    // Test 1: plain valid JSON
    const QString plain = QStringLiteral("{\"schema_version\": 2, \"operations\": []}");
    QCOMPARE(KisAiStrokeProgramCodec::sanitizeAndExtractJson(plain), plain);

    // Test 2: markdown code fence
    const QString markdown = QStringLiteral("Here is your plan:\n```json\n{\"schema_version\": 2, \"operations\": []}\n```\nDone.");
    QCOMPARE(KisAiStrokeProgramCodec::sanitizeAndExtractJson(markdown), plain);

    // Test 3: thinking tokens
    const QString thinking = QStringLiteral("<think>I should plan layers first.</think>\n{\"schema_version\": 2, \"operations\": []}");
    QCOMPARE(KisAiStrokeProgramCodec::sanitizeAndExtractJson(thinking), plain);

    // Test 4: conversational preamble and postamble without codeblock, with single-quote syntax repair
    const QString noisyWithPostamble = QStringLiteral("Here is the JSON:\n{'schema_version': 2, 'operations': []}\nHope you like it!");
    QCOMPARE(KisAiStrokeProgramCodec::sanitizeAndExtractJson(noisyWithPostamble), plain);
}

void KisAiStrokeProgramTest::testParseValidProgram()
{
    const QString jsonText = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"prompt\": \"A lovely flower\",\n"
        "  \"title\": \"Flower Test\",\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"petal_base\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"polygon\": [[0.4, 0.4], [0.6, 0.4], [0.5, 0.7]],\n"
        "      \"brush\": {\"profile\": \"brush\", \"color\": \"#ff4488\", \"size\": 0.02, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"petal_outline\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.4, 0.4, 0.8], [0.6, 0.4, 0.9], [0.5, 0.7, 0.5]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#221122\", \"size\": 0.005, \"is_eraser\": false}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram program;
    QString error;
    const bool ok = KisAiStrokeProgramCodec::parseResponse(jsonText.toUtf8(), &program, &error);

    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(program.schemaVersion, 2);
    QCOMPARE(program.title, QStringLiteral("Flower Test"));
    QCOMPARE(program.operations.size(), 2);

    const KisAiStrokeOperation &op0 = program.operations.at(0);
    QCOMPARE(op0.kind, KisAiStrokeOperation::Kind::Fill);
    QCOMPARE(op0.layer, QStringLiteral("Flats"));
    QCOMPARE(op0.polygon.size(), 3);

    const KisAiStrokeOperation &op1 = program.operations.at(1);
    QCOMPARE(op1.kind, KisAiStrokeOperation::Kind::Path);
    QCOMPARE(op1.layer, QStringLiteral("Lineart"));
    QCOMPARE(op1.points.size(), 3);
}

void KisAiStrokeProgramTest::testBuildChatCompletionsPayload()
{
    const QJsonObject payload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("Landscape with mountain"),
        QSize(1024, 768),
        300
    );

    QCOMPARE(payload.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-4o"));

    const QJsonArray messages = payload.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 2);

    const QString systemPrompt = messages.at(0).toObject().value(QStringLiteral("content")).toString();
    QVERIFY(systemPrompt.contains(QStringLiteral("Flats")));
    QVERIFY(systemPrompt.contains(QStringLiteral("Shading")));
    QVERIFY(systemPrompt.contains(QStringLiteral("Lineart")));
    QVERIFY(systemPrompt.contains(QStringLiteral("MASTER DRAWING WORKFLOW")));
    QVERIFY(systemPrompt.contains(QStringLiteral("silently audit"), Qt::CaseInsensitive));

    const QJsonObject userRequest = QJsonDocument::fromJson(
        messages.at(1).toObject().value(QStringLiteral("content")).toString().toUtf8()
    ).object();
    QCOMPARE(userRequest.value(QStringLiteral("geometry_budget")).toInt(), 300);
    QCOMPARE(userRequest.value(QStringLiteral("operation_target")).toInt(), 20);
    QVERIFY(userRequest.value(QStringLiteral("budget_allocation")).isObject());
    QCOMPARE(payload.value(QStringLiteral("stream")).toBool(), true);
}

void KisAiStrokeProgramTest::testStrokeProgramJsonSchema()
{
    const QJsonObject schema = KisAiStrokeProgramCodec::strokeProgramJsonSchema();
    QCOMPARE(schema.value(QStringLiteral("type")).toString(), QStringLiteral("object"));

    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    QVERIFY(props.contains(QStringLiteral("schema_version")));
    QVERIFY(props.contains(QStringLiteral("operations")));

    const QJsonObject opSchema = props.value(QStringLiteral("operations")).toObject()
        .value(QStringLiteral("items")).toObject();
    const QJsonObject opProps = opSchema.value(QStringLiteral("properties")).toObject();
    QCOMPARE(opProps.value(QStringLiteral("center")).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("array"));
    QVERIFY(opProps.contains(QStringLiteral("width_start")));
    QVERIFY(opProps.contains(QStringLiteral("width_mid")));
    QVERIFY(opProps.contains(QStringLiteral("width_end")));
    QVERIFY(opProps.contains(QStringLiteral("smooth")));
    QCOMPARE(props.value(QStringLiteral("operations")).toObject().value(QStringLiteral("minItems")).toInt(), 1);
}

void KisAiStrokeProgramTest::testTruncatedJsonRecovery()
{
    // Simulates an LLM reaching max_tokens mid-stream during generation
    const QString truncatedAssistantContent = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"stroke_1\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.1, 0.1, 1.0], [0.2, 0.2, 1.0]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#111111\", \"size\": 0.01}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"stroke_2\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.3, 0.3, 1.0], [0.4"
    );

    const QJsonObject chatPayload {
        {QStringLiteral("choices"), QJsonArray{
            QJsonObject{
                {QStringLiteral("message"), QJsonObject{
                    {QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"), truncatedAssistantContent}
                }}
            }
        }}
    };

    const QByteArray responseData = QJsonDocument(chatPayload).toJson(QJsonDocument::Compact);
    KisAiStrokeProgram program;
    QString error;
    const bool parsed = KisAiStrokeProgramCodec::parseResponse(responseData, &program, &error);

    QVERIFY2(parsed, qPrintable(error));
    // Stroke 1 should be successfully salvaged
    QCOMPARE(program.operations.size(), 1);
    QCOMPARE(program.operations.first().id, QStringLiteral("stroke_1"));
}

void KisAiStrokeProgramTest::testLayerAndKindAliases()
{
    const QString jsonText = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"particle\",\n"
        "      \"id\": \"stars\",\n"
        "      \"layer\": \"Shadows\",\n"
        "      \"polygon\": [[0.1, 0.1], [0.9, 0.9]],\n"
        "      \"brush\": {\"profile\": \"spray\", \"color\": \"#ffffff\", \"size\": 0.05}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"line\",\n"
        "      \"id\": \"outline\",\n"
        "      \"layer\": \"Lines\",\n"
        "      \"points\": [[0.2, 0.2, 1.0], [0.8, 0.8, 1.0]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#000000\", \"size\": 0.01}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"bg\",\n"
        "      \"layer\": \"Flat\",\n"
        "      \"polygon\": [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]],\n"
        "      \"brush\": {\"profile\": \"flat\", \"color\": \"#aabbcc\", \"size\": 0.02}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram program;
    QString error;
    const QJsonObject root = QJsonDocument::fromJson(jsonText.toUtf8()).object();
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
    QCOMPARE(program.operations.size(), 3);

    // Kind::Particles alias and Shadows -> Shading alias
    QCOMPARE(program.operations[0].kind, KisAiStrokeOperation::Kind::Particles);
    QCOMPARE(program.operations[0].layer, QStringLiteral("Shading"));

    // Kind::Path alias ("line") and Lines -> Lineart alias
    QCOMPARE(program.operations[1].kind, KisAiStrokeOperation::Kind::Path);
    QCOMPARE(program.operations[1].layer, QStringLiteral("Lineart"));

    // Flat -> Flats alias
    QCOMPARE(program.operations[2].layer, QStringLiteral("Flats"));
}

void KisAiStrokeProgramTest::testObjectPointParsing()
{
    const QString jsonText = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"stroke_obj\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [\n"
        "        {\"x\": 0.2, \"y\": 0.4, \"pressure\": 0.85},\n"
        "        {\"x\": 0.6, \"y\": 0.8, \"pressure\": 0.95}\n"
        "      ],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#333333\", \"size\": 0.01}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram program;
    QString error;
    const QJsonObject root = QJsonDocument::fromJson(jsonText.toUtf8()).object();
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
    QCOMPARE(program.operations.size(), 1);
    const auto &pts = program.operations[0].points;
    QCOMPARE(pts.size(), 2);
    QCOMPARE(pts[0].pos, QPointF(0.2, 0.4));
    QCOMPARE(pts[0].pressure, 0.85);
    QCOMPARE(pts[1].pos, QPointF(0.6, 0.8));
    QCOMPARE(pts[1].pressure, 0.95);
}

void KisAiStrokeProgramTest::testPixelCoordinateAutoNormalization()
{
    // Points given in pixel coordinates (> 1.5) on a 1000x1000 canvas
    const QString jsonText = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"canvas\": {\"width\": 1000, \"height\": 1000},\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"pixel_stroke\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[250, 500, 1.0], [750, 500, 0.8]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#000000\", \"size\": 10}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram program;
    QString error;
    const QJsonObject root = QJsonDocument::fromJson(jsonText.toUtf8()).object();
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
    QCOMPARE(program.operations.size(), 1);
    const auto &pts = program.operations[0].points;
    QCOMPARE(pts.size(), 2);
    // 250 / 1000 = 0.25, 500 / 1000 = 0.5
    QCOMPARE(pts[0].pos.x(), 0.25);
    QCOMPARE(pts[0].pos.y(), 0.5);
    QCOMPARE(pts[1].pos.x(), 0.75);
    QCOMPARE(pts[1].pos.y(), 0.5);
    // Brush size 10 was in ratio mode -> auto-converted to px mode
    QCOMPARE(program.operations[0].brush.sizeMode, QStringLiteral("px"));
}

void KisAiStrokeProgramTest::testColorAndBrushParsing()
{
    const QString jsonText = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"css_rgb\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.1, 0.1, 1.0], [0.9, 0.9, 1.0]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"rgb(255, 0, 128)\", \"size\": 0.01}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"hex8\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.2, 0.2, 1.0], [0.8, 0.8, 1.0]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#ff008080\", \"size\": 0.01}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram program;
    QString error;
    const QJsonObject root = QJsonDocument::fromJson(jsonText.toUtf8()).object();
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
    QCOMPARE(program.operations.size(), 2);

    // CSS rgb(255, 0, 128)
    QCOMPARE(program.operations[0].brush.color.red(), 255);
    QCOMPARE(program.operations[0].brush.color.green(), 0);
    QCOMPARE(program.operations[0].brush.color.blue(), 128);

    // Hex8 #ff008080
    QCOMPARE(program.operations[1].brush.color.red(), 255);
    QCOMPARE(program.operations[1].brush.color.green(), 0);
    QCOMPARE(program.operations[1].brush.color.blue(), 128);
    QCOMPARE(program.operations[1].brush.color.alpha(), 128);

    // 4-digit hex RGBA tests (#RGBA and RGBA)
    const QColor hex4WithHash = KisAiStrokeProgramCodec::parseColor(QStringLiteral("#f08c"));
    QCOMPARE(hex4WithHash.red(), 0xff);
    QCOMPARE(hex4WithHash.green(), 0x00);
    QCOMPARE(hex4WithHash.blue(), 0x88);
    QCOMPARE(hex4WithHash.alpha(), 0xcc);

    const QColor hex4NoHash = KisAiStrokeProgramCodec::parseColor(QStringLiteral("f08c"));
    QCOMPARE(hex4NoHash.red(), 0xff);
    QCOMPARE(hex4NoHash.green(), 0x00);
    QCOMPARE(hex4NoHash.blue(), 0x88);
    QCOMPARE(hex4NoHash.alpha(), 0xcc);

    // TypeChecker color validation with 4-digit RGBA hex
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("#f08c")));
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("f08c")));
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("#ff0080")));
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("ff0080")));

    // TypeChecker checkBrushObject color normalization for 4-char hex
    QJsonObject brushObj;
    brushObj[QStringLiteral("profile")] = QStringLiteral("gpen");
    brushObj[QStringLiteral("color")] = QStringLiteral("f08c");
    int coerced = 0;
    QVERIFY(KisAiStrokeTypeChecker::checkBrushObject(&brushObj, nullptr, &coerced));
    QCOMPARE(brushObj.value(QStringLiteral("color")).toString(), QStringLiteral("#f08c"));
}

void KisAiStrokeProgramTest::testStableSeed()
{
    const quint32 seedA1 = KisAiStrokeProgramCodec::stableSeed(QStringLiteral("Sunset over Tokyo"));
    const quint32 seedA2 = KisAiStrokeProgramCodec::stableSeed(QStringLiteral("Sunset over Tokyo"));
    const quint32 seedB = KisAiStrokeProgramCodec::stableSeed(QStringLiteral("Cyberpunk city rain"));

    QCOMPARE(seedA1, seedA2);
    QVERIFY(seedA1 != seedB);
    QVERIFY(seedA1 > 0);
}

void KisAiStrokeProgramTest::testPromptAnalyzerDomainClassification()
{
    const QSize size(1024, 1024);

    // 1. Character
    const auto specChar = KisAiPromptAnalyzer::analyze(QStringLiteral("anime girl portrait with blue eyes and silver hair"), size);
    QCOMPARE(static_cast<int>(specChar.domain), static_cast<int>(KisAiPromptAnalyzer::DomainType::Character));
    QCOMPARE(specChar.eyeColor, QStringLiteral("#3884ff"));
    QCOMPARE(specChar.hairColor, QStringLiteral("#e0e4f0"));

    // 2. Landscape with Sunset & Sakura
    const auto specLand = KisAiPromptAnalyzer::analyze(QStringLiteral("sunset mountain with sakura trees"), size);
    QCOMPARE(static_cast<int>(specLand.domain), static_cast<int>(KisAiPromptAnalyzer::DomainType::Landscape));
    QCOMPARE(static_cast<int>(specLand.timeOfDay), static_cast<int>(KisAiPromptAnalyzer::TimeOfDay::Sunset));
    QVERIFY(specLand.hasSakura);
    QVERIFY(!specLand.skyGradientColors.isEmpty());

    // 3. Cyberpunk
    const auto specCyber = KisAiPromptAnalyzer::analyze(QStringLiteral("cyberpunk city skyline with neon"), size);
    QCOMPARE(static_cast<int>(specCyber.domain), static_cast<int>(KisAiPromptAnalyzer::DomainType::Cyberpunk));

    // 4. Creature
    const auto specCat = KisAiPromptAnalyzer::analyze(QStringLiteral("mystical black cat with gold eyes"), size);
    QCOMPARE(static_cast<int>(specCat.domain), static_cast<int>(KisAiPromptAnalyzer::DomainType::Creature));
    QCOMPARE(specCat.eyeColor, QStringLiteral("#f4a261"));

    // 5. Botanical
    const auto specRose = KisAiPromptAnalyzer::analyze(QStringLiteral("delicate rose bouquet with green leaves"), size);
    QCOMPARE(static_cast<int>(specRose.domain), static_cast<int>(KisAiPromptAnalyzer::DomainType::Botanical));
}

void KisAiStrokeProgramTest::testHatchOperationParsing()
{
    const QString json = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"hatch\",\n"
        "      \"id\": \"shading_test\",\n"
        "      \"layer\": \"Shading\",\n"
        "      \"angle_deg\": 60,\n"
        "      \"spacing\": 0.025,\n"
        "      \"cross_hatch\": true,\n"
        "      \"polygon\": [[0.1, 0.1], [0.4, 0.1], [0.4, 0.4], [0.1, 0.4]],\n"
        "      \"brush\": {\"profile\": \"pencil\", \"color\": \"#2a2a2a\", \"size\": 0.003, \"is_eraser\": false}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram program;
    QString error;
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(QJsonDocument::fromJson(json.toUtf8()).object(), &program, &error));
    QCOMPARE(program.operations.size(), 1);

    const auto &op = program.operations.first();
    QCOMPARE(static_cast<int>(op.kind), static_cast<int>(KisAiStrokeOperation::Kind::Hatch));
    QCOMPARE(op.layer, QStringLiteral("Shading"));
    QCOMPARE(op.angleDeg, 60.0);
    QCOMPARE(op.spacing, 0.025);
    QVERIFY(op.crossHatch);
    QCOMPARE(op.polygon.size(), 4);
    QCOMPARE(op.brush.profile, QStringLiteral("pencil"));
}

void KisAiStrokeProgramTest::testProceduralCharacterGeneration()
{
    const QSize size(1024, 1024);
    const KisAiStrokeProgram program = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("anime girl portrait with blue eyes and silver hair"),
        size
    );

    QVERIFY(program.isValid());
    QVERIFY(program.operations.size() >= 10);

    QSet<QString> layers;
    bool hasHatch = false;
    for (const auto &op : program.operations) {
        layers.insert(op.layer);
        if (op.kind == KisAiStrokeOperation::Kind::Hatch) {
            hasHatch = true;
        }
    }

    QVERIFY(layers.contains(QStringLiteral("Flats")));
    QVERIFY(layers.contains(QStringLiteral("Shading")));
    QVERIFY(layers.contains(QStringLiteral("Lineart")));
    QVERIFY(layers.contains(QStringLiteral("Highlights")));
    QVERIFY(layers.contains(QStringLiteral("FX")));
    QVERIFY(hasHatch);
}

void KisAiStrokeProgramTest::testNormalizeLayerName()
{
    // Background aliases
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("background")), QStringLiteral("Background"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("bg")), QStringLiteral("Background"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("backdrop")), QStringLiteral("Background"));

    // Flats aliases
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("flat")), QStringLiteral("Flats"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("flats")), QStringLiteral("Flats"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("Base")), QStringLiteral("Flats"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("color")), QStringLiteral("Flats"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("colors")), QStringLiteral("Flats"));

    // Shading aliases
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("shading")), QStringLiteral("Shading"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("shade")), QStringLiteral("Shading"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("shadow")), QStringLiteral("Shading"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("Shadows")), QStringLiteral("Shading"));

    // Lineart aliases
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("lineart")), QStringLiteral("Lineart"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("line_art")), QStringLiteral("Lineart"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("line art")), QStringLiteral("Lineart"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("lines")), QStringLiteral("Lineart"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("ink")), QStringLiteral("Lineart"));

    // Highlights aliases
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("highlight")), QStringLiteral("Highlights"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("highlights")), QStringLiteral("Highlights"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("specular")), QStringLiteral("Highlights"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("glint")), QStringLiteral("Highlights"));

    // FX aliases
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("fx")), QStringLiteral("FX"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("effects")), QStringLiteral("FX"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("particles")), QStringLiteral("FX"));

    // Empty and custom
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("")), QStringLiteral("Lineart"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("   ")), QStringLiteral("Lineart"));
    QCOMPARE(KisAiStrokeProgramCodec::normalizeLayerName(QStringLiteral("SpecialOverlay")), QStringLiteral("SpecialOverlay"));
}

void KisAiStrokeProgramTest::testCountLayerOperationsAndFormatSummary()
{
    KisAiStrokeProgram program;
    auto addOp = [&program](const QString &layer) {
        KisAiStrokeOperation op;
        op.layer = layer;
        program.operations.append(op);
    };

    addOp(QStringLiteral("flat"));
    addOp(QStringLiteral("background"));
    addOp(QStringLiteral("shading"));
    addOp(QStringLiteral("shadow"));
    addOp(QStringLiteral("lines"));
    addOp(QStringLiteral("glint"));
    addOp(QStringLiteral("particles"));

    const QMap<QString, int> counts = KisAiStrokeProgramCodec::countLayerOperations(program);
    QCOMPARE(counts.value(QStringLiteral("Background")), 1);
    QCOMPARE(counts.value(QStringLiteral("Flats")), 1);
    QCOMPARE(counts.value(QStringLiteral("Shading")), 2);
    QCOMPARE(counts.value(QStringLiteral("Lineart")), 1);
    QCOMPARE(counts.value(QStringLiteral("Highlights")), 1);
    QCOMPARE(counts.value(QStringLiteral("FX")), 1);

    const QString summary = KisAiStrokeProgramCodec::formatLayerSummary(program);
    QCOMPARE(summary, QStringLiteral("Background: 1, Flats: 1, Shading: 2, Lineart: 1, Highlights: 1, FX: 1"));
}

void KisAiStrokeProgramTest::testRefineForRenderingRepairsModelGeometry()
{
    KisAiStrokeProgram input;
    input.canvasSize = QSize(1000, 800);

    KisAiStrokeOperation path;
    path.kind = KisAiStrokeOperation::Kind::Path;
    path.layer = QStringLiteral("ink");
    path.brush.profile = QStringLiteral("auto");
    path.brush.color = Qt::black;
    path.brush.size = -4.0;
    path.brush.opacity = 2.0;
    path.points = {
        KisAiStrokePoint(-0.2, 0.2, -1.0),
        KisAiStrokePoint(-0.2, 0.2, 0.7),
        KisAiStrokePoint(1.4, 0.8, 4.0)
    };
    input.operations.append(path);

    KisAiStrokeOperation degenerateFill;
    degenerateFill.kind = KisAiStrokeOperation::Kind::Fill;
    degenerateFill.layer = QStringLiteral("Flats");
    degenerateFill.polygon = {QPointF(0.1, 0.1), QPointF(0.2, 0.2), QPointF(0.3, 0.3)};
    input.operations.append(degenerateFill);

    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(input, &report);
    QCOMPARE(report.inputOperations, 2);
    QCOMPARE(report.outputOperations, 1);
    QCOMPARE(report.droppedOperations, 1);
    QVERIFY(report.repairedValues >= 5);
    QVERIFY(report.deduplicatedPoints >= 1);

    const KisAiStrokeOperation &fixed = refined.operations.first();
    QCOMPARE(fixed.layer, QStringLiteral("Lineart"));
    QCOMPARE(fixed.brush.profile, QStringLiteral("gpen"));
    QCOMPARE(fixed.points.size(), 2);
    QCOMPARE(fixed.points.first().pos, QPointF(0.0, 0.2));
    QCOMPARE(fixed.points.last().pos, QPointF(1.0, 0.8));
    QVERIFY(fixed.points.first().pressure >= 0.05);
    QVERIFY(fixed.brush.size > 0.0);
    QCOMPARE(fixed.brush.opacity, 1.0);
    QVERIFY(fixed.brush.color != QColor(Qt::black));
}

void KisAiStrokeProgramTest::testStructuralQualityScore()
{
    KisAiStrokeProgram sparse;
    KisAiStrokeOperation line;
    line.kind = KisAiStrokeOperation::Kind::Path;
    line.layer = QStringLiteral("Lineart");
    line.points = {KisAiStrokePoint(0.45, 0.45), KisAiStrokePoint(0.55, 0.55)};
    sparse.operations.append(line);

    KisAiStrokeProgram rich = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("sunset mountain landscape with sakura"), QSize(1024, 768));

    const qreal sparseScore = KisAiStrokeProgramCodec::qualityScore(sparse);
    const qreal richScore = KisAiStrokeProgramCodec::qualityScore(rich);
    QVERIFY(sparseScore >= 0.0 && sparseScore <= 1.0);
    QVERIFY(richScore >= 0.0 && richScore <= 1.0);
    QVERIFY2(richScore > sparseScore + 0.25, qPrintable(QStringLiteral("sparse=%1 rich=%2").arg(sparseScore).arg(richScore)));
    QCOMPARE(rich.completionScore, richScore);
}

void KisAiStrokeProgramTest::testParseGeometrySafetyLimits()
{
    QJsonArray points;
    for (int i = 0; i < 257; ++i) {
        points.append(QJsonArray{qreal(i) / 256.0, 0.5, 0.8});
    }

    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("path")},
                {QStringLiteral("id"), QStringLiteral("too_many_points")},
                {QStringLiteral("layer"), QStringLiteral("Lineart")},
                {QStringLiteral("points"), points},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("profile"), QStringLiteral("gpen")},
                    {QStringLiteral("color"), QStringLiteral("#222222")},
                    {QStringLiteral("size"), 0.01},
                    {QStringLiteral("is_eraser"), false},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY(!KisAiStrokeProgramCodec::parseResponse(QJsonDocument(root).toJson(QJsonDocument::Compact), &program, &error));
    QVERIFY(error.contains(QStringLiteral("上限")));
}

void KisAiStrokeProgramTest::testParticleCountClamping()
{
    // A hostile or confused particle count must be clamped instead of aborting
    // the whole parse: the renderer clamps again at rasterization time, so an
    // absurd `count` for one operation must not discard the remaining ops.
    auto makeProgram = [](int count) {
        return QJsonDocument(QJsonObject {
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("operations"), QJsonArray {
                QJsonObject {
                    {QStringLiteral("kind"), QStringLiteral("particles")},
                    {QStringLiteral("id"), QStringLiteral("hostile_particles")},
                    {QStringLiteral("layer"), QStringLiteral("FX")},
                    {QStringLiteral("bounds"), QJsonArray {0.1, 0.1, 0.9, 0.9}},
                    {QStringLiteral("count"), count},
                    {QStringLiteral("brush"), QJsonObject {
                        {QStringLiteral("profile"), QStringLiteral("brush")},
                        {QStringLiteral("color"), QStringLiteral("#331122")},
                        {QStringLiteral("size"), 0.005},
                        {QStringLiteral("is_eraser"), false},
                    }},
                },
                QJsonObject {
                    {QStringLiteral("kind"), QStringLiteral("path")},
                    {QStringLiteral("id"), QStringLiteral("after_hostile_op")},
                    {QStringLiteral("layer"), QStringLiteral("Lineart")},
                    {QStringLiteral("points"), QJsonArray {
                        QJsonArray {0.2, 0.2, 0.8},
                        QJsonArray {0.8, 0.8, 0.7},
                    }},
                    {QStringLiteral("brush"), QJsonObject {
                        {QStringLiteral("profile"), QStringLiteral("gpen")},
                        {QStringLiteral("color"), QStringLiteral("#222222")},
                        {QStringLiteral("size"), 0.005},
                        {QStringLiteral("is_eraser"), false},
                    }},
                },
            }},
        }).toJson(QJsonDocument::Compact);
    };

    KisAiStrokeProgram program;
    QString error;
    // Count far above the per-operation cap (200): parse must succeed and clamp.
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(makeProgram(1000000), &program, &error), qPrintable(error));
    QCOMPARE(program.operations.size(), 2);
    auto particlesIt = std::find_if(program.operations.cbegin(), program.operations.cend(),
                                          [](const KisAiStrokeOperation &op) {
                                              return op.kind == KisAiStrokeOperation::Kind::Particles;
                                          });
    QVERIFY(particlesIt != program.operations.cend());
    QVERIFY2(particlesIt->particleCount >= 1 && particlesIt->particleCount <= 200,
             qPrintable(QString::number(particlesIt->particleCount)));

    // Zero and negative counts are also clamped to the valid range.
    program = KisAiStrokeProgram();
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(makeProgram(0), &program, &error), qPrintable(error));
    particlesIt = std::find_if(program.operations.cbegin(), program.operations.cend(),
                               [](const KisAiStrokeOperation &op) {
                                   return op.kind == KisAiStrokeOperation::Kind::Particles;
                               });
    QVERIFY(particlesIt != program.operations.cend());
    QCOMPARE(particlesIt->particleCount, 1);

    program = KisAiStrokeProgram();
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(makeProgram(-50), &program, &error), qPrintable(error));
    particlesIt = std::find_if(program.operations.cbegin(), program.operations.cend(),
                               [](const KisAiStrokeOperation &op) {
                                   return op.kind == KisAiStrokeOperation::Kind::Particles;
                               });
    QVERIFY(particlesIt != program.operations.cend());
    QCOMPARE(particlesIt->particleCount, 1);

    // The total particle budget (4096) caps the sum across many operations
    // instead of failing the parse.
    QJsonArray manyOps;
    for (int i = 0; i < 30; ++i) {
        manyOps.append(QJsonObject {
            {QStringLiteral("kind"), QStringLiteral("particles")},
            {QStringLiteral("id"), QString("budget_particles_%1").arg(i)},
            {QStringLiteral("layer"), QStringLiteral("FX")},
            {QStringLiteral("bounds"), QJsonArray {0.1, 0.1, 0.9, 0.9}},
            {QStringLiteral("count"), 200},
            {QStringLiteral("brush"), QJsonObject {
                {QStringLiteral("profile"), QStringLiteral("brush")},
                {QStringLiteral("color"), QStringLiteral("#331122")},
                {QStringLiteral("size"), 0.005},
                {QStringLiteral("is_eraser"), false},
            }},
        });
    }
    const QJsonObject budgetRoot {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), manyOps},
    };

    program = KisAiStrokeProgram();
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(QJsonDocument(budgetRoot).toJson(QJsonDocument::Compact), &program, &error), qPrintable(error));
    // V3 Phase 0.1: per-program particle operation cap applies on top of the
    // total particle budget; excess operations are dropped at refine time.
    QCOMPARE(program.operations.size(), KisAiStrokeProgramCodec::maxParticlesOperations());

    int totalParticles = 0;
    for (const KisAiStrokeOperation &op : program.operations) {
        QVERIFY(op.particleCount >= 0);
        totalParticles += op.particleCount;
    }
    QVERIFY2(totalParticles <= 4096,
             qPrintable(QString::number(totalParticles)));
}

void KisAiStrokeProgramTest::testGradientDirectionPointsParsing()
{
    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("gradient_fill")},
                {QStringLiteral("id"), QStringLiteral("directed_gradient")},
                {QStringLiteral("layer"), QStringLiteral("Flats")},
                {QStringLiteral("points"), QJsonArray {
                    QJsonArray {0.1, 0.2},
                    QJsonArray {0.9, 0.8},
                }},
                {QStringLiteral("colors"), QJsonArray {QStringLiteral("#001122"), QStringLiteral("#ddeeff")}},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("profile"), QStringLiteral("brush")},
                    {QStringLiteral("color"), QStringLiteral("#001122")},
                    {QStringLiteral("size"), 0.02},
                    {QStringLiteral("is_eraser"), false},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(QJsonDocument(root).toJson(QJsonDocument::Compact), &program, &error), qPrintable(error));
    QCOMPARE(program.operations.size(), 1);
    QCOMPARE(program.operations.first().points.size(), 2);
    QCOMPARE(program.operations.first().points.first().pos, QPointF(0.1, 0.2));
    QCOMPARE(program.operations.first().points.last().pos, QPointF(0.9, 0.8));

    // Verify refineForRendering clamps GradientFill points
    program.operations.first().points = {
        KisAiStrokePoint(-0.4, 0.2),
        KisAiStrokePoint(1.5, 2.0)
    };
    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program, &report);
    QCOMPARE(refined.operations.size(), 1);
    QCOMPARE(refined.operations.first().points.size(), 2);
    QCOMPARE(refined.operations.first().points.first().pos, QPointF(0.0, 0.2));
    QCOMPARE(refined.operations.first().points.last().pos, QPointF(1.0, 1.0));
}

void KisAiStrokeProgramTest::testMangaLinesParsingAndRefinement()
{
    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("manga_lines")},
                {QStringLiteral("id"), QStringLiteral("speed_burst")},
                {QStringLiteral("layer"), QStringLiteral("FX")},
                {QStringLiteral("center"), QJsonArray {0.5, 0.45}},
                {QStringLiteral("inner_radius"), 0.12},
                {QStringLiteral("outer_radius"), 0.85},
                {QStringLiteral("density"), 56},
                {QStringLiteral("line_length_jitter"), 0.25},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("profile"), QStringLiteral("gpen")},
                    {QStringLiteral("color"), QStringLiteral("#111111")},
                    {QStringLiteral("size"), 0.003},
                    {QStringLiteral("is_eraser"), false},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(QJsonDocument(root).toJson(QJsonDocument::Compact), &program, &error), qPrintable(error));
    QCOMPARE(program.operations.size(), 1);

    const auto &op = program.operations.first();
    QCOMPARE(static_cast<int>(op.kind), static_cast<int>(KisAiStrokeOperation::Kind::MangaLines));
    QCOMPARE(op.layer, QStringLiteral("FX"));
    QCOMPARE(op.gradientCenter, QPointF(0.5, 0.45));
    QCOMPARE(op.innerRadius, 0.12);
    QCOMPARE(op.outerRadius, 0.85);
    QCOMPARE(op.density, 56);
    QCOMPARE(op.lineLengthJitter, 0.25);
    QVERIFY(program.completionScore > 0.0);
}

void KisAiStrokeProgramTest::testGoalModePayloadAndVisionModelDetection()
{
    // 1. Model vision capability: hardcoded whitelist is removed, non-empty model names
    // are assumed vision-capable (as Vision capability is a requirement for Goal mode),
    // with automatic fallback to text-only when requested.
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("gpt-4o")));
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("claude-3-5-sonnet")));
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("my-future-llm-v5")));
    QVERIFY(!KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("")));
    QVERIFY(!KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("   ")));

    // 2. Goal Step Payload without image (step 1 or when image is empty)
    const QSize canvasSize(1024, 1024);
    const QJsonObject textPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("any-model"),
        QStringLiteral("cyberpunk samurai"),
        canvasSize,
        1,
        4
    );
    QCOMPARE(textPayload.value(QStringLiteral("model")).toString(), QStringLiteral("any-model"));
    const QJsonArray msgs1 = textPayload.value(QStringLiteral("messages")).toArray();
    QCOMPARE(msgs1.size(), 2);
    QVERIFY(msgs1.at(1).toObject().value(QStringLiteral("content")).isString());

    // 3. Goal Step Payload with image (vision model)
    const QString fakeB64 = QStringLiteral("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==");
    const QJsonObject visionPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("cyberpunk samurai"),
        canvasSize,
        2,
        4,
        fakeB64,
        QStringLiteral("Make shadows deeper under the jaw")
    );
    const QJsonArray msgs2 = visionPayload.value(QStringLiteral("messages")).toArray();
    QCOMPARE(msgs2.size(), 2);
    QVERIFY(msgs2.at(1).toObject().value(QStringLiteral("content")).isArray());
    const QJsonArray contentParts = msgs2.at(1).toObject().value(QStringLiteral("content")).toArray();
    QCOMPARE(contentParts.size(), 2);
    QCOMPARE(contentParts.at(0).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("text"));
    QCOMPARE(contentParts.at(1).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("image_url"));
    const QString imgUrl = contentParts.at(1).toObject().value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString();
    QVERIFY(imgUrl.startsWith(QStringLiteral("data:image/jpeg;base64,")));

    // 4. Fallback: when includeVision is false, even if imageBase64 is provided, payload falls back to text-only
    const QJsonObject fallbackPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("cyberpunk samurai"),
        canvasSize,
        2,
        4,
        fakeB64,
        QStringLiteral("Text fallback"),
        400,
        QString(),
        false // includeVision = false
    );
    const QJsonArray msgsFallback = fallbackPayload.value(QStringLiteral("messages")).toArray();
    QCOMPARE(msgsFallback.size(), 2);
    QVERIFY(msgsFallback.at(1).toObject().value(QStringLiteral("content")).isString());
}

void KisAiStrokeProgramTest::testGoalModeProgramStepAndMerge()
{
    const QSize canvasSize(1024, 1024);
    const QString prompt = QStringLiteral("anime girl portrait with blue eyes");

    // Generate individual goal steps procedurally
    const KisAiStrokeProgram step1 = KisAiStrokeProgramCodec::createDeterministicProgramStep(prompt, canvasSize, 1, 4);
    const KisAiStrokeProgram step2 = KisAiStrokeProgramCodec::createDeterministicProgramStep(prompt, canvasSize, 2, 4);
    const KisAiStrokeProgram step3 = KisAiStrokeProgramCodec::createDeterministicProgramStep(prompt, canvasSize, 3, 4);
    const KisAiStrokeProgram step4 = KisAiStrokeProgramCodec::createDeterministicProgramStep(prompt, canvasSize, 4, 4);

    QVERIFY(!step1.operations.isEmpty());
    QVERIFY(!step2.operations.isEmpty());
    QVERIFY(!step3.operations.isEmpty());
    QVERIFY(!step4.operations.isEmpty());

    QCOMPARE(step1.currentStep, 1);
    QCOMPARE(step2.currentStep, 2);
    QCOMPARE(step3.currentStep, 3);
    QCOMPARE(step4.currentStep, 4);
    QVERIFY(!step1.goalReached);
    QVERIFY(step4.goalReached);

    // Merge steps iteratively
    KisAiStrokeProgram accumulated = step1;
    QCOMPARE(accumulated.operations.size(), step1.operations.size());

    accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, step2);
    QCOMPARE(accumulated.currentStep, 2);
    QVERIFY(accumulated.operations.size() > step1.operations.size());

    accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, step3);
    QCOMPARE(accumulated.currentStep, 3);

    accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, step4);
    QCOMPARE(accumulated.currentStep, 4);
    QVERIFY(accumulated.goalReached);
    QVERIFY(accumulated.completionScore >= 0.6);
}

void KisAiStrokeProgramTest::testNewProceduralDomains()
{
    const QSize canvasSize(1024, 1024);

    // 1. Cyberpunk
    const KisAiStrokeProgram cyber = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("cyberpunk city street at night with neon lights and skyscrapers"), canvasSize);
    QVERIFY(cyber.isValid());
    QVERIFY(cyber.operations.size() >= 5);
    bool hasNeon = false;
    for (const auto &op : cyber.operations) {
        if (op.brush.profile == QLatin1String("neon")) hasNeon = true;
    }
    QVERIFY(hasNeon);

    // 2. Botanical
    const KisAiStrokeProgram plant = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("beautiful red rose bouquet with watercolor leaves"), canvasSize);
    QVERIFY(plant.isValid());
    QVERIFY(plant.operations.size() >= 5);

    // 3. Creature
    const KisAiStrokeProgram beast = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("fierce red dragon with large wings and horns"), canvasSize);
    QVERIFY(beast.isValid());
    QVERIFY(beast.operations.size() >= 5);

    // 4. MangaFx
    const KisAiStrokeProgram manga = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("manga speed lines action impact comic fx"), canvasSize);
    QVERIFY(manga.isValid());
    QVERIFY(manga.operations.size() >= 5);
    bool hasMangaLines = false;
    for (const auto &op : manga.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::MangaLines) hasMangaLines = true;
    }
    QVERIFY(hasMangaLines);
}

void KisAiStrokeProgramTest::testPixelCoordinateThresholdBoundary()
{
    // Verify that coordinates at exactly 1.5 are NOT auto-normalized,
    // but coordinates above 1.5 ARE auto-normalized.
    const QSize canvasSize(1000, 1000);

    // Case 1: maxCoord = 1.5 (at threshold) - should NOT normalize
    {
        const QString jsonText = QStringLiteral(
            "{\n"
            "  \"schema_version\": 2,\n"
            "  \"canvas\": {\"width\": 1000, \"height\": 1000},\n"
            "  \"operations\": [\n"
            "    {\n"
            "      \"kind\": \"path\",\n"
            "      \"id\": \"at_threshold\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"points\": [[1.5, 0.5, 1.0], [0.5, 0.5, 1.0]],\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#000000\", \"size\": 10}\n"
            "    }\n"
            "  ]\n"
            "}"
        );
        KisAiStrokeProgram program;
        QString error;
        const QJsonObject root = QJsonDocument::fromJson(jsonText.toUtf8()).object();
        QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        // 1.5 is NOT > 1.5, so coordinates should remain as-is
        QCOMPARE(program.operations[0].points[0].pos.x(), 1.5);
    }

    // Case 2: maxCoord = 1.5001 (just above threshold) - should normalize
    {
        const QString jsonText = QStringLiteral(
            "{\n"
            "  \"schema_version\": 2,\n"
            "  \"canvas\": {\"width\": 1000, \"height\": 1000},\n"
            "  \"operations\": [\n"
            "    {\n"
            "      \"kind\": \"path\",\n"
            "      \"id\": \"above_threshold\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"points\": [[1.5001, 0.5, 1.0], [0.5, 0.5, 1.0]],\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#000000\", \"size\": 10}\n"
            "    }\n"
            "  ]\n"
            "}"
        );
        KisAiStrokeProgram program;
        QString error;
        const QJsonObject root = QJsonDocument::fromJson(jsonText.toUtf8()).object();
        QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        // 1.5001 > 1.5, so coordinates should be normalized: 1.5001 / 1000 ≈ 0.0015
        QVERIFY(program.operations[0].points[0].pos.x() < 0.01);
    }

    // Case 3: maxCoord = 1.4999 (just below threshold) - should NOT normalize
    {
        const QString jsonText = QStringLiteral(
            "{\n"
            "  \"schema_version\": 2,\n"
            "  \"canvas\": {\"width\": 1000, \"height\": 1000},\n"
            "  \"operations\": [\n"
            "    {\n"
            "      \"kind\": \"path\",\n"
            "      \"id\": \"below_threshold\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"points\": [[1.4999, 0.5, 1.0], [0.5, 0.5, 1.0]],\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#000000\", \"size\": 10}\n"
            "    }\n"
            "  ]\n"
            "}"
        );
        KisAiStrokeProgram program;
        QString error;
        const QJsonObject root = QJsonDocument::fromJson(jsonText.toUtf8()).object();
        QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        // 1.4999 is NOT > 1.5, so coordinates should remain as-is
        QCOMPARE(program.operations[0].points[0].pos.x(), 1.4999);
    }
}

void KisAiStrokeProgramTest::testGoalModeProgramStepDynamicTotalSteps()
{
    const QSize canvasSize(1024, 1024);
    const QString prompt = QStringLiteral("fantasy landscape with dragon and glowing crystal");

    for (int totalSteps : {2, 3, 4, 5, 6}) {
        KisAiStrokeProgram accumulated;
        QSet<QString> accumulatedLayers;

        for (int step = 1; step <= totalSteps; ++step) {
            const KisAiStrokeProgram stepProg = KisAiStrokeProgramCodec::createDeterministicProgramStep(
                prompt, canvasSize, step, totalSteps);

            QVERIFY2(!stepProg.operations.isEmpty(), qPrintable(QStringLiteral("totalSteps=%1 step=%2 has no operations").arg(totalSteps).arg(step)));
            QCOMPARE(stepProg.currentStep, step);
            QCOMPARE(stepProg.totalSteps, totalSteps);
            QVERIFY(!stepProg.stepPhase.isEmpty());

            if (step == totalSteps) {
                QVERIFY2(stepProg.goalReached, qPrintable(QStringLiteral("final step in %1 totalSteps should reach goal").arg(totalSteps)));
            } else {
                QVERIFY2(!stepProg.goalReached, qPrintable(QStringLiteral("step %1 of %2 should not reach goal early").arg(step).arg(totalSteps)));
            }

            for (const KisAiStrokeOperation &op : stepProg.operations) {
                accumulatedLayers.insert(op.layer);
            }

            if (step == 1) {
                accumulated = stepProg;
            } else {
                accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, stepProg);
            }
        }

        // After all steps, the merged program must contain all essential layers
        for (const QString &essentialLayer : {QStringLiteral("Flats"), QStringLiteral("Shading"), QStringLiteral("Lineart"), QStringLiteral("Highlights"), QStringLiteral("FX")}) {
            QVERIFY2(accumulatedLayers.contains(essentialLayer),
                qPrintable(QStringLiteral("totalSteps=%1 missing essential layer %2").arg(totalSteps).arg(essentialLayer)));
        }
        QVERIFY(accumulated.goalReached);
        QCOMPARE(accumulated.currentStep, totalSteps);
    }
}

void KisAiStrokeProgramTest::testGoalModePayloadDynamicPhase()
{
    const QSize canvasSize(800, 600);
    KisAiPromptAnalyzer::SemanticSpec spec = KisAiPromptAnalyzer::analyze(QStringLiteral("cyberpunk city at dusk"), canvasSize);

    // 1. Guidance reflects totalSteps
    for (int totalSteps : {2, 3, 4, 5, 6}) {
        for (int step = 1; step <= totalSteps; ++step) {
            const QString guidance = KisAiPromptAnalyzer::generateGoalPhaseGuidance(step, spec, canvasSize, totalSteps);
            QVERIFY2(guidance.contains(QStringLiteral("PHASE %1/%2").arg(step).arg(totalSteps)),
                     qPrintable(QStringLiteral("Guidance missing PHASE %1/%2: %3").arg(step).arg(totalSteps).arg(guidance)));
        }
    }

    // 2. Vision model detection: all valid model names are accepted (vision requirement)
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("o1")));
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("o1-preview")));
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("gpt-4.5")));
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("gpt-4o")));
    QVERIFY(KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("deepseek-r1")));
    QVERIFY(!KisAiStrokeProgramCodec::isVisionModel(QStringLiteral("")));

    // 3. Goal step payload passes dynamic step_phase and total_steps
    const QJsonObject payload2Step = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("cyberpunk city"),
        canvasSize,
        1,
        2
    );
    const QJsonArray msgs = payload2Step.value(QStringLiteral("messages")).toArray();
    QCOMPARE(msgs.size(), 2);
    const QString userContent = msgs.at(1).toObject().value(QStringLiteral("content")).toString();
    const QJsonObject userJson = QJsonDocument::fromJson(userContent.toUtf8()).object();
    QCOMPARE(userJson.value(QStringLiteral("current_step")).toInt(), 1);
    QCOMPARE(userJson.value(QStringLiteral("total_steps")).toInt(), 2);
    QCOMPARE(userJson.value(QStringLiteral("step_phase")).toString(), QStringLiteral("Flats & Shading Foundation"));
}

void KisAiStrokeProgramTest::testParsePointsNanAndInfProtection()
{
    const QString jsonNan = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"invalid_pts\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[null, 0.5, 1.0], [0.5, 0.5, 1.0]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#000000\", \"size\": 0.01}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog;
    QString error;
    const QJsonObject root = QJsonDocument::fromJson(jsonNan.toUtf8()).object();
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &prog, &error));
    QCOMPARE(prog.operations.size(), 1);
    QCOMPARE(prog.operations[0].points.size(), 1);
    QCOMPARE(prog.operations[0].points[0].pos, QPointF(0.5, 0.5));
}

void KisAiStrokeProgramTest::testIsReasoningModel()
{
    // Reasoning models
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("o1")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("o1-preview")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("o1-mini")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("o3-mini")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("deepseek-reasoner")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("deepseek-r1")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("deepseek-r1-distill-qwen-32b")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("qwq-32b")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("dots-studio/dots-3-note-preview:free")));

    // Standard non-reasoning models
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("gpt-4o")));
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("gpt-4o-mini")));
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("claude-3-5-sonnet-20241022")));
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("dall-e-3")));
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("")));

    // Verify Chat Completions payload max_completion_tokens vs max_tokens
    const QSize canvasSize(1024, 1024);
    const QJsonObject reasoningPayload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(
        QStringLiteral("o3-mini"), QStringLiteral("cat"), canvasSize, 400);
    QVERIFY(reasoningPayload.contains(QStringLiteral("max_completion_tokens")));
    QVERIFY(!reasoningPayload.contains(QStringLiteral("max_tokens")));

    const QJsonObject regularPayload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(
        QStringLiteral("gpt-4o"), QStringLiteral("cat"), canvasSize, 400);
    QVERIFY(regularPayload.contains(QStringLiteral("max_tokens")));
    QVERIFY(!regularPayload.contains(QStringLiteral("max_completion_tokens")));
}

void KisAiStrokeProgramTest::testScalarCoordinateAutoNormalization()
{
    const QString jsonScalars = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"canvas_size\": [1000, 1000],\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"gradient_fill\",\n"
        "      \"id\": \"grad_test\",\n"
        "      \"layer\": \"Background\",\n"
        "      \"gradient_type\": \"radial\",\n"
        "      \"start_color\": \"#ffffff\",\n"
        "      \"end_color\": \"#000000\",\n"
        "      \"center\": [500, 500],\n"
        "      \"radius\": 500\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"ribbon\",\n"
        "      \"id\": \"ribbon_test\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"points\": [[100, 100], [900, 900]],\n"
        "      \"start_width\": 50,\n"
        "      \"end_width\": 10,\n"
        "      \"brush\": {\"color\": \"#ff0000\", \"size\": 10}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"manga_lines\",\n"
        "      \"id\": \"manga_test\",\n"
        "      \"layer\": \"FX\",\n"
        "      \"line_type\": \"focus\",\n"
        "      \"center\": [500, 500],\n"
        "      \"inner_radius\": 100,\n"
        "      \"outer_radius\": 800,\n"
        "      \"line_count\": 12,\n"
        "      \"brush\": {\"color\": \"#000000\", \"size\": 2}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog;
    QString error;
    const QJsonObject root = QJsonDocument::fromJson(jsonScalars.toUtf8()).object();
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &prog, &error));
    QCOMPARE(prog.operations.size(), 3);

    // Gradient radius should be normalized (500 / 1000 = 0.5)
    QCOMPARE(prog.operations[0].gradientRadius, 0.5);
    QCOMPARE(prog.operations[0].gradientCenter, QPointF(0.5, 0.5));

    // Ribbon widths should be normalized (50 / 1000 = 0.05, 10 / 1000 = 0.01)
    QCOMPARE(prog.operations[1].widthStart, 0.05);
    QCOMPARE(prog.operations[1].widthEnd, 0.01);
    QCOMPARE(prog.operations[1].spine.first(), QPointF(0.1, 0.1));
    QCOMPARE(prog.operations[1].spine.last(), QPointF(0.9, 0.9));

    // MangaLines inner/outer radius should be normalized (100 / 1000 = 0.1, 800 / 1000 = 0.8)
    QCOMPARE(prog.operations[2].innerRadius, 0.1);
    QCOMPARE(prog.operations[2].outerRadius, 0.8);
    QCOMPARE(prog.operations[2].gradientCenter, QPointF(0.5, 0.5));
}

void KisAiStrokeProgramTest::testParseSseStreamChunk()
{
    QByteArray unprocessed;
    QString content;
    bool isDone = false;

    // 1. Partial chunk split across line boundaries
    const QByteArray chunk1 = ": keep-alive\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"{\\\"schema_\"}}]}\n\ndata: {\"choices\":";
    QVERIFY(KisAiStrokeProgramCodec::parseSseStreamChunk(chunk1, &unprocessed, &content, &isDone));
    QCOMPARE(content, QStringLiteral("{\"schema_"));
    QCOMPARE(isDone, false);
    QVERIFY(!unprocessed.isEmpty());

    // 2. Second chunk completing the split JSON and finishing with [DONE]
    const QByteArray chunk2 = "[{\"delta\":{\"content\":\"version\\\":2}\"}}]}\n\ndata: [DONE]\n\n";
    QVERIFY(KisAiStrokeProgramCodec::parseSseStreamChunk(chunk2, &unprocessed, &content, &isDone));
    QCOMPARE(content, QStringLiteral("{\"schema_version\":2}"));
    QCOMPARE(isDone, true);
    QVERIFY(unprocessed.isEmpty());
}

void KisAiStrokeProgramTest::testAcceptedResponseContentType()
{
    QVERIFY(KisAiStrokeProgramCodec::isAcceptedResponseContentType(QByteArray(), true));
    QVERIFY(KisAiStrokeProgramCodec::isAcceptedResponseContentType(QByteArrayLiteral("application/json; charset=utf-8"),
                                                                   false));
    QVERIFY(KisAiStrokeProgramCodec::isAcceptedResponseContentType(QByteArrayLiteral("application/x-json"), false));
    QVERIFY(KisAiStrokeProgramCodec::isAcceptedResponseContentType(QByteArrayLiteral("text/json"), false));

    // Chat Completions with stream=true returns Server-Sent Events, not a
    // buffered JSON media type.
    QVERIFY(
        KisAiStrokeProgramCodec::isAcceptedResponseContentType(QByteArrayLiteral("text/event-stream; charset=utf-8"),
                                                               true));
    QVERIFY(!KisAiStrokeProgramCodec::isAcceptedResponseContentType(QByteArrayLiteral("text/event-stream"), false));
    QVERIFY(!KisAiStrokeProgramCodec::isAcceptedResponseContentType(QByteArrayLiteral("text/html"), true));
}

void KisAiStrokeProgramTest::testGoalModeCompletionInvariant()
{
    const QJsonObject operation{
        {QStringLiteral("kind"), QStringLiteral("path")},
        {QStringLiteral("id"), QStringLiteral("intermediate_line")},
        {QStringLiteral("layer"), QStringLiteral("Lineart")},
        {QStringLiteral("points"),
         QJsonArray{
             QJsonArray{0.2, 0.2, 0.8},
             QJsonArray{0.8, 0.8, 0.7},
         }},
        {QStringLiteral("brush"),
         QJsonObject{
             {QStringLiteral("profile"), QStringLiteral("gpen")},
             {QStringLiteral("color"), QStringLiteral("#222233")},
             {QStringLiteral("size"), 0.006},
         }},
    };
    const QJsonObject root{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("current_step"), 1},
        {QStringLiteral("total_steps"), 4},
        // A model can incorrectly claim completion at an intermediate step.
        {QStringLiteral("goal_reached"), true},
        {QStringLiteral("operations"), QJsonArray{operation}},
    };

    KisAiStrokeProgram parsed;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseProgramJson(root, &parsed, &error), qPrintable(error));
    const KisAiStrokeProgram intermediate = KisAiStrokeProgramCodec::refineForRendering(parsed);
    QVERIFY(!intermediate.goalReached);

    parsed.currentStep = parsed.totalSteps;
    const KisAiStrokeProgram finalStep = KisAiStrokeProgramCodec::refineForRendering(parsed);
    QVERIFY(finalStep.goalReached);
}

void KisAiStrokeProgramTest::testSchemaVersionValidation()
{
    // 1. Valid v2 schema (explicit)
    {
        const QJsonObject root {
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("operations"), QJsonArray {
                QJsonObject {
                    {QStringLiteral("kind"), QStringLiteral("path")},
                    {QStringLiteral("id"), QStringLiteral("test")},
                    {QStringLiteral("layer"), QStringLiteral("Lineart")},
                    {QStringLiteral("points"), QJsonArray {QJsonArray {0.1, 0.1, 1.0}, QJsonArray {0.9, 0.9, 1.0}}},
                    {QStringLiteral("brush"), QJsonObject {
                        {QStringLiteral("profile"), QStringLiteral("gpen")},
                        {QStringLiteral("color"), QStringLiteral("#000000")},
                        {QStringLiteral("size"), 0.01},
                        {QStringLiteral("is_eraser"), false},
                    }},
                },
            }},
        };
        KisAiStrokeProgram program;
        QString error;
        QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        QCOMPARE(program.schemaVersion, 2);
    }

    // 2. Valid v1 schema (no schema_version field defaults to v2 parsing but accepts v1 strokes array)
    {
        const QJsonObject root {
            {QStringLiteral("strokes"), QJsonArray {
                QJsonObject {
                    {QStringLiteral("id"), QStringLiteral("v1_stroke")},
                    {QStringLiteral("layer_name"), QStringLiteral("Lineart")},
                    {QStringLiteral("color"), QStringLiteral("#000000")},
                    {QStringLiteral("size_px"), 5.0},
                    {QStringLiteral("points"), QJsonArray {QJsonArray {0.1, 0.1, 1.0}, QJsonArray {0.9, 0.9, 1.0}}},
                },
            }},
        };
        KisAiStrokeProgram program;
        QString error;
        QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        QCOMPARE(program.operations.size(), 1);
    }

    // 3. Unsupported schema version (v0) must be rejected
    {
        const QJsonObject root {
            {QStringLiteral("schema_version"), 0},
            {QStringLiteral("operations"), QJsonArray {
                QJsonObject {
                    {QStringLiteral("kind"), QStringLiteral("path")},
                    {QStringLiteral("id"), QStringLiteral("test")},
                    {QStringLiteral("layer"), QStringLiteral("Lineart")},
                    {QStringLiteral("points"), QJsonArray {QJsonArray {0.1, 0.1, 1.0}, QJsonArray {0.9, 0.9, 1.0}}},
                    {QStringLiteral("brush"), QJsonObject {
                        {QStringLiteral("profile"), QStringLiteral("gpen")},
                        {QStringLiteral("color"), QStringLiteral("#000000")},
                        {QStringLiteral("size"), 0.01},
                        {QStringLiteral("is_eraser"), false},
                    }},
                },
            }},
        };
        KisAiStrokeProgram program;
        QString error;
        QVERIFY(!KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        QVERIFY(error.contains(QStringLiteral("スキーマバージョン")));
    }

    // 4. Unsupported future schema version (v3) must be rejected
    {
        const QJsonObject root {
            {QStringLiteral("schema_version"), 3},
            {QStringLiteral("operations"), QJsonArray {
                QJsonObject {
                    {QStringLiteral("kind"), QStringLiteral("path")},
                    {QStringLiteral("id"), QStringLiteral("test")},
                    {QStringLiteral("layer"), QStringLiteral("Lineart")},
                    {QStringLiteral("points"), QJsonArray {QJsonArray {0.1, 0.1, 1.0}, QJsonArray {0.9, 0.9, 1.0}}},
                    {QStringLiteral("brush"), QJsonObject {
                        {QStringLiteral("profile"), QStringLiteral("gpen")},
                        {QStringLiteral("color"), QStringLiteral("#000000")},
                        {QStringLiteral("size"), 0.01},
                        {QStringLiteral("is_eraser"), false},
                    }},
                },
            }},
        };
        KisAiStrokeProgram program;
        QString error;
        QVERIFY(!KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        QVERIFY(error.contains(QStringLiteral("v3")));
    }

    // 5. Negative schema version must be rejected
    {
        const QJsonObject root {
            {QStringLiteral("schema_version"), -1},
            {QStringLiteral("operations"), QJsonArray {}},
        };
        KisAiStrokeProgram program;
        QString error;
        QVERIFY(!KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
        QVERIFY(!error.isEmpty());
    }
}

void KisAiStrokeProgramTest::testUserCorruptedJsonRepair()
{
    // The exact corrupted snippet from the user's prompt report with stray 't's, truncated at the end
    const QByteArray corruptedJson = QByteArrayLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"prompt\": \"アニメ美t少女のクローズアップtポートレート、大きな輝tく青い瞳t、二重まぶtた、繊細なtまつ毛、さらtさらの銀髪、t柔らかい頬のt赤み、天使の輪t\",\n"
        "  \"titlet\": \"Silver Htalo Portrait\",\n"
        "  \"operationst\": [\n"
        "    {\n"
        "      \"tkind\": \"gradient_ftill\",\n"
        "      \"id\": \"tbg_sky\",\n"
        "t      \"layer\": \"Backgroundt\",\n"
        "      \"polygon\": [[t0,0],[t1,0],[1t,1],[0,1t]],\n"
        "      \"colors\": [\"t#2a354ta\", \"#4at658a\", \"#t8ba7d4t\"],\n"
        "      \"angle_degt\": 90,\n"
        "      \"tbrush\": {\n"
        "        \"tprofile\": \"watertcolor\",\n"
        "        \"tcolor\": \"#8tba7d4\",\n"
        "t        \"size\": 0t.05,\n"
        "       t \"is_eraser\": falset\n"
        "      }\n"
        "    },\n"
        "t    {\n"
        "      \"kindt\": \"fill\",\n"
        "      \"tid\": \"bgt_halo_tglow\",\n"
        "      \"layert\": \"Background\",\n"
        "t      \"polygont\": [[0.2t,0.15t],[0.8,t0.15],[t0.7,0t.35],[0.t3,0.35]],\n"
        "     t \"brush\": {\n"
        "t        \"profile\": \"airtbrush\",\n"
        "        \"tcolor\": \"#fffbet0\",\n"
        "        \"tsi\n"
    );

    KisAiStrokeProgram program;
    QString error;
    const bool ok = KisAiStrokeProgramCodec::parseResponse(corruptedJson, &program, &error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(!program.operations.isEmpty());

    // First operation was gradient_fill
    const KisAiStrokeOperation &op1 = program.operations.first();
    QCOMPARE(op1.kind, KisAiStrokeOperation::Kind::GradientFill);
    QCOMPARE(op1.layer, QStringLiteral("Background"));
    QCOMPARE(op1.polygon.size(), 4);
    QCOMPARE(op1.polygon.at(0), QPointF(0.0, 0.0));
    QCOMPARE(op1.polygon.at(1), QPointF(1.0, 0.0));
    QCOMPARE(op1.polygon.at(2), QPointF(1.0, 1.0));
    QCOMPARE(op1.polygon.at(3), QPointF(0.0, 1.0));
    QCOMPARE(op1.brush.size, 0.05);
    QCOMPARE(op1.brush.isEraser, false);
    QCOMPARE(op1.gradientColors.size(), 3);
    QCOMPARE(op1.gradientColors.at(0), QColor(QStringLiteral("#2a354a")));
}

void KisAiStrokeProgramTest::testJsonSyntaxRepairVariousCases()
{
    // 1. Comments and trailing commas
    const QByteArray withComments = QByteArrayLiteral(
        "// Generator output\n"
        "{\n"
        "  /* schema version 2 */\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"line_1\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.1, 0.1, 0.8], [0.9, 0.9, 0.8],],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#112233\", \"size\": 0.01,},\n"
        "    },\n"
        "  ],\n"
        "}\n"
    );
    KisAiStrokeProgram prog1;
    QString err1;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(withComments, &prog1, &err1), qPrintable(err1));
    QCOMPARE(prog1.operations.size(), 1);
    QCOMPARE(prog1.operations.first().points.size(), 2);

    // 2. Single quotes and unquoted keys
    const QByteArray singleQuotesAndUnquoted = QByteArrayLiteral(
        "{\n"
        "  schema_version: 2,\n"
        "  operations: [\n"
        "    {\n"
        "      kind: 'fill',\n"
        "      id: 'poly_1',\n"
        "      layer: 'BaseColor',\n"
        "      polygon: [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]],\n"
        "      brush: {profile: 'flat', color: '#ff0000', size: 0.02}\n"
        "    }\n"
        "  ]\n"
        "}\n"
    );
    KisAiStrokeProgram prog2;
    QString err2;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(singleQuotesAndUnquoted, &prog2, &err2), qPrintable(err2));
    QCOMPARE(prog2.operations.size(), 1);
    QCOMPARE(prog2.operations.first().kind, KisAiStrokeOperation::Kind::Fill);

    // 3. Dirty numbers and dirty booleans
    const QByteArray dirtyNumbersAndBools = QByteArrayLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"stroke_dirty\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[t0.1, 0t.2, 0.5t], [0.8t, 0.9t, 1t]],\n"
        "      \"brush\": {\n"
        "        \"profile\": \"gpen\",\n"
        "        \"color\": \"#000000\",\n"
        "        \"size\": 0t.015,\n"
        "        \"is_eraser\": falset\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}\n"
    );
    KisAiStrokeProgram prog3;
    QString err3;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(dirtyNumbersAndBools, &prog3, &err3), qPrintable(err3));
    QCOMPARE(prog3.operations.size(), 1);
    QCOMPARE(prog3.operations.first().points.size(), 2);
    QCOMPARE(prog3.operations.first().brush.size, 0.015);
    QCOMPARE(prog3.operations.first().brush.isEraser, false);
}

void KisAiStrokeProgramTest::testExtractOperationsFromTruncatedEnvelope()
{
    const QString truncatedResponse = QStringLiteral(
        "{\"schema_version\":2,\"operations\":["
        "{\"kind\":\"fill\",\"id\":\"flat_1\",\"layer\":\"Flats\","
        "\"polygon\":[[0.1,0.1],[0.9,0.1],[0.5,0.9]],"
        "\"brush\":{\"profile\":\"flat\",\"color\":\"#ff0000\",\"size\":0.01}},"
        "{\"kind\":\"path\",\"id\":\"line_1\",\"layer\":\"Lineart\","
        "\"points\":[[0.1,0.1,0.8],[0.9,0.9,0.8]],"
        "\"brush\":{\"profile\":\"gpen\",\"color\":\"#000000\",\"size\":0.01}}");

    KisAiStrokeProgram recovered;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::extractOperationsFromRawText(truncatedResponse, &recovered, &error),
             qPrintable(error));
    QCOMPARE(recovered.operations.size(), 2);
    QCOMPARE(recovered.operations.at(0).id, QStringLiteral("flat_1"));
    QCOMPARE(recovered.operations.at(1).id, QStringLiteral("line_1"));
}

void KisAiStrokeProgramTest::testSupportsJsonFormat()
{
    // Supported providers
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("https://api.openai.com/v1/chat/completions")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("https://openrouter.ai/api/v1/chat/completions")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("https://api.deepseek.com/v1/chat/completions")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("https://api.groq.com/openai/v1/chat/completions")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("http://localhost:11434/v1/chat/completions")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("http://127.0.0.1:1234/v1/chat/completions")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/openai/chat/completions")));

    // Unsupported / custom endpoints
    QVERIFY(!KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("https://my-custom-proxy.internal/v1/chat/completions")));
    QVERIFY(!KisAiStrokeProgramCodec::supportsJsonFormat(QStringLiteral("")));
}

void KisAiStrokeProgramTest::testTypeCheckerValidationAndCoercion()
{
    // Program with coerced types: string coordinates, object-formatted points, and hex without '#'
    const QString jsonText = QStringLiteral(
        "{\n"
        "  \"schema_version\": \"2\",\n"
        "  \"prompt\": \"Coercion test\",\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"fill_coerce\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"polygon\": [{\"x\": \"0.1\", \"y\": \"0.2\"}, {\"x\": 0.8, \"y\": 0.2}, {\"x\": 0.5, \"y\": \"0.9\"}],\n"
        "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"ff5500\", \"size\": \"0.03\"}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"path_coerce\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[\"0.1\", \"0.1\", \"0.9\"], [\"0.5\", \"0.5\", \"1.0\"]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#000000\", \"size\": 0.005}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog;
    QString error;
    KisAiJsonDiagnostic diag;
    const bool ok = KisAiStrokeProgramCodec::parseResponse(jsonText.toUtf8(), &prog, &error, &diag);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(prog.operations.size(), 2);

    // Verify coerced fill polygon
    const auto &opFill = prog.operations.at(0);
    QCOMPARE(opFill.polygon.size(), 3);
    QCOMPARE(opFill.polygon.at(0), QPointF(0.1, 0.2));
    QCOMPARE(opFill.brush.color, QColor(0xff, 0x55, 0x00));
    QCOMPARE(opFill.brush.size, 0.03);

    // Verify coerced path points
    const auto &opPath = prog.operations.at(1);
    QCOMPARE(opPath.points.size(), 2);
    QCOMPARE(opPath.points.at(0).pos, QPointF(0.1, 0.1));
    QCOMPARE(opPath.points.at(0).pressure, 0.9);
}

void KisAiStrokeProgramTest::testTestUtilsMockAndCorruptions()
{
    // Test 1: Mock Chat Response
    const QString sample = KisAiTestUtils::createSampleProgramJson(KisAiTestUtils::SampleProgramType::CharacterPortrait);
    const QByteArray mockResponse = KisAiTestUtils::createMockChatResponse(sample);
    QVERIFY(!mockResponse.isEmpty());

    KisAiStrokeProgram prog;
    QString error;
    QVERIFY(KisAiStrokeProgramCodec::parseResponse(mockResponse, &prog, &error));
    QVERIFY(KisAiTestUtils::verifyProgramStructure(prog, 2, 0.5));

    // Test 2: Full-Width Character Repair
    const QString fullWidthCorrupted = KisAiTestUtils::corruptJson(sample, KisAiTestUtils::CorruptionType::FullWidthCharacters);
    KisAiStrokeProgram progFw;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(fullWidthCorrupted.toUtf8(), &progFw, &error), qPrintable(error));
    QVERIFY(progFw.operations.size() >= 2);

    // Test 3: Unescaped Control Characters Repair
    const QString controlCharCorrupted = KisAiTestUtils::corruptJson(sample, KisAiTestUtils::CorruptionType::UnescapedControlCharacters);
    KisAiStrokeProgram progCtrl;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(controlCharCorrupted.toUtf8(), &progCtrl, &error), qPrintable(error));
    QVERIFY(progCtrl.operations.size() >= 2);

    // Test 4: Trailing Commas Repair
    const QString trailingCommas = KisAiTestUtils::corruptJson(sample, KisAiTestUtils::CorruptionType::TrailingCommas);
    KisAiStrokeProgram progTc;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(trailingCommas.toUtf8(), &progTc, &error), qPrintable(error));
    QVERIFY(progTc.operations.size() >= 2);

    // Test 5: Mock SSE Chunks
    const QStringList tokens = {QStringLiteral("{\"schema_version\":"), QStringLiteral(" 2, \"operations\": []}")};
    const auto chunks = KisAiTestUtils::createMockSseChunks(tokens);
    QCOMPARE(chunks.size(), 3); // 2 tokens + [DONE]
}

void KisAiStrokeProgramTest::testSpikeNoiseSuppressionAndLayerSorting()
{
    // Test 1: Layer Sorting
    KisAiStrokeProgram unsorted;
    unsorted.schemaVersion = 2;

    KisAiStrokeOperation opHigh;
    opHigh.id = QStringLiteral("hl_1");
    opHigh.layer = QStringLiteral("Highlights");
    opHigh.kind = KisAiStrokeOperation::Kind::Path;
    opHigh.points = {{0.5, 0.5, 1.0}, {0.6, 0.6, 1.0}};

    KisAiStrokeOperation opFlat;
    opFlat.id = QStringLiteral("flat_1");
    opFlat.layer = QStringLiteral("Flats");
    opFlat.kind = KisAiStrokeOperation::Kind::Fill;
    opFlat.polygon = {{0.1, 0.1}, {0.9, 0.1}, {0.5, 0.9}};

    KisAiStrokeOperation opLine;
    opLine.id = QStringLiteral("line_1");
    opLine.layer = QStringLiteral("Lineart");
    opLine.kind = KisAiStrokeOperation::Kind::Path;
    opLine.points = {{0.2, 0.2, 1.0}, {0.8, 0.8, 1.0}};

    // Append in reverse order: Highlights -> Flats -> Lineart
    unsorted.operations = {opHigh, opFlat, opLine};

    const KisAiStrokeProgram sorted = KisAiStrokeProgramCodec::refineForRendering(unsorted);
    QCOMPARE(sorted.operations.size(), 3);
    // Flats must come first, then Lineart, then Highlights
    QCOMPARE(sorted.operations.at(0).layer, QStringLiteral("Flats"));
    QCOMPARE(sorted.operations.at(1).layer, QStringLiteral("Lineart"));
    QCOMPARE(sorted.operations.at(2).layer, QStringLiteral("Highlights"));

    // Test 2: Angle Spike Suppression
    KisAiStrokeProgram spikyProgram;
    spikyProgram.schemaVersion = 2;
    KisAiStrokeOperation spikyOp;
    spikyOp.id = QStringLiteral("spike_path");
    spikyOp.layer = QStringLiteral("Lineart");
    spikyOp.kind = KisAiStrokeOperation::Kind::Path;
    // Points with an extreme acute back-and-forth spike
    spikyOp.points = {
        {0.1, 0.1, 1.0},
        {0.15, 0.15, 1.0},
        {0.1001, 0.1001, 1.0}, // extreme angle spike (len1 ~ 0.07, len2 ~ 0.07, len1+len2 ~ 0.14 < 0.15)
        {0.3, 0.3, 1.0}
    };
    spikyProgram.operations.append(spikyOp);

    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram smoothedProg = KisAiStrokeProgramCodec::refineForRendering(spikyProgram, &report);
    QCOMPARE(smoothedProg.operations.size(), 1);
    // Spike point should have been smoothed and recorded as repaired value
    QVERIFY(report.repairedValues > 0);
    // Point 1 was smoothed from (0.15, 0.15) towards midpoint of neighbors (~0.10)
    const QPointF smoothedPoint = smoothedProg.operations.at(0).points.at(1).pos;
    QVERIFY(smoothedPoint.x() < 0.12);
}

void KisAiStrokeProgramTest::testJsonDiagnosticReporting()
{
    const QString invalidJson = QStringLiteral("{\"schema_version\": 2, \"operations\": [ unquoted_garbage ]}");
    KisAiStrokeProgram prog;
    QString error;
    KisAiJsonDiagnostic diag;
    const bool ok = KisAiStrokeProgramCodec::parseResponse(invalidJson.toUtf8(), &prog, &error, &diag);
    QVERIFY(!ok);
    QVERIFY(diag.hasError);
    QVERIFY(!diag.errorMessage.isEmpty());
    QVERIFY(diag.errorOffset >= 0);
    const QString logStr = diag.formatForLog();
    QVERIFY(!logStr.isEmpty());
    QVERIFY(logStr.contains(QStringLiteral("JsonDiagnostic")));
}

void KisAiStrokeProgramTest::testGoalModePayloadReasoningEffortAndSamplingParams()
{
    const QSize canvasSize(1024, 1024);

    // 1. Standard model with temperature, top_p, and JSON enforcement
    const QJsonObject stdPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("anime girl"),
        canvasSize,
        1,
        4,
        QString(),
        QString(),
        400,
        QString(), // reasoningEffort
        true,  // isVisionModel
        true,  // enableStreaming
        true,  // enforceJsonFormat
        0.85,  // temperature
        0.95   // topP
    );
    QCOMPARE(stdPayload.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-4o"));
    QCOMPARE(stdPayload.value(QStringLiteral("stream")).toBool(), true);
    QCOMPARE(stdPayload.value(QStringLiteral("temperature")).toDouble(), 0.85);
    QCOMPARE(stdPayload.value(QStringLiteral("top_p")).toDouble(), 0.95);
    QVERIFY(!stdPayload.contains(QStringLiteral("reasoning_effort")));
    QVERIFY(stdPayload.contains(QStringLiteral("response_format")));
    QCOMPARE(stdPayload.value(QStringLiteral("response_format")).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("json_schema"));

    // 2. Reasoning model (e.g. o3-mini) with reasoning_effort
    const QJsonObject reasoningPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("o3-mini"),
        QStringLiteral("anime landscape"),
        canvasSize,
        2,
        4,
        QString(),
        QString(),
        500,
        QStringLiteral("high"), // reasoningEffort
        true,
        true,
        false, // enforceJsonFormat
        0.70,
        1.0
    );
    QCOMPARE(reasoningPayload.value(QStringLiteral("model")).toString(), QStringLiteral("o3-mini"));
    QCOMPARE(reasoningPayload.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));
    // Reasoning models must not have temperature or top_p injected
    QVERIFY(!reasoningPayload.contains(QStringLiteral("temperature")));
    QVERIFY(!reasoningPayload.contains(QStringLiteral("top_p")));
    QVERIFY(!reasoningPayload.contains(QStringLiteral("response_format")));

    // 3. Reasoning model with empty reasoning_effort does not set reasoning_effort key
    const QJsonObject reasoningDefaultPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("o1"),
        QStringLiteral("cyberpunk character"),
        canvasSize,
        1,
        3,
        QString(),
        QString(),
        350,
        QString() // reasoningEffort
    );
    QVERIFY(!reasoningDefaultPayload.contains(QStringLiteral("reasoning_effort")));
    QVERIFY(!reasoningDefaultPayload.contains(QStringLiteral("temperature")));
}

void KisAiStrokeProgramTest::testPythonLiteralsAndMissingCommasRepair()
{
    // 1. Python literals (True, False, None), unit suffixes (px, deg), and BOM/zero-width chars
    const QByteArray rawCorrupted = QByteArray(
        "\xEF\xBB\xBF" // UTF-8 BOM
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \xE2\x80\x8B\"operations\": [\n" // zero-width space before "operations"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"python_line_1\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.1 0.2 0.8] [0.5 0.5 0.9]],\n" // Missing commas between numbers and bracket pairs
        "      \"brush\": {\n"
        "        \"profile\": \"gpen\",\n"
        "        \"color\": \"#000000\",\n"
        "        \"size\": 0.02px,\n" // px unit suffix
        "        \"is_eraser\": False\n" // Python boolean
        "      }\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog1;
    QString err1;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(rawCorrupted, &prog1, &err1), qPrintable(err1));
    QCOMPARE(prog1.operations.size(), 1);
    QCOMPARE(prog1.operations.first().id, QStringLiteral("python_line_1"));
    QCOMPARE(prog1.operations.first().points.size(), 2);
    QCOMPARE(prog1.operations.first().brush.isEraser, false);
    QCOMPARE(prog1.operations.first().brush.size, 0.02);

    // 2. Missing commas between object properties
    const QByteArray missingPropCommas = QByteArrayLiteral(
        "{\n"
        "  \"schema_version\": 2\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"fill\"\n"
        "      \"id\": \"fill_missing_comma\"\n"
        "      \"layer\": \"Flats\"\n"
        "      \"polygon\": [[0.0, 0.0], [1.0, 0.0], [0.5, 1.0]]\n"
        "      \"brush\": {\"profile\": \"flat\", \"color\": \"#ff00ff\", \"size\": 0.01}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog2;
    QString err2;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(missingPropCommas, &prog2, &err2), qPrintable(err2));
    QCOMPARE(prog2.operations.size(), 1);
    QCOMPARE(prog2.operations.first().id, QStringLiteral("fill_missing_comma"));
    QCOMPARE(prog2.operations.first().polygon.size(), 3);
}

void KisAiStrokeProgramTest::testNestedEnvelopeUnwrapping()
{
    // 1. Nested { "result": { "schema_version": 2, "operations": [...] } }
    const QByteArray nestedResult = QByteArrayLiteral(
        "{\n"
        "  \"status\": \"success\",\n"
        "  \"result\": {\n"
        "    \"schema_version\": 2,\n"
        "    \"operations\": [\n"
        "      {\n"
        "        \"kind\": \"path\",\n"
        "        \"id\": \"nested_line\",\n"
        "        \"layer\": \"Lineart\",\n"
        "        \"points\": [[0.2, 0.2, 1.0], [0.8, 0.8, 1.0]],\n"
        "        \"brush\": {\"profile\": \"pencil\", \"color\": \"#333333\", \"size\": 0.01}\n"
        "      }\n"
        "    ]\n"
        "  }\n"
        "}"
    );

    KisAiStrokeProgram prog1;
    QString err1;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(nestedResult, &prog1, &err1), qPrintable(err1));
    QCOMPARE(prog1.operations.size(), 1);
    QCOMPARE(prog1.operations.first().id, QStringLiteral("nested_line"));

    // 2. Deep nested { "data": { "output": { "operations": [...] } } }
    const QByteArray deepNested = QByteArrayLiteral(
        "{\n"
        "  \"data\": {\n"
        "    \"output\": {\n"
        "      \"schema_version\": 2,\n"
        "      \"operations\": [\n"
        "        {\n"
        "          \"kind\": \"fill\",\n"
        "          \"id\": \"deep_fill\",\n"
        "          \"layer\": \"Flats\",\n"
        "          \"polygon\": [[0.1, 0.1], [0.9, 0.1], [0.5, 0.8]],\n"
        "          \"brush\": {\"profile\": \"flat\", \"color\": \"#00aa00\", \"size\": 0.02}\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}"
    );

    KisAiStrokeProgram prog2;
    QString err2;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(deepNested, &prog2, &err2), qPrintable(err2));
    QCOMPARE(prog2.operations.size(), 1);
    QCOMPARE(prog2.operations.first().id, QStringLiteral("deep_fill"));
}

void KisAiStrokeProgramTest::testGoalModePayloadMaxTokensOverride()
{
    const QSize canvasSize(1024, 1024);

    // 1. Standard model with maxTokensOverride > 0
    const QJsonObject stdPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("character art"),
        canvasSize,
        1,
        4,
        QString(),
        QString(),
        400,
        QString(),
        true,
        true,
        true,
        0.70,
        1.0,
        8192 // maxTokensOverride
    );
    QCOMPARE(stdPayload.value(QStringLiteral("max_tokens")).toInt(), 8192);

    // 2. Reasoning model (e.g. o3-mini) with maxTokensOverride > 0
    const QJsonObject reasoningPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("o3-mini"),
        QStringLiteral("character art"),
        canvasSize,
        1,
        4,
        QString(),
        QString(),
        400,
        QStringLiteral("medium"),
        true,
        true,
        false,
        0.70,
        1.0,
        16384 // maxTokensOverride
    );
    QCOMPARE(reasoningPayload.value(QStringLiteral("max_completion_tokens")).toInt(), 16384);
    QVERIFY(!reasoningPayload.contains(QStringLiteral("max_tokens")));

    // 3. Default calculation (maxTokensOverride == 0) ensures budget >= 3072 for standard models
    const QJsonObject defaultPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("character art"),
        canvasSize,
        1,
        4,
        QString(),
        QString(),
        400 // strokeBudget
    );
    QVERIFY(defaultPayload.value(QStringLiteral("max_tokens")).toInt() >= 3072);
}

void KisAiStrokeProgramTest::testNewBrushProfilesNormalization()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(512, 512);

    auto makeOp = [](const QString &profile, const QString &id) {
        KisAiStrokeOperation op;
        op.id = id;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.layer = QStringLiteral("Lineart");
        op.brush.profile = profile;
        op.points = {KisAiStrokePoint(0.1, 0.1), KisAiStrokePoint(0.9, 0.9)};
        return op;
    };

    prog.operations.append(makeOp(QStringLiteral("chisel"), QStringLiteral("op1")));
    prog.operations.append(makeOp(QStringLiteral("flat_pen"), QStringLiteral("op2")));
    prog.operations.append(makeOp(QStringLiteral("carbon"), QStringLiteral("op3")));
    prog.operations.append(makeOp(QStringLiteral("conte"), QStringLiteral("op4")));

    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(prog);
    QCOMPARE(refined.operations.size(), 4);

    QCOMPARE(refined.operations.at(0).brush.profile, QStringLiteral("calligraphy"));
    QCOMPARE(refined.operations.at(1).brush.profile, QStringLiteral("calligraphy"));
    QCOMPARE(refined.operations.at(2).brush.profile, QStringLiteral("charcoal"));
    QCOMPARE(refined.operations.at(3).brush.profile, QStringLiteral("charcoal"));
}

void KisAiStrokeProgramTest::testSignedLeadingDotAndTypeCheckerEdgeCases()
{
    // 1. Direct syntax repair verification: signed leading dot, leading plus, and trailing dot
    QCOMPARE(KisAiStrokeProgramCodec::repairJsonSyntax(QStringLiteral("[-.5, +.5, .5, 5.]")), QStringLiteral("[-0.5, 0.5, 0.5, 5.0]"));

    // 2. Full program parsing with signed leading dots and leading plus
    const QByteArray jsonWithSignedLeadingDots = QByteArrayLiteral(
        "{\n"
        "  \"schema_version\": +2,\n"
        "  \"prompt\": \"Signed dot test\",\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"line_signed\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[.15, .5, .8], [+.5, .25, +.9]],\n"
        "      \"brush\": {\n"
        "        \"profile\": \"pen\",\n"
        "        \"color\": \"rgb(255, 68, 102)\",\n"
        "        \"size\": +.02\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}\n"
    );

    KisAiStrokeProgram prog;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(jsonWithSignedLeadingDots, &prog, &error), qPrintable(error));
    QCOMPARE(prog.schemaVersion, 2);
    QCOMPARE(prog.operations.size(), 1);
    const auto &op = prog.operations.first();
    QCOMPARE(op.points.size(), 2);
    QCOMPARE(op.points.at(0).pos.x(), 0.15);
    QCOMPARE(op.points.at(0).pos.y(), 0.5);
    QCOMPARE(op.points.at(0).pressure, 0.8);
    QCOMPARE(op.points.at(1).pos.x(), 0.5);
    QCOMPARE(op.points.at(1).pos.y(), 0.25);
    QCOMPARE(op.points.at(1).pressure, 0.9);
    QCOMPARE(op.brush.size, 0.02);
    // Verify rgb(...) was parsed
    QCOMPARE(op.brush.color, QColor(255, 68, 102));

    // 2. KisAiStrokeTypeChecker color validation & normalization
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("rgb(10, 20, 30)")));
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("rgba(10, 20, 30, 0.5)")));
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("#ff00a0")));
    QVERIFY(KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("ff00a0")));
    QVERIFY(!KisAiStrokeTypeChecker::isValidColorString(QStringLiteral("not-a-color-at-all")));

    // 3. KisAiStrokeTypeChecker checkBrushObject normalization of rgb
    QJsonObject brushObj;
    brushObj[QStringLiteral("color")] = QStringLiteral("rgb(0, 128, 255)");
    int coerced = 0;
    QVERIFY(KisAiStrokeTypeChecker::checkBrushObject(&brushObj, nullptr, &coerced));
    QCOMPARE(brushObj.value(QStringLiteral("color")).toString(), QStringLiteral("#0080ff"));
    QVERIFY(coerced > 0);

    // 4. KisAiStrokeTypeChecker particles and manga_lines validation
    QJsonObject particlesOp;
    particlesOp[QStringLiteral("kind")] = QStringLiteral("particle"); // synonym
    particlesOp[QStringLiteral("bounds")] = QJsonArray({0.1, 0.2, 0.8, 0.9});
    particlesOp[QStringLiteral("count")] = 32;
    KisAiStrokeTypeCheckReport reportP;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&particlesOp, 0, &reportP));
    QCOMPARE(particlesOp.value(QStringLiteral("kind")).toString(), QStringLiteral("particles"));
    QCOMPARE(particlesOp.value(QStringLiteral("count")).toInt(), 32);

    QJsonObject mangaOp;
    mangaOp[QStringLiteral("kind")] = QStringLiteral("speed_lines"); // synonym
    mangaOp[QStringLiteral("center")] = QJsonArray({0.5, 0.4});
    mangaOp[QStringLiteral("density")] = 60;
    KisAiStrokeTypeCheckReport reportM;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&mangaOp, 1, &reportM));
    QCOMPARE(mangaOp.value(QStringLiteral("kind")).toString(), QStringLiteral("manga_lines"));
    QCOMPARE(mangaOp.value(QStringLiteral("density")).toInt(), 60);

    // 5. KisAiStrokeTypeChecker rejection of invalid kind
    QJsonObject invalidOp;
    invalidOp[QStringLiteral("kind")] = QStringLiteral("system_exec");
    KisAiStrokeTypeCheckReport reportI;
    const bool valid = KisAiStrokeTypeChecker::checkAndCoerceOperation(&invalidOp, 2, &reportI);
    QVERIFY(!valid);
    QVERIFY(reportI.typeErrors > 0);
}

void KisAiStrokeProgramTest::testSchemaAliasesAndGoalModeArtStyle()
{
    // 1. KisAiStrokeTypeChecker alias coercion for all geometry and metadata aliases
    QJsonObject opPath;
    opPath[QStringLiteral("kind")] = QStringLiteral("path");
    opPath[QStringLiteral("pts")] = QJsonArray({QJsonArray({0.1, 0.2}), QJsonArray({0.3, 0.4})});
    opPath[QStringLiteral("name")] = QStringLiteral("stroke_alpha");
    opPath[QStringLiteral("layer_name")] = QStringLiteral("Lineart");
    KisAiStrokeTypeCheckReport reportPath;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&opPath, 0, &reportPath));
    QVERIFY(opPath.contains(QStringLiteral("points")));
    QCOMPARE(opPath.value(QStringLiteral("id")).toString(), QStringLiteral("stroke_alpha"));
    QCOMPARE(opPath.value(QStringLiteral("layer")).toString(), QStringLiteral("Lineart"));

    QJsonObject opFill;
    opFill[QStringLiteral("kind")] = QStringLiteral("fill");
    opFill[QStringLiteral("poly")] = QJsonArray({QJsonArray({0.1, 0.1}), QJsonArray({0.5, 0.1}), QJsonArray({0.5, 0.5})});
    KisAiStrokeTypeCheckReport reportFill;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&opFill, 1, &reportFill));
    QVERIFY(opFill.contains(QStringLiteral("polygon")));

    QJsonObject opRibbon;
    opRibbon[QStringLiteral("kind")] = QStringLiteral("ribbon");
    opRibbon[QStringLiteral("pts")] = QJsonArray({QJsonArray({0.2, 0.2}), QJsonArray({0.8, 0.8})});
    KisAiStrokeTypeCheckReport reportRibbon;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&opRibbon, 2, &reportRibbon));
    QVERIFY(opRibbon.contains(QStringLiteral("spine")));

    QJsonObject opParticles;
    opParticles[QStringLiteral("kind")] = QStringLiteral("particles");
    opParticles[QStringLiteral("rect")] = QJsonArray({0.1, 0.1, 0.9, 0.9});
    KisAiStrokeTypeCheckReport reportParticles;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&opParticles, 3, &reportParticles));
    QVERIFY(opParticles.contains(QStringLiteral("bounds")));

    QJsonObject opManga;
    opManga[QStringLiteral("kind")] = QStringLiteral("manga_lines");
    opManga[QStringLiteral("center_pt")] = QJsonArray({0.5, 0.6});
    KisAiStrokeTypeCheckReport reportManga;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&opManga, 4, &reportManga));
    QVERIFY(opManga.contains(QStringLiteral("center")));

    // 2. Full parseResponse accepts program payload using aliases
    const QString aliasJson = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"pts\": [[0.1, 0.1], [0.5, 0.5]],\n"
        "      \"brush\": {\"color\": \"#000000\", \"size\": 0.01}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"poly\": [[0.2, 0.2], [0.4, 0.2], [0.3, 0.5]],\n"
        "      \"brush\": {\"color\": \"#ff8800\"}\n"
        "    }\n"
        "  ]\n"
        "}"
    );
    KisAiStrokeProgram parsedProg;
    QString parseErr;
    QVERIFY(KisAiStrokeProgramCodec::parseResponse(aliasJson.toUtf8(), &parsedProg, &parseErr));
    QCOMPARE(parsedProg.operations.size(), 2);
    QCOMPARE(parsedProg.operations.at(0).points.size(), 2);
    QCOMPARE(parsedProg.operations.at(1).polygon.size(), 3);

    // 3. Goal Mode buildGoalStepPayload artStyle propagation
    const QSize canvasSize(1024, 1024);
    const QJsonObject payloadDefault = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("portrait of a girl"),
        canvasSize,
        1,
        4
    );
    const QString defaultSystem = payloadDefault.value(QStringLiteral("messages")).toArray().at(0).toObject().value(QStringLiteral("content")).toString();
    QVERIFY(!defaultSystem.contains(QStringLiteral("Luminous Watercolor")));

    const QJsonObject payloadWatercolor = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("portrait of a girl"),
        canvasSize,
        1,
        4,
        QString(), // imageBase64
        QString(), // additionalInstruction
        400,       // strokeBudget
        QString(), // reasoningEffort
        true,      // includeVision
        true,      // enableStreaming
        false,     // enforceJsonFormat
        0.7,       // temperature
        1.0,       // topP
        0,         // maxTokensOverride
        static_cast<int>(KisAiPromptAnalyzer::ArtStyle::Watercolor)
    );
    const QString watercolorSystem = payloadWatercolor.value(QStringLiteral("messages")).toArray().at(0).toObject().value(QStringLiteral("content")).toString();
    QVERIFY(watercolorSystem.contains(QStringLiteral("Luminous Watercolor")));

    // 4. Goal Mode 5-step phase 4 mapping verification
    KisAiPromptAnalyzer::SemanticSpec spec = KisAiPromptAnalyzer::analyze(QStringLiteral("fantasy landscape"), canvasSize);
    const QString guidance5StepP4 = KisAiPromptAnalyzer::generateGoalPhaseGuidance(4, spec, canvasSize, 5);
    QVERIFY(guidance5StepP4.contains(QStringLiteral("PHASE 4 MISSION")));
    QVERIFY(guidance5StepP4.contains(QStringLiteral("SPECULAR HIGHLIGHTS")));
    const QString guidance5StepP3 = KisAiPromptAnalyzer::generateGoalPhaseGuidance(3, spec, canvasSize, 5);
    QVERIFY(guidance5StepP3.contains(QStringLiteral("PHASE 3 MISSION")));
}

void KisAiStrokeProgramTest::testAgentCritiqueAndReadinessParsing()
{
    const QString agentJson = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"agent_critique\": \"Initial face proportions are solid, but hair volume lacks flow and eye highlights are missing.\",\n"
        "  \"target_focus_area\": \"HairFlow & Catchlights\",\n"
        "  \"readiness_score\": 0.78,\n"
        "  \"recommended_action\": \"add_strands_and_highlights\",\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"points\": [[0.2, 0.3], [0.5, 0.6]],\n"
        "      \"brush\": {\"color\": \"#333333\", \"size\": 0.01}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog;
    QString err;
    QVERIFY(KisAiStrokeProgramCodec::parseResponse(agentJson.toUtf8(), &prog, &err));
    QCOMPARE(prog.agentCritique, QStringLiteral("Initial face proportions are solid, but hair volume lacks flow and eye highlights are missing."));
    QCOMPARE(prog.targetFocusArea, QStringLiteral("HairFlow & Catchlights"));
    QVERIFY(qAbs(prog.readinessScore - 0.78) < 1e-4);
    QCOMPARE(prog.recommendedAction, QStringLiteral("add_strands_and_highlights"));
    QCOMPARE(prog.operations.size(), 1);
}

void KisAiStrokeProgramTest::testSanitizeUnescapedControlCharsInStrings()
{
    const QString rawBroken = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"agent_critique\": \"Line 1 critique\nLine 2 with \t tabs\",\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"points\": [[0.1, 0.1], [0.2, 0.2]],\n"
        "      \"brush\": {\"color\": \"#000000\"}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiJsonDiagnostic diag;
    const QString repaired = KisAiStrokeProgramCodec::repairJsonSyntax(rawBroken, &diag);
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(repaired.toUtf8(), &parseErr);
    QCOMPARE(parseErr.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());
    const QString critique = doc.object().value(QStringLiteral("agent_critique")).toString();
    QVERIFY(critique.contains(QStringLiteral("Line 1 critique")));
    QVERIFY(critique.contains(QStringLiteral("Line 2 with")));
}

void KisAiStrokeProgramTest::testRefineBoundsHostileCanvasSizeAndAngle()
{
    // A model-supplied canvas_size must never survive refineForRendering() as a
    // value that could size a multi-gigabyte downstream QImage allocation.
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200000, 200000);

    KisAiStrokeOperation fill;
    fill.kind = KisAiStrokeOperation::Kind::Fill;
    fill.layer = QStringLiteral("Flats");
    fill.polygon << QPointF(0.1, 0.1) << QPointF(0.9, 0.1) << QPointF(0.9, 0.9);
    fill.brush.profile = QStringLiteral("brush");
    fill.angleDeg = std::numeric_limits<qreal>::infinity();
    program.operations.append(fill);

    KisAiStrokeOperation nanAngle = fill;
    nanAngle.angleDeg = std::numeric_limits<qreal>::quiet_NaN();
    program.operations.append(nanAngle);

    KisAiStrokeOperation wrapped = fill;
    wrapped.angleDeg = -450.0; // must wrap into [0, 360)
    program.operations.append(wrapped);

    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program, &report);

    QVERIFY(refined.canvasSize.width() <= 4096);
    QVERIFY(refined.canvasSize.height() <= 4096);
    QVERIFY(refined.canvasSize.width() > 0);
    QVERIFY(refined.canvasSize.height() > 0);

    for (const KisAiStrokeOperation &op : refined.operations) {
        QVERIFY2(std::isfinite(op.angleDeg), "hatch/gradient angle must stay finite");
        QVERIFY(op.angleDeg >= 0.0);
        QVERIFY(op.angleDeg < 360.0);
    }

    QCOMPARE(refined.operations.at(0).angleDeg, 0.0);
    QCOMPARE(refined.operations.at(1).angleDeg, 0.0);
    QCOMPARE(refined.operations.at(2).angleDeg, 270.0);
}

void KisAiStrokeProgramTest::testGradientColorCountIsCapped()
{
    QJsonArray colors;
    for (int i = 0; i < 5000; ++i) {
        colors.append(QStringLiteral("#101010"));
    }

    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("gradient_fill")},
                {QStringLiteral("id"), QStringLiteral("huge_gradient")},
                {QStringLiteral("layer"), QStringLiteral("Flats")},
                {QStringLiteral("colors"), colors},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("profile"), QStringLiteral("brush")},
                    {QStringLiteral("color"), QStringLiteral("#101010")},
                    {QStringLiteral("size"), 0.02},
                    {QStringLiteral("is_eraser"), false},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(QJsonDocument(root).toJson(QJsonDocument::Compact), &program, &error),
             qPrintable(error));
    QCOMPARE(program.operations.size(), 1);
    // Unbounded gradient stops from the model must not translate into unbounded work.
    QVERIFY(program.operations.first().gradientColors.size() <= 64);
    QVERIFY(!program.operations.first().gradientColors.isEmpty());
}

void KisAiStrokeProgramTest::testNumericOverflowFieldsFallBackToDefaults()
{
    // A long digit run parses to +inf while still reporting success; every numeric
    // field must fall back to its default rather than propagate a non-finite value.
    const QString hugeDigits(400, QLatin1Char('9'));

    QString json = QStringLiteral("{\"schema_version\":2,\"completion_score\":\"%1\",\"seed\":1,")
                       .arg(hugeDigits);
    json += QStringLiteral("\"operations\":[{\"kind\":\"fill\",\"id\":\"o\",\"layer\":\"Flats\",")
            + QStringLiteral("\"angle_deg\":\"%1\",").arg(hugeDigits)
            + QStringLiteral("\"polygon\":[[0.1,0.1],[0.9,0.1],[0.9,0.9]],")
            + QStringLiteral("\"brush\":{\"profile\":\"brush\",\"color\":\"#101010\",\"size\":0.02}}]}");

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(json.toUtf8(), &program, &error), qPrintable(error));
    QCOMPARE(program.operations.size(), 1);

    QVERIFY2(std::isfinite(program.completionScore), "completion_score must be finite");
    QVERIFY(program.completionScore >= 0.0 && program.completionScore <= 1.0);
    QVERIFY2(std::isfinite(program.operations.first().angleDeg), "angle_deg must be finite");
}

void KisAiStrokeProgramTest::testParseColorAlphaOverflowIsSafe()
{
    // qRound() on an out-of-range double is undefined behaviour; the alpha value
    // must be clamped before conversion and never produce an invalid color.
    const QColor overflowed = KisAiStrokeProgramCodec::parseColor(QStringLiteral("rgba(10, 20, 30, 1e999)"));
    QVERIFY(overflowed.isValid());
    QCOMPARE(overflowed.alpha(), 255);

    const QColor negative = KisAiStrokeProgramCodec::parseColor(QStringLiteral("rgba(10, 20, 30, -5)"));
    QVERIFY(negative.isValid());
    QVERIFY(negative.alpha() >= 0 && negative.alpha() <= 255);

    const QColor normal = KisAiStrokeProgramCodec::parseColor(QStringLiteral("rgba(10, 20, 30, 0.5)"));
    QCOMPARE(normal.alpha(), 128);

    // Short hex forms must keep working.
    QCOMPARE(KisAiStrokeProgramCodec::parseColor(QStringLiteral("#f00")).rgb(), QColor(255, 0, 0).rgb());
}

void KisAiStrokeProgramTest::testSseCarryOverBufferIsBounded()
{
    // A peer that never terminates a line must not grow the carry-over buffer
    // without bound for the whole stream.
    QByteArray buffer;
    QString content;
    bool done = false;

    const QByteArray chunk(1024 * 1024, 'x');
    bool accepted = true;
    for (int i = 0; i < 64 && accepted; ++i) {
        accepted = KisAiStrokeProgramCodec::parseSseStreamChunk(chunk, &buffer, &content, &done);
    }

    QVERIFY2(!accepted, "oversized unterminated SSE data must be rejected");
    QVERIFY(buffer.size() <= 8 * 1024 * 1024);
    QVERIFY(content.isEmpty());
}

void KisAiStrokeProgramTest::testRepairJsonSyntaxPreservesManyLiterals()
{
    // The mask/restore pass must rebuild the document in one pass; correctness is
    // checked here with many literals, including duplicate values and one that
    // looks like a placeholder. The operation count stays inside the parser's
    // MAX_OPERATIONS budget so this exercises masking, not the limit.
    const int literalCount = 150;
    QJsonArray ops;
    for (int i = 0; i < literalCount; ++i) {
        ops.append(QJsonObject {
            {QStringLiteral("kind"), QStringLiteral("path")},
            {QStringLiteral("id"), QStringLiteral("__AI_STR_MASK_0__")},
            {QStringLiteral("layer"), QStringLiteral("Lineart")},
            {QStringLiteral("points"), QJsonArray {QJsonArray {0.1, 0.2}, QJsonArray {0.8, 0.9}}},
            {QStringLiteral("brush"), QJsonObject {
                {QStringLiteral("profile"), QStringLiteral("gpen")},
                {QStringLiteral("color"), QStringLiteral("#202020")},
                {QStringLiteral("size"), 0.01},
                {QStringLiteral("is_eraser"), false},
            }},
        });
    }

    QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), ops},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(QJsonDocument(root).toJson(QJsonDocument::Compact), &program, &error),
             qPrintable(error));
    QCOMPARE(program.operations.size(), literalCount);
    // A literal whose text resembles the internal placeholder must survive intact.
    QCOMPARE(program.operations.first().id, QStringLiteral("__AI_STR_MASK_0__"));
}

void KisAiStrokeProgramTest::testStrictStructuredOutputsAndJsonSchema()
{
    // B1: Verify OpenAI Strict Structured Outputs detection
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonSchema(QStringLiteral("gpt-4o")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonSchema(QStringLiteral("gpt-4o-mini")));
    QVERIFY(KisAiStrokeProgramCodec::supportsJsonSchema(QStringLiteral("gpt-4.5-preview")));
    QVERIFY(!KisAiStrokeProgramCodec::supportsJsonSchema(QStringLiteral("deepseek/deepseek-chat")));
    QVERIFY(!KisAiStrokeProgramCodec::supportsJsonSchema(QStringLiteral("anthropic/claude-3.7-sonnet")));

    const QJsonObject payload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("Landscape with trees"),
        QSize(800, 600),
        300,
        QString(), // reasoningEffort
        QString(), // customInstructions
        true,      // enableStreaming
        true       // enforceJsonFormat
    );

    const QJsonObject respFormat = payload.value(QStringLiteral("response_format")).toObject();
    QCOMPARE(respFormat.value(QStringLiteral("type")).toString(), QStringLiteral("json_schema"));
    const QJsonObject schemaObj = respFormat.value(QStringLiteral("json_schema")).toObject();
    QCOMPARE(schemaObj.value(QStringLiteral("name")).toString(), QStringLiteral("stroke_program"));
    QCOMPARE(schemaObj.value(QStringLiteral("strict")).toBool(), true);
}

void KisAiStrokeProgramTest::testCompositionPlanPayloadAndParsing()
{
    // B2: Verify composition blueprint payload & parser
    const QJsonObject payload = KisAiStrokeProgramCodec::buildCompositionPlanPayload(
        QStringLiteral("gpt-4o"),
        QStringLiteral("Sunset beach with distant cliffs"),
        QSize(1920, 1080)
    );
    QCOMPARE(payload.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-4o"));
    QCOMPARE(payload.value(QStringLiteral("stream")).toBool(), false);

    const QByteArray mockResponse = QByteArrayLiteral(
        "{\n"
        "  \"composition_type\": \"rule_of_thirds\",\n"
        "  \"focal_point\": {\"x\": 0.65, \"y\": 0.45},\n"
        "  \"primary_palette\": [\"#1d1b32\", \"#d97736\", \"#f4b251\", \"#3a6b88\"],\n"
        "  \"artistic_directives\": \"Establish warm golden backlight with strong silhouettes for the cliffs.\"\n"
        "}"
    );

    QString directives;
    QString error;
    const bool ok = KisAiStrokeProgramCodec::parseCompositionPlan(mockResponse, &directives, &error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(directives.contains(QStringLiteral("Establish warm golden backlight")));
    QVERIFY(directives.contains(QStringLiteral("Focal point at (0.65, 0.45)")));
}

void KisAiStrokeProgramTest::testHueShiftedShadowCalculation()
{
    // B3: Verify hue-shifted shadow color avoiding dirty black shading
    const QColor skinBase(QStringLiteral("#fedac6")); // warm skin
    const QColor warmShadow = KisAiStrokeProgramCodec::calculateHueShiftedShadow(skinBase, true);
    QVERIFY(warmShadow.isValid());
    // Warm light -> shadow shifts toward cool / purple-blue (hsvHue around 240)
    QVERIFY(warmShadow.hsvHue() > 0);
    // Must retain saturation and not collapse to flat grey or black
    QVERIFY(warmShadow.hsvSaturation() > 30);
    QVERIFY(warmShadow.value() > 20);
}

void KisAiStrokeProgramTest::testRevisedQualityScoreAndLinting()
{
    // A5: Shading coarse hatch rescue
    KisAiStrokeProgram prog;
    prog.prompt = QStringLiteral("A calm quiet portrait of an anime girl");
    prog.canvasSize = QSize(800, 800);

    KisAiStrokeOperation coarseHatch;
    coarseHatch.kind = KisAiStrokeOperation::Kind::Hatch;
    coarseHatch.layer = QStringLiteral("Shading");
    coarseHatch.spacing = 0.04; // too coarse!
    coarseHatch.crossHatch = false;
    coarseHatch.polygon << QPointF(0.3, 0.3) << QPointF(0.5, 0.3) << QPointF(0.4, 0.5);
    coarseHatch.brush.color = QColor(QStringLiteral("#402030"));
    prog.operations.append(coarseHatch);

    // A5: Degenerate tiny polygon (area < 1e-4)
    KisAiStrokeOperation tinyPoly;
    tinyPoly.kind = KisAiStrokeOperation::Kind::Fill;
    tinyPoly.layer = QStringLiteral("Flats");
    tinyPoly.polygon << QPointF(0.1, 0.1) << QPointF(0.1001, 0.1) << QPointF(0.1, 0.1001);
    prog.operations.append(tinyPoly);

    // A5: Manga lines in non-FX layer or in calm context
    KisAiStrokeOperation badMangaLines;
    badMangaLines.kind = KisAiStrokeOperation::Kind::MangaLines;
    badMangaLines.layer = QStringLiteral("Lineart"); // wrong layer
    prog.operations.append(badMangaLines);

    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(prog, &report);

    // Coarse hatch must have been rescued to smooth watercolor fill
    bool foundRescuedFill = false;
    for (const auto &op : refined.operations) {
        if (op.layer == QLatin1String("Shading") && op.kind == KisAiStrokeOperation::Kind::Fill) {
            foundRescuedFill = true;
            QCOMPARE(op.brush.profile, QStringLiteral("watercolor"));
        }
        // Tiny polygon and bad manga lines must have been discarded
        QVERIFY(op.kind != KisAiStrokeOperation::Kind::MangaLines);
    }
    QVERIFY(foundRescuedFill);
    QVERIFY(report.repairedValues > 0);

    // B5: Structural quality score on full procedural program
    const KisAiStrokeProgram full = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("anime girl portrait"), QSize(1024, 1024)
    );
    const qreal score = KisAiStrokeProgramCodec::qualityScore(full);
    QVERIFY(score >= 0.60);
}

void KisAiStrokeProgramTest::testIntentAdherenceCheck()
{
    // B7: Verify intent adherence calculation
    KisAiStrokeProgram nightProg;
    nightProg.prompt = QStringLiteral("A dark starry night with crescent moon");
    nightProg.canvasSize = QSize(800, 600);

    KisAiStrokeOperation bg;
    bg.kind = KisAiStrokeOperation::Kind::Fill;
    bg.layer = QStringLiteral("Background");
    bg.polygon << QPointF(0, 0) << QPointF(1, 0) << QPointF(1, 1) << QPointF(0, 1);
    bg.brush.color = QColor(QStringLiteral("#0f1226")); // deep dark night tone
    nightProg.operations.append(bg);

    KisAiStrokeOperation flat;
    flat.kind = KisAiStrokeOperation::Kind::Fill;
    flat.layer = QStringLiteral("Flats");
    flat.polygon << QPointF(0.4, 0.4) << QPointF(0.6, 0.4) << QPointF(0.5, 0.7);
    flat.brush.color = QColor(QStringLiteral("#1e2338"));
    nightProg.operations.append(flat);

    const auto result = KisAiStrokeProgramCodec::checkIntentAdherence(nightProg, nightProg.prompt);
    QVERIFY(result.score >= 0.80);
    QVERIFY(!result.matchedAspects.isEmpty());
}

void KisAiStrokeProgramTest::testTrimOperationsToBudget()
{
    // B6: Verify intelligent operation trimming
    KisAiStrokeProgram prog;
    prog.prompt = QStringLiteral("Landscape");
    prog.canvasSize = QSize(800, 600);

    // Add 40 operations across layers
    for (int i = 0; i < 15; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.layer = QStringLiteral("Flats");
        op.polygon << QPointF(0.1 + i * 0.02, 0.1) << QPointF(0.2 + i * 0.02, 0.1) << QPointF(0.15, 0.3);
        prog.operations.append(op);
    }
    for (int i = 0; i < 15; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.layer = QStringLiteral("Lineart");
        op.points << KisAiStrokePoint(0.1, 0.1 + i * 0.02) << KisAiStrokePoint(0.2, 0.2) << KisAiStrokePoint(0.3, 0.3);
        prog.operations.append(op);
    }
    for (int i = 0; i < 10; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.layer = QStringLiteral("Shading");
        op.polygon << QPointF(0.2, 0.2) << QPointF(0.4, 0.2) << QPointF(0.3, 0.4);
        prog.operations.append(op);
    }

    QCOMPARE(prog.operations.size(), 40);
    const KisAiStrokeProgram trimmed = KisAiStrokeProgramCodec::trimOperationsToBudget(prog, 12);
    QVERIFY(trimmed.operations.size() <= 12);
    // Essential layers must be preserved
    const auto counts = KisAiStrokeProgramCodec::countLayerOperations(trimmed);
    QVERIFY(counts.contains(QStringLiteral("Flats")));
    QVERIFY(counts.contains(QStringLiteral("Lineart")));
}

void KisAiStrokeProgramTest::testGoalModeGeometryDigest()
{
    // Phase 2: Verify geometry digest generation
    const KisAiStrokeProgram full = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("anime girl portrait"), QSize(1024, 1024)
    );
    const QJsonObject digest = KisAiStrokeProgramCodec::buildGeometryDigest(full);

    QVERIFY(digest.contains(QStringLiteral("total_operations")));
    QVERIFY(digest.contains(QStringLiteral("layer_counts")));
    QVERIFY(digest.contains(QStringLiteral("flats_coverage_estimated")));
    QVERIFY(digest.contains(QStringLiteral("active_palette")));
    QVERIFY(digest.contains(QStringLiteral("bounding_box")));

    const QJsonObject layers = digest.value(QStringLiteral("layer_counts")).toObject();
    QVERIFY(layers.contains(QStringLiteral("Flats")));
    QVERIFY(layers.contains(QStringLiteral("Lineart")));
}

void KisAiStrokeProgramTest::testPromptFirstPriorityBlock()
{
    // A0: Verify Prompt-First absolute priority block
    const QString prompt = QStringLiteral("Cyberpunk motorcycle speeding through rainy neon city");
    const QString systemText = KisAiStrokeProgramCodec::buildSystemPrompt(QSize(1280, 720), prompt);

    QVERIFY(systemText.startsWith(QStringLiteral("=== USER REQUEST (ABSOLUTE HIGHEST PRIORITY) ===")));
    QVERIFY(systemText.contains(prompt));
    QVERIFY(systemText.contains(QStringLiteral("=== MASTER DRAWING WORKFLOW (MANDATORY) ===")));
}

void KisAiStrokeProgramTest::testCompositionPlanOpenAiChoicesUnwrapping()
{
    // Verify unwrapping OpenAI response choices[0].message.content with markdown fences
    const QByteArray openAiPayload = QByteArrayLiteral(
        "{\n"
        "  \"id\": \"chatcmpl-123\",\n"
        "  \"choices\": [{\n"
        "    \"message\": {\n"
        "      \"role\": \"assistant\",\n"
        "      \"content\": \"```json\\n{\\n  \\\"composition_type\\\": \\\"golden_spiral\\\",\\n  \\\"focal_point\\\": {\\\"x\\\": 0.38, \\\"y\\\": 0.62},\\n  \\\"primary_palette\\\": [\\\"#2b1055\\\", \\\"#7597de\\\"],\\n  \\\"artistic_directives\\\": \\\"Create high-contrast dynamic lighting with spiral flow.\\\"\\n}\\n```\"\n"
        "    }\n"
        "  }]\n"
        "}"
    );

    QString directives;
    QString error;
    const bool ok = KisAiStrokeProgramCodec::parseCompositionPlan(openAiPayload, &directives, &error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(directives.contains(QStringLiteral("Create high-contrast dynamic lighting")));
    QVERIFY(directives.contains(QStringLiteral("Focal point at (0.38, 0.62)")));
    QVERIFY(directives.contains(QStringLiteral("golden_spiral")));
}

void KisAiStrokeProgramTest::testTrimOperationsPreservesRibbonAndParticles()
{
    // Verify intelligent trimming preserves Ribbon and Particles when they contain geometric content
    KisAiStrokeProgram prog;
    prog.prompt = QStringLiteral("Magic sparkle effect");
    prog.canvasSize = QSize(800, 600);

    // Add ribbon operation with spine
    KisAiStrokeOperation ribbon;
    ribbon.kind = KisAiStrokeOperation::Kind::Ribbon;
    ribbon.layer = QStringLiteral("FX");
    for (int i = 0; i < 10; ++i) {
        ribbon.spine.append(QPointF(0.1 + i * 0.08, 0.2 + (i % 2) * 0.1));
    }
    prog.operations.append(ribbon);

    // Add particles operation with bounds and count
    KisAiStrokeOperation particles;
    particles.kind = KisAiStrokeOperation::Kind::Particles;
    particles.layer = QStringLiteral("FX");
    particles.bounds = QRectF(0.2, 0.2, 0.5, 0.5);
    particles.particleCount = 50;
    prog.operations.append(particles);

    // Add many small low-priority operations in Details/FX
    for (int i = 0; i < 20; ++i) {
        KisAiStrokeOperation smallOp;
        smallOp.kind = KisAiStrokeOperation::Kind::Fill;
        smallOp.layer = QStringLiteral("Details");
        smallOp.polygon << QPointF(0.01, 0.01) << QPointF(0.011, 0.01) << QPointF(0.01, 0.011);
        prog.operations.append(smallOp);
    }

    // Trim to budget of 5 operations
    const KisAiStrokeProgram trimmed = KisAiStrokeProgramCodec::trimOperationsToBudget(prog, 5);
    QVERIFY(trimmed.operations.size() <= 5);

    bool hasRibbon = false;
    bool hasParticles = false;
    for (const auto &op : trimmed.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::Ribbon) hasRibbon = true;
        if (op.kind == KisAiStrokeOperation::Kind::Particles) hasParticles = true;
    }
    QVERIFY(hasRibbon);
    QVERIFY(hasParticles);
}

void KisAiStrokeProgramTest::testQualityScoreWithHatch()
{
    KisAiStrokeProgram prog;
    prog.prompt = QStringLiteral("Manga hatching sketch");
    prog.canvasSize = QSize(1000, 1000);

    // Add Hatch operation with a polygon on Shading layer
    KisAiStrokeOperation hatchOp;
    hatchOp.kind = KisAiStrokeOperation::Kind::Hatch;
    hatchOp.layer = QStringLiteral("Shading");
    hatchOp.polygon << QPointF(0.1, 0.1) << QPointF(0.5, 0.1) << QPointF(0.5, 0.5) << QPointF(0.1, 0.5);
    hatchOp.brush.color = QColor(20, 20, 20);
    hatchOp.angleDeg = 45.0;
    hatchOp.spacing = 0.02;
    prog.operations.append(hatchOp);

    // Also add lineart and flats
    KisAiStrokeOperation flatsOp;
    flatsOp.kind = KisAiStrokeOperation::Kind::Fill;
    flatsOp.layer = QStringLiteral("Flats");
    flatsOp.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
    flatsOp.brush.color = QColor(240, 240, 230);
    prog.operations.append(flatsOp);

    KisAiStrokeOperation lineOp;
    lineOp.kind = KisAiStrokeOperation::Kind::Path;
    lineOp.layer = QStringLiteral("Lineart");
    lineOp.points.append(KisAiStrokePoint(0.1, 0.1, 0.8));
    lineOp.points.append(KisAiStrokePoint(0.5, 0.5, 0.8));
    lineOp.points.append(KisAiStrokePoint(0.9, 0.9, 0.8));
    lineOp.brush.color = QColor(10, 10, 10);
    prog.operations.append(lineOp);

    const qreal scoreWithHatch = KisAiStrokeProgramCodec::qualityScore(prog);
    QVERIFY(scoreWithHatch > 0.4);

    // If Hatch polygon was ignored, silhouette and point metrics would be lower.
    // Ensure Hatch contributes positively.
    KisAiStrokeProgram progNoHatch = prog;
    progNoHatch.operations.removeFirst(); // remove Hatch
    const qreal scoreNoHatch = KisAiStrokeProgramCodec::qualityScore(progNoHatch);
    QVERIFY(scoreWithHatch > scoreNoHatch);
}

void KisAiStrokeProgramTest::testTrimOperationsPreservesOriginalOrderWithinLayers()
{
    // Verify that when trimOperationsToBudget selects the top-scoring operations,
    // their relative chronological execution order within each layer is preserved.
    KisAiStrokeProgram prog;
    prog.prompt = QStringLiteral("Layer order test");
    prog.canvasSize = QSize(1000, 1000);

    // Add 3 operations to "Flats", with specific IDs and varied scores/sizes
    KisAiStrokeOperation op1;
    op1.id = QStringLiteral("first_small_bg");
    op1.layer = QStringLiteral("Flats");
    op1.kind = KisAiStrokeOperation::Kind::Fill;
    op1.polygon << QPointF(0.1, 0.1) << QPointF(0.3, 0.1) << QPointF(0.3, 0.3); // area ~ 0.02
    prog.operations.append(op1);

    KisAiStrokeOperation op2;
    op2.id = QStringLiteral("second_huge_main");
    op2.layer = QStringLiteral("Flats");
    op2.kind = KisAiStrokeOperation::Kind::Fill;
    op2.polygon << QPointF(0.0, 0.0) << QPointF(0.8, 0.0) << QPointF(0.8, 0.8) << QPointF(0.0, 0.8); // area 0.64
    prog.operations.append(op2);

    KisAiStrokeOperation op3;
    op3.id = QStringLiteral("third_medium_overlay");
    op3.layer = QStringLiteral("Flats");
    op3.kind = KisAiStrokeOperation::Kind::Fill;
    op3.polygon << QPointF(0.2, 0.2) << QPointF(0.6, 0.2) << QPointF(0.6, 0.6) << QPointF(0.2, 0.6); // area 0.16
    prog.operations.append(op3);

    // Trim to budget of 2 operations.
    // The top 2 scoring operations are op2 (huge) and op3 (medium).
    // In chronological order, op2 came before op3!
    const KisAiStrokeProgram trimmed = KisAiStrokeProgramCodec::trimOperationsToBudget(prog, 2);
    QCOMPARE(trimmed.operations.size(), 2);

    // op2 must be before op3 because op2 was defined before op3 in original operations!
    QCOMPARE(trimmed.operations.at(0).id, QStringLiteral("second_huge_main"));
    QCOMPARE(trimmed.operations.at(1).id, QStringLiteral("third_medium_overlay"));
}

void KisAiStrokeProgramTest::testExtractOperationsQualityReportPassthrough()
{
    // Verify fallback extraction populates the KisAiStrokeQualityReport
    const QString mangledResponse = QStringLiteral(
        "I couldn't generate full json but here is a stroke:\n"
        "{\"kind\": \"fill\", \"layer\": \"Flats\", \"polygon\": [[0.1, 0.1], [0.5, 0.1], [0.5, 0.5]], "
        "\"brush\": {\"color\": \"#ff0055\"}}\n"
        "And another:\n"
        "{\"kind\": \"path\", \"layer\": \"Lineart\", \"points\": [[0.1, 0.1], [0.5, 0.5], [0.9, 0.9]], "
        "\"brush\": {\"color\": \"#111111\"}}\n"
        "Hope this helps!"
    );

    KisAiStrokeProgram prog;
    QString error;
    KisAiJsonDiagnostic diag;
    KisAiStrokeQualityReport report;
    const bool ok = KisAiStrokeProgramCodec::extractOperationsFromRawText(
        mangledResponse, &prog, &error, &diag, &report
    );

    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(prog.operations.size(), 2);
    QVERIFY(report.outputOperations >= 2);
    QVERIFY(report.score > 0.0);

    // Also test parseResponse passthrough when fallback is triggered
    KisAiStrokeProgram prog2;
    KisAiStrokeQualityReport report2;
    const bool ok2 = KisAiStrokeProgramCodec::parseResponse(
        mangledResponse.toUtf8(), &prog2, &error, &diag, &report2
    );
    QVERIFY2(ok2, qPrintable(error));
    QCOMPARE(prog2.operations.size(), 2);
    QVERIFY(report2.outputOperations >= 2);
    QVERIFY(report2.score > 0.0);
}

void KisAiStrokeProgramTest::testTypeCheckerParticleAndMangaLinesAliases()
{
    // 1. Test particle_count and particle_shape aliases and string-to-int coercion
    QJsonObject particleOp;
    particleOp[QStringLiteral("kind")] = QStringLiteral("particles");
    particleOp[QStringLiteral("layer")] = QStringLiteral("FX");
    particleOp[QStringLiteral("particle_shape")] = QStringLiteral("sparkle");
    particleOp[QStringLiteral("particle_count")] = QStringLiteral("42");
    particleOp[QStringLiteral("rect")] = QJsonArray({0.2, 0.2, 0.8, 0.8});

    QJsonObject root;
    root[QStringLiteral("schema_version")] = 2;
    root[QStringLiteral("operations")] = QJsonArray({particleOp});

    KisAiStrokeTypeCheckReport report;
    KisAiStrokeTypeChecker::checkAndCoerceProgram(&root, &report);

    const QJsonObject coercedParticle = root[QStringLiteral("operations")].toArray().at(0).toObject();
    QCOMPARE(coercedParticle.value(QStringLiteral("shape")).toString(), QStringLiteral("sparkle"));
    QCOMPARE(coercedParticle.value(QStringLiteral("count")).toInt(), 42);
    QVERIFY(coercedParticle.contains(QStringLiteral("bounds")));

    // 2. Test manga_lines inner_radius, outer_radius, jitter string coercions
    QJsonObject mangaOp;
    mangaOp[QStringLiteral("kind")] = QStringLiteral("manga_lines");
    mangaOp[QStringLiteral("layer")] = QStringLiteral("FX");
    mangaOp[QStringLiteral("center_pt")] = QJsonArray({QStringLiteral("0.4"), QStringLiteral("0.6")});
    mangaOp[QStringLiteral("inner")] = QStringLiteral("0.25");
    mangaOp[QStringLiteral("outer")] = QStringLiteral("0.85");
    mangaOp[QStringLiteral("jitter")] = QStringLiteral("0.15");
    mangaOp[QStringLiteral("density")] = QStringLiteral("60");

    QJsonObject root2;
    root2[QStringLiteral("schema_version")] = 2;
    root2[QStringLiteral("operations")] = QJsonArray({mangaOp});

    KisAiStrokeTypeCheckReport report2;
    KisAiStrokeTypeChecker::checkAndCoerceProgram(&root2, &report2);

    const QJsonObject coercedManga = root2[QStringLiteral("operations")].toArray().at(0).toObject();
    QVERIFY(coercedManga.contains(QStringLiteral("center")));
    QCOMPARE(coercedManga.value(QStringLiteral("inner_radius")).toDouble(), 0.25);
    QCOMPARE(coercedManga.value(QStringLiteral("outer_radius")).toDouble(), 0.85);
    QCOMPARE(coercedManga.value(QStringLiteral("line_length_jitter")).toDouble(), 0.15);
    QCOMPARE(coercedManga.value(QStringLiteral("density")).toInt(), 60);
}

void KisAiStrokeProgramTest::testAnimeEyeParsingAndRefinement()
{
    const QString json = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"anime_eye\",\n"
        "      \"id\": \"left_eye\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"center\": [0.38, 0.42],\n"
        "      \"size\": [0.08, 0.10],\n"
        "      \"iris_color\": \"#3a7bd5\",\n"
        "      \"secondary_color\": \"#00d2ff\",\n"
        "      \"style\": \"sparkle\",\n"
        "      \"expression\": \"open\",\n"
        "      \"is_right\": false\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(json.toUtf8(), &prog, &error), qPrintable(error));
    QCOMPARE(prog.operations.size(), 1);

    const KisAiStrokeOperation &op = prog.operations.at(0);
    QCOMPARE(op.kind, KisAiStrokeOperation::Kind::AnimeEye);
    QCOMPARE(op.id, QStringLiteral("left_eye"));
    QCOMPARE(op.eyeCenter, QPointF(0.38, 0.42));
    QCOMPARE(op.eyeSize, QSizeF(0.08, 0.10));
    QCOMPARE(op.eyeIrisColor, QColor(QStringLiteral("#3a7bd5")));
    QCOMPARE(op.eyeSecondaryColor, QColor(QStringLiteral("#00d2ff")));
    QCOMPARE(op.eyeStyle, QStringLiteral("sparkle"));
    QCOMPARE(op.eyeExpression, QStringLiteral("open"));
    QCOMPARE(op.eyeIsRight, false);

    // Verify type checker handles eye alias
    QJsonObject aliasEye;
    aliasEye[QStringLiteral("kind")] = QStringLiteral("eye");
    aliasEye[QStringLiteral("layer")] = QStringLiteral("Flats");
    aliasEye[QStringLiteral("eye_center")] = QJsonArray({0.62, 0.42});
    aliasEye[QStringLiteral("eye_size")] = QJsonArray({0.08, 0.10});
    aliasEye[QStringLiteral("color")] = QStringLiteral("#e040fb");
    aliasEye[QStringLiteral("right")] = true;

    QJsonObject root;
    root[QStringLiteral("schema_version")] = 2;
    root[QStringLiteral("operations")] = QJsonArray({aliasEye});

    KisAiStrokeTypeCheckReport report;
    KisAiStrokeTypeChecker::checkAndCoerceProgram(&root, &report);

    const QJsonObject coercedEye = root[QStringLiteral("operations")].toArray().at(0).toObject();
    QCOMPARE(coercedEye.value(QStringLiteral("kind")).toString(), QStringLiteral("anime_eye"));
    QVERIFY(coercedEye.contains(QStringLiteral("center")));
    QVERIFY(coercedEye.contains(QStringLiteral("size")));
    QVERIFY(coercedEye.contains(QStringLiteral("iris_color")));
}

void KisAiStrokeProgramTest::testDotNoiseSuppression()
{
    // A single isolated point in Lineart/Shading (stippling / dot noise) should be dropped
    // unless it is an intentional catchlight/glint.
    KisAiStrokeProgram prog;
    prog.schemaVersion = 2;
    prog.canvasSize = QSize(512, 512);

    KisAiStrokeOperation noiseDot;
    noiseDot.kind = KisAiStrokeOperation::Kind::Path;
    noiseDot.id = QStringLiteral("noise_speck_1");
    noiseDot.layer = QStringLiteral("Lineart");
    noiseDot.points = {KisAiStrokePoint(0.5, 0.5, 1.0)};
    prog.operations.append(noiseDot);

    KisAiStrokeOperation catchlight;
    catchlight.kind = KisAiStrokeOperation::Kind::Path;
    catchlight.id = QStringLiteral("eye_catchlight");
    catchlight.layer = QStringLiteral("Highlights");
    catchlight.points = {KisAiStrokePoint(0.38, 0.40, 1.0)};
    prog.operations.append(catchlight);

    KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(prog);
    // Only the intentional catchlight survives; the random noise dot is dropped.
    QCOMPARE(refined.operations.size(), 1);
    QCOMPARE(refined.operations.at(0).id, QStringLiteral("eye_catchlight"));
}

void KisAiStrokeProgramTest::testParticleAccumulationBlockedInMerge()
{
    // V3 Phase 0.1: Goal Mode must not accumulate particle layers step after step.
    const bool wasEnabled = KisAiStrokeProgramCodec::isParticleSuppressionEnabled();
    KisAiStrokeProgramCodec::setParticleSuppressionEnabled(true);

    auto makeParticles = [](const QString &id) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Particles;
        op.id = id;
        op.layer = QStringLiteral("FX");
        op.bounds = QRectF(0.1, 0.1, 0.8, 0.8);
        op.particleCount = 12;
        return op;
    };
    auto makePath = [](const QString &id) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.id = id;
        op.layer = QStringLiteral("Lineart");
        op.points = {KisAiStrokePoint(0.2, 0.2, 0.8), KisAiStrokePoint(0.5, 0.5, 0.9),
                     KisAiStrokePoint(0.8, 0.2, 0.8)};
        return op;
    };

    KisAiStrokeProgram base;
    base.schemaVersion = 2;
    base.canvasSize = QSize(512, 512);
    base.operations.append(makeParticles(QStringLiteral("step1_petals")));
    base.operations.append(makePath(QStringLiteral("contour")));

    KisAiStrokeProgram extension;
    extension.schemaVersion = 2;
    extension.canvasSize = QSize(512, 512);
    extension.operations.append(makeParticles(QStringLiteral("step2_sparkles")));
    extension.operations.append(makePath(QStringLiteral("detail")));

    const KisAiStrokeProgram merged = KisAiStrokeProgramCodec::mergePrograms(base, extension);
    int particleOps = 0;
    for (const KisAiStrokeOperation &op : merged.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::Particles)
            ++particleOps;
    }
    // Only the base step's particles survive; the extension's are dropped.
    QCOMPARE(particleOps, 1);
    QCOMPARE(merged.operations.size(), 3);

    // Suppression OFF restores legacy additive behavior.
    KisAiStrokeProgramCodec::setParticleSuppressionEnabled(false);
    const KisAiStrokeProgram legacy = KisAiStrokeProgramCodec::mergePrograms(base, extension);
    int legacyParticles = 0;
    for (const KisAiStrokeOperation &op : legacy.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::Particles)
            ++legacyParticles;
    }
    QCOMPARE(legacyParticles, 2);
    KisAiStrokeProgramCodec::setParticleSuppressionEnabled(wasEnabled);
}

void KisAiStrokeProgramTest::testParticlesOperationCapInRefine()
{
    // V3 Phase 0.1: A single response can carry at most maxParticlesOperations().
    const bool wasEnabled = KisAiStrokeProgramCodec::isParticleSuppressionEnabled();
    KisAiStrokeProgramCodec::setParticleSuppressionEnabled(true);

    KisAiStrokeProgram prog;
    prog.schemaVersion = 2;
    prog.canvasSize = QSize(512, 512);
    for (int i = 0; i < KisAiStrokeProgramCodec::maxParticlesOperations() + 3; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Particles;
        op.id = QStringLiteral("noise_%1").arg(i);
        op.layer = QStringLiteral("FX");
        op.bounds = QRectF(0.05, 0.05, 0.9, 0.9);
        op.particleCount = 10;
        prog.operations.append(op);
    }
    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(prog, &report);
    int particleOps = 0;
    for (const KisAiStrokeOperation &op : refined.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::Particles)
            ++particleOps;
    }
    QCOMPARE(particleOps, KisAiStrokeProgramCodec::maxParticlesOperations());
    QVERIFY(report.droppedOperations >= 3);
    QVERIFY(!report.warnings.isEmpty());
    KisAiStrokeProgramCodec::setParticleSuppressionEnabled(wasEnabled);
}

void KisAiStrokeProgramTest::testEyePairSymmetryLint()
{
    // V3 Phase 0.3: A skewed eye pair must raise a symmetry warning for the
    // quality self-correction loop, while a symmetric pair stays silent.
    auto makeEye = [](const QString &id, qreal cx, qreal cy) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::AnimeEye;
        op.id = id;
        op.layer = QStringLiteral("Lineart");
        op.eyeCenter = QPointF(cx, cy);
        op.eyeSize = QSizeF(0.10, 0.12);
        return op;
    };

    KisAiStrokeProgram symmetric;
    symmetric.schemaVersion = 2;
    symmetric.canvasSize = QSize(512, 512);
    symmetric.operations.append(makeEye(QStringLiteral("eye_l"), 0.38, 0.42));
    symmetric.operations.append(makeEye(QStringLiteral("eye_r"), 0.62, 0.42));
    KisAiStrokeQualityReport symReport;
    KisAiStrokeProgramCodec::refineForRendering(symmetric, &symReport);
    QVERIFY(!symReport.warnings.join(QStringLiteral("\n")).contains(QStringLiteral("asymmetric")));

    KisAiStrokeProgram skewed;
    skewed.schemaVersion = 2;
    skewed.canvasSize = QSize(512, 512);
    skewed.operations.append(makeEye(QStringLiteral("eye_l"), 0.30, 0.40));
    skewed.operations.append(makeEye(QStringLiteral("eye_r"), 0.75, 0.55));
    KisAiStrokeQualityReport skewReport;
    KisAiStrokeProgramCodec::refineForRendering(skewed, &skewReport);
    QVERIFY(skewReport.warnings.join(QStringLiteral("\n")).contains(QStringLiteral("asymmetric")));
}

void KisAiStrokeProgramTest::testSceneSpecSchemaStrict()
{
    // V3 Phase 1.1: sceneSpecJsonSchema must enforce meaning only, no raw coordinates
    const QJsonObject schema = KisAiSceneSpecCodec::sceneSpecJsonSchema();
    QCOMPARE(schema.value(QStringLiteral("type")).toString(), QStringLiteral("object"));
    const QJsonArray req = schema.value(QStringLiteral("required")).toArray();
    QVERIFY(req.contains(QJsonValue(QStringLiteral("subject"))));
    QVERIFY(req.contains(QJsonValue(QStringLiteral("head"))));

    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    QVERIFY(props.contains(QStringLiteral("subject")));
    QVERIFY(props.contains(QStringLiteral("head")));
    QVERIFY(props.contains(QStringLiteral("composition")));
    QVERIFY(props.contains(QStringLiteral("light")));
    QVERIFY(props.contains(QStringLiteral("negative")));
    // No coordinate fields (operations/strokes/points)
    QVERIFY(!props.contains(QStringLiteral("operations")));
    QVERIFY(!props.contains(QStringLiteral("strokes")));
    QVERIFY(!props.contains(QStringLiteral("points")));
}

void KisAiStrokeProgramTest::testSceneSpecParsingAndDefault()
{
    // V3 Phase 1.1: Spec parsing and keyword-derived defaults
    const QByteArray json = QByteArrayLiteral(
        "{\n"
        "  \"subject\": {\"type\": \"character\", \"pose_id\": \"three_quarter_bust\", \"facing\": \"front-right\"},\n"
        "  \"head\": {\"expression\": \"smile_open\", \"gaze\": \"front\", \"hair_style\": \"twin_tails\", \"hair_color\": \"#fa0055\", \"eye_color\": \"#00ccff\"},\n"
        "  \"composition\": {\"framing\": \"bust_up\", \"head_center\": [0.5, 0.40], \"head_height\": 0.45},\n"
        "  \"light\": {\"warmth\": \"warm_key_cool_fill\", \"time\": \"night\"}\n"
        "}"
    );

    KisAiSceneSpec spec;
    QString err;
    QStringList warnings;
    QVERIFY(KisAiSceneSpecCodec::parseSceneSpec(json, &spec, &err, &warnings));
    QCOMPARE(spec.head.hairStyle, QStringLiteral("twin_tails"));
    QCOMPARE(spec.light.timeOfDay, QStringLiteral("night"));
    QCOMPARE(spec.composition.headHeight, 0.45);
    QCOMPARE(spec.subject.facing, QStringLiteral("front-right"));

    // Fallback keyword derivation
    const KisAiSceneSpec defSpec = KisAiSceneSpecCodec::defaultSpecForPrompt(
        QStringLiteral("silver hair girl with green eyes in starry night sky"), QSize(1024, 1024));
    QCOMPARE(defSpec.light.timeOfDay, QStringLiteral("night"));
    QCOMPARE(defSpec.head.eyeColor, QColor(34, 197, 94));
    QCOMPARE(defSpec.head.hairColor, QColor(226, 232, 240));
}

void KisAiStrokeProgramTest::testHeadRigSymmetryAndHairMass()
{
    // V3 Phase 1.2: HeadRig must be symmetric around center.x()
    const QPointF center(0.5, 0.4);
    const qreal width = 0.30;
    const qreal height = 0.40;
    const QPolygonF outline = KisAiLayoutEngine::headOutlinePolygon(center, width, height);
    QVERIFY(outline.size() >= 20);

    // Verify left-right symmetry
    for (int i = 0; i < outline.size(); ++i) {
        const QPointF &pt = outline.at(i);
        const qreal dx = pt.x() - center.x();
        // For every point with dx, there must be a point with -dx at approximately same y
        bool foundMirror = false;
        for (int j = 0; j < outline.size(); ++j) {
            const QPointF &mirror = outline.at(j);
            if (qAbs((mirror.x() - center.x()) + dx) < 0.015 && qAbs(mirror.y() - pt.y()) < 0.015) {
                foundMirror = true;
                break;
            }
        }
        QVERIFY2(foundMirror, "Head outline must be symmetric around center.x()");
    }

    // Eye pair centers must be symmetric for front-facing
    const auto eyes = KisAiLayoutEngine::eyePairCenters(center, width, height, QStringLiteral("front"));
    QVERIFY(qAbs((eyes.first.x() + eyes.second.x()) * 0.5 - center.x()) < 1.0e-5);
    QCOMPARE(eyes.first.y(), eyes.second.y());

    // HairMass generation produces ribbon side locks
    KisAiSceneSpec spec;
    spec.head.hairStyle = QStringLiteral("twin_tails");
    spec.head.hairColor = QColor(40, 60, 120);
    const auto hairOps = KisAiLayoutEngine::hairMassForStyle(spec, center, width, height);
    QVERIFY(!hairOps.isEmpty());
    bool hasRibbon = false;
    for (const auto &op : hairOps) {
        if (op.kind == KisAiStrokeOperation::Kind::Ribbon) {
            hasRibbon = true;
            break;
        }
    }
    QVERIFY(hasRibbon);
}

void KisAiStrokeProgramTest::testLayoutEngineGeneratesProgram()
{
    // V3 Phase 1.2: LayoutEngine generates a complete, valid KisAiStrokeProgram
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("Anime girl with twintails");
    spec.subject.type = QStringLiteral("character");
    spec.head.expression = QStringLiteral("smile_open");
    spec.head.hairStyle = QStringLiteral("twin_tails");

    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(1024, 1024));
    QVERIFY(prog.isValid());
    QVERIFY(!prog.operations.isEmpty());

    int animeEyeCount = 0;
    bool hasFaceSkin = false;
    bool hasHair = false;
    for (const auto &op : prog.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye)
            ++animeEyeCount;
        if (op.id.contains(QStringLiteral("face_skin")))
            hasFaceSkin = true;
        if (op.id.contains(QStringLiteral("hair")))
            hasHair = true;
    }
    QCOMPARE(animeEyeCount, 2);
    QVERIFY(hasFaceSkin);
    QVERIFY(hasHair);
}

void KisAiStrokeProgramTest::testLightRigConsistency()
{
    // V3 Phase 2.1: LightRig shadow & highlight coherence
    KisAiSceneSpec nightSpec;
    nightSpec.light.timeOfDay = QStringLiteral("night");
    nightSpec.light.warmth = QStringLiteral("warm_key_cool_fill");
    const KisAiLightSettings nightRig = KisAiLightRig::fromSpec(nightSpec);

    const QColor skin(255, 224, 192);
    const QColor nightShadow = KisAiLightRig::shadowColor(skin, nightRig);
    // Never pure black
    QVERIFY(nightShadow != QColor(0, 0, 0));
    QVERIFY(nightShadow.value() >= 30);
    // Night fill tint is bluish/cool (hue between 180 and 300)
    QVERIFY(nightRig.fillTint.hue() >= 180 && nightRig.fillTint.hue() <= 300);

    const QColor midTone(100, 120, 160);
    const QColor nightHighlight = KisAiLightRig::highlightColor(midTone, nightRig);
    QVERIFY(nightHighlight.value() > midTone.value());
}

void KisAiStrokeProgramTest::testFourLayerShadingPresent()
{
    // V3 Phase 2.2: 4-layer shading synthesized by LightRig (core shadow, rim, chin AO, hair cast)
    KisAiSceneSpec spec;
    spec.subject.type = QStringLiteral("character");
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(1024, 1024));

    bool hasCoreShadow = false;
    bool hasRim = false;
    bool hasChinAo = false;
    bool hasHairCast = false;
    for (const auto &op : prog.operations) {
        if (op.id.contains(QStringLiteral("core_shadow")))
            hasCoreShadow = true;
        if (op.id.contains(QStringLiteral("rim_light")))
            hasRim = true;
        if (op.id == QStringLiteral("chin_ao"))
            hasChinAo = true;
        if (op.id == QStringLiteral("hair_cast_shadow"))
            hasHairCast = true;
    }
    QVERIFY(hasCoreShadow);
    QVERIFY(hasRim);
    QVERIFY(hasChinAo);
    QVERIFY(hasHairCast);
}

void KisAiStrokeProgramTest::testLineartHierarchy()
{
    // V3/V4 Phase 2.3: Lineart stroke weights into 4-tier hierarchy
    // Tier 3: Major outer silhouette contours (length >= 1.0 -> 0.008)
    // Tier 2: Structural outlines (0.35 <= length < 1.0 -> 0.005)
    // Tier 1: Intermediate contours (0.12 <= length < 0.35 -> 0.003)
    // Tier 0: Micro details, hair strands, delicate hatches, eyelashes (< 0.12 or delicate profile -> 0.0015)
    QVector<KisAiStrokeOperation> ops;

    KisAiStrokeOperation silhouette;
    silhouette.kind = KisAiStrokeOperation::Kind::Path;
    silhouette.layer = QStringLiteral("Lineart");
    silhouette.points = {KisAiStrokePoint(0.1, 0.1), KisAiStrokePoint(0.9, 0.9)}; // length ~ 1.13 >= 1.0
    silhouette.brush.size = 0.02; // non-canonical size
    ops.append(silhouette);

    KisAiStrokeOperation structural;
    structural.kind = KisAiStrokeOperation::Kind::Path;
    structural.layer = QStringLiteral("Lineart");
    structural.points = {KisAiStrokePoint(0.2, 0.2), KisAiStrokePoint(0.6, 0.5)}; // length = 0.50 (0.35 <= length < 1.0)
    structural.brush.size = 0.02;
    ops.append(structural);

    KisAiStrokeOperation intermediate;
    intermediate.kind = KisAiStrokeOperation::Kind::Path;
    intermediate.layer = QStringLiteral("Lineart");
    intermediate.points = {KisAiStrokePoint(0.4, 0.4), KisAiStrokePoint(0.55, 0.55)}; // length ~ 0.212 (0.12 <= length < 0.35)
    intermediate.brush.size = 0.02;
    ops.append(intermediate);

    KisAiStrokeOperation micro;
    micro.kind = KisAiStrokeOperation::Kind::Path;
    micro.layer = QStringLiteral("Lineart");
    micro.points = {KisAiStrokePoint(0.5, 0.5), KisAiStrokePoint(0.55, 0.55)}; // length ~ 0.071 (< 0.12)
    micro.brush.size = 0.02;
    ops.append(micro);

    const int adjusted = KisAiStrokeQualityUtils::applyLineartHierarchy(ops);
    QCOMPARE(adjusted, 4);
    QCOMPARE(ops[0].brush.size, 0.008);  // Tier 3: major outer contours
    QCOMPARE(ops[1].brush.size, 0.005);  // Tier 2: structural outlines
    QCOMPARE(ops[2].brush.size, 0.003);  // Tier 1: intermediate contours
    QCOMPARE(ops[3].brush.size, 0.0015); // Tier 0: micro details
}

void KisAiStrokeProgramTest::testBrushPresetMapping()
{
    // V3 Phase 2.4: Profile to Krita preset mapping
    QCOMPARE(KisAiStrokeQualityUtils::brushPresetName(QStringLiteral("gpen")), QStringLiteral("Pencil-2"));
    QCOMPARE(KisAiStrokeQualityUtils::brushPresetName(QStringLiteral("watercolor")), QStringLiteral("Watercolor Soft"));
    QCOMPARE(KisAiStrokeQualityUtils::brushPresetName(QStringLiteral("airbrush")), QStringLiteral("Airbrush Soft"));
    QCOMPARE(KisAiStrokeQualityUtils::brushPresetName(QStringLiteral("crayon")), QStringLiteral("Chalk Soft"));

    KisAiStrokeProgram prog;
    prog.schemaVersion = 2;
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.brush.profile = QStringLiteral("gpen");
    prog.operations.append(op);

    const int assigned = KisAiStrokeQualityUtils::assignBrushPresetHints(prog);
    QCOMPARE(assigned, 1);
    QCOMPARE(prog.operations[0].brush.presetHint, QStringLiteral("Pencil-2"));
}

void KisAiStrokeProgramTest::testStructuredCritiqueParsing()
{
    // V3 Phase 3.2: Machine-readable critique regions parsing
    const QByteArray json = QByteArrayLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"prompt\": \"Anime portrait\",\n"
        "  \"agent_critique\": \"Right eye is slightly drifted and chin shading lacks soft diffusion.\",\n"
        "  \"regions\": [\n"
        "    {\"area\": \"right_eye\", \"issue\": \"offset from eye center line\", \"action\": \"repaint\", \"priority\": 4},\n"
        "    {\"area\": \"chin_ao\", \"issue\": \"harsh edge on contact shadow\", \"action\": \"soften\", \"priority\": 2}\n"
        "  ],\n"
        "  \"operations\": [\n"
        "    {\"kind\": \"fill\", \"id\": \"face_skin\", \"layer\": \"Flats\", \"polygon\": [[0.3,0.3],[0.7,0.3],[0.5,0.7]], \"brush\": {\"color\": \"#ffe0c0\"}}\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog;
    QString err;
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(QJsonDocument::fromJson(json).object(), &prog, &err));
    QCOMPARE(prog.critiqueRegions.size(), 2);
    QCOMPARE(prog.critiqueRegions[0].area, QStringLiteral("right_eye"));
    QCOMPARE(prog.critiqueRegions[0].action, QStringLiteral("repaint"));
    QCOMPARE(prog.critiqueRegions[0].priority, 4);
    QCOMPARE(prog.critiqueRegions[1].area, QStringLiteral("chin_ao"));
    QCOMPARE(prog.critiqueRegions[1].action, QStringLiteral("soften"));
    QCOMPARE(prog.critiqueRegions[1].priority, 2);

    // Verify feedback loop propagation into next step payload
    const QJsonObject nextPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"), QStringLiteral("Anime portrait"), QSize(1024, 1024),
        2, 3, QString(), QString(), 500, QString(), false, false, true, 0.7, 1.0, 0, 0, &prog);

    const QJsonArray messages = nextPayload.value(QStringLiteral("messages")).toArray();
    const QString userText = messages.at(1).toObject().value(QStringLiteral("content")).toString();
    QVERIFY(userText.contains(QStringLiteral("previous_step_critique_regions")));
    QVERIFY(userText.contains(QStringLiteral("right_eye")));
}

void KisAiStrokeProgramTest::testFindFieldDoesNotHijackShortKeys()
{
    // The fuzzy fallback used to let "kind".contains("id") assign the kind
    // string as the operation id when no explicit id existed, and let a stray
    // 1-char key match "size"/"points".
    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("path")},
                {QStringLiteral("layer"), QStringLiteral("Lineart")},
                {QStringLiteral("points"), QJsonArray {QJsonArray {0.1, 0.1, 0.8}, QJsonArray {0.9, 0.9, 0.8}}},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("profile"), QStringLiteral("gpen")},
                    {QStringLiteral("color"), QStringLiteral("#000000")},
                    {QStringLiteral("size"), 0.01},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
    QCOMPARE(program.operations.size(), 1);
    // No explicit id/name key: the id must stay empty at parse time (refine
    // generates a synthetic one), never the kind string "path".
    QVERIFY(!program.operations.first().id.contains(QStringLiteral("path")));
}

void KisAiStrokeProgramTest::testSseAccumulatedContentIsBounded()
{
    // A peer streaming endless deltas must not grow the accumulated content
    // without limit; the stream parser gives up once the budget is exceeded.
    QByteArray unprocessed;
    QString content;
    bool isDone = false;

    const QByteArray chunk = QByteArrayLiteral("data: {\"choices\":[{\"delta\":{\"content\":\"AAAA\"}}]}\n\n");
    for (int i = 0; i < 400000; ++i) {
        if (!KisAiStrokeProgramCodec::parseSseStreamChunk(chunk, &unprocessed, &content, &isDone)) {
            break;
        }
    }
    QVERIFY2(content.size() <= 33 * 1024 * 1024,
             qPrintable(QStringLiteral("content grew to %1").arg(content.size())));
    QVERIFY(unprocessed.isEmpty());
}

void KisAiStrokeProgramTest::testSseChunkWithManyLinesIsLinear()
{
    // A coalesced chunk with many SSE lines used to be quadratic because every
    // line removal memmoved the whole remainder of the buffer.
    QByteArray unprocessed;
    QString content;
    bool isDone = false;

    QByteArray bigChunk;
    bigChunk.reserve(1024 * 1024);
    for (int i = 0; i < 40000; ++i) {
        bigChunk += QByteArrayLiteral(": keep-alive\n");
    }
    bigChunk += QByteArrayLiteral("data: {\"choices\":[{\"delta\":{\"content\":\"X\"}}]}\n\n");

    QElapsedTimer timer;
    timer.start();
    QVERIFY(KisAiStrokeProgramCodec::parseSseStreamChunk(bigChunk, &unprocessed, &content, &isDone));
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY2(elapsedMs < 5000, qPrintable(QStringLiteral("SSE parse took %1 ms").arg(elapsedMs)));
    QCOMPARE(content, QStringLiteral("X"));
    QVERIFY(unprocessed.isEmpty());
}

void KisAiStrokeProgramTest::testCanvasSizeClampedAtParseTime()
{
    // Pixel-vs-normalized auto-scaling divides by the parsed canvas size, so a
    // hostile value must be clamped before any coordinate division happens.
    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("canvas_size"), QJsonObject {
            {QStringLiteral("width"), 2000000000},
            {QStringLiteral("height"), 2000000000},
        }},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("ribbon")},
                {QStringLiteral("layer"), QStringLiteral("Flats")},
                {QStringLiteral("spine"), QJsonArray {QJsonArray {1000.0, 1000.0}, QJsonArray {2000.0, 2000.0}}},
                {QStringLiteral("start_width"), 50.0},
                {QStringLiteral("end_width"), 10.0},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("profile"), QStringLiteral("gpen")},
                    {QStringLiteral("color"), QStringLiteral("#000000")},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error));
    QVERIFY(program.canvasSize.width() <= 4096);
    QVERIFY(program.canvasSize.height() <= 4096);

    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program);
    QVERIFY(!refined.operations.isEmpty());
    // Ribbon spine points must not have been divided down to ~0.
    QVERIFY(refined.operations.first().spine.first().x() > 0.0);
}

void KisAiStrokeProgramTest::testReasoningModelFamilyPrefixMatching()
{
    // "o1"/"o3" must match as a model-family prefix, not as an arbitrary
    // substring (proto1, radio3 must stay non-reasoning).
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("o1")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("o3-mini")));
    QVERIFY(KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("openai/o1-preview")));
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("proto1")));
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("radio3")));
    QVERIFY(!KisAiStrokeProgramCodec::isReasoningModel(QStringLiteral("gpt-4o")));
}

void KisAiStrokeProgramTest::testExtractOperationsDiagnosticNotFabricated()
{
    // A clean JSON document that simply contains no program must not produce a
    // fabricated "error at line 1, column 1" syntax diagnostic.
    KisAiStrokeProgram program;
    QString error;
    KisAiJsonDiagnostic diagnostic;
    const QByteArray noProgram = QByteArrayLiteral("{\"foo\": \"bar\", \"note\": \"no operations here\"}");
    QVERIFY(!KisAiStrokeProgramCodec::parseResponse(noProgram, &program, &error, &diagnostic));
    QVERIFY(!diagnostic.hasError);
    QVERIFY(diagnostic.errorLine <= 0);
}

void KisAiStrokeProgramTest::testSceneSpecPayloadReasoningModelOmitsTemperature()
{
    const QSize canvasSize(1024, 1024);
    const QString prompt = QStringLiteral("A magical girl with glowing twin braids");

    // Reasoning model: must omit temperature & top_p, and set max_completion_tokens
    const QJsonObject reasoningPayload = KisAiSceneSpecCodec::buildSceneSpecPayload(
        QStringLiteral("o3-mini"),
        prompt,
        canvasSize,
        0, // artStyle
        QStringLiteral("high"), // reasoningEffort
        QStringLiteral("Do not draw background particles") // customInstructions
    );

    QVERIFY(!reasoningPayload.contains(QStringLiteral("temperature")));
    QVERIFY(!reasoningPayload.contains(QStringLiteral("top_p")));
    QVERIFY(reasoningPayload.contains(QStringLiteral("max_completion_tokens")));
    QCOMPARE(reasoningPayload.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));
    QVERIFY(reasoningPayload.contains(QStringLiteral("stream")));
    QVERIFY(reasoningPayload.value(QStringLiteral("stream")).toBool());

    // Messages must contain custom instructions
    const QJsonArray messages = reasoningPayload.value(QStringLiteral("messages")).toArray();
    QVERIFY(messages.size() >= 2);
    const QString systemText = messages.at(0).toObject().value(QStringLiteral("content")).toString();
    QVERIFY(systemText.contains(QStringLiteral("Do not draw background particles")));

    // Standard model: must contain temperature & max_tokens
    const QJsonObject standardPayload = KisAiSceneSpecCodec::buildSceneSpecPayload(
        QStringLiteral("gpt-4o"),
        prompt,
        canvasSize,
        0,
        QString(),
        QString(),
        false, // enableStreaming = false
        true,  // enforceJsonFormat = true
        0.7,   // temperature
        0.95   // topP
    );

    QVERIFY(standardPayload.contains(QStringLiteral("temperature")));
    QCOMPARE(standardPayload.value(QStringLiteral("temperature")).toDouble(), 0.7);
    QVERIFY(standardPayload.contains(QStringLiteral("top_p")));
    QCOMPARE(standardPayload.value(QStringLiteral("top_p")).toDouble(), 0.95);
    QVERIFY(standardPayload.contains(QStringLiteral("max_tokens")));
    QVERIFY(!standardPayload.contains(QStringLiteral("stream")));
}

void KisAiStrokeProgramTest::testSceneSpecPayloadStreamingAndJsonSchema()
{
    const QSize canvasSize(1024, 1024);
    const QString prompt = QStringLiteral("Sunset meadow landscape");

    // Model supporting json_schema
    const QJsonObject schemaPayload = KisAiSceneSpecCodec::buildSceneSpecPayload(
        QStringLiteral("gpt-4o"),
        prompt,
        canvasSize,
        0,
        QString(),
        QString(),
        true, // enableStreaming
        true, // enforceJsonFormat
        0.5,
        1.0,
        0,
        false // forceJsonObjectOnly
    );

    QVERIFY(schemaPayload.value(QStringLiteral("stream")).toBool());
    const QJsonObject respFormat = schemaPayload.value(QStringLiteral("response_format")).toObject();
    QCOMPARE(respFormat.value(QStringLiteral("type")).toString(), QStringLiteral("json_schema"));
    const QJsonObject jsonSchema = respFormat.value(QStringLiteral("json_schema")).toObject();
    QCOMPARE(jsonSchema.value(QStringLiteral("name")).toString(), QStringLiteral("scene_spec"));
    QVERIFY(jsonSchema.value(QStringLiteral("strict")).toBool());

    // forceJsonObjectOnly = true
    const QJsonObject forcedObjectPayload = KisAiSceneSpecCodec::buildSceneSpecPayload(
        QStringLiteral("gpt-4o"),
        prompt,
        canvasSize,
        0,
        QString(),
        QString(),
        true,
        true,
        0.5,
        1.0,
        0,
        true // forceJsonObjectOnly
    );
    const QJsonObject forcedFormat = forcedObjectPayload.value(QStringLiteral("response_format")).toObject();
    QCOMPARE(forcedFormat.value(QStringLiteral("type")).toString(), QStringLiteral("json_object"));
    QVERIFY(!forcedFormat.contains(QStringLiteral("json_schema")));

    // enforceJsonFormat = false
    const QJsonObject noJsonPayload = KisAiSceneSpecCodec::buildSceneSpecPayload(
        QStringLiteral("gpt-4o"),
        prompt,
        canvasSize,
        0,
        QString(),
        QString(),
        true,
        false // enforceJsonFormat = false
    );
    QVERIFY(!noJsonPayload.contains(QStringLiteral("response_format")));
}

void KisAiStrokeProgramTest::testJsonModeForcedJsonObjectHandling()
{
    const QSize canvasSize(1024, 1024);
    const QString prompt = QStringLiteral("Cyberpunk city street");

    // buildChatCompletionsPayload with forced json_object
    const QJsonObject forcedChatPayload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(
        QStringLiteral("gpt-4o"),
        prompt,
        canvasSize,
        400,
        QString(),
        QString(),
        true,
        true, // enforceJsonFormat
        0.5,
        1.0,
        0,
        0,
        true // forceJsonObjectOnly
    );
    const QJsonObject chatFmt = forcedChatPayload.value(QStringLiteral("response_format")).toObject();
    QCOMPARE(chatFmt.value(QStringLiteral("type")).toString(), QStringLiteral("json_object"));
    QVERIFY(!chatFmt.contains(QStringLiteral("json_schema")));

    // buildGoalStepPayload with forced json_object
    const QJsonObject forcedGoalPayload = KisAiStrokeProgramCodec::buildGoalStepPayload(
        QStringLiteral("gpt-4o"),
        prompt,
        canvasSize,
        1,
        4,
        QString(),
        QString(),
        400,
        QString(),
        true,
        true,
        true, // enforceJsonFormat
        0.5,
        1.0,
        0,
        0,
        nullptr,
        QString(),
        QStringLiteral("auto"),
        true // forceJsonObjectOnly
    );
    const QJsonObject goalFmt = forcedGoalPayload.value(QStringLiteral("response_format")).toObject();
    QCOMPARE(goalFmt.value(QStringLiteral("type")).toString(), QStringLiteral("json_object"));
    QVERIFY(!goalFmt.contains(QStringLiteral("json_schema")));
}

void KisAiStrokeProgramTest::testColorClauseDeduplication()
{
    // Verify that syncing palette color replaces an existing color clause
    // instead of repeatedly appending duplicates.
    static const QRegularExpression colorPattern(QStringLiteral("(?:、|\\s)*メイン配色:\\s*#[0-9a-fA-F]{6}"));

    QString prompt = QStringLiteral("雨上がりの夜、青い光に包まれた街");
    const QString color1 = QStringLiteral("メイン配色: #2b3a67");
    if (prompt.contains(colorPattern)) {
        prompt.replace(colorPattern, QStringLiteral("、") + color1);
    } else if (!prompt.isEmpty()) {
        prompt += QStringLiteral("、") + color1;
    } else {
        prompt = color1;
    }
    QCOMPARE(prompt, QStringLiteral("雨上がりの夜、青い光に包まれた街、メイン配色: #2b3a67"));

    // Syncing a second color should replace #2b3a67 with #ff5533
    const QString color2 = QStringLiteral("メイン配色: #ff5533");
    if (prompt.contains(colorPattern)) {
        prompt.replace(colorPattern, QStringLiteral("、") + color2);
        if (prompt.startsWith(QStringLiteral("、"))) {
            prompt = prompt.mid(1).trimmed();
        }
    } else if (!prompt.isEmpty()) {
        prompt += QStringLiteral("、") + color2;
    } else {
        prompt = color2;
    }
    QCOMPARE(prompt, QStringLiteral("雨上がりの夜、青い光に包まれた街、メイン配色: #ff5533"));
    QCOMPARE(prompt.count(QStringLiteral("メイン配色:")), 1);
}

void KisAiStrokeProgramTest::testEyeKindWinsOverLineSubstring()
{
    // Regression: kind aliases such as "eye_outline" / "eyeliner" / "eye_lineart"
    // contain "line", which used to be tested first and reclassified the eye as a
    // plain Path, silently losing the AnimeEye renderer.
    static const QStringList kinds {
        QStringLiteral("anime_eye"),
        QStringLiteral("eye"),
        QStringLiteral("eye_outline"),
        QStringLiteral("eyeliner"),
        QStringLiteral("eye_lineart"),
        QStringLiteral("eye_contour"),
    };

    for (const QString &kind : kinds) {
        const QJsonObject root {
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("operations"), QJsonArray {
                QJsonObject {
                    {QStringLiteral("kind"), kind},
                    {QStringLiteral("id"), QStringLiteral("left_eye")},
                    {QStringLiteral("layer"), QStringLiteral("Lineart")},
                    {QStringLiteral("eye_center"), QJsonArray {0.4, 0.4}},
                    {QStringLiteral("eye_size"), QJsonArray {0.12, 0.08}},
                },
            }},
        };

        KisAiStrokeProgram program;
        QString error;
        QVERIFY2(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error),
                 qPrintable(QStringLiteral("kind=%1 error=%2").arg(kind, error)));
        QCOMPARE(program.operations.size(), 1);
        QVERIFY2(program.operations.first().kind == KisAiStrokeOperation::Kind::AnimeEye,
                 qPrintable(QStringLiteral("kind '%1' must stay AnimeEye, not Path").arg(kind)));
    }
}

void KisAiStrokeProgramTest::testNumericStringExponentNotMangled()
{
    // Regression: the numeric-string scrubber used to whitelist only [0-9.+-],
    // so "1e3" became "13" and "-1.5e-3" became "-1.53" - a silently wrong value
    // rather than a rejection. Exponent notation must survive.
    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("seed"), QStringLiteral("1e3")},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("fill")},
                {QStringLiteral("id"), QStringLiteral("exponent_test")},
                {QStringLiteral("layer"), QStringLiteral("Flats")},
                {QStringLiteral("polygon"), QJsonArray {
                    QJsonArray {QStringLiteral("0.2"), QStringLiteral("0.2")},
                    QJsonArray {QStringLiteral("9e-1"), QStringLiteral("0.2")},
                    QJsonArray {QStringLiteral("0.9"), QStringLiteral("0.8")},
                }},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("color"), QStringLiteral("#223344")},
                    {QStringLiteral("size"), QStringLiteral("1e-2")},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error), qPrintable(error));

    // 1e3 == 1000, not 13.
    QCOMPARE(program.seed, 1000);

    QCOMPARE(program.operations.size(), 1);
    const QPolygonF polygon = program.operations.first().polygon;
    QCOMPARE(polygon.size(), 3);
    // "9e-1" == 0.9. Under the old scrub it became "91", which the pixel
    // auto-normalizer would then have scaled against the canvas.
    QVERIFY2(qAbs(polygon.at(1).x() - 0.9) < 1e-6,
             qPrintable(QStringLiteral("got x=%1, expected 0.9 (exponent was mangled)").arg(polygon.at(1).x())));
}

void KisAiStrokeProgramTest::testLiteralsMaskingBudgetIsBounded()
{
    // Regression: the token masker appended one entry per string literal with no
    // cap, so a hostile body of `"a""a""a"...` amplified into millions of QStrings
    // that were then copied by every repair pass. The masking budget must bail out
    // instead of materializing them.
    constexpr int literalCount = 9000; // above the 4096 budget
    QString hostile = QStringLiteral("{\"operations\": [\"");
    hostile.reserve(literalCount * 4 + 64);
    for (int i = 0; i < literalCount; ++i) {
        hostile += QStringLiteral("a\"\"");
    }
    hostile += QStringLiteral("]}");

    KisAiJsonDiagnostic diagnostic;
    const QString sanitized = KisAiStrokeProgramCodec::sanitizeAndExtractJson(hostile, &diagnostic);

    QVERIFY(!sanitized.isEmpty());
    const bool maskingApplied = std::any_of(
        diagnostic.appliedRepairs.cbegin(), diagnostic.appliedRepairs.cend(),
        [](const QString &entry) { return entry.startsWith(QStringLiteral("TokenMasking")); });
    QVERIFY2(!maskingApplied, "masking budget was exceeded but TokenMasking still reported as applied");
}

void KisAiStrokeProgramTest::testCritiqueRegionsCountIsCapped()
{
    // Regression: critique regions were iterated without a size cap, so a hostile
    // multi-megabyte array of {"area": "a"} inflated the program without bound.
    QJsonArray regions;
    for (int i = 0; i < 500; ++i) {
        regions.append(QJsonObject {
            {QStringLiteral("area"), QStringLiteral("face")},
            {QStringLiteral("issue"), QStringLiteral("region %1").arg(i)},
            {QStringLiteral("action"), QStringLiteral("refine")},
        });
    }

    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("regions"), regions},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("path")},
                {QStringLiteral("id"), QStringLiteral("critique_carrier")},
                {QStringLiteral("layer"), QStringLiteral("Lineart")},
                {QStringLiteral("points"), QJsonArray {
                    QJsonArray {0.1, 0.1, 1.0},
                    QJsonArray {0.9, 0.9, 1.0},
                }},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("color"), QStringLiteral("#000000")},
                    {QStringLiteral("size"), 0.01},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error), qPrintable(error));
    QCOMPARE(program.critiqueRegions.size(), 32);
}

void KisAiStrokeProgramTest::testHatchSpacingClampedBeforeRescue()
{
    // Regression: the "A5" hatch rescue compared op.spacing against thresholds
    // before the switch clamped it, so a hostile 1e300 reached the comparison as
    // an unbounded value. spacing must be bounded before any policy test reads it.
    const QJsonObject root {
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("operations"), QJsonArray {
            QJsonObject {
                {QStringLiteral("kind"), QStringLiteral("hatch")},
                {QStringLiteral("id"), QStringLiteral("hostile_hatch")},
                {QStringLiteral("layer"), QStringLiteral("Shading")},
                {QStringLiteral("spacing"), 1.0e300},
                {QStringLiteral("polygon"), QJsonArray {
                    QJsonArray {0.1, 0.1},
                    QJsonArray {0.9, 0.1},
                    QJsonArray {0.9, 0.9},
                    QJsonArray {0.1, 0.9},
                }},
                {QStringLiteral("brush"), QJsonObject {
                    {QStringLiteral("color"), QStringLiteral("#223344")},
                    {QStringLiteral("size"), 0.02},
                }},
            },
        }},
    };

    KisAiStrokeProgram program;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseProgramJson(root, &program, &error), qPrintable(error));
    QCOMPARE(program.operations.size(), 1);

    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program, &report);
    QCOMPARE(refined.operations.size(), 1);

    // The clamp ceiling is 0.2; a non-finite or >0.2 spacing must never survive.
    QVERIFY2(std::isfinite(refined.operations.first().spacing), "spacing must be finite after refine");
    QVERIFY2(refined.operations.first().spacing <= 0.2 + 1e-9,
             qPrintable(QStringLiteral("spacing=%1 exceeded the clamp").arg(refined.operations.first().spacing)));
}

void KisAiStrokeProgramTest::testCompositionPlanRejectsOversizedBody()
{
    // Regression: parseCompositionPlan had no size guard while parseResponse did,
    // allowing the same oversized hostile input through a different entry point.
    QByteArray oversized;
    oversized.reserve(33 * 1024 * 1024);
    oversized.append("{\"directives\": \"");
    oversized.append(QByteArray(33 * 1024 * 1024, 'x'));
    oversized.append("\"}");

    QString directives;
    QString error;
    QVERIFY2(!KisAiStrokeProgramCodec::parseCompositionPlan(oversized, &directives, &error),
             "an oversized composition plan must be rejected");
    QVERIFY(!error.isEmpty());

    // Sanity: a normal-sized payload still parses. The parser reads the
    // "artistic_directives" key and requires the result to be non-empty.
    QString okDirectives;
    QString okError;
    const QByteArray normal = QByteArrayLiteral("{\"artistic_directives\": \"draw a calm portrait\"}");
    QVERIFY2(KisAiStrokeProgramCodec::parseCompositionPlan(normal, &okDirectives, &okError), qPrintable(okError));
    QCOMPARE(okDirectives, QStringLiteral("draw a calm portrait"));
}

void KisAiStrokeProgramTest::testExtractOperationsSchemaVersionGate()
{
    // Regression: the recovery path used captured(1).toInt() directly, so an
    // overflowing digit run silently became 0 and bypassed the v1/v2 gate that
    // parseProgramJson enforces.
    const QString hostile = QStringLiteral(
        "{\"schema_version\": 99999999999999999999, \"operations\": "
        "[{\"kind\":\"path\",\"id\":\"p\",\"layer\":\"Lineart\",\"points\":[[0.1,0.1,1.0],[0.9,0.9,1.0]],"
        "\"brush\":{\"color\":\"#000000\",\"size\":0.01}}]}");

    KisAiStrokeProgram program;
    QString error;
    KisAiStrokeProgramCodec::extractOperationsFromRawText(hostile, &program, &error);

    // An out-of-range/overflowing version must fall back to the current schema (2),
    // never be recorded as a bogus version.
    QCOMPARE(program.schemaVersion, 2);
}

void KisAiStrokeProgramTest::testSceneSpecRejectsOversizedBody()
{
    // Regression: parseSceneSpec had no size guard while parseResponse and
    // parseCompositionPlan did, allowing the same oversized hostile input through
    // a third entry point instead of rejecting it up front.
    QByteArray oversized;
    oversized.reserve(33 * 1024 * 1024);
    oversized.append("{\"subject\": {\"type\": \"");
    oversized.append(QByteArray(33 * 1024 * 1024, 'x'));
    oversized.append("\"}}");

    QString error;
    KisAiSceneSpec spec;
    QVERIFY2(!KisAiSceneSpecCodec::parseSceneSpec(oversized, &spec, &error),
             "an oversized SceneSpec response must be rejected");
    QVERIFY(!error.isEmpty());

    // Sanity: a normal-sized payload still parses after the guard.
    QString okError;
    QStringList okWarnings;
    KisAiSceneSpec okSpec;
    const QByteArray normal = QByteArrayLiteral(
        "{\"subject\": {\"type\": \"character\"}, "
        "\"head\": {\"hair_color\": \"#2b3a67\"}}");
    QVERIFY2(KisAiSceneSpecCodec::parseSceneSpec(normal, &okSpec, &okError, &okWarnings),
             qPrintable(okError));
    QCOMPARE(okSpec.subject.type, QStringLiteral("character"));
    QCOMPARE(okSpec.head.hairColor, QColor(0x2b, 0x3a, 0x67));
}

void KisAiStrokeProgramTest::testStructuredOutputsJsonSchemaCompleteness()
{
    const QJsonObject schema = KisAiStrokeProgramCodec::strokeProgramJsonSchema();
    QVERIFY(!schema.isEmpty());
    QCOMPARE(schema.value(QStringLiteral("type")).toString(), QStringLiteral("object"));

    const QJsonObject rootProps = schema.value(QStringLiteral("properties")).toObject();
    QVERIFY(rootProps.contains(QStringLiteral("schema_version")));
    QVERIFY(rootProps.contains(QStringLiteral("operations")));
    QVERIFY(rootProps.contains(QStringLiteral("agent_critique")));
    QVERIFY(rootProps.contains(QStringLiteral("target_focus_area")));
    QVERIFY(rootProps.contains(QStringLiteral("readiness_score")));
    QVERIFY(rootProps.contains(QStringLiteral("critique_regions")));
    QVERIFY(rootProps.contains(QStringLiteral("regions")));

    const QJsonObject opItem = rootProps.value(QStringLiteral("operations")).toObject().value(QStringLiteral("items")).toObject();
    const QJsonObject opProps = opItem.value(QStringLiteral("properties")).toObject();

    // Verify vital new operation properties are registered in schema
    QVERIFY(opProps.contains(QStringLiteral("fill_profile")));
    QVERIFY(opProps.contains(QStringLiteral("blend_mode")));
    QVERIFY(opProps.contains(QStringLiteral("clip_to_id")));
    QVERIFY(opProps.contains(QStringLiteral("is_shading")));
    QVERIFY(opProps.contains(QStringLiteral("shading_type")));
    QVERIFY(opProps.contains(QStringLiteral("shading_intensity")));

    // Verify AnimeEye properties in schema
    QVERIFY(opProps.contains(QStringLiteral("iris_color")));
    QVERIFY(opProps.contains(QStringLiteral("secondary_color")));
    QVERIFY(opProps.contains(QStringLiteral("expression")));
    QVERIFY(opProps.contains(QStringLiteral("is_right")));
    QVERIFY(opProps.contains(QStringLiteral("size")));
}

void KisAiStrokeProgramTest::testLenientParsingCasingAndAliases()
{
    const QString json = QStringLiteral(
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"face_base\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"fill-profile\": \"watercolor\",\n"
        "      \"blend-mode\": \"normal\",\n"
        "      \"polygon\": [[0.2,0.2],[0.8,0.2],[0.8,0.8],[0.2,0.8]],\n"
        "      \"brush\": {\"profile\": \"brush\", \"color\": \"#ffd9c2\", \"size\": 0.05, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"eye_highlight\",\n"
        "      \"layer\": \"Highlights\",\n"
        "      \"clip-to-id\": \"face_base\",\n"
        "      \"blend-mode\": \"color-dodge\",\n"
        "      \"points\": [[0.4,0.4,0.8],[0.45,0.42,0.5]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#ffffff\", \"size\": 0.003, \"is_eraser\": false}\n"
        "    }\n"
        "  ]\n"
        "}"
    );

    KisAiStrokeProgram prog;
    QString error;
    QVERIFY2(KisAiStrokeProgramCodec::parseResponse(json.toUtf8(), &prog, &error), qPrintable(error));
    QCOMPARE(prog.operations.size(), 2);

    const KisAiStrokeOperation &op1 = prog.operations.at(0);
    QCOMPARE(op1.fillProfile, QStringLiteral("watercolor"));
    QCOMPARE(op1.brush.profile, QStringLiteral("watercolor")); // Auto-mapped for wet edge
    QCOMPARE(op1.fillStyle, QStringLiteral("wash"));

    const KisAiStrokeOperation &op2 = prog.operations.at(1);
    QCOMPARE(op2.clipToId, QStringLiteral("face_base"));
    QCOMPARE(op2.blendMode, QStringLiteral("color_dodge")); // Hyphen converted to underscore
}

void KisAiStrokeProgramTest::testResampleEquidistantClosesTruncatedClosedCurve()
{
    // Regression: resampleEquidistant used to stop at an internal point cap and
    // only re-append the final vertex for OPEN curves, so a dense CLOSED stroke
    // that hit the cap rendered with a long straight chord from where sampling
    // stopped back to the start point.
    QVector<KisAiStrokePoint> points;
    constexpr int kVertices = 4096;
    for (int i = 0; i < kVertices; ++i) {
        // High-frequency radial zigzag: forces many segments per unit length.
        const qreal t = 2.0 * M_PI * i / kVertices;
        const qreal r = 0.45 + 0.02 * ((i % 2) ? 1.0 : -1.0);
        points.append(KisAiStrokePoint(0.5 + r * qCos(t), 0.5 + r * qSin(t), 0.8));
    }

    const QVector<KisAiStrokePoint> res = KisAiStrokeQualityUtils::resampleEquidistant(points, 1.0e-5, true);
    QVERIFY(res.size() >= 2);

    // A sealed ring must return to its origin. Before the fix this gap was >100px
    // on a 4096px canvas; the tolerance below is 0.41px at 4096.
    const QPointF d = res.last().pos - res.first().pos;
    const qreal closingGap = qSqrt(d.x() * d.x() + d.y() * d.y());
    QVERIFY2(closingGap < 1.0e-4, qPrintable(QStringLiteral("closing gap=%1").arg(closingGap)));
}

void KisAiStrokeProgramTest::testResampleEquidistantStaysBounded()
{
    // Regression guard for the memory bound the cap exists for: a caller passing
    // an absurdly small step must not allocate unbounded memory.
    QVector<KisAiStrokePoint> points;
    for (int i = 0; i < 64; ++i) {
        const qreal t = 2.0 * M_PI * i / 64.0;
        points.append(KisAiStrokePoint(0.5 + 0.4 * qCos(t), 0.5 + 0.4 * qSin(t), 0.8));
    }

    const QVector<KisAiStrokePoint> res = KisAiStrokeQualityUtils::resampleEquidistant(points, 1.0e-9, true);
    QVERIFY(res.size() >= 2);
    QVERIFY2(res.size() <= 33002, qPrintable(QStringLiteral("count=%1").arg(res.size())));
}

void KisAiStrokeProgramTest::testSchemaVersionCoercionRejectsHugeStringValue()
{
    // Regression: a string schema_version like "1e300" passed coerceToNumber's
    // finiteness check, then static_cast<int>(1e300) was undefined behaviour and
    // the codec saw an out-of-range version, rejecting the whole program.
    QJsonObject program;
    program[QStringLiteral("schema_version")] = QStringLiteral("1e300");
    QJsonArray ops;
    QJsonObject op;
    op[QStringLiteral("kind")] = QStringLiteral("path");
    op[QStringLiteral("points")] = QJsonArray{QJsonArray{0.2, 0.2}, QJsonArray{0.8, 0.8}};
    ops.append(op);
    program[QStringLiteral("operations")] = ops;

    QJsonObject coerced = program;
    KisAiStrokeTypeChecker::checkAndCoerceProgram(&coerced);
    const QJsonValue sv = coerced.value(QStringLiteral("schema_version"));
    QVERIFY(sv.isDouble());
    QCOMPARE(sv.toInt(), 2);

    // And the program must still survive the real codec path.
    KisAiStrokeProgram parsed;
    QString error;
    const bool ok = KisAiStrokeProgramCodec::parseProgramJson(coerced, &parsed, &error);
    QVERIFY2(ok, qPrintable(error));
}

void KisAiStrokeProgramTest::testCharacterDomainArtDirectionSubstitutions()
{
    // Regression: Character domain art direction directive had an unescaped %2 without %1,
    // causing QString::arg to emit runtime warnings and fail to replace %2 or consume hairColor.
    const QSize canvasSize(1024, 1024);

    // 1. With explicit hair and eye colors
    KisAiPromptAnalyzer::SemanticSpec specExplicit;
    specExplicit.domain = KisAiPromptAnalyzer::DomainType::Character;
    specExplicit.hairColor = QStringLiteral("#c8d0e0");
    specExplicit.eyeColor = QStringLiteral("#3070d0");
    const QString directiveExplicit = KisAiPromptAnalyzer::generateArtDirection(specExplicit, canvasSize);

    QVERIFY(directiveExplicit.contains(QStringLiteral("'#c8d0e0'")));
    QVERIFY(directiveExplicit.contains(QStringLiteral("'#3070d0'")));
    QVERIFY(!directiveExplicit.contains(QStringLiteral("%1")));
    QVERIFY(!directiveExplicit.contains(QStringLiteral("%2")));

    // 2. With empty colors (fallback strings)
    KisAiPromptAnalyzer::SemanticSpec specDefault;
    specDefault.domain = KisAiPromptAnalyzer::DomainType::Character;
    specDefault.hairColor = QString();
    specDefault.eyeColor = QString();
    const QString directiveDefault = KisAiPromptAnalyzer::generateArtDirection(specDefault, canvasSize);

    QVERIFY(directiveDefault.contains(QStringLiteral("prompt-specified hue")));
    QVERIFY(directiveDefault.contains(QStringLiteral("harmonious eye color")));
    QVERIFY(!directiveDefault.contains(QStringLiteral("%1")));
    QVERIFY(!directiveDefault.contains(QStringLiteral("%2")));
}

void KisAiStrokeProgramTest::testNeutralSchemaExampleNoSpecificAnatomy()
{
    const QString schemaSection = KisAiStrokeProgramCodec::buildOutputSchemaExampleSection();
    // Verify that the schema example does NOT contain specific character anatomy anchors that cause copy-paste distortion
    QVERIFY(!schemaSection.contains(QStringLiteral("jaw_contour")));
    QVERIFY(!schemaSection.contains(QStringLiteral("face_skin")));
    QVERIFY(!schemaSection.contains(QStringLiteral("hero_left_eye")));
    QVERIFY(!schemaSection.contains(QStringLiteral("hair_bangs")));
    // Verify that neutral geometric primitives are present instead
    QVERIFY(schemaSection.contains(QStringLiteral("subject_silhouette")));
    QVERIFY(schemaSection.contains(QStringLiteral("subject_shadow")));
    QVERIFY(schemaSection.contains(QStringLiteral("primary_contour")));
}

void KisAiStrokeProgramTest::testAnimeMouthParsingAndValidation()
{
    const QString json = QStringLiteral(R"({
        "schema_version": 2,
        "prompt": "anime smile portrait",
        "operations": [
            {
                "kind": "anime_mouth",
                "id": "hero_mouth",
                "layer": "Lineart",
                "center": [0.50, 0.65],
                "size": [0.08, 0.04],
                "expression": "open_smile",
                "lip_color": "#ff758c",
                "has_highlight": true,
                "brush": { "profile": "gpen", "color": "#1a1224" }
            }
        ]
    })");

    const QJsonObject rootObj = QJsonDocument::fromJson(json.toUtf8()).object();
    KisAiStrokeProgram program;
    QVERIFY(KisAiStrokeProgramCodec::parseProgramJson(rootObj, &program));
    QCOMPARE(program.operations.size(), 1);

    const KisAiStrokeOperation &op = program.operations.first();
    QCOMPARE(op.kind, KisAiStrokeOperation::Kind::AnimeMouth);
    QCOMPARE(op.id, QStringLiteral("hero_mouth"));
    QCOMPARE(op.mouthExpression, QStringLiteral("open_smile"));
    QCOMPARE(op.mouthCenter, QPointF(0.50, 0.65));
    QCOMPARE(op.mouthSize, QSizeF(0.08, 0.04));
    QCOMPARE(op.mouthLipColor, QColor(QStringLiteral("#ff758c")));
    QVERIFY(op.mouthHasHighlight);

    // Verify refinement and validation keeps it valid
    KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program);
    QCOMPARE(refined.operations.size(), 1);
    QCOMPARE(refined.operations.first().kind, KisAiStrokeOperation::Kind::AnimeMouth);
}

void KisAiStrokeProgramTest::testLandscapeRigsAndMultiTierComposition()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("壮大な富士山と満開の桜の木、夕暮れのグラデーション空、舞い散る花びら、伝統的な日本風景");
    spec.subject.type = QStringLiteral("landscape");
    spec.light.timeOfDay = QStringLiteral("sunset");

    const QSize canvasSize(1024, 1024);
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, canvasSize);

    bool hasMountain = false;
    bool hasSnow = false;
    bool hasWater = false;
    bool hasSakuraTrunk = false;
    bool hasSakuraPetals = false;
    bool hasDisruptiveQuad = false;

    for (const KisAiStrokeOperation &op : prog.operations) {
        if (op.id.contains(QStringLiteral("mountain_body"))) {
            hasMountain = true;
        }
        if (op.id.contains(QStringLiteral("mountain_snow"))) {
            hasSnow = true;
        }
        if (op.id.contains(QStringLiteral("water_wash"))) {
            hasWater = true;
        }
        if (op.id.contains(QStringLiteral("sakura_trunk"))) {
            hasSakuraTrunk = true;
        }
        if (op.id.contains(QStringLiteral("sakura_drifting_petals"))) {
            hasSakuraPetals = true;
        }
        if (op.kind == KisAiStrokeOperation::Kind::Fill && op.polygon.size() == 4 && op.id == QStringLiteral("subject_silhouette")) {
            hasDisruptiveQuad = true;
        }
    }

    QVERIFY(hasMountain);
    QVERIFY(hasSnow);
    QVERIFY(hasWater);
    QVERIFY(hasSakuraTrunk);
    QVERIFY(hasSakuraPetals);
    QVERIFY(!hasDisruptiveQuad);

    // Verify Rim Light safety: no wireframe rim strokes on tiny clusters/petals
    const QVector<KisAiStrokeOperation> rims = KisAiStrokeQualityUtils::generateRimLightStrokes(prog.operations, canvasSize);
    for (const KisAiStrokeOperation &rim : rims) {
        QVERIFY(!rim.id.contains(QStringLiteral("cluster")));
        QVERIFY(!rim.id.contains(QStringLiteral("petal")));
    }
}

KISTEST_MAIN(KisAiStrokeProgramTest)




