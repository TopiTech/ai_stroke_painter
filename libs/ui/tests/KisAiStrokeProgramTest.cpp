/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeProgramTest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#include "KisAiTestUtils.h"

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

KISTEST_MAIN(KisAiStrokeProgramTest)



