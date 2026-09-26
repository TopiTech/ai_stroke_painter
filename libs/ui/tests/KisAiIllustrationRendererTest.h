/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ILLUSTRATION_RENDERER_TEST_H
#define KIS_AI_ILLUSTRATION_RENDERER_TEST_H

#include <QObject>

class KisAiIllustrationRendererTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testValidateImageEndpoint();
    void testDisplayEndpoint();
    void testNormalizedPrompt();
    void testCreateConceptImage();
    void testCreateConceptImageDeterministicWithLowHuePrompts();
    void testPromptExpansionPayloadAndParsing();
    void testCityConceptImagePrecedence();
    void testIsLoopbackEndpoint();
    void testFormatBearerAuthHeader();
    void testRedactCredentialText();
    void testEncodeReferenceImageBase64();
};

#endif // KIS_AI_ILLUSTRATION_RENDERER_TEST_H
