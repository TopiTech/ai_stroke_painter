/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_PROGRAM_H
#define KIS_AI_STROKE_PROGRAM_H

#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include "kritaui_export.h"

struct KRITAUI_EXPORT KisAiStrokePoint
{
    QPointF pos;       // Normalized coordinates [0.0, 1.0]
    qreal pressure {0.8}; // Pressure [0.0, 1.0]
    qint64 timeMs {0};

    KisAiStrokePoint() = default;
    KisAiStrokePoint(qreal x, qreal y, qreal p = 0.8, qint64 t = 0)
        : pos(x, y), pressure(p), timeMs(t) {}
};

struct KRITAUI_EXPORT KisAiStrokeBrush
{
    QString profile {QStringLiteral("auto")}; // auto, gpen, brush, watercolor, airbrush, eraser
    QColor color {QColor(35, 35, 35)};
    qreal size {0.008};                       // ratio [0.0, 1.0] or px
    QString sizeMode {QStringLiteral("ratio")}; // ratio or px
    qreal opacity {1.0};
    bool isEraser {false};
    QString presetHint;
};

struct KRITAUI_EXPORT KisAiStrokeOperation
{
    enum class Kind {
        Path,
        Fill,
        GradientFill,
        Ribbon,
        Particles,
        Unknown
    };

    Kind kind {Kind::Unknown};
    QString id;
    QString layer {QStringLiteral("Lineart")};
    KisAiStrokeBrush brush;

    // Path
    QVector<KisAiStrokePoint> points;
    bool closed {false};
    bool smooth {true};
    QString role {QStringLiteral("auto")};

    // Fill & GradientFill
    QPolygonF polygon;
    QString fillStyle {QStringLiteral("wash")}; // wash, contour, scanline, radial, directional
    QVector<QColor> gradientColors;
    qreal angleDeg {0.0};
    qreal spacing {0.5};

    // Ribbon
    QVector<QPointF> spine;
    qreal widthStart {0.02};
    qreal widthMid {0.015};
    qreal widthEnd {0.005};

    // Particles
    QRectF bounds;
    QString particleShape {QStringLiteral("petal")}; // petal, sparkle, star, bokeh, dot
    int particleCount {16};
};

struct KRITAUI_EXPORT KisAiStrokeProgram
{
    int schemaVersion {2};
    QString prompt;
    int seed {42};
    QString title;
    int iteration {1};
    bool goalReached {true};
    qreal completionScore {1.0};
    QSize canvasSize {1024, 1024};
    QVector<KisAiStrokeOperation> operations;

    bool isValid() const { return !operations.isEmpty(); }
};

/**
 * High-level parser, serializer, and prompt builder for text-based LLMs
 * generating coordinate-directed strokes (StrokeProgram v2).
 */
class KRITAUI_EXPORT KisAiStrokeProgramCodec
{
public:
    /**
     * Build the OpenAI Chat Completions request payload (messages, json_schema / response_format).
     */
    static QJsonObject buildChatCompletionsPayload(
        const QString &model,
        const QString &prompt,
        const QSize &canvasSize,
        int strokeBudget = 500,
        const QString &reasoningEffort = QString()
    );

    /**
     * Generate the comprehensive artistic digital painting system prompt with
     * layer hierarchy, 4-tier lighting, spatial anchors, and schema instructions.
     */
    static QString buildSystemPrompt(const QSize &canvasSize, const QString &prompt);

    /**
     * JSON schema for OpenAI Structured Outputs (response_format: json_schema).
     */
    static QJsonObject strokeProgramJsonSchema();

    /**
     * Parse raw response body from Chat Completions API into a KisAiStrokeProgram.
     * Handles markdown codeblocks, thinking tokens, and minor repairs.
     */
    static bool parseResponse(
        const QByteArray &responseBytes,
        KisAiStrokeProgram *outProgram,
        QString *errorMessage = nullptr
    );

    /**
     * Parse a JSON object or string into a KisAiStrokeProgram.
     */
    static bool parseProgramJson(
        const QJsonObject &rootObj,
        KisAiStrokeProgram *outProgram,
        QString *errorMessage = nullptr
    );

    /**
     * Extract JSON substring from raw model output (handles ```json ... ``` and <think>...</think>).
     */
    static QString sanitizeAndExtractJson(const QString &rawText);

    /**
     * Offline deterministic procedural stroke generator for testing coordinate rendering
     * without remote API calls.
     */
    static KisAiStrokeProgram createDeterministicProgram(const QString &prompt, const QSize &canvasSize);

    /**
     * Safely parse CSS-like hex color with alpha (#RGB, #RRGGBB, #RRGGBBAA).
     */
    static QColor parseColor(const QString &colorStr, const QColor &fallback = QColor(35, 35, 35));
};

#endif // KIS_AI_STROKE_PROGRAM_H
