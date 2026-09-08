/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ILLUSTRATION_RENDERER_H
#define KIS_AI_ILLUSTRATION_RENDERER_H

#include <QImage>
#include <QSize>
#include <QString>

#include "kritaui_export.h"

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
};

#endif // KIS_AI_ILLUSTRATION_RENDERER_H
