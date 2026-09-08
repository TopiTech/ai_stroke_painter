/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeProgram.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace
{
qreal clamp01(qreal v)
{
    return qMax<qreal>(0.0, qMin<qreal>(1.0, v));
}

bool isReasoningModel(const QString &model)
{
    const QString lower = model.toLower().trimmed();
    return lower.contains(QLatin1String("o1"))
        || lower.contains(QLatin1String("o3"))
        || lower.contains(QLatin1String("deepseek-r1"))
        || lower.contains(QLatin1String("deepseek-reasoner"))
        || lower.contains(QLatin1String("thinking"))
        || lower.contains(QLatin1String("reasoner"))
        || lower.contains(QLatin1String("qwq"));
}
}

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
        if (okR && okG && okB) return QColor(r, g, b);
    } else if (s.length() == 4) {
        // RGBA -> RRGGBBAA
        bool okR = false, okG = false, okB = false, okA = false;
        const int r = s.mid(0, 1).repeated(2).toInt(&okR, 16);
        const int g = s.mid(1, 1).repeated(2).toInt(&okG, 16);
        const int b = s.mid(2, 1).repeated(2).toInt(&okB, 16);
        const int a = s.mid(3, 1).repeated(2).toInt(&okA, 16);
        if (okR && okG && okB && okA) return QColor(r, g, b, a);
    } else if (s.length() == 6) {
        bool ok = false;
        const int val = s.toInt(&ok, 16);
        if (ok) return QColor((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
    } else if (s.length() == 8) {
        bool ok = false;
        const uint val = s.toUInt(&ok, 16);
        if (ok) return QColor((val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
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
    brushProps[QStringLiteral("size")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}};
    brushProps[QStringLiteral("size_mode")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    brushProps[QStringLiteral("opacity")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}};
    brushProps[QStringLiteral("is_eraser")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    brushSchema[QStringLiteral("properties")] = brushProps;
    brushSchema[QStringLiteral("required")] = QJsonArray{
        QStringLiteral("profile"),
        QStringLiteral("color"),
        QStringLiteral("size"),
        QStringLiteral("is_eraser")
    };

    QJsonObject opItem;
    opItem[QStringLiteral("type")] = QStringLiteral("object");
    QJsonObject opProps;
    opProps[QStringLiteral("kind")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("layer")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("brush")] = brushSchema;
    opProps[QStringLiteral("points")] = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), pointSchema}
    };
    opProps[QStringLiteral("polygon")] = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), pointSchema}
    };
    opProps[QStringLiteral("spine")] = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), pointSchema}
    };
    opProps[QStringLiteral("colors")] = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    };
    opProps[QStringLiteral("style")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("angle_deg")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}};
    opProps[QStringLiteral("bounds")] = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), pointItem}
    };
    opProps[QStringLiteral("count")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
    opProps[QStringLiteral("shape")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opItem[QStringLiteral("properties")] = opProps;
    opItem[QStringLiteral("required")] = QJsonArray{
        QStringLiteral("kind"),
        QStringLiteral("id"),
        QStringLiteral("layer"),
        QStringLiteral("brush")
    };

    QJsonObject rootProps;
    rootProps[QStringLiteral("schema_version")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
    rootProps[QStringLiteral("prompt")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("title")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("operations")] = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), opItem}
    };

    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    schema[QStringLiteral("properties")] = rootProps;
    schema[QStringLiteral("required")] = QJsonArray{
        QStringLiteral("schema_version"),
        QStringLiteral("operations")
    };

    return schema;
}

QString KisAiStrokeProgramCodec::buildSystemPrompt(const QSize &canvasSize, const QString &prompt)
{
    Q_UNUSED(prompt);
    const qreal aspect = canvasSize.height() > 0 ? qreal(canvasSize.width()) / canvasSize.height() : 1.0;

    return QStringLiteral(
        "You are an autonomous AI master digital painter directing layer-by-layer drawing plans for Krita.\n"
        "Generate a rich, cohesive, painterly illustration by specifying coordinate-directed strokes in StrokeProgram JSON format.\n"
        "Output ONLY valid JSON. Do NOT include markdown explanations, thought text, or conversational chatter.\n\n"
        "=== COORDINATE SYSTEM & RESOLUTION ===\n"
        "Coordinates are normalized float numbers strictly in [0.0, 1.0]. (0.0, 0.0) is top-left, (1.0, 1.0) is bottom-right.\n"
        "Canvas size: %1x%2 (Aspect %3:1).\n\n"
        "=== LAYER ARCHITECTURE & COMPOSITION (Back-to-Front) ===\n"
        "1. 'Flats': Backdrop gradients, sky wash, terrain base, major silhouette color blocking (hair base, skin base, clothing base).\n"
        "2. 'Shading': Form shadows, ambient occlusion, depth crevices, cast shadows (rendered with Multiply blend and automatically clipped to Flats).\n"
        "3. 'Lineart': Crisp contours, facial details, hair strands, structural outlines (rendered with natural Catmull-Rom spline curves and tapering).\n"
        "4. 'Highlights': Specular glints, eye catchlights, rim lighting, atmospheric glow (rendered with Addition blend and clipped to Flats).\n"
        "5. 'FX': Particle accents, petals, embers, stars, sparkles, bloom.\n\n"
        "=== ARTISTIC & ANATOMICAL GUIDELINES ===\n"
        "- Contours & Splines: The engine interpolates path points with Catmull-Rom splines and natural pressure tapering.\n"
        "  Provide 4-8 smooth anchor points per curved feature (silhouette curves, hair flow, eyes, fabric folds) for expressive lines.\n"
        "- Color Harmony: Avoid harsh pure black (#000000) for lineart; use rich dark tones (e.g. #1a162b, #1c2438, #2b1b17).\n"
        "  Employ warm key lights paired with cool shadows, or cool ambient light paired with warm saturated bounced light.\n"
        "- Clean Silhouettes: Ensure Flats cover the full subject silhouette so that Shading and Highlights stay cleanly bounded.\n\n"
        "=== OPERATION KINDS ===\n"
        "- 'gradient_fill': Full/partial sky & background washes. Polygon [ [x, y], ... ], colors [ '#hex', ... ], angle_deg (0=horizontal, 90=vertical).\n"
        "- 'fill': Color masses, silhouettes, hair/clothing base, shadow blocks. Polygon [ [x, y], ... ], brush { 'profile': 'watercolor'/'brush', 'color': '#hex' }, style ('wash'/'contour'/'directional').\n"
        "- 'ribbon': Tapered organic strokes (tree trunks, hair clumps, cloth folds). Spine [ [x, y], ... ], width_start, width_mid, width_end (0.005-0.05).\n"
        "- 'path': Expressive linework, contours, facial features. Points [ [x, y, pressure], ... ] where pressure is 0.1-1.0. brush { 'profile': 'gpen'/'pencil'/'airbrush'/'watercolor', 'color': '#hex', 'size': 0.002-0.01 }.\n"
        "- 'particles': Atmospheric particles. Bounds [x1, y1, x2, y2], count (10-50), shape ('petal'/'sparkle'/'star'/'dot'), brush { 'color': '#hex' }.\n\n"
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
        "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"#416788\", \"size\": 0.05, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"ground\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"polygon\": [[0.0,0.55],[1.0,0.55],[1.0,1.0],[0.0,1.0]],\n"
        "      \"brush\": {\"profile\": \"brush\", \"color\": \"#2d3748\", \"size\": 0.04, \"is_eraser\": false}\n"
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
        "}"
    ).arg(canvasSize.width()).arg(canvasSize.height()).arg(QString::number(aspect, 'f', 2));
}

QJsonObject KisAiStrokeProgramCodec::buildChatCompletionsPayload(
    const QString &model,
    const QString &prompt,
    const QSize &canvasSize,
    int strokeBudget,
    const QString &reasoningEffort
)
{
    const bool reasoning = isReasoningModel(model);
    const QString systemText = buildSystemPrompt(canvasSize, prompt);

    QJsonObject userObj;
    userObj[QStringLiteral("prompt")] = prompt;
    userObj[QStringLiteral("canvas_width")] = canvasSize.width();
    userObj[QStringLiteral("canvas_height")] = canvasSize.height();
    userObj[QStringLiteral("stroke_budget")] = strokeBudget;
    userObj[QStringLiteral("directive")] = QStringLiteral("Generate complete coordinate-directed illustration strokes covering Flats, Shading, Lineart, Highlights, and FX.");

    const QString userText = QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact));

    QJsonArray messages;
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("content"), systemText}
    });
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), userText}
    });

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.trimmed();
    payload[QStringLiteral("messages")] = messages;

    // Structured output via json_object or json_schema
    QJsonObject responseFormat;
    responseFormat[QStringLiteral("type")] = QStringLiteral("json_object");
    payload[QStringLiteral("response_format")] = responseFormat;

    const int calculatedTokens = qBound(4096, strokeBudget * 60 + (reasoning ? 8192 : 2048), 32768);
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
    if (text.isEmpty()) return text;

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
                if (inString) continue;

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

bool KisAiStrokeProgramCodec::parseResponse(
    const QByteArray &responseBytes,
    KisAiStrokeProgram *outProgram,
    QString *errorMessage
)
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

bool KisAiStrokeProgramCodec::parseProgramJson(
    const QJsonObject &rootObj,
    KisAiStrokeProgram *outProgram,
    QString *errorMessage
)
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

    const auto normalizeLayerName = [](const QString &name) -> QString {
        const QString lower = name.trimmed().toLower();
        if (lower == QLatin1String("flat") || lower == QLatin1String("flats") ||
            lower == QLatin1String("base") || lower == QLatin1String("background") ||
            lower == QLatin1String("color") || lower == QLatin1String("colors")) {
            return QStringLiteral("Flats");
        }
        if (lower == QLatin1String("shading") || lower == QLatin1String("shade") ||
            lower == QLatin1String("shadow") || lower == QLatin1String("shadows")) {
            return QStringLiteral("Shading");
        }
        if (lower == QLatin1String("lineart") || lower == QLatin1String("line_art") ||
            lower == QLatin1String("line art") || lower == QLatin1String("lines") ||
            lower == QLatin1String("line") || lower == QLatin1String("ink")) {
            return QStringLiteral("Lineart");
        }
        if (lower == QLatin1String("highlight") || lower == QLatin1String("highlights") ||
            lower == QLatin1String("specular") || lower == QLatin1String("glint")) {
            return QStringLiteral("Highlights");
        }
        if (lower == QLatin1String("fx") || lower == QLatin1String("effects") ||
            lower == QLatin1String("effect") || lower == QLatin1String("particles")) {
            return QStringLiteral("FX");
        }
        return name.trimmed().isEmpty() ? QStringLiteral("Lineart") : name.trimmed();
    };

    const auto normalizeKind = [](const QString &rawKind) -> KisAiStrokeOperation::Kind {
        const QString k = rawKind.trimmed().toLower();
        if (k == QLatin1String("path") || k == QLatin1String("stroke") ||
            k == QLatin1String("line") || k == QLatin1String("contour")) {
            return KisAiStrokeOperation::Kind::Path;
        }
        if (k == QLatin1String("fill") || k == QLatin1String("polygon") ||
            k == QLatin1String("color_fill") || k == QLatin1String("solid_fill")) {
            return KisAiStrokeOperation::Kind::Fill;
        }
        if (k == QLatin1String("gradient_fill") || k == QLatin1String("gradient") ||
            k == QLatin1String("gradientfill") || k == QLatin1String("gradient-fill")) {
            return KisAiStrokeOperation::Kind::GradientFill;
        }
        if (k == QLatin1String("ribbon") || k == QLatin1String("band") ||
            k == QLatin1String("tapered_path") || k == QLatin1String("taper")) {
            return KisAiStrokeOperation::Kind::Ribbon;
        }
        if (k == QLatin1String("particles") || k == QLatin1String("particle") ||
            k == QLatin1String("scatter") || k == QLatin1String("sparkles")) {
            return KisAiStrokeOperation::Kind::Particles;
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
            const qreal p = po.contains(QStringLiteral("pressure")) ? po.value(QStringLiteral("pressure")).toDouble(defaultPressure) : defaultPressure;
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
            if (!v.isObject()) continue;
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
                op.angleDeg = o.value(QStringLiteral("angle_deg")).toDouble(90.0);
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
                        x1 /= canvasW; y1 /= canvasH;
                        x2 /= canvasW; y2 /= canvasH;
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
            if (!sv.isObject()) continue;
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

    if (outProgram->operations.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("描画可能なストローク操作が1件も含まれていません。");
        }
        return false;
    }

    return true;
}

KisAiStrokeProgram KisAiStrokeProgramCodec::createDeterministicProgram(const QString &prompt, const QSize &canvasSize)
{
    KisAiStrokeProgram program;
    program.prompt = prompt;
    program.title = QStringLiteral("Procedural Artwork: ") + prompt.left(24);
    program.canvasSize = canvasSize;

    QRandomGenerator rng(stableSeed(prompt.simplified()));
    const int baseHue = rng.bounded(360);

    const auto makeHslColor = [](int h, int s, int l, int a = 255) {
        QColor c;
        c.setHsl((h % 360 + 360) % 360, qBound(0, s, 255), qBound(0, l, 255), a);
        return c;
    };

    // 1. Flats: Sky wash
    {
        KisAiStrokeOperation sky;
        sky.kind = KisAiStrokeOperation::Kind::GradientFill;
        sky.id = QStringLiteral("sky_gradient");
        sky.layer = QStringLiteral("Flats");
        sky.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 0.65) << QPointF(0.0, 0.65);
        sky.gradientColors << makeHslColor(baseHue - 25, 140, 60)
                           << makeHslColor(baseHue, 160, 140)
                           << makeHslColor(baseHue + 35, 190, 220);
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
        mountain.polygon << QPointF(0.0, 0.55)
                         << QPointF(0.22, 0.38)
                         << QPointF(0.50, 0.46)
                         << QPointF(0.78, 0.35)
                         << QPointF(1.0, 0.50)
                         << QPointF(1.0, 0.75)
                         << QPointF(0.0, 0.75);
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
        ground.polygon << QPointF(0.0, 0.68)
                       << QPointF(0.40, 0.64)
                       << QPointF(0.75, 0.69)
                       << QPointF(1.0, 0.65)
                       << QPointF(1.0, 1.0)
                       << QPointF(0.0, 1.0);
        program.operations.append(ground);
    }

    // 4. Shading: Mountain shadow facets
    {
        KisAiStrokeOperation mtnShadow;
        mtnShadow.kind = KisAiStrokeOperation::Kind::Fill;
        mtnShadow.id = QStringLiteral("mountain_shadow");
        mtnShadow.layer = QStringLiteral("Shading");
        mtnShadow.brush.color = makeHslColor(baseHue - 10, 90, 45, 180);
        mtnShadow.polygon << QPointF(0.22, 0.38)
                          << QPointF(0.35, 0.49)
                          << QPointF(0.28, 0.60)
                          << QPointF(0.12, 0.56);
        program.operations.append(mtnShadow);
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
        trunk.spine << QPointF(0.36, 0.95)
                    << QPointF(0.34, 0.72)
                    << QPointF(0.42, 0.48)
                    << QPointF(0.40, 0.32);
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
        particles.brush.color = makeHslColor(baseHue + 50, 180, 210, 200);
        particles.bounds = QRectF(0.1, 0.15, 0.8, 0.8);
        particles.particleCount = 28;
        particles.particleShape = prompt.contains(QStringLiteral("桜")) || prompt.toLower().contains(QLatin1String("sakura"))
            ? QStringLiteral("petal") : QStringLiteral("sparkle");
        program.operations.append(particles);
    }

    return program;
}
