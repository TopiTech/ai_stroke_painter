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

    // Valid loopback with trailing dot (FQDN)
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://localhost.:11434/v1/chat/completions"), &errorMsg));

    // Valid IPv4-mapped IPv6 loopback
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://[::ffff:127.0.0.1]:8080/v1/images/generations"), &errorMsg));

    // Valid IPv6 loopback with Zone ID / Scope ID
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://[::1%1]:8080/v1/images/generations"), &errorMsg));

    // Valid percent-encoded loopback hostname
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://%6c%6f%63%61%6c%68%6f%73%74:11434/v1"), &errorMsg));

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
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/v1/chat?param=sk-proj-secretKey123"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());

    // Secrets embedded in the URL path must also be rejected: the endpoint is
    // persisted to QSettings in plaintext and sent in the request line.
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/sk-live-XXXX123/v1/chat/completions"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/api/key/SECRET123/v1/chat"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());

    // Normal versioned API paths without secrets stay valid.
    errorMsg.clear();
    QVERIFY(KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://example.invalid/v1/chat/completions"), &errorMsg));
    QVERIFY(errorMsg.isEmpty());

    // Insecure non-loopback HTTP endpoint must fail
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://api.openai.com/v1/images/generations"), &errorMsg));
    QVERIFY(!errorMsg.isEmpty());
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(QStringLiteral("http://192.168.1.100:8000/v1"), &errorMsg));
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

void KisAiIllustrationRendererTest::testCreateConceptImageDeterministicWithLowHuePrompts()
{
    // Regression: hueColor() used `hue % 360`, which stays negative in C++ for
    // negative dividends (e.g. hue - 48 with a low seed hue). QColor::setHsl()
    // is only specified for hue in [0, 359], so such prompts produced
    // unspecified colors and non-deterministic rendering across platforms.
    // Prompts hashing to low seed hues must still render deterministically.
    const QSize targetSize(256, 256);
    QImage first;

    // Sweep many prompts so the sweep is guaranteed to cover seed hues below
    // 48, where (hue - 28) / (hue - 48) previously went negative.
    for (int i = 0; i < 200; ++i) {
        const QString prompt = QStringLiteral("gradient study #%1").arg(i);
        const QImage a = KisAiIllustrationRenderer::createConceptImage(prompt, targetSize);
        QVERIFY(!a.isNull());

        const QImage b = KisAiIllustrationRenderer::createConceptImage(prompt, targetSize);
        QCOMPARE(a, b); // deterministic

        if (i == 0) {
            first = a;
        }
        // Every rendered pixel must be fully opaque; an out-of-range hue
        // produces unspecified color garbage that can fail this check.
        for (int y = 0; y < a.height(); y += 8) {
            for (int x = 0; x < a.width(); x += 8) {
                const QRgb px = a.pixel(x, y);
                QVERIFY(qAlpha(px) == 255);
            }
        }
    }
    QCOMPARE(first.size(), targetSize);
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

void KisAiIllustrationRendererTest::testCityConceptImagePrecedence()
{
    const QSize targetSize(256, 256);
    const QImage cityImg = KisAiIllustrationRenderer::createConceptImage(QStringLiteral("Cyberpunk neon city skyline at dusk"), targetSize);
    QVERIFY(!cityImg.isNull());
    QCOMPARE(cityImg.size(), targetSize);
}

void KisAiIllustrationRendererTest::testIsLoopbackEndpoint()
{
    // Localhost hostnames
    QVERIFY(KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://localhost:11434/v1/chat/completions")));
    QVERIFY(KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://localhost.:11434/v1")));
    QVERIFY(KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://%6c%6f%63%61%6c%68%6f%73%74:11434/v1")));

    // IPv4 loopback addresses
    QVERIFY(KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://127.0.0.1:8000/v1")));
    QVERIFY(KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://127.0.0.2:8000/v1")));

    // IPv6 loopback addresses
    QVERIFY(KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://[::1]:8080/v1")));
    QVERIFY(KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://[::ffff:127.0.0.1]:8080/v1")));

    // Remote endpoints must NOT be loopback
    QVERIFY(!KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("https://api.openai.com/v1/chat/completions")));
    QVERIFY(!KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("https://openrouter.ai/api/v1/chat/completions")));
    QVERIFY(!KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://192.168.1.100:11434/v1")));
    QVERIFY(!KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("http://example.com")));
    QVERIFY(!KisAiIllustrationRenderer::isLoopbackEndpoint(QStringLiteral("")));
}

KISTEST_MAIN(KisAiIllustrationRendererTest)
