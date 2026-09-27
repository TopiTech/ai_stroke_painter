/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiFullStrokeTest.h"

#include "KisAiTestCrashGuard.h"
#include <QJsonArray>
#include <QJsonObject>

#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) AI_STROKE_TEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiFullStroke.h"
#include "aiillustration/KisAiFullStrokeScene.h"

void KisAiFullStrokeTest::testTextOnlyChatPayload()
{
    const QJsonObject payload = KisAiFullStroke::buildPayload(QStringLiteral("gpt-4o"),
                                                              QStringLiteral("A knight with a lantern"),
                                                              QSize(256, 256),
                                                              500);
    QVERIFY(!payload.contains(QStringLiteral("prompt")));
    QVERIFY(!payload.contains(QStringLiteral("size")));
    const QJsonArray messages = payload.value(QStringLiteral("messages")).toArray();
    QVERIFY(messages.size() >= 2);
    for (const auto &message : messages) {
        QVERIFY(message.toObject().value(QStringLiteral("content")).isString());
        QVERIFY(
            !message.toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("data:image/")));
    }
}

void KisAiFullStrokeTest::testSceneValidationAndBudget()
{
    KisAiFullStrokeScene scene;
    QJsonObject first{{QStringLiteral("id"), QStringLiteral("hero")},
                      {QStringLiteral("type"), QStringLiteral("person")},
                      {QStringLiteral("bounds"), QJsonArray{0.1, 0.1, 0.4, 0.6}},
                      {QStringLiteral("focal_weight"), 0.9}};
    QJsonObject second{{QStringLiteral("id"), QStringLiteral("lamp")},
                       {QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("bounds"), QJsonArray{0.6, 0.2, 0.15, 0.4}},
                       {QStringLiteral("required"), false},
                       {QStringLiteral("focal_weight"), 0.1}};
    QVERIFY(KisAiFullStrokeScene::parse(QJsonObject{{QStringLiteral("objects"), QJsonArray{first, second}}}, &scene));
    KisAiStrokeProgram program;
    KisAiStrokeOperation main;
    main.kind = KisAiStrokeOperation::Kind::Fill;
    main.id = QStringLiteral("hero_fill");
    main.groupId = QStringLiteral("hero");
    program.operations.append(main);
    KisAiStrokeOperation detail = main;
    detail.id = QStringLiteral("lamp_fill");
    detail.groupId = QStringLiteral("lamp");
    program.operations.append(detail);
    QVERIFY(scene.missingRequiredObjects(program).isEmpty());
    const KisAiStrokeProgram trimmed = scene.prioritize(program, 1);
    QCOMPARE(trimmed.operations.size(), 1);
    QCOMPARE(trimmed.operations.first().id, QStringLiteral("hero_fill"));
    first.insert(QStringLiteral("parent_id"), QStringLiteral("lamp"));
    second.insert(QStringLiteral("parent_id"), QStringLiteral("hero"));
    QVERIFY(!KisAiFullStrokeScene::parse(QJsonObject{{QStringLiteral("objects"), QJsonArray{first, second}}}, &scene));
}

void KisAiFullStrokeTest::testImageEndpointRejected()
{
    QVERIFY(KisAiFullStroke::acceptsEndpoint(QStringLiteral("https://api.openai.com/v1/chat/completions")));
    QVERIFY(!KisAiFullStroke::acceptsEndpoint(QStringLiteral("https://api.openai.com/v1/images/generations")));
    QVERIFY(!KisAiFullStroke::acceptsEndpoint(QStringLiteral("http://example.org/v1/chat/completions")));
}

void KisAiFullStrokeTest::testMissingProgramRejected()
{
    KisAiStrokeProgram program;
    QVERIFY(!KisAiFullStroke::acceptsProgram(program));
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.id = QStringLiteral("contour");
    op.points = {KisAiStrokePoint(0.2, 0.2), KisAiStrokePoint(0.7, 0.7)};
    program.operations.append(op);
    QVERIFY(KisAiFullStroke::acceptsProgram(program));
}

void KisAiFullStrokeTest::testPrioritizeBudgetSmallerThanGroup()
{
    KisAiFullStrokeScene scene;
    QJsonObject heroObj{{QStringLiteral("id"), QStringLiteral("hero")},
                        {QStringLiteral("type"), QStringLiteral("person")},
                        {QStringLiteral("bounds"), QJsonArray{0.1, 0.1, 0.5, 0.5}},
                        {QStringLiteral("focal_weight"), 0.9},
                        {QStringLiteral("required"), true}};
    QJsonObject bgObj{{QStringLiteral("id"), QStringLiteral("bg")},
                      {QStringLiteral("type"), QStringLiteral("background")},
                      {QStringLiteral("bounds"), QJsonArray{0.0, 0.0, 1.0, 1.0}},
                      {QStringLiteral("focal_weight"), 0.1},
                      {QStringLiteral("required"), false}};
    QVERIFY(KisAiFullStrokeScene::parse(QJsonObject{{QStringLiteral("objects"), QJsonArray{heroObj, bgObj}}}, &scene));

    KisAiStrokeProgram program;
    // Hero has 3 operations
    for (int i = 0; i < 3; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.id = QStringLiteral("hero_stroke_%1").arg(i);
        op.groupId = QStringLiteral("hero");
        op.points = {KisAiStrokePoint(0.2, 0.2), KisAiStrokePoint(0.3, 0.3)};
        program.operations.append(op);
    }
    // BG has 3 operations
    for (int i = 0; i < 3; ++i) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.id = QStringLiteral("bg_fill_%1").arg(i);
        op.groupId = QStringLiteral("bg");
        op.polygon = QPolygonF{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1)};
        program.operations.append(op);
    }

    // Budget of 2 is smaller than hero (3) and bg (3).
    // Previously, this would produce 0 operations (silent drop).
    // Now it should safely retain 2 operations from the highest priority group (hero).
    const KisAiStrokeProgram prioritized = scene.prioritize(program, 2);
    QCOMPARE(prioritized.operations.size(), 2);
    QCOMPARE(prioritized.operations.at(0).id, QStringLiteral("hero_stroke_0"));
    QCOMPARE(prioritized.operations.at(1).id, QStringLiteral("hero_stroke_1"));
}

KISTEST_MAIN(KisAiFullStrokeTest)
