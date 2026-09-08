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

KISTEST_MAIN(KisAiStrokeProgramTest)
