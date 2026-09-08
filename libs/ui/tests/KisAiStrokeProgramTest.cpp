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
}

void KisAiStrokeProgramTest::testStrokeProgramJsonSchema()
{
    const QJsonObject schema = KisAiStrokeProgramCodec::strokeProgramJsonSchema();
    QCOMPARE(schema.value(QStringLiteral("type")).toString(), QStringLiteral("object"));

    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    QVERIFY(props.contains(QStringLiteral("schema_version")));
    QVERIFY(props.contains(QStringLiteral("operations")));
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

KISTEST_MAIN(KisAiStrokeProgramTest)

