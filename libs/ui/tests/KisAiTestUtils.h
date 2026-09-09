/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_TEST_UTILS_H
#define KIS_AI_TEST_UTILS_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "aiillustration/KisAiStrokeProgram.h"

/**
 * Comprehensive testing utilities and fixtures for AI Stroke Painter.
 * Provides mock HTTP response generators, corruption/fuzz helpers,
 * and high-level program assertions.
 */
class KisAiTestUtils
{
public:
    enum class SampleProgramType {
        Minimal,
        CharacterPortrait,
        LandscapeWashes,
        MixedMultiLayer,
        BrokenTypesToCoerce
    };

    enum class CorruptionType {
        TruncatedMidway,
        MissingQuotesOnKeys,
        StrayComments,
        FullWidthCharacters,
        UnescapedControlCharacters,
        NonStandardNumbers,
        MissingCommasBetweenObjects,
        TrailingCommas
    };

    /**
     * Create standard OpenAI Chat Completions JSON payload response.
     */
    static QByteArray createMockChatResponse(
        const QString &assistantContent,
        const QString &model = QStringLiteral("gpt-4o"),
        const QString &finishReason = QStringLiteral("stop")
    );

    /**
     * Create mock Server-Sent Events (SSE) data stream from token chunks.
     */
    static QVector<QByteArray> createMockSseChunks(
        const QStringList &tokens,
        const QString &model = QStringLiteral("gpt-4o")
    );

    /**
     * Generate sample StrokeProgram JSON text for testing.
     */
    static QString createSampleProgramJson(SampleProgramType type);

    /**
     * Injects synthetic syntax or format corruptions into valid JSON text for resilience testing.
     */
    static QString corruptJson(const QString &validJson, CorruptionType type);

    /**
     * Verify that a program meets minimum structural requirements.
     */
    static bool verifyProgramStructure(
        const KisAiStrokeProgram &program,
        int minOperations = 1,
        qreal minCompletionScore = 0.5,
        QString *outFailureReason = nullptr
    );
};

#endif // KIS_AI_TEST_UTILS_H
