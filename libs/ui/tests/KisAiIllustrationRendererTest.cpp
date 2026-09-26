/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiIllustrationRendererTest.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "KisAiTestCrashGuard.h"
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) AI_STROKE_TEST_MAIN(TestClass)
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

    // Groq and Perplexity token patterns in URL path or query
    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://api.groq.com/openai/v1/gsk_abcdef1234567890abcdef"),
        &errorMsg));
    QVERIFY(!errorMsg.isEmpty());

    errorMsg.clear();
    QVERIFY(!KisAiIllustrationRenderer::validateImageEndpoint(
        QStringLiteral("https://api.perplexity.ai/chat/completions?token=pplx-abcdef1234567890"),
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

    // Port numbers should be preserved in displayEndpoint
    const QString portDisplayed = KisAiIllustrationRenderer::displayEndpoint(QStringLiteral("http://localhost:11434/v1/chat"));
    QVERIFY(portDisplayed.contains(QStringLiteral("localhost:11434")));
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
    QCOMPARE(obj.value(QStringLiteral("temperature")).toDouble(), 0.7);
    QCOMPARE(obj.value(QStringLiteral("max_tokens")).toInt(), 300);
    const QJsonArray messages = obj.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 2);
    QVERIFY(messages[0].toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("Anime Cel")));
    QCOMPARE(messages[1].toObject().value(QStringLiteral("content")).toString(), shortPrompt);

    // Test payload generation for reasoning models (e.g. o3-mini)
    const QByteArray reasoningPayload = KisAiPromptAnalyzer::buildPromptExpansionPayload(
        shortPrompt, QStringLiteral("o3-mini"), KisAiPromptAnalyzer::ArtStyle::AnimeCel);
    QVERIFY(!reasoningPayload.isEmpty());
    QJsonDocument docReasoning = QJsonDocument::fromJson(reasoningPayload);
    QVERIFY(!docReasoning.isNull() && docReasoning.isObject());
    const QJsonObject objReasoning = docReasoning.object();
    QCOMPARE(objReasoning.value(QStringLiteral("model")).toString(), QStringLiteral("o3-mini"));
    QVERIFY(!objReasoning.contains(QStringLiteral("temperature")));
    QVERIFY(!objReasoning.contains(QStringLiteral("max_tokens")));
    QVERIFY(objReasoning.contains(QStringLiteral("max_completion_tokens")));
    QVERIFY(objReasoning.value(QStringLiteral("max_completion_tokens")).toInt() >= 600);

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

    // Test Gemini candidate format parsing
    const QByteArray geminiResponse = R"({
        "candidates": [
            {
                "content": {
                    "parts": [
                        { "text": "サイバーパンク都市の雨夜、ネオンサインの反射" }
                    ]
                }
            }
        ]
    })";
    QString geminiErr;
    const QString geminiParsed = KisAiPromptAnalyzer::parseExpandedPrompt(geminiResponse, &geminiErr);
    QVERIFY(geminiErr.isEmpty());
    QCOMPARE(geminiParsed, QStringLiteral("サイバーパンク都市の雨夜、ネオンサインの反射"));

    // Test Anthropic Claude Messages API format parsing
    const QByteArray claudeResponse = R"({
        "content": [
            {
                "type": "text",
                "text": "青空の下に咲くひまわり畑、水彩調のイラスト"
            }
        ]
    })";
    QString claudeErr;
    const QString claudeParsed = KisAiPromptAnalyzer::parseExpandedPrompt(claudeResponse, &claudeErr);
    QVERIFY(claudeErr.isEmpty());
    QCOMPARE(claudeParsed, QStringLiteral("青空の下に咲くひまわり畑、水彩調のイラスト"));

    // Test reasoning model <think> tag stripping
    const QByteArray reasoningResponse = R"({
        "choices": [
            {
                "message": {
                    "role": "assistant",
                    "content": "<think>\nUser wants an anime portrait.\nLet's add nice lighting details.\n</think>\n銀髪の少女、青い瞳、冬の制服、雪景色の背景"
                }
            }
        ]
    })";
    QString reasoningErr;
    const QString reasoningParsed = KisAiPromptAnalyzer::parseExpandedPrompt(reasoningResponse, &reasoningErr);
    QVERIFY(reasoningErr.isEmpty());
    QVERIFY(!reasoningParsed.contains(QStringLiteral("<think>")));
    QVERIFY(!reasoningParsed.contains(QStringLiteral("User wants")));
    QCOMPARE(reasoningParsed, QStringLiteral("銀髪の少女、青い瞳、冬の制服、雪景色の背景"));

    // Test markdown code block stripping
    const QByteArray markdownResponse = R"({
        "choices": [
            {
                "message": {
                    "role": "assistant",
                    "content": "```markdown\n魔法使いの少年、輝く杖、星空の図書館\n```"
                }
            }
        ]
    })";
    QString mdErr;
    const QString mdParsed = KisAiPromptAnalyzer::parseExpandedPrompt(markdownResponse, &mdErr);
    QVERIFY(mdErr.isEmpty());
    QCOMPARE(mdParsed, QStringLiteral("魔法使いの少年、輝く杖、星空の図書館"));
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

void KisAiIllustrationRendererTest::testFormatBearerAuthHeader()
{
    // Standard API key
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("sk-1234567890")),
             QByteArrayLiteral("Bearer sk-1234567890"));

    // Already has "Bearer " prefix (case-insensitive)
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("Bearer secret-token")),
             QByteArrayLiteral("Bearer secret-token"));
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("bearer secret-token")),
             QByteArrayLiteral("Bearer secret-token"));

    // Strips surrounding whitespace
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("  my-key-abc   ")),
             QByteArrayLiteral("Bearer my-key-abc"));

    // Strips CRLF / newline characters to prevent HTTP header injection
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("clean-key\r\nX-Injected: attack\r\n")),
             QByteArrayLiteral("Bearer clean-keyX-Injected: attack"));
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("key\nwith\nnewlines")),
             QByteArrayLiteral("Bearer keywithnewlines"));

    // Empty or whitespace-only keys return empty QByteArray
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QString()), QByteArray());
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("   ")), QByteArray());
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("Bearer ")), QByteArray());
    QCOMPARE(KisAiIllustrationRenderer::formatBearerAuthHeader(QStringLiteral("bearer \r\n")), QByteArray());
}

void KisAiIllustrationRendererTest::testRedactCredentialText()
{
    // Empty text
    QCOMPARE(KisAiIllustrationRenderer::redactCredentialText(QString()), QString());

    // Bearer token redaction
    const QString bearerInput = QStringLiteral("Authorization: Bearer my-secret-token-12345.abc");
    const QString bearerOutput = KisAiIllustrationRenderer::redactCredentialText(bearerInput);
    QVERIFY(!bearerOutput.contains(QStringLiteral("my-secret-token")));
    QVERIFY(bearerOutput.contains(QStringLiteral("Bearer ***")));

    // Case-insensitive bearer token
    const QString lowerBearer = QStringLiteral("auth: bearer tok-12345-abcde");
    const QString lowerBearerOut = KisAiIllustrationRenderer::redactCredentialText(lowerBearer);
    QVERIFY(!lowerBearerOut.contains(QStringLiteral("tok-12345")));
    QVERIFY(lowerBearerOut.contains(QStringLiteral("bearer ***")));

    // OpenAI / Anthropic sk- key redaction
    const QString skInput = QStringLiteral("Error: invalid key sk-proj-1234567890abcdef at line 1");
    const QString skOutput = KisAiIllustrationRenderer::redactCredentialText(skInput);
    QVERIFY(!skOutput.contains(QStringLiteral("1234567890abcdef")));
    QVERIFY(skOutput.contains(QStringLiteral("sk-***")));

    // Google Gemini API key redaction (AIzaSy...)
    const QString geminiInput = QStringLiteral("Request to https://generativelanguage.googleapis.com failed: key AIzaSyA1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q");
    const QString geminiOutput = KisAiIllustrationRenderer::redactCredentialText(geminiInput);
    QVERIFY(!geminiOutput.contains(QStringLiteral("A1B2C3D4E5F6G7H8I9J0K1L2M3N4O5P6Q")));
    QVERIFY(geminiOutput.contains(QStringLiteral("AIzaSy***")));

    // Sensitive URL query parameters
    const QString urlInput = QStringLiteral("GET https://example.com/api?api_key=super_secret_token&user=alice");
    const QString urlOutput = KisAiIllustrationRenderer::redactCredentialText(urlInput);
    QVERIFY(!urlOutput.contains(QStringLiteral("super_secret_token")));
    QVERIFY(urlOutput.contains(QStringLiteral("api_key=***")));
    QVERIFY(urlOutput.contains(QStringLiteral("user=alice")));

    const QString urlParam2 = QStringLiteral("https://local:8000/v1/generate?key=xyz123&format=json");
    const QString urlOut2 = KisAiIllustrationRenderer::redactCredentialText(urlParam2);
    QVERIFY(!urlOut2.contains(QStringLiteral("xyz123")));
    QVERIFY(urlOut2.contains(QStringLiteral("key=***")));

    const QString urlParam3 = QStringLiteral("https://api.example.com/v1?access_token=secret_val&client_secret=topsecret");
    const QString urlOut3 = KisAiIllustrationRenderer::redactCredentialText(urlParam3);
    QVERIFY(!urlOut3.contains(QStringLiteral("secret_val")));
    QVERIFY(!urlOut3.contains(QStringLiteral("topsecret")));
    QVERIFY(urlOut3.contains(QStringLiteral("access_token=***")));
    QVERIFY(urlOut3.contains(QStringLiteral("client_secret=***")));

    // JSON response body containing credentials
    const QString jsonInput = QStringLiteral("{\"apiKey\": \"secret-api-key-999\", \"status\": \"ok\"}");
    const QString jsonOutput = KisAiIllustrationRenderer::redactCredentialText(jsonInput);
    QVERIFY(!jsonOutput.contains(QStringLiteral("secret-api-key-999")));
    QVERIFY(jsonOutput.contains(QStringLiteral("\"apiKey\": \"***\"")));

    const QString jsonInput2 = QStringLiteral("{\"access_token\": \"token123\", \"clientSecret\": \"sec456\", \"authorization\": \"bearer-abc\"}");
    const QString jsonOutput2 = KisAiIllustrationRenderer::redactCredentialText(jsonInput2);
    QVERIFY(!jsonOutput2.contains(QStringLiteral("token123")));
    QVERIFY(!jsonOutput2.contains(QStringLiteral("sec456")));
    QVERIFY(!jsonOutput2.contains(QStringLiteral("bearer-abc")));

    // HuggingFace token redaction (hf_...)
    const QString hfInput = QStringLiteral("Connecting to HF inference endpoint with token hf_AbCdEfGhIjKlMnOpQrStUvWxYz0123456789");
    const QString hfOutput = KisAiIllustrationRenderer::redactCredentialText(hfInput);
    QVERIFY(!hfOutput.contains(QStringLiteral("AbCdEfGhIjKlMnOpQrStUvWxYz0123456789")));
    QVERIFY(hfOutput.contains(QStringLiteral("hf_***")));

    // Replicate token redaction (r8_...)
    const QString repInput = QStringLiteral("Model deployed on Replicate with auth token r8_1234567890abcdefghijklmnopqrstuvwxyz");
    const QString repOutput = KisAiIllustrationRenderer::redactCredentialText(repInput);
    QVERIFY(!repOutput.contains(QStringLiteral("1234567890abcdefghijklmnopqrstuvwxyz")));
    QVERIFY(repOutput.contains(QStringLiteral("r8_***")));

    // GitHub PAT redaction (ghp_... and github_pat_...)
    const QString ghInput = QStringLiteral("Git clone failed: ghp_1234567890abcdefghijklmnopqrstuvwxyz and github_pat_11AAAAAAA000000000_BBBBBBBBBBBBBBBB");
    const QString ghOutput = KisAiIllustrationRenderer::redactCredentialText(ghInput);
    QVERIFY(!ghOutput.contains(QStringLiteral("1234567890abcdefghijklmnopqrstuvwxyz")));
    QVERIFY(!ghOutput.contains(QStringLiteral("11AAAAAAA000000000_BBBBBBBBBBBBBBBB")));
    QVERIFY(ghOutput.contains(QStringLiteral("gh_***")));

    // Groq API key redaction (gsk_...)
    const QString groqInput = QStringLiteral("Groq connection failed with key gsk_1234567890abcdef1234567890abcdef");
    const QString groqOutput = KisAiIllustrationRenderer::redactCredentialText(groqInput);
    QVERIFY(!groqOutput.contains(QStringLiteral("1234567890abcdef")));
    QVERIFY(groqOutput.contains(QStringLiteral("gsk_***")));

    // Perplexity API key redaction (pplx-...)
    const QString pplxInput = QStringLiteral("Perplexity auth error: pplx-1234567890abcdef1234567890abcdef");
    const QString pplxOutput = KisAiIllustrationRenderer::redactCredentialText(pplxInput);
    QVERIFY(!pplxOutput.contains(QStringLiteral("1234567890abcdef")));
    QVERIFY(pplxOutput.contains(QStringLiteral("pplx-***")));

    // Custom header redaction (x-api-key: ...)
    const QString headerInput = QStringLiteral("HTTP headers: x-api-key: secret_custom_key_xyz123\nHost: api.example.com");
    const QString headerOutput = KisAiIllustrationRenderer::redactCredentialText(headerInput);
    QVERIFY(!headerOutput.contains(QStringLiteral("secret_custom_key_xyz123")));
    QVERIFY(headerOutput.contains(QStringLiteral("x-api-key: ***")));
}

KISTEST_MAIN(KisAiIllustrationRendererTest)
