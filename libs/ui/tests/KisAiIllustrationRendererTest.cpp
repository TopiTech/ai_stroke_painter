/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiIllustrationRendererTest.h"

#include <QImage>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiIllustrationRenderer.h"

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

KISTEST_MAIN(KisAiIllustrationRendererTest)
