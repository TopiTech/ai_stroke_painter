/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ILLUSTRATION_RENDERER_H
#define KIS_AI_ILLUSTRATION_RENDERER_H

#include <QImage>
#include <QSize>
#include <QString>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

/**
 * Lightweight native image generation helpers used by the AI illustration
 * workspace. The local renderer intentionally has no Python or plugin
 * dependency, so it remains available when no remote model is configured.
 */
class KRITAUI_EXPORT KisAiIllustrationRenderer
{
public:
    static QImage createConceptImage(const QString &prompt, const QSize &requestedSize);

    /**
     * Validate a user-configured OpenAI-compatible image endpoint. Remote
     * endpoints must use HTTPS, except for an explicitly local development
     * endpoint.
     */
    static bool validateImageEndpoint(const QString &endpoint, QString *errorMessage = nullptr);

    /** Return a credential-safe label for status messages. */
    static QString displayEndpoint(const QString &endpoint);

    /** Normalize a prompt before it is sent to a model or used in a layer name. */
    static QString normalizedPrompt(const QString &prompt);

    /** Check whether an endpoint URL points to a loopback/local development address. */
    static bool isLoopbackEndpoint(const QString &endpoint);

    /** Format an API key as a Bearer authorization header value, stripping any CRLF characters. */
    static QByteArray formatBearerAuthHeader(const QString &apiKey);

    /** Mask Bearer tokens, API keys, and sensitive URL/JSON query credentials from status text or logs. */
    static QString redactCredentialText(const QString &text);

    /** Encode a reference image to Base64 (composited on white, max dimension 1024px, JPEG/PNG fallback).
     *  Returns empty string and sets errorMessage on failure. */
    static QString encodeReferenceImageBase64(const QImage &image, QString *errorMessage = nullptr);
};

#endif // KIS_AI_ILLUSTRATION_RENDERER_H
