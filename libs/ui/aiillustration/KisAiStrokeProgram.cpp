/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeProgram.h"
#include "KisAiPromptAnalyzer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace
{
qreal clamp01(qreal v)
{
    if (!std::isfinite(v)) {
        return 0.0;
    }
    return qMax<qreal>(0.0, qMin<qreal>(1.0, v));
}

qreal polygonArea(const QPolygonF &polygon)
{
    qreal twiceArea = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon.at(i);
        const QPointF &b = polygon.at((i + 1) % polygon.size());
        twiceArea += a.x() * b.y() - b.x() * a.y();
    }
    return qAbs(twiceArea) * 0.5;
}

bool finitePoint(const QPointF &point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

QPointF clampedPoint(const QPointF &point, int *repairCount)
{
    if (!finitePoint(point)) {
        if (repairCount)
            ++(*repairCount);
        return QPointF(0.5, 0.5);
    }

    const QPointF result(clamp01(point.x()), clamp01(point.y()));
    if (result != point && repairCount)
        ++(*repairCount);
    return result;
}

template<typename PointAccessor>
void removeAdjacentDuplicates(int count, PointAccessor pointAt, QVector<int> *keptIndices, int *removedCount)
{
    keptIndices->clear();
    keptIndices->reserve(count);
    constexpr qreal epsilonSquared = 1.0e-8;
    for (int i = 0; i < count; ++i) {
        if (keptIndices->isEmpty()) {
            keptIndices->append(i);
            continue;
        }
        const QPointF delta = pointAt(i) - pointAt(keptIndices->last());
        if (delta.x() * delta.x() + delta.y() * delta.y() <= epsilonSquared) {
            if (removedCount)
                ++(*removedCount);
        } else {
            keptIndices->append(i);
        }
    }
}

bool isReasoningModel(const QString &model)
{
    const QString lower = model.toLower().trimmed();
    return lower.contains(QLatin1String("o1")) || lower.contains(QLatin1String("o3"))
        || lower.contains(QLatin1String("deepseek-r1")) || lower.contains(QLatin1String("deepseek-reasoner"))
        || lower.contains(QLatin1String("thinking")) || lower.contains(QLatin1String("reasoner"))
        || lower.contains(QLatin1String("qwq"));
}
} // namespace

quint32 KisAiStrokeProgramCodec::stableSeed(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    quint32 h = 2166136261u;
    for (char b : bytes) {
        h = (h ^ static_cast<quint8>(b)) * 16777619u;
    }
    return h;
}

QColor KisAiStrokeProgramCodec::parseColor(const QString &colorStr, const QColor &fallback)
{
    QString s = colorStr.trimmed();
    if (s.isEmpty()) {
        return fallback;
    }

    if (s.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
        static const QRegularExpression rgbRegex(
            QStringLiteral(R"(rgba?\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*([\d.]+))?\s*\))"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = rgbRegex.match(s);
        if (match.hasMatch()) {
            const int r = qBound(0, match.captured(1).toInt(), 255);
            const int g = qBound(0, match.captured(2).toInt(), 255);
            const int b = qBound(0, match.captured(3).toInt(), 255);
            int a = 255;
            if (match.lastCapturedIndex() >= 4 && !match.captured(4).isEmpty()) {
                const qreal alphaVal = match.captured(4).toDouble();
                a = qBound(0, qRound(alphaVal * 255.0), 255);
            }
            return QColor(r, g, b, a);
        }
    }

    if (s.startsWith(QLatin1Char('#'))) {
        s = s.mid(1);
    }

    if (s.length() == 3) {
        // RGB -> RRGGBB
        bool okR = false, okG = false, okB = false;
        const int r = s.mid(0, 1).repeated(2).toInt(&okR, 16);
        const int g = s.mid(1, 1).repeated(2).toInt(&okG, 16);
        const int b = s.mid(2, 1).repeated(2).toInt(&okB, 16);
        if (okR && okG && okB)
            return QColor(r, g, b);
    } else if (s.length() == 4) {
        // RGBA -> RRGGBBAA
        bool okR = false, okG = false, okB = false, okA = false;
        const int r = s.mid(0, 1).repeated(2).toInt(&okR, 16);
        const int g = s.mid(1, 1).repeated(2).toInt(&okG, 16);
        const int b = s.mid(2, 1).repeated(2).toInt(&okB, 16);
        const int a = s.mid(3, 1).repeated(2).toInt(&okA, 16);
        if (okR && okG && okB && okA)
            return QColor(r, g, b, a);
    } else if (s.length() == 6) {
        bool ok = false;
        const int val = s.toInt(&ok, 16);
        if (ok)
            return QColor((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
    } else if (s.length() == 8) {
        bool ok = false;
        const uint val = s.toUInt(&ok, 16);
        if (ok)
            return QColor((val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
    }

    const QColor fromStr = QColor::fromString(colorStr.trimmed());
    if (fromStr.isValid()) {
        return fromStr;
    }

    const QColor named(colorStr);
    if (named.isValid()) {
        return named;
    }

    return fallback;
}

QJsonObject KisAiStrokeProgramCodec::strokeProgramJsonSchema()
{
    QJsonObject pointItem;
    pointItem[QStringLiteral("type")] = QStringLiteral("number");
    pointItem[QStringLiteral("minimum")] = 0.0;
    pointItem[QStringLiteral("maximum")] = 1.0;

    QJsonObject pointSchema;
    pointSchema[QStringLiteral("type")] = QStringLiteral("array");
    pointSchema[QStringLiteral("items")] = pointItem;
    pointSchema[QStringLiteral("minItems")] = 2;
    pointSchema[QStringLiteral("maxItems")] = 3;

    QJsonObject brushSchema;
    brushSchema[QStringLiteral("type")] = QStringLiteral("object");
    QJsonObject brushProps;
    brushProps[QStringLiteral("profile")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    brushProps[QStringLiteral("color")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    brushProps[QStringLiteral("size")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("exclusiveMinimum"), 0.0}};
    brushProps[QStringLiteral("size_mode")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                    {QStringLiteral("enum"), QJsonArray{QStringLiteral("ratio"), QStringLiteral("px")}}};
    brushProps[QStringLiteral("opacity")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                                                        {QStringLiteral("minimum"), 0.0},
                                                        {QStringLiteral("maximum"), 1.0}};
    brushProps[QStringLiteral("is_eraser")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    brushSchema[QStringLiteral("properties")] = brushProps;
    brushSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("profile"),
                                                         QStringLiteral("color"),
                                                         QStringLiteral("size"),
                                                         QStringLiteral("is_eraser")};

    QJsonObject opItem;
    opItem[QStringLiteral("type")] = QStringLiteral("object");
    QJsonObject opProps;
    opProps[QStringLiteral("kind")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                  {QStringLiteral("enum"),
                                                   QJsonArray{QStringLiteral("path"),
                                                              QStringLiteral("fill"),
                                                              QStringLiteral("gradient_fill"),
                                                              QStringLiteral("ribbon"),
                                                              QStringLiteral("particles"),
                                                              QStringLiteral("hatch")}}};
    opProps[QStringLiteral("id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("layer")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("brush")] = brushSchema;
    opProps[QStringLiteral("points")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), pointSchema}};
    opProps[QStringLiteral("polygon")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), pointSchema}};
    opProps[QStringLiteral("spine")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), pointSchema}};
    opProps[QStringLiteral("colors")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                    {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}};
    opProps[QStringLiteral("style")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("closed")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    opProps[QStringLiteral("smooth")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    opProps[QStringLiteral("role")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("angle_deg")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}};
    opProps[QStringLiteral("spacing")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}};
    opProps[QStringLiteral("cross_hatch")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    opProps[QStringLiteral("is_radial")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    opProps[QStringLiteral("center")] = pointSchema;
    opProps[QStringLiteral("radius")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("exclusiveMinimum"), 0.0}};
    opProps[QStringLiteral("width_start")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("exclusiveMinimum"), 0.0}};
    opProps[QStringLiteral("width_mid")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("exclusiveMinimum"), 0.0}};
    opProps[QStringLiteral("width_end")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("exclusiveMinimum"), 0.0}};
    opProps[QStringLiteral("bounds")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                    {QStringLiteral("items"), pointItem},
                                                    {QStringLiteral("minItems"), 4},
                                                    {QStringLiteral("maxItems"), 4}};
    opProps[QStringLiteral("count")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
    opProps[QStringLiteral("shape")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opItem[QStringLiteral("properties")] = opProps;
    opItem[QStringLiteral("required")] =
        QJsonArray{QStringLiteral("kind"), QStringLiteral("id"), QStringLiteral("layer"), QStringLiteral("brush")};

    QJsonObject rootProps;
    rootProps[QStringLiteral("schema_version")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
    rootProps[QStringLiteral("prompt")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("title")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("operations")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                          {QStringLiteral("items"), opItem},
                                                          {QStringLiteral("minItems"), 1},
                                                          {QStringLiteral("maxItems"), 2000}};

    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    schema[QStringLiteral("properties")] = rootProps;
    schema[QStringLiteral("required")] = QJsonArray{QStringLiteral("schema_version"), QStringLiteral("operations")};

    return schema;
}

QString KisAiStrokeProgramCodec::buildSystemPrompt(const QSize &canvasSize,
                                                   const QString &prompt,
                                                   const QString &customInstructions)
{
    const qreal aspect = canvasSize.height() > 0 ? qreal(canvasSize.width()) / canvasSize.height() : 1.0;
    const auto spec = KisAiPromptAnalyzer::analyze(prompt, canvasSize);
    const QString artDirection = KisAiPromptAnalyzer::generateArtDirection(spec, canvasSize);

    QString systemText =
        QStringLiteral(
            "You are an autonomous AI master digital painter directing layer-by-layer drawing plans for Krita.\n"
            "Generate a rich, cohesive, painterly illustration by specifying coordinate-directed strokes in "
            "StrokeProgram JSON format.\n"
            "Output ONLY valid JSON. Do NOT include markdown explanations, thought text, or conversational chatter.\n\n"
            "=== COORDINATE SYSTEM & RESOLUTION ===\n"
            "Coordinates are normalized float numbers strictly in [0.0, 1.0]. (0.0, 0.0) is top-left, (1.0, 1.0) is "
            "bottom-right.\n"
            "Canvas size: %1x%2 (Aspect %3:1).\n\n"
            "=== LAYER ARCHITECTURE & COMPOSITION (Back-to-Front) ===\n"
            "1. 'Flats': Backdrop gradients, sky wash, terrain base, major silhouette color blocking (hair base, skin "
            "base, clothing base).\n"
            "2. 'Shading': Form shadows, ambient occlusion, depth crevices, cast shadows (rendered with Multiply blend "
            "and automatically clipped to Flats).\n"
            "3. 'Lineart': Crisp contours, facial details, hair strands, structural outlines (rendered with natural "
            "Catmull-Rom spline curves and tapering).\n"
            "4. 'Highlights': Specular glints, eye catchlights, rim lighting, atmospheric glow (rendered with Screen "
            "blend and clipped to Flats).\n"
            "5. 'FX': Particle accents, petals, embers, stars, sparkles, bloom.\n\n"
            "=== MASTER DRAWING WORKFLOW (MANDATORY) ===\n"
            "Silently design the complete image before emitting JSON: establish a focal point, horizon/gesture, "
            "foreground-midground-background depth, and a limited 5-8 color palette.\n"
            "Then emit painter-order geometry from large to small: (A) full-canvas underpainting, (B) large "
            "overlapping silhouettes, (C) form and cast shadows, (D) continuous structural contours, (E) focal "
            "micro-details, (F) restrained highlights/FX.\n"
            "Reuse the same landmark coordinates across Flats, Shading, and Lineart so boundaries register. Prefer one "
            "coherent 4-8 point path over many disconnected 2-point fragments.\n"
            "Use asymmetry, overlap, varied scale, and negative space. Do not tile generic symbols, trace every fill "
            "edge, or distribute detail uniformly. Concentrate the smallest marks and highest contrast at the focal "
            "point.\n"
            "Before returning JSON, silently audit: canvas coverage, recognizable silhouette, layer registration, "
            "depth ordering, tangent continuity, palette harmony, and required geometry for every operation. Fix "
            "failures in the final JSON.\n\n"
            "=== ARTISTIC & ANATOMICAL GUIDELINES ===\n"
            "- Contours & Splines: The engine interpolates path points with centripetal Catmull-Rom splines, monotone "
            "pressure changes, and natural tapering.\n"
            "  Provide 4-8 smooth anchor points per curved feature (silhouette curves, hair flow, eyes, fabric folds) "
            "for expressive lines.\n"
            "- Color Harmony: Avoid harsh pure black (#000000) for lineart; use rich dark tones (e.g. #1a162b, "
            "#1c2438, #2b1b17).\n"
            "  Employ warm key lights paired with cool shadows, or cool ambient light paired with warm saturated "
            "bounced light.\n"
            "- Clean Silhouettes: Ensure Flats cover the full subject silhouette so that Shading and Highlights stay "
            "cleanly bounded.\n\n"
            "=== OPERATION KINDS ===\n"
            "- 'gradient_fill': Full/partial sky & background washes. Polygon [ [x, y], ... ], colors [ '#hex', ... ], "
            "angle_deg (0=horizontal, 90=vertical), is_radial (true/false), center [cx, cy], radius.\n"
            "- 'fill': Color masses, silhouettes, hair/clothing base, shadow blocks. Polygon [ [x, y], ... ], brush { "
            "'profile': 'watercolor'/'brush', 'color': '#hex' }, style ('wash'/'contour'/'directional').\n"
            "- 'hatch': Parallel shading or cross-hatching. Polygon [ [x, y], ... ], angle_deg (0-180), spacing "
            "(0.005-0.03), cross_hatch (true/false), brush { 'profile': 'pencil'/'gpen', 'color': '#hex' }.\n"
            "- 'ribbon': Tapered organic strokes (tree trunks, hair clumps, cloth folds). Spine [ [x, y], ... ], "
            "width_start, width_mid, width_end (0.005-0.05).\n"
            "- 'path': Expressive linework, contours, facial features. Points [ [x, y, pressure], ... ] where pressure "
            "is 0.1-1.0. brush { 'profile': 'gpen'/'pencil'/'airbrush'/'watercolor', 'color': '#hex', 'size': "
            "0.002-0.01 }.\n"
            "- 'particles': Atmospheric particles. Bounds [x1, y1, x2, y2], count (10-50), shape "
            "('petal'/'sparkle'/'star'/'dot'), brush { 'color': '#hex' }.\n\n"
            "%4\n"
            "=== OUTPUT SCHEMA EXAMPLE ===\n"
            "{\n"
            "  \"schema_version\": 2,\n"
            "  \"prompt\": \"illustration description\",\n"
            "  \"title\": \"Title\",\n"
            "  \"operations\": [\n"
            "    {\n"
            "      \"kind\": \"gradient_fill\",\n"
            "      \"id\": \"sky\",\n"
            "      \"layer\": \"Flats\",\n"
            "      \"polygon\": [[0.0,0.0],[1.0,0.0],[1.0,0.6],[0.0,0.6]],\n"
            "      \"colors\": [\"#1b2a4a\", \"#416788\", \"#dbe7f2\"],\n"
            "      \"angle_deg\": 90,\n"
            "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"#416788\", \"size\": 0.05, \"is_eraser\": "
            "false}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"fill\",\n"
            "      \"id\": \"ground\",\n"
            "      \"layer\": \"Flats\",\n"
            "      \"polygon\": [[0.0,0.55],[1.0,0.55],[1.0,1.0],[0.0,1.0]],\n"
            "      \"brush\": {\"profile\": \"brush\", \"color\": \"#2d3748\", \"size\": 0.04, \"is_eraser\": false}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"hatch\",\n"
            "      \"id\": \"ground_shade\",\n"
            "      \"layer\": \"Shading\",\n"
            "      \"polygon\": [[0.1,0.65],[0.5,0.65],[0.4,0.85],[0.1,0.85]],\n"
            "      \"angle_deg\": 45,\n"
            "      \"spacing\": 0.015,\n"
            "      \"cross_hatch\": false,\n"
            "      \"brush\": {\"profile\": \"pencil\", \"color\": \"#1a202c\", \"size\": 0.002, \"is_eraser\": "
            "false}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"ribbon\",\n"
            "      \"id\": \"main_tree\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"spine\": [[0.35,0.95],[0.36,0.65],[0.42,0.40]],\n"
            "      \"width_start\": 0.04,\n"
            "      \"width_mid\": 0.025,\n"
            "      \"width_end\": 0.01,\n"
            "      \"brush\": {\"profile\": \"brush\", \"color\": \"#221612\", \"size\": 0.01, \"is_eraser\": false}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"path\",\n"
            "      \"id\": \"branch_lines\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"points\": [[0.36,0.65,0.8],[0.48,0.52,0.6],[0.58,0.48,0.2]],\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#1a100d\", \"size\": 0.003, \"is_eraser\": false}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"particles\",\n"
            "      \"id\": \"stars\",\n"
            "      \"layer\": \"FX\",\n"
            "      \"bounds\": [0.05,0.05,0.95,0.50],\n"
            "      \"count\": 30,\n"
            "      \"shape\": \"sparkle\",\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#ffffff\", \"size\": 0.002, \"is_eraser\": false}\n"
            "    }\n"
            "  ]\n"
            "}")
            .arg(canvasSize.width())
            .arg(canvasSize.height())
            .arg(QString::number(aspect, 'f', 2))
            .arg(artDirection);

    if (!customInstructions.trimmed().isEmpty()) {
        systemText += QStringLiteral("\n\n[USER CUSTOM INSTRUCTIONS]\n") + customInstructions.trimmed();
    }

    return systemText;
}

QJsonObject KisAiStrokeProgramCodec::buildChatCompletionsPayload(const QString &model,
                                                                 const QString &prompt,
                                                                 const QSize &canvasSize,
                                                                 int strokeBudget,
                                                                 const QString &reasoningEffort,
                                                                 const QString &customInstructions)
{
    const bool reasoning = isReasoningModel(model);
    const QString systemText = buildSystemPrompt(canvasSize, prompt, customInstructions);

    QJsonObject userObj;
    userObj[QStringLiteral("prompt")] = prompt;
    userObj[QStringLiteral("canvas_width")] = canvasSize.width();
    userObj[QStringLiteral("canvas_height")] = canvasSize.height();
    const int geometryBudget = qBound(20, strokeBudget, 2000);
    const int operationTarget = qBound(24, geometryBudget / 5, 160);
    userObj[QStringLiteral("geometry_budget")] = geometryBudget;
    userObj[QStringLiteral("operation_target")] = operationTarget;
    userObj[QStringLiteral("budget_allocation")] =
        QJsonObject{{QStringLiteral("Flats"), QStringLiteral("20-30%")},
                    {QStringLiteral("Shading"), QStringLiteral("20-30%")},
                    {QStringLiteral("Lineart"), QStringLiteral("30-40%")},
                    {QStringLiteral("Highlights_FX"), QStringLiteral("10-20%")}};
    userObj[QStringLiteral("directive")] = QStringLiteral(
        "Create a complete, presentation-ready coordinate illustration. Treat operation_target as a quality target, "
        "not a quota: spend geometry on large registered shapes first, then focal details. Every operation must "
        "contain valid geometry and a unique semantic id.");

    const QString userText = QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact));

    QJsonArray messages;
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), userText}});

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.trimmed();
    payload[QStringLiteral("messages")] = messages;

    // Structured output via json_object or json_schema
    QJsonObject responseFormat;
    responseFormat[QStringLiteral("type")] = QStringLiteral("json_object");
    payload[QStringLiteral("response_format")] = responseFormat;

    const int calculatedTokens = qBound(6144, operationTarget * 150 + (reasoning ? 8192 : 3072), 32768);
    if (reasoning) {
        payload[QStringLiteral("max_completion_tokens")] = calculatedTokens;
        if (!reasoningEffort.isEmpty() && reasoningEffort.toLower() != QLatin1String("none")) {
            payload[QStringLiteral("reasoning_effort")] = reasoningEffort.toLower();
        }
    } else {
        payload[QStringLiteral("max_tokens")] = calculatedTokens;
        payload[QStringLiteral("temperature")] = 0.7;
    }

    return payload;
}

QString KisAiStrokeProgramCodec::repairTruncatedJson(const QString &jsonText)
{
    QString text = jsonText.trimmed();
    if (text.isEmpty())
        return text;

    QJsonParseError testErr;
    QJsonDocument::fromJson(text.toUtf8(), &testErr);
    if (testErr.error == QJsonParseError::NoError) {
        return text;
    }

    const int opsIdx = text.indexOf(QStringLiteral("\"operations\""));
    const int strokesIdx = text.indexOf(QStringLiteral("\"strokes\""));
    const int targetIdx = opsIdx >= 0 ? opsIdx : strokesIdx;

    if (targetIdx >= 0) {
        const int bracketIdx = text.indexOf(QLatin1Char('['), targetIdx);
        if (bracketIdx >= 0) {
            int lastCloseBrace = -1;
            int depth = 0;
            bool inString = false;
            bool escape = false;

            for (int i = bracketIdx + 1; i < text.length(); ++i) {
                const QChar ch = text.at(i);
                if (escape) {
                    escape = false;
                    continue;
                }
                if (ch == QLatin1Char('\\')) {
                    escape = true;
                    continue;
                }
                if (ch == QLatin1Char('"')) {
                    inString = !inString;
                    continue;
                }
                if (inString)
                    continue;

                if (ch == QLatin1Char('{')) {
                    depth++;
                } else if (ch == QLatin1Char('}')) {
                    depth--;
                    if (depth == 0) {
                        lastCloseBrace = i;
                    }
                } else if (ch == QLatin1Char(']')) {
                    lastCloseBrace = -1;
                    break;
                }
            }

            if (lastCloseBrace > bracketIdx) {
                QString repaired = text.left(lastCloseBrace + 1);
                repaired.append(QStringLiteral("\n  ]\n}"));
                QJsonParseError repErr;
                const QJsonDocument testDoc = QJsonDocument::fromJson(repaired.toUtf8(), &repErr);
                if (!testDoc.isNull() && testDoc.isObject()) {
                    return repaired;
                }
            }
        }
    }

    int openBraces = text.count(QLatin1Char('{')) - text.count(QLatin1Char('}'));
    int openBrackets = text.count(QLatin1Char('[')) - text.count(QLatin1Char(']'));
    QString fallback = text;
    while (openBrackets > 0) {
        fallback.append(QLatin1Char(']'));
        openBrackets--;
    }
    while (openBraces > 0) {
        fallback.append(QLatin1Char('}'));
        openBraces--;
    }
    return fallback;
}

QString KisAiStrokeProgramCodec::sanitizeAndExtractJson(const QString &rawText)
{
    QString text = rawText.trimmed();

    // 1. Remove <think> ... </think> or thinking tags (including unclosed tags if truncated)
    static const QRegularExpression thinkRe(QStringLiteral("(?s)<think>.*?(?:</think>|$)"));
    text.remove(thinkRe);
    static const QRegularExpression thoughtRe(QStringLiteral("(?s)<thought>.*?(?:</thought>|$)"));
    text.remove(thoughtRe);

    // 2. Extract ```json ... ``` codeblock if present (tolerant of missing closing fence)
    static const QRegularExpression codeBlockRe(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)(?:```|$)"));
    const auto match = codeBlockRe.match(text);
    if (match.hasMatch()) {
        const QString block = match.captured(1).trimmed();
        if (block.contains(QLatin1Char('{'))) {
            text = block;
        }
    }

    // 3. Find outermost { ... }
    const int firstBrace = text.indexOf(QLatin1Char('{'));
    if (firstBrace >= 0) {
        const int lastBrace = text.lastIndexOf(QLatin1Char('}'));
        if (lastBrace > firstBrace) {
            text = text.mid(firstBrace, lastBrace - firstBrace + 1).trimmed();
        } else {
            text = text.mid(firstBrace).trimmed();
        }
    }

    // 4. If invalid or truncated, attempt recovery
    QJsonParseError pErr;
    QJsonDocument::fromJson(text.toUtf8(), &pErr);
    if (pErr.error != QJsonParseError::NoError) {
        text = repairTruncatedJson(text);
    }

    return text;
}

bool KisAiStrokeProgramCodec::parseResponse(const QByteArray &responseBytes,
                                            KisAiStrokeProgram *outProgram,
                                            QString *errorMessage)
{
    const QJsonDocument doc = QJsonDocument::fromJson(responseBytes);
    if (!doc.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LLM応答がJSONオブジェクトではありません。");
        }
        return false;
    }

    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("error"))) {
        const QString err = root.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        if (errorMessage) {
            *errorMessage = QStringLiteral("APIエラー: ") + (err.isEmpty() ? QStringLiteral("不明なエラー") : err);
        }
        return false;
    }

    // Direct StrokeProgram check
    if (root.contains(QStringLiteral("operations")) || root.contains(QStringLiteral("strokes"))) {
        return parseProgramJson(root, outProgram, errorMessage);
    }

    // OpenAI Chat Completions choices[0].message.content
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LLM応答にchoices配列が存在しません。");
        }
        return false;
    }

    const QJsonObject firstChoice = choices.at(0).toObject();
    const QJsonObject messageObj = firstChoice.value(QStringLiteral("message")).toObject();
    const QString content = messageObj.value(QStringLiteral("content")).toString();

    if (content.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LLMモデルの生成テキストが空でした。");
        }
        return false;
    }

    const QString cleanJson = sanitizeAndExtractJson(content);
    QJsonParseError parseErr;
    const QJsonDocument programDoc = QJsonDocument::fromJson(cleanJson.toUtf8(), &parseErr);
    if (programDoc.isNull() || !programDoc.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("生成結果のJSON抽出またはパースに失敗しました: ") + parseErr.errorString();
        }
        return false;
    }

    return parseProgramJson(programDoc.object(), outProgram, errorMessage);
}

QString KisAiStrokeProgramCodec::normalizeLayerName(const QString &name)
{
    const QString lower = name.trimmed().toLower();
    if (lower == QLatin1String("flat") || lower == QLatin1String("flats") || lower == QLatin1String("base")
        || lower == QLatin1String("background") || lower == QLatin1String("color")
        || lower == QLatin1String("colors")) {
        return QStringLiteral("Flats");
    }
    if (lower == QLatin1String("shading") || lower == QLatin1String("shade") || lower == QLatin1String("shadow")
        || lower == QLatin1String("shadows")) {
        return QStringLiteral("Shading");
    }
    if (lower == QLatin1String("lineart") || lower == QLatin1String("line_art") || lower == QLatin1String("line art")
        || lower == QLatin1String("lines") || lower == QLatin1String("line") || lower == QLatin1String("ink")) {
        return QStringLiteral("Lineart");
    }
    if (lower == QLatin1String("highlight") || lower == QLatin1String("highlights")
        || lower == QLatin1String("specular") || lower == QLatin1String("glint")) {
        return QStringLiteral("Highlights");
    }
    if (lower == QLatin1String("fx") || lower == QLatin1String("effects") || lower == QLatin1String("effect")
        || lower == QLatin1String("particles")) {
        return QStringLiteral("FX");
    }
    return name.trimmed().isEmpty() ? QStringLiteral("Lineart") : name.trimmed();
}

QMap<QString, int> KisAiStrokeProgramCodec::countLayerOperations(const KisAiStrokeProgram &program)
{
    QMap<QString, int> counts;
    for (const auto &op : program.operations) {
        const QString layer = normalizeLayerName(op.layer);
        counts[layer]++;
    }
    return counts;
}

QString KisAiStrokeProgramCodec::formatLayerSummary(const KisAiStrokeProgram &program)
{
    const QMap<QString, int> counts = countLayerOperations(program);
    return QStringLiteral("Flats: %1, Shading: %2, Lineart: %3, Highlights: %4, FX: %5")
        .arg(counts.value(QStringLiteral("Flats"), 0))
        .arg(counts.value(QStringLiteral("Shading"), 0))
        .arg(counts.value(QStringLiteral("Lineart"), 0))
        .arg(counts.value(QStringLiteral("Highlights"), 0))
        .arg(counts.value(QStringLiteral("FX"), 0));
}

bool KisAiStrokeProgramCodec::parseProgramJson(const QJsonObject &rootObj,
                                               KisAiStrokeProgram *outProgram,
                                               QString *errorMessage)
{
    if (!outProgram) {
        return false;
    }

    outProgram->schemaVersion = rootObj.value(QStringLiteral("schema_version")).toInt(2);
    outProgram->prompt = rootObj.value(QStringLiteral("prompt")).toString();
    outProgram->title = rootObj.value(QStringLiteral("title")).toString(QStringLiteral("AI Artwork"));
    outProgram->seed = rootObj.value(QStringLiteral("seed")).toInt(42);

    if (rootObj.contains(QStringLiteral("canvas_size")) || rootObj.contains(QStringLiteral("canvas"))) {
        const QJsonObject cObj = rootObj.contains(QStringLiteral("canvas_size"))
            ? rootObj.value(QStringLiteral("canvas_size")).toObject()
            : rootObj.value(QStringLiteral("canvas")).toObject();
        const int cw = cObj.value(QStringLiteral("width")).toInt(cObj.value(QStringLiteral("w")).toInt(0));
        const int ch = cObj.value(QStringLiteral("height")).toInt(cObj.value(QStringLiteral("h")).toInt(0));
        if (cw > 0 && ch > 0) {
            outProgram->canvasSize = QSize(cw, ch);
        }
    }

    outProgram->operations.clear();

    const auto parseBrush = [](const QJsonObject &bObj) -> KisAiStrokeBrush {
        KisAiStrokeBrush b;
        b.profile = bObj.value(QStringLiteral("profile")).toString(QStringLiteral("auto"));
        b.color = parseColor(bObj.value(QStringLiteral("color")).toString(QStringLiteral("#232323")));
        b.size = bObj.value(QStringLiteral("size")).toDouble(0.008);
        b.sizeMode = bObj.value(QStringLiteral("size_mode")).toString(QStringLiteral("ratio"));
        b.opacity = clamp01(bObj.value(QStringLiteral("opacity")).toDouble(1.0));
        b.isEraser = bObj.value(QStringLiteral("is_eraser")).toBool(false);
        b.presetHint = bObj.value(QStringLiteral("preset_hint")).toString();

        // Safety: a ratio size >= 1.0 would cover 100%+ of the canvas; treat as pixels
        if (b.sizeMode.compare(QLatin1String("ratio"), Qt::CaseInsensitive) == 0 && b.size >= 1.0) {
            b.sizeMode = QStringLiteral("px");
        }
        return b;
    };

    const auto normalizeKind = [](const QString &rawKind) -> KisAiStrokeOperation::Kind {
        const QString k = rawKind.trimmed().toLower();
        if (k == QLatin1String("path") || k == QLatin1String("stroke") || k == QLatin1String("line")
            || k == QLatin1String("contour")) {
            return KisAiStrokeOperation::Kind::Path;
        }
        if (k == QLatin1String("fill") || k == QLatin1String("polygon") || k == QLatin1String("color_fill")
            || k == QLatin1String("solid_fill")) {
            return KisAiStrokeOperation::Kind::Fill;
        }
        if (k == QLatin1String("gradient_fill") || k == QLatin1String("gradient") || k == QLatin1String("gradientfill")
            || k == QLatin1String("gradient-fill")) {
            return KisAiStrokeOperation::Kind::GradientFill;
        }
        if (k == QLatin1String("ribbon") || k == QLatin1String("band") || k == QLatin1String("tapered_path")
            || k == QLatin1String("taper")) {
            return KisAiStrokeOperation::Kind::Ribbon;
        }
        if (k == QLatin1String("particles") || k == QLatin1String("particle") || k == QLatin1String("scatter")
            || k == QLatin1String("sparkles")) {
            return KisAiStrokeOperation::Kind::Particles;
        }
        if (k == QLatin1String("hatch") || k == QLatin1String("cross_hatch") || k == QLatin1String("crosshatch")
            || k == QLatin1String("shading_hatch")) {
            return KisAiStrokeOperation::Kind::Hatch;
        }
        return KisAiStrokeOperation::Kind::Unknown;
    };

    const auto parsePoint = [](const QJsonValue &pv, qreal defaultPressure = 0.8) -> QPair<QPointF, qreal> {
        if (pv.isArray()) {
            const QJsonArray pa = pv.toArray();
            if (pa.size() >= 2) {
                const qreal x = pa.at(0).toDouble();
                const qreal y = pa.at(1).toDouble();
                const qreal p = pa.size() >= 3 ? pa.at(2).toDouble(defaultPressure) : defaultPressure;
                return {QPointF(x, y), p};
            }
        } else if (pv.isObject()) {
            const QJsonObject po = pv.toObject();
            const qreal x = po.value(QStringLiteral("x")).toDouble();
            const qreal y = po.value(QStringLiteral("y")).toDouble();
            const qreal p = po.contains(QStringLiteral("pressure"))
                ? po.value(QStringLiteral("pressure")).toDouble(defaultPressure)
                : defaultPressure;
            return {QPointF(x, y), p};
        }
        return {QPointF(), -1.0};
    };

    constexpr int MAX_OPERATIONS = 2000;
    constexpr int MAX_POINTS_PER_STROKE = 1000;

    const qreal canvasW = outProgram->canvasSize.width() > 0 ? outProgram->canvasSize.width() : 1024.0;
    const qreal canvasH = outProgram->canvasSize.height() > 0 ? outProgram->canvasSize.height() : 1024.0;

    // v2: operations array
    if (rootObj.contains(QStringLiteral("operations"))) {
        const QJsonArray opArray = rootObj.value(QStringLiteral("operations")).toArray();
        const int count = qMin(opArray.size(), MAX_OPERATIONS);
        for (int oi = 0; oi < count; ++oi) {
            const QJsonValue &v = opArray.at(oi);
            if (!v.isObject())
                continue;
            const QJsonObject o = v.toObject();
            KisAiStrokeOperation op;
            op.kind = normalizeKind(o.value(QStringLiteral("kind")).toString());
            op.id = o.value(QStringLiteral("id")).toString();
            op.layer = normalizeLayerName(o.value(QStringLiteral("layer")).toString(QStringLiteral("Lineart")));
            op.brush = parseBrush(o.value(QStringLiteral("brush")).toObject());

            if (op.kind == KisAiStrokeOperation::Kind::Path) {
                op.closed = o.value(QStringLiteral("closed")).toBool(false);
                op.smooth = o.value(QStringLiteral("smooth")).toBool(true);
                op.role = o.value(QStringLiteral("role")).toString(QStringLiteral("auto"));
                const QJsonArray pts = o.value(QStringLiteral("points")).toArray();
                const int ptCount = qMin(pts.size(), MAX_POINTS_PER_STROKE);
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < ptCount; ++pi) {
                    const auto pt = parsePoint(pts.at(pi), 0.8);
                    if (pt.second >= 0.0) {
                        op.points.append(KisAiStrokePoint(pt.first.x(), pt.first.y(), pt.second));
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                // Auto-normalize if coordinates were provided in pixel values instead of [0.0, 1.0]
                if (maxCoord > 1.5) {
                    for (KisAiStrokePoint &p : op.points) {
                        p.pos = QPointF(p.pos.x() / canvasW, p.pos.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Fill) {
                op.fillStyle = o.value(QStringLiteral("style")).toString(QStringLiteral("wash"));
                op.smooth = o.value(QStringLiteral("smooth"))
                                .toBool(op.fillStyle.compare(QLatin1String("contour"), Qt::CaseInsensitive) == 0);
                op.angleDeg = o.value(QStringLiteral("angle_deg")).toDouble(0.0);
                op.spacing = o.value(QStringLiteral("spacing")).toDouble(0.5);
                const QJsonArray poly = o.value(QStringLiteral("polygon")).toArray();
                const int polyCount = qMin(poly.size(), MAX_POINTS_PER_STROKE);
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < polyCount; ++pi) {
                    const auto pt = parsePoint(poly.at(pi));
                    if (pt.second >= 0.0) {
                        op.polygon.append(pt.first);
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                if (maxCoord > 1.5) {
                    for (QPointF &p : op.polygon) {
                        p = QPointF(p.x() / canvasW, p.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::GradientFill) {
                op.fillStyle = o.value(QStringLiteral("style")).toString(QStringLiteral("linear"));
                op.smooth = o.value(QStringLiteral("smooth")).toBool(false);
                op.angleDeg = o.value(QStringLiteral("angle_deg")).toDouble(90.0);
                op.isRadial = o.value(QStringLiteral("is_radial"))
                                  .toBool(op.fillStyle.compare(QLatin1String("radial"), Qt::CaseInsensitive) == 0);
                if (o.contains(QStringLiteral("center"))) {
                    const auto cp = parsePoint(o.value(QStringLiteral("center")));
                    if (cp.second >= 0.0) {
                        op.gradientCenter = cp.first;
                        if (qMax(op.gradientCenter.x(), op.gradientCenter.y()) > 1.5) {
                            op.gradientCenter =
                                QPointF(op.gradientCenter.x() / canvasW, op.gradientCenter.y() / canvasH);
                        }
                    }
                }
                op.gradientRadius = o.value(QStringLiteral("radius")).toDouble(0.5);
                const QJsonArray colors = o.value(QStringLiteral("colors")).toArray();
                for (const QJsonValue &cv : colors) {
                    op.gradientColors.append(parseColor(cv.toString()));
                }
                const QJsonArray poly = o.value(QStringLiteral("polygon")).toArray();
                const int polyCount = qMin(poly.size(), MAX_POINTS_PER_STROKE);
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < polyCount; ++pi) {
                    const auto pt = parsePoint(poly.at(pi));
                    if (pt.second >= 0.0) {
                        op.polygon.append(pt.first);
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                if (maxCoord > 1.5) {
                    for (QPointF &p : op.polygon) {
                        p = QPointF(p.x() / canvasW, p.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Hatch) {
                op.smooth = o.value(QStringLiteral("smooth")).toBool(false);
                op.angleDeg = o.value(QStringLiteral("angle_deg")).toDouble(45.0);
                op.spacing = o.value(QStringLiteral("spacing")).toDouble(0.015);
                op.crossHatch =
                    o.value(QStringLiteral("cross_hatch")).toBool(o.value(QStringLiteral("crosshatch")).toBool(false));
                const QJsonArray poly = o.value(QStringLiteral("polygon")).toArray();
                const int polyCount = qMin(poly.size(), MAX_POINTS_PER_STROKE);
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < polyCount; ++pi) {
                    const auto pt = parsePoint(poly.at(pi));
                    if (pt.second >= 0.0) {
                        op.polygon.append(pt.first);
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                if (maxCoord > 1.5) {
                    for (QPointF &p : op.polygon) {
                        p = QPointF(p.x() / canvasW, p.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Ribbon) {
                op.widthStart = o.value(QStringLiteral("width_start")).toDouble(0.02);
                op.widthMid = o.value(QStringLiteral("width_mid")).toDouble(0.015);
                op.widthEnd = o.value(QStringLiteral("width_end")).toDouble(0.005);
                const QJsonArray spine = o.value(QStringLiteral("spine")).toArray();
                const int spineCount = qMin(spine.size(), MAX_POINTS_PER_STROKE);
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < spineCount; ++pi) {
                    const auto pt = parsePoint(spine.at(pi));
                    if (pt.second >= 0.0) {
                        op.spine.append(pt.first);
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                if (maxCoord > 1.5) {
                    for (QPointF &p : op.spine) {
                        p = QPointF(p.x() / canvasW, p.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Particles) {
                op.particleShape = o.value(QStringLiteral("shape")).toString(QStringLiteral("petal"));
                op.particleCount = o.value(QStringLiteral("count")).toInt(16);
                const QJsonArray b = o.value(QStringLiteral("bounds")).toArray();
                if (b.size() >= 4) {
                    qreal x1 = b.at(0).toDouble();
                    qreal y1 = b.at(1).toDouble();
                    qreal x2 = b.at(2).toDouble();
                    qreal y2 = b.at(3).toDouble();
                    if (qMax(qMax(x1, y1), qMax(x2, y2)) > 1.5) {
                        x1 /= canvasW;
                        y1 /= canvasH;
                        x2 /= canvasW;
                        y2 /= canvasH;
                    }
                    op.bounds = QRectF(QPointF(x1, y1), QPointF(x2, y2)).normalized();
                } else {
                    op.bounds = QRectF(0.1, 0.1, 0.8, 0.8);
                }
            }

            if (op.kind != KisAiStrokeOperation::Kind::Unknown) {
                outProgram->operations.append(op);
            }
        }
    }

    // v1 fallback: strokes array
    if (outProgram->operations.isEmpty() && rootObj.contains(QStringLiteral("strokes"))) {
        const QJsonArray strokeArray = rootObj.value(QStringLiteral("strokes")).toArray();
        const int count = qMin(strokeArray.size(), MAX_OPERATIONS);
        for (int si = 0; si < count; ++si) {
            const QJsonValue &sv = strokeArray.at(si);
            if (!sv.isObject())
                continue;
            const QJsonObject s = sv.toObject();
            KisAiStrokeOperation op;
            op.kind = KisAiStrokeOperation::Kind::Path;
            op.id = s.value(QStringLiteral("id")).toString();
            op.layer = normalizeLayerName(s.value(QStringLiteral("layer_name")).toString(QStringLiteral("Lineart")));
            op.brush.color = parseColor(s.value(QStringLiteral("color")).toString(QStringLiteral("#232323")));
            op.brush.size = s.value(QStringLiteral("size_px")).toDouble(6.0) / 1024.0;
            op.brush.opacity = clamp01(s.value(QStringLiteral("opacity")).toDouble(1.0));
            op.brush.isEraser = s.value(QStringLiteral("is_eraser")).toBool(false);
            op.brush.profile = QStringLiteral("gpen");

            const QJsonArray pts = s.value(QStringLiteral("points")).toArray();
            const int ptCount = qMin(pts.size(), MAX_POINTS_PER_STROKE);
            qreal maxCoord = 0.0;
            for (int pi = 0; pi < ptCount; ++pi) {
                const auto pt = parsePoint(pts.at(pi), 0.8);
                if (pt.second >= 0.0) {
                    op.points.append(KisAiStrokePoint(pt.first.x(), pt.first.y(), pt.second));
                    maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                }
            }
            if (maxCoord > 1.5) {
                for (KisAiStrokePoint &p : op.points) {
                    p.pos = QPointF(p.pos.x() / canvasW, p.pos.y() / canvasH);
                }
            }

            if (!op.points.isEmpty()) {
                outProgram->operations.append(op);
            }
        }
    }

    KisAiStrokeQualityReport qualityReport;
    *outProgram = refineForRendering(*outProgram, &qualityReport);

    if (outProgram->operations.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("描画可能なストローク操作が1件も含まれていません。");
        }
        return false;
    }

    return true;
}

KisAiStrokeProgram KisAiStrokeProgramCodec::refineForRendering(const KisAiStrokeProgram &program,
                                                               KisAiStrokeQualityReport *report)
{
    KisAiStrokeQualityReport localReport;
    localReport.inputOperations = program.operations.size();

    KisAiStrokeProgram refined = program;
    refined.operations.clear();
    refined.operations.reserve(program.operations.size());

    const qreal minCanvasDimension = qMax<qreal>(64.0, qMin(program.canvasSize.width(), program.canvasSize.height()));
    int generatedId = 1;

    for (const KisAiStrokeOperation &source : program.operations) {
        KisAiStrokeOperation op = source;
        op.layer = normalizeLayerName(op.layer);
        if (op.id.trimmed().isEmpty()) {
            op.id = QStringLiteral("op_%1").arg(generatedId);
            ++localReport.repairedValues;
        }
        ++generatedId;

        QString profile = op.brush.profile.trimmed().toLower();
        if (profile == QLatin1String("pen") || profile == QLatin1String("ink"))
            profile = QStringLiteral("gpen");
        if (profile == QLatin1String("spray") || profile == QLatin1String("soft"))
            profile = QStringLiteral("airbrush");
        if (profile == QLatin1String("flat") || profile == QLatin1String("paint"))
            profile = QStringLiteral("brush");
        if (profile.isEmpty() || profile == QLatin1String("auto")) {
            if (op.kind == KisAiStrokeOperation::Kind::Hatch)
                profile = QStringLiteral("pencil");
            else if (op.layer == QLatin1String("Lineart"))
                profile = QStringLiteral("gpen");
            else if (op.layer == QLatin1String("Shading") && op.kind == KisAiStrokeOperation::Kind::Path)
                profile = QStringLiteral("airbrush");
            else
                profile = QStringLiteral("brush");
            ++localReport.repairedValues;
        }
        op.brush.profile = profile;

        if (!std::isfinite(op.brush.opacity)) {
            op.brush.opacity = 1.0;
            ++localReport.repairedValues;
        } else {
            const qreal opacity = clamp01(op.brush.opacity);
            if (opacity != op.brush.opacity)
                ++localReport.repairedValues;
            op.brush.opacity = opacity;
        }

        if (op.brush.sizeMode.compare(QLatin1String("px"), Qt::CaseInsensitive) != 0) {
            op.brush.sizeMode = QStringLiteral("ratio");
            const qreal fallback = op.layer == QLatin1String("Lineart") ? 0.0045 : 0.018;
            const qreal size = std::isfinite(op.brush.size) && op.brush.size > 0.0 ? op.brush.size : fallback;
            const qreal bounded = qBound<qreal>(0.0005, size, 0.25);
            if (bounded != op.brush.size)
                ++localReport.repairedValues;
            op.brush.size = bounded;
        } else {
            op.brush.sizeMode = QStringLiteral("px");
            const qreal size = std::isfinite(op.brush.size) && op.brush.size > 0.0 ? op.brush.size : 4.0;
            const qreal bounded = qBound<qreal>(0.5, size, minCanvasDimension * 0.35);
            if (bounded != op.brush.size)
                ++localReport.repairedValues;
            op.brush.size = bounded;
        }

        if (!op.brush.color.isValid()) {
            op.brush.color = QColor(35, 35, 35);
            ++localReport.repairedValues;
        } else if (op.layer == QLatin1String("Lineart") && op.brush.color.red() == 0 && op.brush.color.green() == 0
                   && op.brush.color.blue() == 0) {
            op.brush.color.setRgb(18, 18, 24, op.brush.color.alpha());
            ++localReport.repairedValues;
        }

        QVector<int> kept;
        bool renderable = true;
        switch (op.kind) {
        case KisAiStrokeOperation::Kind::Path: {
            for (KisAiStrokePoint &point : op.points) {
                point.pos = clampedPoint(point.pos, &localReport.repairedValues);
                const qreal pressure = std::isfinite(point.pressure) ? qBound<qreal>(0.05, point.pressure, 1.0) : 0.8;
                if (pressure != point.pressure)
                    ++localReport.repairedValues;
                point.pressure = pressure;
            }
            removeAdjacentDuplicates(
                op.points.size(),
                [&op](int i) {
                    return op.points.at(i).pos;
                },
                &kept,
                &localReport.deduplicatedPoints);
            QVector<KisAiStrokePoint> points;
            points.reserve(kept.size());
            for (int index : kept)
                points.append(op.points.at(index));
            if (op.closed && points.size() > 2) {
                const QPointF delta = points.first().pos - points.last().pos;
                if (delta.x() * delta.x() + delta.y() * delta.y() <= 1.0e-8) {
                    points.removeLast();
                    ++localReport.deduplicatedPoints;
                }
            }
            if (points.size() >= 3) {
                QVector<qreal> pressures;
                pressures.reserve(points.size());
                pressures.append(points.first().pressure);
                for (int i = 1; i + 1 < points.size(); ++i) {
                    pressures.append(points.at(i - 1).pressure * 0.2 + points.at(i).pressure * 0.6
                                     + points.at(i + 1).pressure * 0.2);
                }
                pressures.append(points.last().pressure);
                for (int i = 0; i < points.size(); ++i)
                    points[i].pressure = pressures.at(i);
            }
            op.points = points;
            renderable = !op.points.isEmpty();
            break;
        }
        case KisAiStrokeOperation::Kind::Fill:
        case KisAiStrokeOperation::Kind::Hatch:
        case KisAiStrokeOperation::Kind::GradientFill: {
            for (QPointF &point : op.polygon)
                point = clampedPoint(point, &localReport.repairedValues);
            removeAdjacentDuplicates(
                op.polygon.size(),
                [&op](int i) {
                    return op.polygon.at(i);
                },
                &kept,
                &localReport.deduplicatedPoints);
            QPolygonF polygon;
            polygon.reserve(kept.size());
            for (int index : kept)
                polygon.append(op.polygon.at(index));
            if (polygon.size() > 2) {
                const QPointF delta = polygon.first() - polygon.last();
                if (delta.x() * delta.x() + delta.y() * delta.y() <= 1.0e-8) {
                    polygon.removeLast();
                    ++localReport.deduplicatedPoints;
                }
            }
            op.polygon = polygon;
            if (op.kind == KisAiStrokeOperation::Kind::GradientFill && op.polygon.size() < 3) {
                op.polygon.clear(); // Empty gradient geometry deliberately means full canvas.
            } else {
                renderable = op.polygon.size() >= 3 && polygonArea(op.polygon) > 1.0e-6;
            }
            if (!std::isfinite(op.spacing))
                op.spacing = 0.015;
            op.spacing = qBound<qreal>(0.002, op.spacing, 0.2);
            op.gradientCenter = clampedPoint(op.gradientCenter, &localReport.repairedValues);
            op.gradientRadius = qBound<qreal>(0.01, std::isfinite(op.gradientRadius) ? op.gradientRadius : 0.5, 2.0);
            break;
        }
        case KisAiStrokeOperation::Kind::Ribbon: {
            for (QPointF &point : op.spine)
                point = clampedPoint(point, &localReport.repairedValues);
            removeAdjacentDuplicates(
                op.spine.size(),
                [&op](int i) {
                    return op.spine.at(i);
                },
                &kept,
                &localReport.deduplicatedPoints);
            QVector<QPointF> spine;
            spine.reserve(kept.size());
            for (int index : kept)
                spine.append(op.spine.at(index));
            op.spine = spine;
            auto safeWidth = [&localReport](qreal width, qreal fallback) {
                const qreal bounded = qBound<qreal>(0.0005, std::isfinite(width) ? width : fallback, 0.35);
                if (bounded != width)
                    ++localReport.repairedValues;
                return bounded;
            };
            op.widthStart = safeWidth(op.widthStart, 0.02);
            op.widthMid = safeWidth(op.widthMid, 0.015);
            op.widthEnd = safeWidth(op.widthEnd, 0.005);
            renderable = op.spine.size() >= 2;
            break;
        }
        case KisAiStrokeOperation::Kind::Particles: {
            QRectF bounds = op.bounds.isValid() ? op.bounds.normalized() : QRectF(0.1, 0.1, 0.8, 0.8);
            const QPointF topLeft = clampedPoint(bounds.topLeft(), &localReport.repairedValues);
            const QPointF bottomRight = clampedPoint(bounds.bottomRight(), &localReport.repairedValues);
            op.bounds = QRectF(topLeft, bottomRight).normalized();
            op.particleCount = qBound(1, op.particleCount, 300);
            renderable = op.bounds.width() > 1.0e-4 && op.bounds.height() > 1.0e-4;
            break;
        }
        case KisAiStrokeOperation::Kind::Unknown:
            renderable = false;
            break;
        }

        if (renderable) {
            refined.operations.append(op);
        } else {
            ++localReport.droppedOperations;
        }
    }

    localReport.outputOperations = refined.operations.size();
    localReport.score = qualityScore(refined);
    if (!refined.operations.isEmpty() && !countLayerOperations(refined).contains(QStringLiteral("Flats"))) {
        localReport.warnings.append(
            QStringLiteral("Flats layer is missing; shading cannot share a complete silhouette mask."));
    }
    if (localReport.score < 0.6) {
        localReport.warnings.append(QStringLiteral("The program has low structural detail or layer coverage."));
    }
    refined.completionScore = localReport.score;
    refined.goalReached = localReport.score >= 0.6;
    if (report)
        *report = localReport;
    return refined;
}

qreal KisAiStrokeProgramCodec::qualityScore(const KisAiStrokeProgram &program)
{
    if (program.operations.isEmpty())
        return 0.0;

    QSet<QString> layers;
    QSet<int> kinds;
    QRectF occupied;
    int geometryPoints = 0;

    const auto includePoint = [&occupied](const QPointF &point) {
        const QRectF tiny(point, QSizeF(0.0001, 0.0001));
        occupied = occupied.isNull() ? tiny : occupied.united(tiny);
    };

    for (const KisAiStrokeOperation &op : program.operations) {
        layers.insert(normalizeLayerName(op.layer));
        kinds.insert(static_cast<int>(op.kind));
        for (const KisAiStrokePoint &point : op.points) {
            includePoint(point.pos);
            ++geometryPoints;
        }
        for (const QPointF &point : op.polygon) {
            includePoint(point);
            ++geometryPoints;
        }
        for (const QPointF &point : op.spine) {
            includePoint(point);
            ++geometryPoints;
        }
        if (op.kind == KisAiStrokeOperation::Kind::GradientFill && op.polygon.isEmpty()) {
            occupied = QRectF(0.0, 0.0, 1.0, 1.0);
        } else if (op.kind == KisAiStrokeOperation::Kind::Particles) {
            occupied = occupied.isNull() ? op.bounds : occupied.united(op.bounds);
            geometryPoints += op.particleCount;
        }
    }

    int essentialLayers = 0;
    for (const QString &layer : {QStringLiteral("Flats"),
                                 QStringLiteral("Shading"),
                                 QStringLiteral("Lineart"),
                                 QStringLiteral("Highlights")}) {
        if (layers.contains(layer))
            ++essentialLayers;
    }
    const qreal layerScore = qreal(essentialLayers) / 4.0;
    const qreal primitiveScore = qMin<qreal>(1.0, qreal(kinds.size()) / 4.0);
    const qreal operationScore = qMin<qreal>(1.0, qreal(program.operations.size()) / 24.0);
    const qreal detailScore = qMin<qreal>(1.0, qreal(geometryPoints) / 120.0);
    const QRectF canvas(0.0, 0.0, 1.0, 1.0);
    const QRectF clipped = occupied.intersected(canvas);
    const qreal coverageScore = qBound<qreal>(0.0, clipped.width() * clipped.height(), 1.0);

    return qBound<qreal>(0.0,
                         0.28 * layerScore + 0.18 * primitiveScore + 0.18 * operationScore + 0.18 * detailScore
                             + 0.18 * coverageScore,
                         1.0);
}

KisAiStrokeProgram KisAiStrokeProgramCodec::createDeterministicProgram(const QString &prompt, const QSize &canvasSize)
{
    KisAiStrokeProgram program;
    program.prompt = prompt;
    program.title = QStringLiteral("Procedural Artwork: ") + prompt.left(24);
    program.canvasSize = canvasSize;

    const auto spec = KisAiPromptAnalyzer::analyze(prompt, canvasSize);
    QRandomGenerator rng(stableSeed(prompt.simplified()));
    const int baseHue = rng.bounded(360);

    const auto makeHslColor = [](int h, int s, int l, int a = 255) {
        QColor c;
        c.setHsl((h % 360 + 360) % 360, qBound(0, s, 255), qBound(0, l, 255), a);
        return c;
    };

    if (spec.domain == KisAiPromptAnalyzer::DomainType::Character) {
        // =========================================================================
        // CHARACTER PORTRAIT (Anime Girl Portrait with Exquisite Eyes & Hair)
        // =========================================================================
        const QColor skinColor(QStringLiteral("#fff0e6"));
        const QColor blushColor(QStringLiteral("#ff9fb2"));
        const QColor hairColor = parseColor(spec.hairColor, QColor(QStringLiteral("#2d2036")));
        const QColor eyeColor = parseColor(spec.eyeColor, QColor(QStringLiteral("#3884ff")));
        const QColor inkColor(QStringLiteral("#1c1824"));

        // 1. Flats: Back hair mass
        {
            KisAiStrokeOperation backHair;
            backHair.kind = KisAiStrokeOperation::Kind::Fill;
            backHair.id = QStringLiteral("back_hair");
            backHair.layer = QStringLiteral("Flats");
            backHair.brush.color = hairColor.darker(130);
            backHair.polygon << QPointF(0.20, 0.35) << QPointF(0.50, 0.15) << QPointF(0.80, 0.35) << QPointF(0.88, 0.75)
                             << QPointF(0.68, 0.85) << QPointF(0.32, 0.85) << QPointF(0.12, 0.75);
            program.operations.append(backHair);
        }

        // 2. Flats: Face & Neck Skin Base
        {
            KisAiStrokeOperation skin;
            skin.kind = KisAiStrokeOperation::Kind::Fill;
            skin.id = QStringLiteral("skin_base");
            skin.layer = QStringLiteral("Flats");
            skin.brush.color = skinColor;
            skin.polygon << QPointF(0.30, 0.32) << QPointF(0.50, 0.28) << QPointF(0.70, 0.32) << QPointF(0.72, 0.52)
                         << QPointF(0.50, 0.70) << QPointF(0.28, 0.52);
            program.operations.append(skin);

            KisAiStrokeOperation neck;
            neck.kind = KisAiStrokeOperation::Kind::Fill;
            neck.id = QStringLiteral("neck_base");
            neck.layer = QStringLiteral("Flats");
            neck.brush.color = skinColor.darker(105);
            neck.polygon << QPointF(0.42, 0.65) << QPointF(0.58, 0.65) << QPointF(0.62, 0.85) << QPointF(0.38, 0.85);
            program.operations.append(neck);
        }

        // 3. Flats: Sclera & Irises
        for (int side : {-1, 1}) {
            const qreal ecx = 0.50 + side * 0.13;
            const qreal ecy = 0.46;

            KisAiStrokeOperation sclera;
            sclera.kind = KisAiStrokeOperation::Kind::Fill;
            sclera.id = QStringLiteral("sclera_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
            sclera.layer = QStringLiteral("Flats");
            sclera.brush.color = QColor(QStringLiteral("#f8f9fa"));
            sclera.polygon << QPointF(ecx - 0.055, ecy) << QPointF(ecx, ecy - 0.035) << QPointF(ecx + 0.055, ecy)
                           << QPointF(ecx, ecy + 0.035);
            program.operations.append(sclera);

            KisAiStrokeOperation iris;
            iris.kind = KisAiStrokeOperation::Kind::GradientFill;
            iris.id = QStringLiteral("iris_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
            iris.layer = QStringLiteral("Flats");
            iris.isRadial = true;
            iris.gradientCenter = QPointF(ecx, ecy);
            iris.gradientRadius = 0.035;
            iris.gradientColors << eyeColor.lighter(130) << eyeColor << eyeColor.darker(150);
            iris.polygon << QPointF(ecx - 0.032, ecy - 0.035) << QPointF(ecx + 0.032, ecy - 0.035)
                         << QPointF(ecx + 0.032, ecy + 0.035) << QPointF(ecx - 0.032, ecy + 0.035);
            program.operations.append(iris);
        }

        // 4. Flats: Front Hair Clumps
        {
            KisAiStrokeOperation bangs;
            bangs.kind = KisAiStrokeOperation::Kind::Fill;
            bangs.id = QStringLiteral("bangs_mass");
            bangs.layer = QStringLiteral("Flats");
            bangs.brush.color = hairColor;
            bangs.polygon << QPointF(0.24, 0.30) << QPointF(0.50, 0.18) << QPointF(0.76, 0.30) << QPointF(0.72, 0.42)
                          << QPointF(0.58, 0.38) << QPointF(0.50, 0.44) << QPointF(0.42, 0.38) << QPointF(0.28, 0.42);
            program.operations.append(bangs);
        }

        // 5. Shading: Cheek blush, neck AO, hair cast shadow
        {
            // Forehead / Bangs cast shadow
            KisAiStrokeOperation bangsCast;
            bangsCast.kind = KisAiStrokeOperation::Kind::Fill;
            bangsCast.id = QStringLiteral("bangs_shadow");
            bangsCast.layer = QStringLiteral("Shading");
            bangsCast.brush.color = QColor(QStringLiteral("#c48b80"));
            bangsCast.brush.opacity = 0.45;
            bangsCast.polygon << QPointF(0.28, 0.38) << QPointF(0.50, 0.40) << QPointF(0.72, 0.38)
                              << QPointF(0.70, 0.44) << QPointF(0.50, 0.46) << QPointF(0.30, 0.44);
            program.operations.append(bangsCast);

            // Neck Contact Hatch Shading
            KisAiStrokeOperation neckHatch;
            neckHatch.kind = KisAiStrokeOperation::Kind::Hatch;
            neckHatch.id = QStringLiteral("neck_hatch");
            neckHatch.layer = QStringLiteral("Shading");
            neckHatch.brush.color = QColor(QStringLiteral("#a36a60"));
            neckHatch.brush.opacity = 0.55;
            neckHatch.angleDeg = 45.0;
            neckHatch.spacing = 0.012;
            neckHatch.polygon << QPointF(0.40, 0.66) << QPointF(0.60, 0.66) << QPointF(0.62, 0.82)
                              << QPointF(0.38, 0.82);
            program.operations.append(neckHatch);

            // Cheeks Soft Blush
            for (int side : {-1, 1}) {
                KisAiStrokeOperation blush;
                blush.kind = KisAiStrokeOperation::Kind::Fill;
                blush.id = QStringLiteral("blush_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                blush.layer = QStringLiteral("Shading");
                blush.brush.color = blushColor;
                blush.brush.opacity = 0.35;
                const qreal bcx = 0.50 + side * 0.14;
                blush.polygon << QPointF(bcx - 0.04, 0.52) << QPointF(bcx + 0.04, 0.52) << QPointF(bcx + 0.04, 0.56)
                              << QPointF(bcx - 0.04, 0.56);
                program.operations.append(blush);
            }
        }

        // 6. Lineart: Face contour, exquisite eyelashes, double eyelids, nose, mouth
        {
            // Jawline
            KisAiStrokeOperation jaw;
            jaw.kind = KisAiStrokeOperation::Kind::Path;
            jaw.id = QStringLiteral("jawline");
            jaw.layer = QStringLiteral("Lineart");
            jaw.brush.profile = QStringLiteral("gpen");
            jaw.brush.color = inkColor;
            jaw.brush.size = 0.0035;
            jaw.points << KisAiStrokePoint(0.28, 0.48, 0.4) << KisAiStrokePoint(0.32, 0.58, 0.8)
                       << KisAiStrokePoint(0.50, 0.70, 0.9) << KisAiStrokePoint(0.68, 0.58, 0.8)
                       << KisAiStrokePoint(0.72, 0.48, 0.4);
            program.operations.append(jaw);

            // Eyes: Upper lashes, double eyelid, pupil, lower lash
            for (int side : {-1, 1}) {
                const qreal ecx = 0.50 + side * 0.13;
                const qreal ecy = 0.46;

                // Upper thick eyelash arch
                KisAiStrokeOperation upperLash;
                upperLash.kind = KisAiStrokeOperation::Kind::Path;
                upperLash.id = QStringLiteral("upper_lash_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                upperLash.layer = QStringLiteral("Lineart");
                upperLash.brush.profile = QStringLiteral("gpen");
                upperLash.brush.color = inkColor;
                upperLash.brush.size = 0.0045;
                upperLash.points << KisAiStrokePoint(ecx - side * 0.05, ecy + 0.005, 0.3)
                                 << KisAiStrokePoint(ecx, ecy - 0.025, 1.0)
                                 << KisAiStrokePoint(ecx + side * 0.055, ecy - 0.015, 0.7)
                                 << KisAiStrokePoint(ecx + side * 0.07, ecy - 0.025, 0.2); // Outer flick
                program.operations.append(upperLash);

                // Double eyelid crease (delicate fine line)
                KisAiStrokeOperation doubleLid;
                doubleLid.kind = KisAiStrokeOperation::Kind::Path;
                doubleLid.id = QStringLiteral("double_lid_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                doubleLid.layer = QStringLiteral("Lineart");
                doubleLid.brush.profile = QStringLiteral("gpen");
                doubleLid.brush.color = inkColor;
                doubleLid.brush.size = 0.0018;
                doubleLid.points << KisAiStrokePoint(ecx - side * 0.035, ecy - 0.035, 0.3)
                                 << KisAiStrokePoint(ecx, ecy - 0.042, 0.6)
                                 << KisAiStrokePoint(ecx + side * 0.04, ecy - 0.038, 0.2);
                program.operations.append(doubleLid);

                // Pupil Core
                KisAiStrokeOperation pupil;
                pupil.kind = KisAiStrokeOperation::Kind::Path;
                pupil.id = QStringLiteral("pupil_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                pupil.layer = QStringLiteral("Lineart");
                pupil.brush.profile = QStringLiteral("gpen");
                pupil.brush.color = inkColor;
                pupil.brush.size = 0.007;
                pupil.points << KisAiStrokePoint(ecx, ecy - 0.005, 1.0) << KisAiStrokePoint(ecx, ecy + 0.005, 1.0);
                program.operations.append(pupil);
            }

            // Nose tip point & Mouth smile line
            KisAiStrokeOperation nose;
            nose.kind = KisAiStrokeOperation::Kind::Path;
            nose.id = QStringLiteral("nose");
            nose.layer = QStringLiteral("Lineart");
            nose.brush.profile = QStringLiteral("gpen");
            nose.brush.color = inkColor;
            nose.brush.size = 0.0022;
            nose.points << KisAiStrokePoint(0.50, 0.54, 0.6) << KisAiStrokePoint(0.505, 0.548, 0.4);
            program.operations.append(nose);

            KisAiStrokeOperation mouth;
            mouth.kind = KisAiStrokeOperation::Kind::Path;
            mouth.id = QStringLiteral("mouth");
            mouth.layer = QStringLiteral("Lineart");
            mouth.brush.profile = QStringLiteral("gpen");
            mouth.brush.color = inkColor;
            mouth.brush.size = 0.0025;
            mouth.points << KisAiStrokePoint(0.46, 0.61, 0.3) << KisAiStrokePoint(0.50, 0.616, 0.8)
                         << KisAiStrokePoint(0.54, 0.61, 0.3);
            program.operations.append(mouth);

            // Hair Strands (Ribbons & Fine Paths)
            KisAiStrokeOperation hairStrandL;
            hairStrandL.kind = KisAiStrokeOperation::Kind::Ribbon;
            hairStrandL.id = QStringLiteral("hair_strand_l");
            hairStrandL.layer = QStringLiteral("Lineart");
            hairStrandL.brush.color = hairColor.darker(110);
            hairStrandL.widthStart = 0.025;
            hairStrandL.widthMid = 0.018;
            hairStrandL.widthEnd = 0.004;
            hairStrandL.spine << QPointF(0.32, 0.28) << QPointF(0.24, 0.48) << QPointF(0.22, 0.70);
            program.operations.append(hairStrandL);

            KisAiStrokeOperation hairStrandR;
            hairStrandR.kind = KisAiStrokeOperation::Kind::Ribbon;
            hairStrandR.id = QStringLiteral("hair_strand_r");
            hairStrandR.layer = QStringLiteral("Lineart");
            hairStrandR.brush.color = hairColor.darker(110);
            hairStrandR.widthStart = 0.025;
            hairStrandR.widthMid = 0.018;
            hairStrandR.widthEnd = 0.004;
            hairStrandR.spine << QPointF(0.68, 0.28) << QPointF(0.76, 0.48) << QPointF(0.78, 0.70);
            program.operations.append(hairStrandR);
        }

        // 7. Highlights: Specular catchlights & angel halo
        {
            for (int side : {-1, 1}) {
                const qreal ecx = 0.50 + side * 0.13;
                const qreal ecy = 0.46;

                // Main bright eye catchlight
                KisAiStrokeOperation catchlight;
                catchlight.kind = KisAiStrokeOperation::Kind::Path;
                catchlight.id = QStringLiteral("catchlight_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                catchlight.layer = QStringLiteral("Highlights");
                catchlight.brush.profile = QStringLiteral("gpen");
                catchlight.brush.color = QColor(QStringLiteral("#ffffff"));
                catchlight.brush.size = 0.0045;
                catchlight.points << KisAiStrokePoint(ecx - 0.012, ecy - 0.012, 1.0)
                                  << KisAiStrokePoint(ecx - 0.008, ecy - 0.008, 1.0);
                program.operations.append(catchlight);

                // Crescent lower rim glow
                KisAiStrokeOperation crescent;
                crescent.kind = KisAiStrokeOperation::Kind::Path;
                crescent.id = QStringLiteral("crescent_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                crescent.layer = QStringLiteral("Highlights");
                crescent.brush.profile = QStringLiteral("gpen");
                crescent.brush.color = QColor(QStringLiteral("#aae0ff"));
                crescent.brush.size = 0.0022;
                crescent.points << KisAiStrokePoint(ecx - 0.018, ecy + 0.018, 0.4)
                                << KisAiStrokePoint(ecx, ecy + 0.025, 0.8)
                                << KisAiStrokePoint(ecx + 0.018, ecy + 0.018, 0.4);
                program.operations.append(crescent);
            }

            // Hair Angel Halo Rim Light
            KisAiStrokeOperation halo;
            halo.kind = KisAiStrokeOperation::Kind::Path;
            halo.id = QStringLiteral("hair_halo");
            halo.layer = QStringLiteral("Highlights");
            halo.brush.profile = QStringLiteral("airbrush");
            halo.brush.color = QColor(QStringLiteral("#ffffff"));
            halo.brush.opacity = 0.65;
            halo.brush.size = 0.012;
            halo.points << KisAiStrokePoint(0.32, 0.26, 0.2) << KisAiStrokePoint(0.50, 0.22, 0.8)
                        << KisAiStrokePoint(0.68, 0.26, 0.2);
            program.operations.append(halo);
        }

        // 8. FX: Floating sparkles / Petals
        {
            KisAiStrokeOperation fx;
            fx.kind = KisAiStrokeOperation::Kind::Particles;
            fx.id = QStringLiteral("sparkles");
            fx.layer = QStringLiteral("FX");
            fx.brush.color = QColor(QStringLiteral("#ffebf0"));
            fx.bounds = QRectF(0.10, 0.15, 0.80, 0.75);
            fx.particleCount = 24;
            fx.particleShape = spec.hasSakura ? QStringLiteral("petal") : QStringLiteral("sparkle");
            program.operations.append(fx);
        }

        return program;
    }

    // =========================================================================
    // LANDSCAPE / NATURE / GENERAL PROCEDURAL SCENERY
    // =========================================================================

    // 1. Flats: Sky wash with time-of-day gradient
    {
        KisAiStrokeOperation sky;
        sky.kind = KisAiStrokeOperation::Kind::GradientFill;
        sky.id = QStringLiteral("sky_gradient");
        sky.layer = QStringLiteral("Flats");
        sky.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 0.65) << QPointF(0.0, 0.65);
        for (const QString &cStr : spec.skyGradientColors) {
            sky.gradientColors.append(parseColor(cStr));
        }
        sky.angleDeg = 90.0;
        program.operations.append(sky);
    }

    // 2. Flats: Distant Mountains
    {
        KisAiStrokeOperation mountain;
        mountain.kind = KisAiStrokeOperation::Kind::Fill;
        mountain.id = QStringLiteral("distant_mountain");
        mountain.layer = QStringLiteral("Flats");
        mountain.brush.color = makeHslColor(baseHue + 15, 110, 80, 240);
        mountain.brush.profile = QStringLiteral("watercolor");
        mountain.polygon << QPointF(0.0, 0.55) << QPointF(0.22, 0.38) << QPointF(0.50, 0.46) << QPointF(0.78, 0.35)
                         << QPointF(1.0, 0.50) << QPointF(1.0, 0.75) << QPointF(0.0, 0.75);
        program.operations.append(mountain);
    }

    // 3. Flats: Foreground ground
    {
        KisAiStrokeOperation ground;
        ground.kind = KisAiStrokeOperation::Kind::Fill;
        ground.id = QStringLiteral("foreground_ground");
        ground.layer = QStringLiteral("Flats");
        ground.brush.color = makeHslColor(baseHue - 40, 120, 50);
        ground.brush.profile = QStringLiteral("brush");
        ground.polygon << QPointF(0.0, 0.68) << QPointF(0.40, 0.64) << QPointF(0.75, 0.69) << QPointF(1.0, 0.65)
                       << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
        program.operations.append(ground);
    }

    // 4. Shading: Mountain shadow facets & Hatching
    {
        KisAiStrokeOperation mtnHatch;
        mtnHatch.kind = KisAiStrokeOperation::Kind::Hatch;
        mtnHatch.id = QStringLiteral("mountain_hatch");
        mtnHatch.layer = QStringLiteral("Shading");
        mtnHatch.brush.color = makeHslColor(baseHue - 10, 90, 45, 180);
        mtnHatch.angleDeg = 35.0;
        mtnHatch.spacing = 0.015;
        mtnHatch.crossHatch = false;
        mtnHatch.polygon << QPointF(0.22, 0.38) << QPointF(0.35, 0.49) << QPointF(0.28, 0.60) << QPointF(0.12, 0.56);
        program.operations.append(mtnHatch);
    }

    // 5. Lineart: Trunk ribbon
    {
        KisAiStrokeOperation trunk;
        trunk.kind = KisAiStrokeOperation::Kind::Ribbon;
        trunk.id = QStringLiteral("tree_trunk");
        trunk.layer = QStringLiteral("Lineart");
        trunk.brush.color = makeHslColor(baseHue + 180, 80, 30);
        trunk.widthStart = 0.045;
        trunk.widthMid = 0.028;
        trunk.widthEnd = 0.012;
        trunk.spine << QPointF(0.36, 0.95) << QPointF(0.34, 0.72) << QPointF(0.42, 0.48) << QPointF(0.40, 0.32);
        program.operations.append(trunk);
    }

    // 6. Lineart: Tree branches
    for (int b = 0; b < 4; ++b) {
        KisAiStrokeOperation branch;
        branch.kind = KisAiStrokeOperation::Kind::Path;
        branch.id = QStringLiteral("branch_") + QString::number(b);
        branch.layer = QStringLiteral("Lineart");
        branch.brush.profile = QStringLiteral("gpen");
        branch.brush.color = makeHslColor(baseHue + 180, 85, 24);
        branch.brush.size = 0.0035;

        const qreal startY = 0.60 - b * 0.08;
        const qreal dir = (b % 2 == 0) ? 1.0 : -1.0;
        branch.points.append(KisAiStrokePoint(0.36, startY, 0.9));
        branch.points.append(KisAiStrokePoint(0.36 + dir * 0.12, startY - 0.05, 0.6));
        branch.points.append(KisAiStrokePoint(0.36 + dir * 0.24, startY - 0.09, 0.2));
        program.operations.append(branch);
    }

    // 7. Highlights: Rim light & speculars
    {
        KisAiStrokeOperation rim;
        rim.kind = KisAiStrokeOperation::Kind::Path;
        rim.id = QStringLiteral("tree_rim_light");
        rim.layer = QStringLiteral("Highlights");
        rim.brush.profile = QStringLiteral("gpen");
        rim.brush.color = makeHslColor(baseHue + 40, 200, 240, 220);
        rim.brush.size = 0.003;
        rim.points.append(KisAiStrokePoint(0.38, 0.90, 0.3));
        rim.points.append(KisAiStrokePoint(0.36, 0.70, 0.8));
        rim.points.append(KisAiStrokePoint(0.43, 0.46, 0.4));
        program.operations.append(rim);
    }

    // 8. FX: Falling petals / Sparkles
    {
        KisAiStrokeOperation particles;
        particles.kind = KisAiStrokeOperation::Kind::Particles;
        particles.id = QStringLiteral("floating_particles");
        particles.layer = QStringLiteral("FX");
        particles.brush.color =
            spec.hasSakura ? QColor(QStringLiteral("#ffb8cd")) : makeHslColor(baseHue + 50, 180, 210, 200);
        particles.bounds = QRectF(0.1, 0.15, 0.8, 0.8);
        particles.particleCount = 32;
        particles.particleShape = spec.hasSakura ? QStringLiteral("petal") : QStringLiteral("sparkle");
        program.operations.append(particles);
    }

    return refineForRendering(program);
}
