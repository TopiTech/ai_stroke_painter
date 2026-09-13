/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiIllustrationRendererTest.h"

#include <QImage>
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

#include "aiillustration/KisAiIllustrationRenderer.h"
#include "aiillustration/KisAiPromptAnalyzer.h"

void KisAiIllustrationRendererTest::testValidateImageEndpoint()
{
    QString errorMsg;

    // Valid HTTPS remote endpoint
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("https://api.openai.com/v1/chat/completions"), &errorMsg));
    QVERIFY(errorMsg.isEmpty());

    // Valid localhost HTTP endpoint
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://localhost:11434/v1/chat/completions"), &errorMsg));

    // Valid loopback IPv4 HTTP endpoint
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://127.0.0.1:8000/v1/images/generations"), &errorMsg));

    // Valid loopback IPv6 HTTP endpoint
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://[::1]:8080/v1/images/generations"), &errorMsg));

    // Provider-specific, non-secret query parameters remain supported.
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/v1/chat?api-version=2026-01-01"),
        &errorMsg));

    // Secrets in a URL would be persisted in settings and debug logs.
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/v1/chat?api_key=secret"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/v1/chat?access%5Ftoken=secret"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/v1/chat?accessToken=secret"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/v1/chat#token=secret"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());

    // Insecure non-loopback HTTP endpoint must fail
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://api.openai.com/v1/images/generations"), &errorMsg));
    QVERIFY(!errorMsg.isEmpty());

    // Empty endpoint
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(QString(), &errorMsg));
    QVERIFY(!errorMsg.isEmpty());

    // Non-HTTP/HTTPS protocol
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("ftp://localhost/api"), &errorMsg));
    QVERIFY(!errorMsg.isEmpty());
}

void KisAiIllustrationRendererTest::testDisplayEndpoint()
{
    // Credentials in URL should not leak in display
    const QString displayed = KisAiIllustrationRenderer::displayEndpoint(QStringLiteral("https://user:secret@api.openai.com:443/v1/chat/completions"));
    QVERIFY(!displayed.contains(QStringLiteral("secret")));
    QVERIFY(displayed.contains(QStringLiteral("api.openai.com")));
}

void KisAiIllustrationRendererTest::testNormalizedPrompt()
{
    const QString raw = QStringLiteral("  \t Beautiful   landscape \r\nwith mountains \n ");
    const QString normalized = KisAiIllustrationRenderer::normalizedPrompt(raw);
    QCOMPARE(normalized, QStringLiteral("Beautiful landscape with mountains"));

    QCOMPARE(KisAiIllustrationRenderer::normalizedPrompt(QString()), QString());
}

void KisAiIllustrationRendererTest::testCreateConceptImage()
{
    const QSize targetSize(256, 256);
    const QString prompt = QStringLiteral("Sunset over Tokyo skyline");

    const QImage img1 = KisAiIllustrationRenderer::createConceptImage(prompt, targetSize);
    QCOMPARE(img1.size(), targetSize);
    QVERIFY(!img1.isNull());

    const QImage img2 = KisAiIllustrationRenderer::createConceptImage(prompt, targetSize);
    QCOMPARE(img2.size(), targetSize);

    // Deterministic generation: identical prompts should produce identical images
    QCOMPARE(img1, img2);

    // Different prompts produce different concept art
    const QImage img3 = KisAiIllustrationRenderer::createConceptImage(QStringLiteral("Green forest with river"), targetSize);
    QVERIFY(img1 != img3);
}

void KisAiIllustrationRendererTest::testPromptExpansionPayloadAndParsing()
{
    // Test payload generation
    const QString shortPrompt = QStringLiteral("黒髪ツインテールの少女");
    const QByteArray payload = KisAiPromptAnalyzer::buildPromptExpansionPayload(
        shortPrompt, QStringLiteral("gpt-4o"), KisAiPromptAnalyzer::ArtStyle::AnimeCel);

    QVERIFY(!payload.isEmpty());
    QJsonDocument doc = QJsonDocument::fromJson(payload);
    QVERIFY(!doc.isNull() && doc.isObject());
    const QJsonObject obj = doc.object();
    QCOMPARE(obj.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-4o"));
    const QJsonArray messages = obj.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 2);
    QVERIFY(messages[0].toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("Anime Cel")));
    QCOMPARE(messages[1].toObject().value(QStringLiteral("content")).toString(), shortPrompt);

    // Test parse response with JSON
    const QByteArray mockResponse = R"({
        "choices": [
            {
                "message": {
                    "role": "assistant",
                    "content": "\"黒髪ツインテールの美少女、大きな紫の瞳、爽やかな笑顔、学校の制服、夕暮れの柔らかい光、桜の花びらが舞う背景\""
                }
            }
        ]
    })";

    QString errorMsg;
    const QString parsed = KisAiPromptAnalyzer::parseExpandedPrompt(mockResponse, &errorMsg);
    QVERIFY(errorMsg.isEmpty());
    QVERIFY(!parsed.startsWith(QLatin1Char('"')));
    QVERIFY(!parsed.endsWith(QLatin1Char('"')));
    QVERIFY(parsed.contains(QStringLiteral("ツインテール")));
    QVERIFY(parsed.contains(QStringLiteral("紫の瞳")));
}

KISTEST_MAIN(KisAiIllustrationRendererTest)
