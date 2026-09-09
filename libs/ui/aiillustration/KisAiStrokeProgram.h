/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_PROGRAM_H
#define KIS_AI_STROKE_PROGRAM_H

#include <QByteArray>
#include <QColor>
#include <QJsonObject>
#include <QMap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

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
    QString profile {QStringLiteral("auto")}; // auto, gpen, brush, watercolor, airbrush, eraser, marker, crayon, neon, splatter
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
        Hatch,
        MangaLines,
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

    // Fill & GradientFill & Hatch
    QPolygonF polygon;
    QString fillStyle {QStringLiteral("wash")}; // wash, contour, scanline, radial, directional
    QVector<QColor> gradientColors;
    qreal angleDeg {0.0};
    qreal spacing {0.5};

    // GradientFill & MangaLines detailed properties
    bool isRadial {false};
    QPointF gradientCenter {0.5, 0.5};
    qreal gradientRadius {0.5};

    // Hatch
    bool crossHatch {false};

    // Ribbon
    QVector<QPointF> spine;
    qreal widthStart {0.02};
    qreal widthMid {0.015};
    qreal widthEnd {0.005};

    // Particles
    QRectF bounds;
    QString particleShape {QStringLiteral("petal")}; // petal, sparkle, star, bokeh, dot
    int particleCount {16};

    // MangaLines (Speed / Focus Lines)
    qreal innerRadius {0.15};
    qreal outerRadius {0.70};
    int density {48};
    qreal lineLengthJitter {0.20};
};

struct KRITAUI_EXPORT KisAiStrokeProgram
{
    int schemaVersion {2};
    QString prompt;
    int seed {42};
    QString title;
    int iteration {1};
    int currentStep {1};
    int totalSteps {1};
    QString stepPhase {QStringLiteral("complete")}; // blocking, shading, lineart, finishing, complete
    QString visualCritique;
    bool goalReached {true};
    qreal completionScore {1.0};
    QSize canvasSize {1024, 1024};
    QVector<KisAiStrokeOperation> operations;

    bool isValid() const { return !operations.isEmpty(); }
};

struct KRITAUI_EXPORT KisAiStrokeQualityReport
{
    int inputOperations {0};
    int outputOperations {0};
    int droppedOperations {0};
    int repairedValues {0};
    int deduplicatedPoints {0};
    qreal score {0.0};
    QStringList warnings;
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
        const QString &reasoningEffort = QString(),
        const QString &customInstructions = QString(),
        bool enableStreaming = true,
        bool enforceJsonFormat = false
    );

    /**
     * Generate the comprehensive artistic digital painting system prompt with
     * layer hierarchy, 4-tier lighting, spatial anchors, and schema instructions.
     */
    static QString buildSystemPrompt(
        const QSize &canvasSize,
        const QString &prompt,
        const QString &customInstructions = QString()
    );

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
     * Parse a JSON object into a KisAiStrokeProgram.
     * Coordinate units are decoded here; call refineForRendering() before
     * rasterization when using this low-level parser directly.
     */
    static bool parseProgramJson(
        const QJsonObject &rootObj,
        KisAiStrokeProgram *outProgram,
        QString *errorMessage = nullptr
    );

    /**
     * Canonicalize unreliable model geometry before rasterization. This pass
     * clamps non-finite/out-of-range values, removes duplicate points and
     * degenerate operations, normalizes brush dynamics, and computes a
     * structural quality score without changing the intended composition.
     */
    static KisAiStrokeProgram refineForRendering(
        const KisAiStrokeProgram &program,
        KisAiStrokeQualityReport *report = nullptr
    );

    /** Return a deterministic structural quality score in [0, 1]. */
    static qreal qualityScore(const KisAiStrokeProgram &program);

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
     * Safely parse CSS-like hex color with alpha (#RGB, #RRGGBB, #RRGGBBAA) or CSS rgb/rgba/named.
     */
    static QColor parseColor(const QString &colorStr, const QColor &fallback = QColor(35, 35, 35));

    /**
     * Compute a stable 32-bit hash across sessions and platforms (FNV-1a).
     */
    static quint32 stableSeed(const QString &text);

    /**
     * Attempt to repair common JSON syntax errors (comments, trailing commas, single quotes,
     * unquoted keys, dirty numbers, dirty booleans, stray tokens).
     */
    static QString repairJsonSyntax(const QString &text);

    /**
     * Attempt to repair truncated JSON containing an operations or strokes array.
     */
    static QString repairTruncatedJson(const QString &jsonText);

    /**
     * Extract individual stroke operations from arbitrary or severely mangled text when
     * document-level JSON parsing fails completely.
     */
    static bool extractOperationsFromRawText(
        const QString &rawText,
        KisAiStrokeProgram *outProgram,
        QString *errorMessage = nullptr
    );

    /**
     * Check if an API endpoint is known to support structured JSON mode (response_format: {"type": "json_object"}).
     */
    static bool supportsJsonFormat(const QString &endpoint);



    /**
     * Normalize layer name into one of the standard layers: Flats, Shading, Lineart, Highlights, FX.
     * Returns trimmed original name if no standard alias matched, or "Lineart" if empty.
     */
    static QString normalizeLayerName(const QString &name);

    /**
     * Count operations per normalized layer for a given program.
     */
    static QMap<QString, int> countLayerOperations(const KisAiStrokeProgram &program);

    /**
     * Format a summary of operations per standard layer (e.g. "Flats: 2, Shading: 3, Lineart: 4, Highlights: 1, FX: 0").
     */
    static QString formatLayerSummary(const KisAiStrokeProgram &program);

    /**
     * Build the Goal Mode Chat Completions request payload with vision feedback (image base64 data URL).
     */
    static QJsonObject buildGoalStepPayload(
        const QString &model,
        const QString &prompt,
        const QSize &canvasSize,
        int step,
        int totalSteps = 4,
        const QString &imageBase64 = QString(),
        const QString &additionalInstruction = QString(),
        int strokeBudget = 400,
        const QString &reasoningEffort = QString(),
        bool includeVision = true,
        bool enableStreaming = true,
        bool enforceJsonFormat = false
    );

    /**
     * Parse SSE (Server-Sent Events) chunks into accumulated text content.
     * Updates unprocessedBuffer with any trailing line fragment and appends extracted
     * delta content to accumulatedContent. Sets isDone to true when [DONE] is encountered.
     */
    static bool parseSseStreamChunk(
        const QByteArray &chunk,
        QByteArray *unprocessedBuffer,
        QString *accumulatedContent,
        bool *isDone = nullptr
    );

    /**
     * Check if a model is treated as vision-capable. The hardcoded whitelist is deprecated;
     * modern LLMs are assumed vision-capable by default with automatic text-only fallback.
     */
    static bool isVisionModel(const QString &model);

    /**
     * Check if a model belongs to a reasoning/thinking family (e.g. o1, o3, deepseek-r1, qwq)
     * requiring max_completion_tokens rather than max_tokens.
     */
    static bool isReasoningModel(const QString &model);

    /**
     * Check the media type used by a Chat Completions response. Streaming
     * responses are Server-Sent Events; buffered responses are JSON. Missing
     * headers are retained for compatibility with otherwise valid providers.
     */
    static bool isAcceptedResponseContentType(const QByteArray &contentType, bool streaming);

    /**
     * Offline deterministic procedural stroke generator for a specific Goal Mode step.
     * Step 1: Background & Flats, Step 2: Shading, Step 3: Lineart, Step 4: Highlights & FX.
     */
    static KisAiStrokeProgram createDeterministicProgramStep(
        const QString &prompt,
        const QSize &canvasSize,
        int step,
        int totalSteps = 4
    );

    /**
     * Merge operations from an extension program into a base program.
     */
    static KisAiStrokeProgram mergePrograms(
        const KisAiStrokeProgram &base,
        const KisAiStrokeProgram &extension
    );
};

#endif // KIS_AI_STROKE_PROGRAM_H
