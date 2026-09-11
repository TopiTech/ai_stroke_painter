/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeProgram.h"
#include "KisAiPromptAnalyzer.h"
#include "KisAiStrokeTypeChecker.h"

QString KisAiJsonDiagnostic::formatForLog() const
{
    if (!hasError) {
        return QStringLiteral("JsonDiagnostic: No errors. Repairs applied: [%1]")
            .arg(appliedRepairs.join(QStringLiteral(", ")));
    }
    return QStringLiteral("JsonDiagnostic: Error at line %1 col %2 (offset %3): %4\nSnippet: %5\nRepairs applied: [%6]")
        .arg(errorLine)
        .arg(errorColumn)
        .arg(errorOffset)
        .arg(errorMessage)
        .arg(errorSnippet)
        .arg(appliedRepairs.join(QStringLiteral(", ")));
}

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPair>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace
{
// Threshold above which coordinates are interpreted as pixel values
// rather than normalized [0.0, 1.0] values.
constexpr qreal kPixelCoordinateThreshold = 1.5;

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

// Placeholder written in place of a preserved string literal while the syntax
// repair passes run. Kept in one place so the masking and restore steps cannot
// drift apart.
QString stringMaskPlaceholder(int index)
{
    return QStringLiteral("\"__AI_STR_MASK_%1__\"").arg(index);
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
} // namespace

bool KisAiStrokeProgramCodec::isReasoningModel(const QString &model)
{
    const QString lower = model.toLower().trimmed();
    return lower.contains(QLatin1String("o1")) || lower.contains(QLatin1String("o3"))
        || lower.contains(QLatin1String("deepseek-r1")) || lower.contains(QLatin1String("deepseek-reasoner"))
        || lower.contains(QLatin1String("thinking")) || lower.contains(QLatin1String("reasoner"))
        || lower.contains(QLatin1String("qwq")) || lower.contains(QLatin1String("dots"))
        || lower.contains(QLatin1String("note")) || lower.contains(QLatin1String("r1-distill"));
}

bool KisAiStrokeProgramCodec::isAcceptedResponseContentType(const QByteArray &contentType, bool streaming)
{
    if (contentType.isEmpty()) {
        // A few OpenAI-compatible servers omit the header despite returning a
        // valid response. The parser remains the final validation boundary.
        return true;
    }

    const int parameterStart = contentType.indexOf(';');
    const QByteArray primary =
        contentType.left(parameterStart < 0 ? contentType.size() : parameterStart).trimmed().toLower();
    if (primary == "application/json" || primary == "application/x-json" || primary == "text/json") {
        return true;
    }

    // Chat Completions with stream=true returns data-only Server-Sent Events.
    return streaming && primary == "text/event-stream";
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
                bool alphaOk = false;
                const qreal alphaVal = match.captured(4).toDouble(&alphaOk);
                // Clamp before qRound(): converting an out-of-range double to int is
                // undefined behaviour and a long digit run parses as infinity.
                if (alphaOk && std::isfinite(alphaVal)) {
                    a = qRound(qBound<qreal>(0.0, alphaVal, 1.0) * 255.0);
                }
            }
            return QColor(r, g, b, a);
        }
    }

    if (s.contains(QLatin1Char('#'))) {
        const int hashPos = s.indexOf(QLatin1Char('#'));
        const QString hexPart = s.mid(hashPos + 1);
        QString cleanHex;
        cleanHex.reserve(hexPart.size());
        for (const QChar &ch : hexPart) {
            const ushort u = ch.unicode();
            if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F')) {
                cleanHex.append(ch);
            }
        }
        if (cleanHex.length() == 3 || cleanHex.length() == 4 || cleanHex.length() == 6 || cleanHex.length() == 8) {
            s = cleanHex;
        } else if (cleanHex.length() == 7 || cleanHex.length() > 8) {
            s = cleanHex.left(cleanHex.length() >= 8 ? 8 : 6);
        } else if (cleanHex.length() == 5) {
            s = cleanHex.left(4);
        } else {
            s = cleanHex;
        }
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

    QJsonObject controlPointListSchema;
    controlPointListSchema[QStringLiteral("type")] = QStringLiteral("array");
    controlPointListSchema[QStringLiteral("items")] = pointSchema;
    controlPointListSchema[QStringLiteral("maxItems")] = 256;

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
                                                              QStringLiteral("hatch"),
                                                              QStringLiteral("manga_lines")}}};
    opProps[QStringLiteral("id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("layer")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("brush")] = brushSchema;
    opProps[QStringLiteral("points")] = controlPointListSchema;
    opProps[QStringLiteral("polygon")] = controlPointListSchema;
    opProps[QStringLiteral("spine")] = controlPointListSchema;
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
    opProps[QStringLiteral("inner_radius")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), 0.0}};
    opProps[QStringLiteral("outer_radius")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("exclusiveMinimum"), 0.0}};
    opProps[QStringLiteral("density")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 4}};
    opProps[QStringLiteral("line_length_jitter")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), 0.0}};
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
    opProps[QStringLiteral("count")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                                     {QStringLiteral("minimum"), 1},
                                                     {QStringLiteral("maximum"), 200}};
    opProps[QStringLiteral("shape")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opItem[QStringLiteral("properties")] = opProps;
    opItem[QStringLiteral("required")] =
        QJsonArray{QStringLiteral("kind"), QStringLiteral("id"), QStringLiteral("layer"), QStringLiteral("brush")};

    QJsonObject rootProps;
    rootProps[QStringLiteral("schema_version")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
    rootProps[QStringLiteral("prompt")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("title")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("visual_critique")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("step_phase")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("current_step")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
    rootProps[QStringLiteral("total_steps")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
    rootProps[QStringLiteral("goal_reached")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    rootProps[QStringLiteral("completion_score")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}};
    rootProps[QStringLiteral("operations")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                          {QStringLiteral("items"), opItem},
                                                          {QStringLiteral("minItems"), 1},
                                                          {QStringLiteral("maxItems"), 160}};

    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    schema[QStringLiteral("properties")] = rootProps;
    schema[QStringLiteral("required")] = QJsonArray{QStringLiteral("schema_version"), QStringLiteral("operations")};

    return schema;
}

QString KisAiStrokeProgramCodec::buildJsonContractSection()
{
    return QStringLiteral(
        "=== STRICT JSON SYNTAX RULES (ZERO TOLERANCE) ===\n"
        "1. No trailing commas: never put a comma before a closing '}' or ']'.\n"
        "2. Double quotes only: all object keys and string values must use standard double quotes (\"), never single quotes or backticks.\n"
        "3. No comments: never include JavaScript comments (// or /* */) anywhere in the output.\n"
        "4. No Python literals: use true, false, and null (all lowercase) instead of True, False, None.\n"
        "5. No ellipses or placeholders: never write '...' or placeholder entries; emit complete geometry only.\n"
        "6. Valid numbers only: coordinates must be standard decimal numbers (e.g. 0.5, -0.1). Never output NaN, Infinity, or unit suffixes (no 'px', 'deg', '%').\n"
        "7. Complete JSON: budget your points and operations so your output completes fully before reaching token limits."
    );
}

QString KisAiStrokeProgramCodec::buildCoordinateSection(const QSize &canvasSize)
{
    const qreal aspect = canvasSize.height() > 0 ? qreal(canvasSize.width()) / canvasSize.height() : 1.0;
    return QStringLiteral(
        "=== COORDINATE SYSTEM & RESOLUTION ===\n"
        "Coordinates are normalized float numbers strictly in [0.0, 1.0]. (0.0, 0.0) is top-left, (1.0, 1.0) is bottom-right.\n"
        "Canvas size: %1x%2 (Aspect %3:1)."
    ).arg(canvasSize.width()).arg(canvasSize.height()).arg(QString::number(aspect, 'f', 2));
}

QString KisAiStrokeProgramCodec::buildLayerSemanticsSection()
{
    return QStringLiteral(
        "=== LAYER ARCHITECTURE & COMPOSITION (Back-to-Front) ===\n"
        "1. 'Background': Far distance, sky/environment washes, atmospheric depth (rendered behind all subjects; NOT clipped).\n"
        "2. 'Flats': Major subject silhouette color blocking (hair base, skin base, clothing base, foreground terrain). Base volumes for everything.\n"
        "3. 'Shading': Form shadows, ambient occlusion, depth crevices, cast shadows (rendered with Multiply blend and automatically clipped to Flats).\n"
        "4. 'Lineart': Crisp contours, facial details, hair strands, structural outlines (rendered with natural Catmull-Rom spline curves and tapering).\n"
        "5. 'Highlights': Specular glints, eye catchlights, rim lighting, atmospheric glow (rendered with Screen blend and clipped to Flats).\n"
        "6. 'FX': Particle accents, petals, embers, stars, sparkles, bloom, manga focus lines."
    );
}

QString KisAiStrokeProgramCodec::buildDrawingWorkflowSection()
{
    return QStringLiteral(
        "=== MASTER DRAWING WORKFLOW (MANDATORY) ===\n"
        "Silently design the complete image before emitting JSON: establish a focal point, horizon/gesture, "
        "foreground-midground-background depth, and a limited 5-8 color palette.\n"
        "Then emit painter-order geometry from large to small: (A) full-canvas underpainting, (B) large "
        "overlapping silhouettes, (C) form and cast shadows, (D) continuous structural contours, (E) focal "
        "micro-details, (F) restrained highlights/FX.\n"
        "Reuse the same landmark coordinates across Flats, Shading, and Lineart so boundaries register. Prefer one "
        "coherent 4-8 point path over many disconnected 2-point fragments.\n"
        "Concentrate the smallest marks and highest contrast at the focal point. Maintain clean silhouettes on Flats.\n"
        "Before returning JSON, silently audit: canvas coverage, recognizable silhouette, layer registration, "
        "depth ordering, tangent continuity, palette harmony, and required geometry for every operation. Fix "
        "failures in the final JSON."
    );
}

QString KisAiStrokeProgramCodec::buildArtisticGuidelinesSection()
{
    return QStringLiteral(
        "=== ARTISTIC & ANATOMICAL GUIDELINES ===\n"
        "- Contours & Splines: Smooth anchor points per curved feature (silhouette curves, hair flow, eyes, fabric folds).\n"
        "- Subject Anatomical Structure: Build focal features with registered multi-layer operations (Flats base -> Shading plane -> Lineart contour -> Highlights glint).\n"
        "- Shading Surfaces: Avoid harsh mechanical 'hatch' across smooth organic skin or flat skies; use soft 'fill' with 'watercolor' or 'brush' profile.\n"
        "- Natural Foliage & Canopies: Group foliage and landscape features into undulating organic masses with natural curved contours.\n"
        "- Line & Color Harmony: Avoid harsh pure black (#000000) for lineart; use deep dark harmonious tones (e.g. #1a162b, #1c2438, #2b1b17).\n"
        "  Pair warm key lights with cool shadows, or cool ambient light with warm bounce light."
    );
}

QString KisAiStrokeProgramCodec::buildOperationKindsSection()
{
    return QStringLiteral(
        "=== OPERATION KINDS ===\n"
        "- 'gradient_fill': Full/partial sky & background washes. Polygon [ [x, y], ... ], colors [ '#hex', ... ], "
        "angle_deg (0=horizontal, 90=vertical), is_radial (true/false), center [cx, cy], radius.\n"
        "- 'fill': Color masses, silhouettes, hair/clothing base, shadow blocks. Polygon [ [x, y], ... ], brush { "
        "'profile': 'watercolor'/'brush'/'marker', 'color': '#hex' }, style ('wash'/'contour'/'directional').\n"
        "- 'hatch': Technical screentone hatching. Polygon [ [x, y], ... ], angle_deg (0-180), spacing "
        "(0.005-0.03), cross_hatch (true/false), brush { 'profile': 'pencil'/'gpen', 'color': '#hex' }.\n"
        "- 'ribbon': Tapered organic strokes (tree limbs, hair clumps, cloth folds). Spine [ [x, y], ... ], "
        "width_start, width_mid, width_end (0.005-0.05).\n"
        "- 'path': Expressive linework, contours, facial features. Points [ [x, y, pressure], ... ] where pressure "
        "is 0.1-1.0. brush { 'profile': 'gpen'/'pencil'/'airbrush'/'watercolor'/'marker'/'crayon'/'neon'/'splatter', 'color': '#hex', 'size': "
        "0.002-0.01 }.\n"
        "- 'particles': Atmospheric particles. Bounds [x1, y1, x2, y2], count (10-50), shape "
        "('petal'/'sparkle'/'star'/'dot'), brush { 'color': '#hex' }.\n"
        "- 'manga_lines': Radial speed/focus lines toward a center. center [cx, cy], inner_radius (0.05-0.3), outer_radius (0.5-1.0), density (16-80), brush { 'profile': 'gpen', 'color': '#hex', 'size': 0.002 }."
    );
}

QString KisAiStrokeProgramCodec::buildOutputSchemaExampleSection()
{
    // A0: Neutral abstract geometry schema example completely free of thematic motifs (no trees, stars, or night skies)
    return QStringLiteral(
        "=== OUTPUT SCHEMA EXAMPLE ===\n"
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"prompt\": \"user illustration description\",\n"
        "  \"title\": \"Artwork Title\",\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"gradient_fill\",\n"
        "      \"id\": \"bg_wash\",\n"
        "      \"layer\": \"Background\",\n"
        "      \"polygon\": [[0.0,0.0],[1.0,0.0],[1.0,1.0],[0.0,1.0]],\n"
        "      \"colors\": [\"#1e293b\", \"#0f172a\"],\n"
        "      \"angle_deg\": 90,\n"
        "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"#1e293b\", \"size\": 0.05, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"subject_silhouette\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"polygon\": [[0.2,0.2],[0.8,0.2],[0.75,0.85],[0.25,0.85]],\n"
        "      \"brush\": {\"profile\": \"brush\", \"color\": \"#64748b\", \"size\": 0.04, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"core_shadow\",\n"
        "      \"layer\": \"Shading\",\n"
        "      \"polygon\": [[0.45,0.3],[0.75,0.3],[0.7,0.8],[0.4,0.8]],\n"
        "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"#334155\", \"size\": 0.03, \"is_eraser\": false},\n"
        "      \"style\": \"wash\"\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"primary_contour\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.2,0.2,0.8],[0.25,0.5,0.9],[0.25,0.85,0.7]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#0f172a\", \"size\": 0.004, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"specular_point\",\n"
        "      \"layer\": \"Highlights\",\n"
        "      \"points\": [[0.35,0.3,0.9],[0.36,0.31,0.3]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#ffffff\", \"size\": 0.003, \"is_eraser\": false}\n"
        "    }\n"
        "  ]\n"
        "}"
    );
}

QString KisAiStrokeProgramCodec::buildSystemPrompt(const QSize &canvasSize,
                                                   const QString &prompt,
                                                   const QString &customInstructions,
                                                   int artStyle)
{
    auto spec = KisAiPromptAnalyzer::analyze(prompt, canvasSize);
    if (artStyle > 0 && artStyle <= 5) {
        spec.style = static_cast<KisAiPromptAnalyzer::ArtStyle>(artStyle);
    }
    const QString artDirection = KisAiPromptAnalyzer::generateArtDirection(spec, canvasSize);

    // A0: User Request is placed at the absolute top with strict priority rule
    QString systemText;
    systemText += QStringLiteral(
        "=== USER REQUEST (ABSOLUTE HIGHEST PRIORITY) ===\n"
        "\"%1\"\n"
        "PRIORITY RULE: If any artistic guideline, example, or default suggestion below conflicts with the USER REQUEST, you MUST follow the USER REQUEST. Every subject, character, mood, color palette, and detail MUST be derived strictly from the USER REQUEST.\n\n"
    ).arg(prompt.trimmed());

    systemText += QStringLiteral(
        "You are an autonomous AI master digital painter directing layer-by-layer drawing plans for Krita.\n"
        "Generate a rich, cohesive, painterly illustration by specifying coordinate-directed strokes in StrokeProgram JSON format.\n"
        "Output ONLY valid, parseable RFC 8259 JSON starting with '{' and ending with '}'.\n"
        "Do NOT include markdown explanations, thought text, or conversational chatter outside the JSON.\n\n"
    );

    systemText += buildJsonContractSection() + QStringLiteral("\n\n");
    systemText += buildCoordinateSection(canvasSize) + QStringLiteral("\n\n");
    systemText += buildLayerSemanticsSection() + QStringLiteral("\n\n");
    systemText += buildDrawingWorkflowSection() + QStringLiteral("\n\n");
    systemText += buildArtisticGuidelinesSection() + QStringLiteral("\n\n");
    systemText += buildOperationKindsSection() + QStringLiteral("\n\n");
    systemText += artDirection + QStringLiteral("\n\n");
    systemText += buildOutputSchemaExampleSection();

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
                                                                 const QString &customInstructions,
                                                                 bool enableStreaming,
                                                                 bool enforceJsonFormat,
                                                                 qreal temperature,
                                                                 qreal topP,
                                                                 int maxTokensOverride,
                                                                 int artStyle)
{
    const bool reasoning = isReasoningModel(model);
    const QString systemText = buildSystemPrompt(canvasSize, prompt, customInstructions, artStyle);

    QJsonObject userObj;
    userObj[QStringLiteral("prompt")] = prompt;
    userObj[QStringLiteral("canvas_width")] = canvasSize.width();
    userObj[QStringLiteral("canvas_height")] = canvasSize.height();
    const int geometryBudget = qBound(20, strokeBudget, 2000);
    const int operationTarget = qBound(16, geometryBudget / 15, 60);
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
        "contain valid geometry and a unique semantic id. Output strictly complete, valid RFC 8259 JSON without markdown fences.");

    const QString userText = QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact));

    QJsonArray messages;
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), userText}});

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.trimmed();
    payload[QStringLiteral("messages")] = messages;
    payload[QStringLiteral("seed")] = static_cast<int>(stableSeed(prompt.simplified()));

    if (enableStreaming) {
        payload[QStringLiteral("stream")] = true;
    }

    // Structured output via json_object or json_schema (optional, only when explicitly enforced or native OpenAI)
    if (enforceJsonFormat) {
        if (supportsJsonSchema(model)) {
            QJsonObject schemaObj;
            schemaObj[QStringLiteral("name")] = QStringLiteral("stroke_program");
            schemaObj[QStringLiteral("strict")] = true;
            schemaObj[QStringLiteral("schema")] = strokeProgramJsonSchema();

            QJsonObject responseFormat;
            responseFormat[QStringLiteral("type")] = QStringLiteral("json_schema");
            responseFormat[QStringLiteral("json_schema")] = schemaObj;
            payload[QStringLiteral("response_format")] = responseFormat;
        } else {
            QJsonObject responseFormat;
            responseFormat[QStringLiteral("type")] = QStringLiteral("json_object");
            payload[QStringLiteral("response_format")] = responseFormat;
        }
    }

    int calculatedTokens = maxTokensOverride > 0
        ? maxTokensOverride
        : qBound(4096, operationTarget * 160 + (reasoning ? 12288 : 2560), reasoning ? 32768 : 16384);

    if (reasoning) {
        payload[QStringLiteral("max_completion_tokens")] = calculatedTokens;
        if (!reasoningEffort.isEmpty() && reasoningEffort.toLower() != QLatin1String("none")) {
            payload[QStringLiteral("reasoning_effort")] = reasoningEffort.toLower();
        }
    } else {
        payload[QStringLiteral("max_tokens")] = calculatedTokens;
        payload[QStringLiteral("temperature")] = qBound<qreal>(0.0, temperature, 2.0);
        if (topP > 0.0 && topP < 1.0) {
            payload[QStringLiteral("top_p")] = qBound<qreal>(0.01, topP, 1.0);
        }
    }

    return payload;
}

QString KisAiStrokeProgramCodec::repairJsonSyntax(const QString &jsonText, KisAiJsonDiagnostic *diagnostic)
{
    QString text = jsonText.trimmed();
    if (text.isEmpty()) {
        return text;
    }

    if (diagnostic) {
        diagnostic->appliedRepairs.clear();
    }

    // 0. Remove BOM and invisible/zero-width Unicode characters
    text.remove(QChar(0xFEFF)); // UTF-8 BOM
    text.remove(QChar(0x200B)); // Zero-width space
    text.remove(QChar(0x200C)); // Zero-width non-joiner
    text.remove(QChar(0x200D)); // Zero-width joiner
    text.remove(QChar(0x2060)); // Word joiner
    text.replace(QChar(0x00A0), QLatin1Char(' ')); // Non-breaking space

    // Normalize markdown codeblocks / backticks outside fences
    static const QRegularExpression tripleBacktick(QStringLiteral("```(?:json)?"));
    text.remove(tripleBacktick);
    text.replace(QLatin1Char('`'), QLatin1Char('"'));

    // 1. Normalize smart quotes to standard quotes before string extraction
    text.replace(QChar(0x201C), QLatin1Char('"')); // “
    text.replace(QChar(0x201D), QLatin1Char('"')); // ”
    text.replace(QChar(0x2018), QLatin1Char('\'')); // ‘
    text.replace(QChar(0x2019), QLatin1Char('\'')); // ’

    // 2. Single quotes to double quotes when outside double-quoted strings
    {
        QString quoteFixed;
        quoteFixed.reserve(text.size());
        bool inDouble = false;
        bool inSingle = false;
        bool esc = false;
        for (int i = 0; i < text.size(); ++i) {
            const QChar ch = text.at(i);
            if (esc) {
                esc = false;
                quoteFixed.append(ch);
                continue;
            }
            if (ch == QLatin1Char('\\')) {
                esc = true;
                quoteFixed.append(ch);
                continue;
            }
            if (ch == QLatin1Char('"') && !inSingle) {
                inDouble = !inDouble;
                quoteFixed.append(ch);
                continue;
            }
            if (ch == QLatin1Char('\'') && !inDouble) {
                inSingle = !inSingle;
                quoteFixed.append(QLatin1Char('"'));
                continue;
            }
            quoteFixed.append(ch);
        }
        text = quoteFixed;
    }

    // 3. Token Masking: Extract and preserve all string literals so regex repairs don't mutate them
    QVector<QString> maskedStrings;
    maskedStrings.reserve(128);

    {
        QString masked;
        masked.reserve(text.size());
        bool inStr = false;
        bool esc = false;
        QString currentStr;

        for (int i = 0; i < text.size(); ++i) {
            const QChar ch = text.at(i);

            if (inStr) {
                if (esc) {
                    esc = false;
                    currentStr.append(ch);
                    continue;
                }
                if (ch == QLatin1Char('\\')) {
                    esc = true;
                    currentStr.append(ch);
                    continue;
                }
                if (ch == QLatin1Char('"')) {
                    inStr = false;
                    // Sanitize unescaped control characters inside the literal
                    QString cleanLiteral;
                    cleanLiteral.reserve(currentStr.size() + 16);
                    for (int cIdx = 0; cIdx < currentStr.size(); ++cIdx) {
                        const QChar sc = currentStr.at(cIdx);
                        if (sc == QLatin1Char('\n')) {
                            cleanLiteral.append(QStringLiteral("\\n"));
                        } else if (sc == QLatin1Char('\r')) {
                            cleanLiteral.append(QStringLiteral("\\r"));
                        } else if (sc == QLatin1Char('\t')) {
                            cleanLiteral.append(QStringLiteral("\\t"));
                        } else if (sc.unicode() < 0x20) {
                            // Strip ASCII control bytes
                        } else {
                            cleanLiteral.append(sc);
                        }
                    }
                    const int maskIndex = maskedStrings.size();
                    maskedStrings.append(cleanLiteral);
                    masked.append(stringMaskPlaceholder(maskIndex));
                    currentStr.clear();
                    continue;
                }
                currentStr.append(ch);
            } else {
                if (ch == QLatin1Char('"')) {
                    inStr = true;
                    esc = false;
                    currentStr.clear();
                    continue;
                }
                masked.append(ch);
            }
        }

        // If a string was left open at EOF, close it safely
        if (inStr) {
            QString cleanLiteral;
            cleanLiteral.reserve(currentStr.size() + 16);
            for (int cIdx = 0; cIdx < currentStr.size(); ++cIdx) {
                const QChar sc = currentStr.at(cIdx);
                if (sc == QLatin1Char('\n')) {
                    cleanLiteral.append(QStringLiteral("\\n"));
                } else if (sc == QLatin1Char('\r')) {
                    cleanLiteral.append(QStringLiteral("\\r"));
                } else if (sc == QLatin1Char('\t')) {
                    cleanLiteral.append(QStringLiteral("\\t"));
                } else if (sc.unicode() < 0x20) {
                    // Strip ASCII control bytes
                } else {
                    cleanLiteral.append(sc);
                }
            }
            const int maskIndex = maskedStrings.size();
            maskedStrings.append(cleanLiteral);
            masked.append(stringMaskPlaceholder(maskIndex));
        }

        text = masked;
    }

    if (diagnostic && !maskedStrings.isEmpty()) {
        diagnostic->appliedRepairs.append(QStringLiteral("TokenMasking(%1 strings preserved)").arg(maskedStrings.size()));
    }

    // 4. Full-width punctuation to standard half-width symbols (safe: strings are masked!)
    text.replace(QChar(0xFF5B), QLatin1Char('{')); // ｛
    text.replace(QChar(0xFF5D), QLatin1Char('}')); // ｝
    text.replace(QChar(0x3014), QLatin1Char('[')); // 〔
    text.replace(QChar(0x3015), QLatin1Char(']')); // 〕
    text.replace(QChar(0x3010), QLatin1Char('[')); // 【
    text.replace(QChar(0x3011), QLatin1Char(']')); // 】
    text.replace(QChar(0xFF1A), QLatin1Char(':')); // ：
    text.replace(QChar(0xFF0C), QLatin1Char(',')); // ，
    text.replace(QChar(0x3001), QLatin1Char(',')); // 、
    text.replace(QChar(0x3000), QLatin1Char(' ')); // 全角スペース

    // 5. Remove comments outside strings (// ... and /* ... */)
    {
        QString noComments;
        noComments.reserve(text.size());
        for (int i = 0; i < text.size(); ++i) {
            const QChar ch = text.at(i);
            if (ch == QLatin1Char('/') && i + 1 < text.size()) {
                if (text.at(i + 1) == QLatin1Char('/')) {
                    const int nextNl = text.indexOf(QLatin1Char('\n'), i + 2);
                    if (nextNl < 0) {
                        break;
                    }
                    i = nextNl - 1;
                    continue;
                } else if (text.at(i + 1) == QLatin1Char('*')) {
                    const int nextEnd = text.indexOf(QStringLiteral("*/"), i + 2);
                    if (nextEnd < 0) {
                        break;
                    }
                    i = nextEnd + 1;
                    continue;
                }
            }
            noComments.append(ch);
        }
        text = noComments;
    }

    // 6. Remove ellipsis / placeholder tokens outside strings
    static const QRegularExpression ellipsisRe(QStringLiteral(R"(,?\s*\.\.\.\s*(?=[\}\]]))"));
    text.replace(ellipsisRe, QStringLiteral(""));
    static const QRegularExpression strayEllipsis(QStringLiteral(R"(\.\.\.)"));
    text.replace(strayEllipsis, QStringLiteral(""));

    // 7. Convert Python literals to JSON equivalents
    static const QRegularExpression pyTrue(QStringLiteral(R"((?<=[,\:\[\s])True(?=[,\:\]\}\s]))"));
    text.replace(pyTrue, QStringLiteral("true"));
    static const QRegularExpression pyFalse(QStringLiteral(R"((?<=[,\:\[\s])False(?=[,\:\]\}\s]))"));
    text.replace(pyFalse, QStringLiteral("false"));
    static const QRegularExpression pyNone(QStringLiteral(R"((?<=[,\:\[\s])None(?=[,\:\]\}\s]))"));
    text.replace(pyNone, QStringLiteral("null"));

    // 8. Stray identifiers before quotes or opening structural braces
    static const QRegularExpression strayTokenBeforeQuote(
        QStringLiteral(R"((?<=[,\{\[\s])([a-zA-Z_]{1,3})\s+(?="))"));
    text.replace(strayTokenBeforeQuote, QStringLiteral(""));

    static const QRegularExpression strayTokenBeforeOpen(
        QStringLiteral(R"((?<=[,\{\[\s])([a-zA-Z_]{1,3})\s+(?=[\{\[]))"));
    text.replace(strayTokenBeforeOpen, QStringLiteral(""));

    // 9. Quote unquoted object keys (supports alphanumeric, underscores, and hyphens)
    static const QRegularExpression unquotedKey(
        QStringLiteral(R"((?<=[,\{\s])([a-zA-Z_][a-zA-Z0-9_\-]*)\s*:)"));
    text.replace(unquotedKey, QStringLiteral("\"\\1\":"));

    // 10. Non-standard numbers, units, and corruptions
    // Leading plus before digits or period: +5 -> 5, +.5 -> .5
    static const QRegularExpression leadingPlusNum(QStringLiteral(R"((?<=[,\:\[\s])\+(?=\.?\d))"));
    text.replace(leadingPlusNum, QStringLiteral(""));

    // Leading period: -.5 -> -0.5, .5 -> 0.5
    static const QRegularExpression leadingDotNegative(QStringLiteral(R"((?<=[,\:\[\s])-(\.\d+))"));
    text.replace(leadingDotNegative, QStringLiteral("-0\\1"));
    static const QRegularExpression leadingDot(QStringLiteral(R"((?<=[,\:\[\s])(\.\d+))"));
    text.replace(leadingDot, QStringLiteral("0\\1"));

    // Trailing period: 5. -> 5.0
    static const QRegularExpression trailingDot(QStringLiteral(R"((?<=[,\:\[\s])(-?\d+\.)(?=[,\:\]\}\s]))"));
    text.replace(trailingDot, QStringLiteral("\\10"));

    // Strip unit suffixes (px, deg, %) from numbers
    static const QRegularExpression numUnitPx(
        QStringLiteral(R"((?<=[,\:\[\s])-?(\d+(?:\.\d+)?)\s*px(?=[,\:\]\}\s]))"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(numUnitPx, QStringLiteral("\\1"));

    static const QRegularExpression numUnitDeg(
        QStringLiteral(R"((?<=[,\:\[\s])-?(\d+(?:\.\d+)?)\s*deg(?=[,\:\]\}\s]))"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(numUnitDeg, QStringLiteral("\\1"));

    // NaN / Infinity
    static const QRegularExpression nanRe(QStringLiteral(R"((?<=[,\:\[\s])NaN(?=[,\:\]\}\s]))"), QRegularExpression::CaseInsensitiveOption);
    text.replace(nanRe, QStringLiteral("0.0"));

    static const QRegularExpression infRe(QStringLiteral(R"((?<=[,\:\[\s])\+?Infinity(?=[,\:\]\}\s]))"), QRegularExpression::CaseInsensitiveOption);
    text.replace(infRe, QStringLiteral("1.0"));

    static const QRegularExpression negInfRe(QStringLiteral(R"((?<=[,\:\[\s])-Infinity(?=[,\:\]\}\s]))"), QRegularExpression::CaseInsensitiveOption);
    text.replace(negInfRe, QStringLiteral("-1.0"));

    // Corrupted / noisy numbers in coordinates or values (e.g. 0t.05, 0.t3, t0, 1t, 0.08t)
    static const QRegularExpression numLetterBeforeDot(
        QStringLiteral(R"((?<=[,\:\[\s])-?(\d+)[a-zA-Z]+(\.\d+))"));
    text.replace(numLetterBeforeDot, QStringLiteral("\\1\\2"));

    static const QRegularExpression numLetterAfterDot(
        QStringLiteral(R"((?<=[,\:\[\s])-?(\d+\.)[a-zA-Z]+(\d+))"));
    text.replace(numLetterAfterDot, QStringLiteral("\\1\\2"));

    static const QRegularExpression numLetterPrefix(
        QStringLiteral(R"((?<=[,\:\[\s])-?[a-zA-Z]+(\d+(?:\.\d+)?)(?=[,\:\]\}\s]))"));
    text.replace(numLetterPrefix, QStringLiteral("\\1"));

    static const QRegularExpression numLetterSuffix(
        QStringLiteral(R"((?<=[,\:\[\s])-?(\d+(?:\.\d+)?)[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(numLetterSuffix, QStringLiteral("\\1"));

    // 11. Corrupted booleans and null (e.g. falset -> false, truet -> true, nullt -> null)
    static const QRegularExpression boolFalse(
        QStringLiteral(R"((?<=[,\:\[\s])false[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(boolFalse, QStringLiteral("false"));

    static const QRegularExpression boolTrue(
        QStringLiteral(R"((?<=[,\:\[\s])true[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(boolTrue, QStringLiteral("true"));

    static const QRegularExpression valNull(
        QStringLiteral(R"((?<=[,\:\[\s])null[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(valNull, QStringLiteral("null"));

    // 12. Fix missing commas between object properties
    static const QRegularExpression missingCommaProp(
        QStringLiteral(R"re((?<="|\d|true|false|null|\}|\])\s+(?="(?:[a-zA-Z_][a-zA-Z0-9_\-]*|__AI_STR_MASK_\d+__)"\s*:))re"));
    text.replace(missingCommaProp, QStringLiteral(", "));

    // 13. Fix missing commas between numbers in coordinate arrays (e.g. [0.1 0.2 0.8] -> [0.1, 0.2, 0.8])
    static const QRegularExpression missingCommaNum(
        QStringLiteral(R"((?<=\d)\s+(?=-?\d+\.?\d*))"));
    text.replace(missingCommaNum, QStringLiteral(", "));

    // 14. Fix missing commas between structural elements (} {, ] [)
    static const QRegularExpression missingCommaBraces(QStringLiteral(R"(\}\s*\{)"));
    text.replace(missingCommaBraces, QStringLiteral("}, {"));

    static const QRegularExpression missingCommaBrackets(QStringLiteral(R"(\]\s*\[)"));
    text.replace(missingCommaBrackets, QStringLiteral("], ["));

    // 15. Consecutive and leading comma cleanup
    static const QRegularExpression consecutiveCommas(QStringLiteral(R"(,\s*,+)"));
    text.replace(consecutiveCommas, QStringLiteral(", "));

    static const QRegularExpression leadingComma(QStringLiteral(R"((?<=[\[\{])\s*,+)"));
    text.replace(leadingComma, QStringLiteral(""));

    // 16. Remove trailing commas before } or ]
    static const QRegularExpression trailingComma(QStringLiteral(R"(,\s*([\}\]]))"));
    text.replace(trailingComma, QStringLiteral("\\1"));

    // 17. Restore preserved string literals in a single left-to-right pass.
    // Replacing one placeholder at a time rescans the whole document per literal
    // (quadratic in the number of literals) and can stall on a large response,
    // so the masked document is rewritten once.
    if (!maskedStrings.isEmpty()) {
        static const QRegularExpression maskPlaceholder(QStringLiteral("\"__AI_STR_MASK_(\\d+)__\""));
        QString restored;
        restored.reserve(text.size());
        int lastIndex = 0;
        auto matches = maskPlaceholder.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const int start = static_cast<int>(match.capturedStart());
            const int end = static_cast<int>(match.capturedEnd());
            restored.append(text.mid(lastIndex, start - lastIndex));
            const int maskIndex = match.captured(1).toInt();
            restored.append(QLatin1Char('"'));
            if (maskIndex >= 0 && maskIndex < maskedStrings.size()) {
                restored.append(maskedStrings.at(maskIndex));
            }
            restored.append(QLatin1Char('"'));
            lastIndex = end;
        }
        restored.append(text.mid(lastIndex));
        text = restored;
    }

    return text;
}

QString KisAiStrokeProgramCodec::repairTruncatedJson(const QString &jsonText, KisAiJsonDiagnostic *diagnostic)
{
    QString text = jsonText.trimmed();
    if (text.isEmpty())
        return text;

    QJsonParseError testErr;
    QJsonDocument::fromJson(text.toUtf8(), &testErr);
    if (testErr.error == QJsonParseError::NoError) {
        return text;
    }

    // Run syntax repair first
    text = repairJsonSyntax(text, diagnostic);
    QJsonDocument::fromJson(text.toUtf8(), &testErr);
    if (testErr.error == QJsonParseError::NoError) {
        return text;
    }

    // Check if there is an operations or strokes array
    static const QStringList arrayKeys = {
        QStringLiteral("\"operations\""),
        QStringLiteral("\"strokes\""),
        QStringLiteral("\"ops\""),
        QStringLiteral("\"layers\""),
        QStringLiteral("\"data\"")
    };

    int targetIdx = -1;
    for (const auto &k : arrayKeys) {
        targetIdx = text.indexOf(k, 0, Qt::CaseInsensitive);
        if (targetIdx >= 0) break;
    }

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
                    if (depth == 0) {
                        break;
                    }
                }
            }

            if (lastCloseBrace > bracketIdx) {
                QString repaired = text.left(lastCloseBrace + 1);
                // Dynamically close whatever structural containers are open in repaired
                QVector<QChar> rbStack;
                bool rbInStr = false;
                bool rbEsc = false;
                for (int si = 0; si < repaired.length(); ++si) {
                    const QChar sc = repaired.at(si);
                    if (rbEsc) { rbEsc = false; continue; }
                    if (sc == QLatin1Char('\\')) { rbEsc = true; continue; }
                    if (sc == QLatin1Char('"')) { rbInStr = !rbInStr; continue; }
                    if (rbInStr) continue;
                    if (sc == QLatin1Char('{') || sc == QLatin1Char('[')) {
                        rbStack.append(sc);
                    } else if (sc == QLatin1Char('}') || sc == QLatin1Char(']')) {
                        if (!rbStack.isEmpty()) {
                            const QChar expected = (sc == QLatin1Char('}')) ? QLatin1Char('{') : QLatin1Char('[');
                            if (rbStack.last() == expected) {
                                rbStack.removeLast();
                            }
                        }
                    }
                }
                while (!rbStack.isEmpty()) {
                    const QChar open = rbStack.takeLast();
                    if (open == QLatin1Char('{')) repaired.append(QLatin1Char('}'));
                    else if (open == QLatin1Char('[')) repaired.append(QLatin1Char(']'));
                }
                repaired = repairJsonSyntax(repaired, diagnostic);
                QJsonParseError repErr;
                const QJsonDocument testDoc = QJsonDocument::fromJson(repaired.toUtf8(), &repErr);
                if (!testDoc.isNull() && testDoc.isObject()) {
                    if (diagnostic) diagnostic->appliedRepairs.append(QStringLiteral("ArrayRollbackClosure"));
                    return repaired;
                }
            }
        }
    }

    // General stack-based closure
    QString result = text;
    bool inString = false;
    bool escape = false;
    for (int i = 0; i < result.length(); ++i) {
        const QChar ch = result.at(i);
        if (escape) { escape = false; continue; }
        if (ch == QLatin1Char('\\')) { escape = true; continue; }
        if (ch == QLatin1Char('"')) { inString = !inString; continue; }
    }

    if (inString) {
        result.append(QLatin1Char('"'));
    }

    result = result.trimmed();
    while (result.endsWith(QLatin1Char(',')) || result.endsWith(QLatin1Char(':'))) {
        result.chop(1);
        result = result.trimmed();
    }

    if (result.endsWith(QLatin1Char('"'))) {
        const int prevQuote = result.lastIndexOf(QLatin1Char('"'), result.length() - 2);
        if (prevQuote >= 0) {
            const QString beforeKey = result.left(prevQuote).trimmed();
            if (beforeKey.endsWith(QLatin1Char(',')) || beforeKey.endsWith(QLatin1Char('{'))) {
                result = beforeKey;
                while (result.endsWith(QLatin1Char(','))) {
                    result.chop(1);
                    result = result.trimmed();
                }
            }
        }
    }

    // Recompute stack on trimmed result to close exactly what's open
    QVector<QChar> stack;
    inString = false;
    escape = false;
    for (int i = 0; i < result.length(); ++i) {
        const QChar ch = result.at(i);
        if (escape) { escape = false; continue; }
        if (ch == QLatin1Char('\\')) { escape = true; continue; }
        if (ch == QLatin1Char('"')) { inString = !inString; continue; }
        if (inString) continue;
        if (ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
            stack.append(ch);
        } else if (ch == QLatin1Char('}') || ch == QLatin1Char(']')) {
            if (!stack.isEmpty()) {
                const QChar expected = (ch == QLatin1Char('}')) ? QLatin1Char('{') : QLatin1Char('[');
                if (stack.last() == expected) {
                    stack.removeLast();
                }
            }
        }
    }

    while (!stack.isEmpty()) {
        const QChar open = stack.takeLast();
        if (open == QLatin1Char('{')) {
            result.append(QLatin1Char('}'));
        } else if (open == QLatin1Char('[')) {
            result.append(QLatin1Char(']'));
        }
    }

    result = repairJsonSyntax(result, diagnostic);
    if (diagnostic) diagnostic->appliedRepairs.append(QStringLiteral("StackBasedClosure"));
    return result;
}

QString KisAiStrokeProgramCodec::sanitizeAndExtractJson(const QString &rawText, KisAiJsonDiagnostic *diagnostic)
{
    QString text = rawText.trimmed();

    // 1. Remove <think> ... </think> or thinking tags (including unclosed tags if truncated)
    static const QRegularExpression thinkRe(QStringLiteral("(?s)<think>.*?(?:</think>|$)"));
    text.remove(thinkRe);
    static const QRegularExpression thoughtRe(QStringLiteral("(?s)<thought>.*?(?:</thought>|$)"));
    text.remove(thoughtRe);
    static const QRegularExpression detailsRe(QStringLiteral("(?s)<details>.*?(?:</details>|$)"));
    text.remove(detailsRe);

    // 2. Extract ```json ... ``` codeblocks and score candidates
    static const QRegularExpression codeBlockRe(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)(?:```|$)"));
    auto it = codeBlockRe.globalMatch(text);
    QString bestBlock;
    int bestScore = -1;

    while (it.hasNext()) {
        const auto match = it.next();
        const QString block = match.captured(1).trimmed();
        if (block.contains(QLatin1Char('{'))) {
            int score = 0;
            if (block.contains(QLatin1String("operations"), Qt::CaseInsensitive)) score += 10;
            if (block.contains(QLatin1String("strokes"), Qt::CaseInsensitive)) score += 8;
            if (block.contains(QLatin1String("schema_version"), Qt::CaseInsensitive)) score += 5;
            if (block.contains(QLatin1String("kind"), Qt::CaseInsensitive)) score += 3;
            if (score > bestScore) {
                bestScore = score;
                bestBlock = block;
            }
        }
    }

    if (!bestBlock.isEmpty()) {
        text = bestBlock;
        if (diagnostic) diagnostic->appliedRepairs.append(QStringLiteral("BestCodeBlockExtracted"));
    }

    // 3. Find outermost { ... }
    const int firstBrace = text.indexOf(QLatin1Char('{'));
    if (firstBrace >= 0) {
        const int lastBrace = text.lastIndexOf(QLatin1Char('}'));
        if (lastBrace > firstBrace) {
            const QString candidate = text.mid(firstBrace, lastBrace - firstBrace + 1).trimmed();
            QJsonParseError cErr;
            QJsonDocument::fromJson(candidate.toUtf8(), &cErr);
            if (cErr.error == QJsonParseError::NoError) {
                text = candidate;
            } else {
                const QString repairedCand = repairJsonSyntax(candidate);
                QJsonParseError rErr;
                const QJsonDocument rDoc = QJsonDocument::fromJson(repairedCand.toUtf8(), &rErr);
                if (rErr.error == QJsonParseError::NoError && !rDoc.isNull()) {
                    text = candidate;
                } else {
                    text = text.mid(firstBrace).trimmed();
                }
            }
        } else {
            text = text.mid(firstBrace).trimmed();
        }
    }

    // 4. Run syntax repair first
    text = repairJsonSyntax(text, diagnostic);

    // 5. If invalid or truncated, attempt recovery
    QJsonParseError pErr;
    QJsonDocument::fromJson(text.toUtf8(), &pErr);
    if (pErr.error != QJsonParseError::NoError) {
        text = repairTruncatedJson(text, diagnostic);
    }

    return text;
}

bool KisAiStrokeProgramCodec::parseSseStreamChunk(
    const QByteArray &chunk,
    QByteArray *unprocessedBuffer,
    QString *accumulatedContent,
    bool *isDone)
{
    if (isDone) {
        *isDone = false;
    }
    if (!unprocessedBuffer || !accumulatedContent) {
        return false;
    }

    // A partial SSE line is bounded by the response size limit; refuse to grow
    // the carry-over buffer without bound if a peer never terminates a line.
    constexpr int MAX_SSE_LINE_BYTES = 8 * 1024 * 1024;
    if (chunk.size() > MAX_SSE_LINE_BYTES - unprocessedBuffer->size()) {
        unprocessedBuffer->clear();
        return false;
    }

    unprocessedBuffer->append(chunk);
    bool anyDeltaExtracted = false;

    while (true) {
        const int newlineIdx = unprocessedBuffer->indexOf('\n');
        if (newlineIdx < 0) {
            break;
        }

        QByteArray line = unprocessedBuffer->left(newlineIdx).trimmed();
        unprocessedBuffer->remove(0, newlineIdx + 1);

        if (line.isEmpty() || line.startsWith(':')) {
            // SSE keep-alive or comment
            continue;
        }

        if (line.startsWith("data:")) {
            const QByteArray dataPayload = line.mid(5).trimmed();
            if (dataPayload == "[DONE]") {
                if (isDone) {
                    *isDone = true;
                }
                continue;
            }

            QJsonParseError parseErr;
            const QJsonDocument doc = QJsonDocument::fromJson(dataPayload, &parseErr);
            if (!doc.isObject()) {
                continue;
            }

            const QJsonObject root = doc.object();
            if (root.contains(QStringLiteral("error"))) {
                const QJsonObject errObj = root.value(QStringLiteral("error")).toObject();
                const QString errMsg = errObj.value(QStringLiteral("message")).toString();
                if (!errMsg.isEmpty() && accumulatedContent->isEmpty()) {
                    accumulatedContent->append(QStringLiteral("ERROR: ") + errMsg);
                }
            }

            const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
            if (!choices.isEmpty()) {
                const QJsonObject choice0 = choices.at(0).toObject();
                const QJsonObject delta = choice0.value(QStringLiteral("delta")).toObject();
                if (delta.contains(QStringLiteral("content"))) {
                    const QString deltaContent = delta.value(QStringLiteral("content")).toString();
                    if (!deltaContent.isEmpty()) {
                        accumulatedContent->append(deltaContent);
                        anyDeltaExtracted = true;
                    }
                } else if (choice0.contains(QStringLiteral("text"))) {
                    const QString textChunk = choice0.value(QStringLiteral("text")).toString();
                    if (!textChunk.isEmpty()) {
                        accumulatedContent->append(textChunk);
                        anyDeltaExtracted = true;
                    }
                }
            }
        }
    }

    return anyDeltaExtracted;
}

bool KisAiStrokeProgramCodec::extractOperationsFromRawText(const QString &rawText,
                                                           KisAiStrokeProgram *outProgram,
                                                           QString *errorMessage,
                                                           KisAiJsonDiagnostic *diagnostic)
{
    Q_UNUSED(diagnostic);
    if (!outProgram) {
        return false;
    }

    outProgram->operations.clear();

    // Try to extract schema_version, title, prompt if present
    static const QRegularExpression schemaVerRe(QStringLiteral(R"("schema_version"\s*:\s*(\d+))"));
    const auto svMatch = schemaVerRe.match(rawText);
    if (svMatch.hasMatch()) {
        outProgram->schemaVersion = svMatch.captured(1).toInt();
    } else {
        outProgram->schemaVersion = 2;
    }

    static const QRegularExpression titleRe(QStringLiteral(R"re("[a-z]*title[a-z]*"\s*:\s*"([^"\\]*(?:\\.[^"\\]*)*)")re"));
    const auto tMatch = titleRe.match(rawText);
    if (tMatch.hasMatch()) {
        outProgram->title = tMatch.captured(1);
    } else {
        outProgram->title = QStringLiteral("AI Artwork");
    }

    static const QRegularExpression promptRe(QStringLiteral(R"re("prompt"\s*:\s*"([^"\\]*(?:\\.[^"\\]*)*)")re"));
    const auto pMatch = promptRe.match(rawText);
    if (pMatch.hasMatch()) {
        outProgram->prompt = pMatch.captured(1);
    }

    // Collect completed objects in a single pass.  A truncated outer response
    // must not prevent us from reaching complete operation objects nested in it.
    // The caps keep this last-resort recovery path bounded for a hostile response.
    constexpr int MAX_RECOVERED_OPERATIONS = 160;
    constexpr int MAX_OBJECT_RANGES = 512;
    constexpr int MAX_OPERATION_OBJECT_LENGTH = 64 * 1024;
    QVector<int> openBraces;
    QVector<QPair<int, int>> objectRanges;
    bool inString = false;
    bool escaped = false;

    for (int i = 0; i < rawText.length(); ++i) {
        const QChar ch = rawText.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inString = true;
        } else if (ch == QLatin1Char('{')) {
            openBraces.append(i);
        } else if (ch == QLatin1Char('}') && !openBraces.isEmpty()) {
            const int openBrace = openBraces.takeLast();
            if (objectRanges.size() < MAX_OBJECT_RANGES) {
                objectRanges.append(qMakePair(openBrace, i));
            }
        }
    }

    for (const QPair<int, int> &range : objectRanges) {
        if (outProgram->operations.size() >= MAX_RECOVERED_OPERATIONS) {
            break;
        }

        const int blockLength = range.second - range.first + 1;
        if (blockLength <= 0 || blockLength > MAX_OPERATION_OBJECT_LENGTH) {
            continue;
        }
        const QString blockText = rawText.mid(range.first, blockLength);
        if (!blockText.contains(QLatin1String("kind"), Qt::CaseInsensitive)
            && !blockText.contains(QLatin1String("type"), Qt::CaseInsensitive)
            && !blockText.contains(QLatin1String("polygon"), Qt::CaseInsensitive)
            && !blockText.contains(QLatin1String("points"), Qt::CaseInsensitive)) {
            continue;
        }

        const QString cleanedBlock = repairJsonSyntax(blockText);
        QJsonParseError bErr;
        QJsonDocument bDoc = QJsonDocument::fromJson(cleanedBlock.toUtf8(), &bErr);
        if (!bDoc.isObject()) {
            const QString truncRepaired = repairTruncatedJson(cleanedBlock);
            bDoc = QJsonDocument::fromJson(truncRepaired.toUtf8(), &bErr);
            if (!bDoc.isObject()) {
                continue;
            }
        }

        const QJsonObject operationObject = bDoc.object();
        if (!operationObject.contains(QStringLiteral("kind")) && !operationObject.contains(QStringLiteral("type"))
            && !operationObject.contains(QStringLiteral("polygon"))
            && !operationObject.contains(QStringLiteral("points"))
            && !operationObject.contains(QStringLiteral("brush"))) {
            continue;
        }

        QJsonObject syntheticRoot;
        syntheticRoot[QStringLiteral("schema_version")] = outProgram->schemaVersion;
        syntheticRoot[QStringLiteral("operations")] = QJsonArray{operationObject};

        KisAiStrokeProgram singleProg;
        QString singleErr;
        if (KisAiStrokeProgramCodec::parseProgramJson(syntheticRoot, &singleProg, &singleErr)
            && !singleProg.operations.isEmpty()) {
            outProgram->operations.append(singleProg.operations.first());
        }
    }

    if (outProgram->operations.isEmpty()) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral("LLM応答から有効なストローク操作を救出できませんでした。");
        }
        return false;
    }

    KisAiStrokeQualityReport qualityReport;
    *outProgram = KisAiStrokeProgramCodec::refineForRendering(*outProgram, &qualityReport);
    return !outProgram->operations.isEmpty();
}

bool KisAiStrokeProgramCodec::supportsJsonFormat(const QString &endpoint)
{
    const QString ep = endpoint.trimmed().toLower();
    if (ep.isEmpty()) {
        return false;
    }
    return ep.contains(QLatin1String("api.openai.com"))
        || ep.contains(QLatin1String("openrouter.ai"))
        || ep.contains(QLatin1String("deepseek.com"))
        || ep.contains(QLatin1String("groq.com"))
        || ep.contains(QLatin1String("googleapis.com"))
        || ep.contains(QLatin1String("mistral.ai"))
        || ep.contains(QLatin1String("together.xyz"))
        || ep.contains(QLatin1String("together.ai"))
        || ep.contains(QLatin1String("fireworks.ai"))
        || ep.contains(QLatin1String("perplexity.ai"))
        || ep.contains(QLatin1String("x.ai"))
        || ep.contains(QLatin1String("cerebras.ai"))
        || ep.contains(QLatin1String("anthropic.com"))
        || ep.contains(QLatin1String("cohere.com"))
        || ep.contains(QLatin1String("11434"))  // Ollama default port
        || ep.contains(QLatin1String("1234"));  // LM Studio default port
}

bool KisAiStrokeProgramCodec::supportsJsonSchema(const QString &model)
{
    const QString m = model.trimmed().toLower();
    return (m.contains(QLatin1String("gpt-4o")) || m.contains(QLatin1String("gpt-4.5")))
        && !m.contains(QLatin1String("vision-preview"));
}

QColor KisAiStrokeProgramCodec::calculateHueShiftedShadow(const QColor &baseColor, bool warmLight)
{
    if (!baseColor.isValid()) return QColor(30, 24, 45, 120);

    int h, s, v, a;
    baseColor.getHsv(&h, &s, &v, &a);
    if (h < 0) h = 240;

    if (warmLight) {
        if (h >= 60 && h < 240) {
            h = qMin(240, h + 25);
        } else if (h < 60 || h > 240) {
            h = (h - 25 + 360) % 360;
        }
    } else {
        if (h >= 180 && h <= 300) {
            h = (h + 30) % 360;
        }
    }

    s = qBound(30, s + 35, 220);
    v = qBound(25, static_cast<int>(v * 0.55), 150);

    return QColor::fromHsv(h, s, v, a);
}

KisAiStrokeProgramCodec::IntentAdherenceResult KisAiStrokeProgramCodec::checkIntentAdherence(
    const KisAiStrokeProgram &program,
    const QString &prompt)
{
    IntentAdherenceResult result;
    const QString pLower = prompt.toLower();

    bool expectDark = pLower.contains(QLatin1String("night")) ||
                      pLower.contains(QLatin1String("dark")) ||
                      pLower.contains(QLatin1String("evening")) ||
                      pLower.contains(QLatin1String("starry")) ||
                      pLower.contains(QLatin1String("midnight"));
    bool expectSunset = pLower.contains(QLatin1String("sunset")) ||
                        pLower.contains(QLatin1String("dusk")) ||
                        pLower.contains(QLatin1String("golden hour"));

    int darkCount = 0;
    int warmCount = 0;
    int totalColors = 0;

    for (const KisAiStrokeOperation &op : program.operations) {
        const QColor col = op.brush.color;
        if (col.isValid()) {
            ++totalColors;
            if (col.value() < 120) {
                ++darkCount;
            }
            int h = col.hsvHue();
            if (h >= 0 && (h <= 55 || h >= 330)) {
                ++warmCount;
            }
        }
    }

    qreal adherence = 1.0;

    if (expectDark) {
        if (totalColors > 0 && qreal(darkCount) / totalColors >= 0.25) {
            result.matchedAspects.append(QStringLiteral("Night/Dark atmospheric tones matched"));
        } else {
            result.missingAspects.append(QStringLiteral("Expected darker night/evening tones"));
            adherence -= 0.2;
        }
    }

    if (expectSunset) {
        if (totalColors > 0 && qreal(warmCount) / totalColors >= 0.20) {
            result.matchedAspects.append(QStringLiteral("Sunset warm golden tones matched"));
        } else {
            result.missingAspects.append(QStringLiteral("Expected warm sunset/golden palette"));
            adherence -= 0.2;
        }
    }

    const auto layerCounts = countLayerOperations(program);
    if (!layerCounts.contains(QStringLiteral("Flats")) && !layerCounts.contains(QStringLiteral("Background"))) {
        result.missingAspects.append(QStringLiteral("Missing base silhouette and flats"));
        adherence -= 0.3;
    } else {
        result.matchedAspects.append(QStringLiteral("Base color silhouettes present"));
    }

    result.score = qBound<qreal>(0.0, adherence, 1.0);
    return result;
}

KisAiStrokeProgram KisAiStrokeProgramCodec::trimOperationsToBudget(
    const KisAiStrokeProgram &program,
    int maxOperations)
{
    if (program.operations.size() <= maxOperations || maxOperations <= 0) {
        return program;
    }

    KisAiStrokeProgram trimmed = program;
    trimmed.operations.clear();

    QMap<QString, QVector<KisAiStrokeOperation>> byLayer;
    for (const KisAiStrokeOperation &op : program.operations) {
        byLayer[normalizeLayerName(op.layer)].append(op);
    }

    const int flatsBudget = qMax(1, static_cast<int>(maxOperations * 0.35));
    const int lineartBudget = qMax(1, static_cast<int>(maxOperations * 0.30));
    const int shadingBudget = qMax(1, static_cast<int>(maxOperations * 0.20));
    const int highlightsBudget = qMax(1, static_cast<int>(maxOperations * 0.10));
    const int fxBudget = maxOperations - (flatsBudget + lineartBudget + shadingBudget + highlightsBudget);

    auto takeBest = [](QVector<KisAiStrokeOperation> &ops, int budget) -> QVector<KisAiStrokeOperation> {
        if (ops.size() <= budget) {
            return ops;
        }
        std::stable_sort(ops.begin(), ops.end(), [](const KisAiStrokeOperation &a, const KisAiStrokeOperation &b) {
            const qreal scoreA = a.polygon.size() > 0 ? polygonArea(a.polygon) : (a.points.size() * 0.01);
            const qreal scoreB = b.polygon.size() > 0 ? polygonArea(b.polygon) : (b.points.size() * 0.01);
            return scoreA > scoreB;
        });
        return ops.mid(0, budget);
    };

    QVector<KisAiStrokeOperation> selected;
    if (byLayer.contains(QStringLiteral("Background"))) {
        selected.append(byLayer[QStringLiteral("Background")]);
    }
    selected.append(takeBest(byLayer[QStringLiteral("Flats")], flatsBudget));
    selected.append(takeBest(byLayer[QStringLiteral("Shading")], shadingBudget));
    selected.append(takeBest(byLayer[QStringLiteral("Lineart")], lineartBudget));
    selected.append(takeBest(byLayer[QStringLiteral("Highlights")], highlightsBudget));
    selected.append(takeBest(byLayer[QStringLiteral("FX")], qMax(0, fxBudget)));

    trimmed.operations = selected;
    return refineForRendering(trimmed);
}

QJsonObject KisAiStrokeProgramCodec::buildCompositionPlanPayload(
    const QString &model,
    const QString &prompt,
    const QSize &canvasSize,
    int artStyle)
{
    Q_UNUSED(artStyle);
    const qreal aspect = canvasSize.height() > 0 ? qreal(canvasSize.width()) / canvasSize.height() : 1.0;

    const QString systemText = QStringLiteral(
        "You are an expert art director and master illustrator. "
        "Before generating digital strokes, formulate a high-level composition and lighting blueprint in JSON.\n"
        "Output strictly valid JSON with this schema:\n"
        "{\n"
        "  \"composition_type\": \"rule_of_thirds\" | \"centered\" | \"diagonal\" | \"panoramic\",\n"
        "  \"focal_point\": {\"x\": 0.5, \"y\": 0.4},\n"
        "  \"primary_palette\": [\"#hex1\", \"#hex2\", \"#hex3\", \"#hex4\", \"#hex5\"],\n"
        "  \"light_source\": {\"direction\": \"top_left\" | \"top_right\" | \"rim\" | \"ambient\", \"temperature\": \"warm\" | \"cool\"},\n"
        "  \"depth_planes\": {\"background\": \"...\", \"midground\": \"...\", \"foreground\": \"...\"},\n"
        "  \"artistic_directives\": \"Concise 2-sentence directive for stroke generation.\"\n"
        "}"
    );

    QJsonObject userObj;
    userObj[QStringLiteral("prompt")] = prompt;
    userObj[QStringLiteral("canvas_width")] = canvasSize.width();
    userObj[QStringLiteral("canvas_height")] = canvasSize.height();
    userObj[QStringLiteral("aspect_ratio")] = aspect;

    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact))}});

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.trimmed();
    payload[QStringLiteral("messages")] = messages;
    payload[QStringLiteral("stream")] = false;

    QJsonObject responseFormat;
    responseFormat[QStringLiteral("type")] = QStringLiteral("json_object");
    payload[QStringLiteral("response_format")] = responseFormat;

    payload[QStringLiteral("max_tokens")] = 1024;
    payload[QStringLiteral("temperature")] = 0.6;
    return payload;
}

bool KisAiStrokeProgramCodec::parseCompositionPlan(
    const QByteArray &responseBytes,
    QString *outDirectives,
    QString *errorMessage)
{
    if (outDirectives) outDirectives->clear();
    const QString raw = QString::fromUtf8(responseBytes);
    const QString jsonStr = sanitizeAndExtractJson(raw);

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &parseErr);
    if (!doc.isObject()) {
        if (errorMessage) *errorMessage = QStringLiteral("Failed to parse composition plan JSON: %1").arg(parseErr.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    QString directives;
    if (root.contains(QStringLiteral("artistic_directives"))) {
        directives = root.value(QStringLiteral("artistic_directives")).toString().trimmed();
    }
    const QJsonObject focal = root.value(QStringLiteral("focal_point")).toObject();
    if (!focal.isEmpty()) {
        directives += QStringLiteral(" [Focal point at (%1, %2)]")
                          .arg(QString::number(focal.value(QStringLiteral("x")).toDouble(0.5), 'f', 2))
                          .arg(QString::number(focal.value(QStringLiteral("y")).toDouble(0.5), 'f', 2));
    }

    if (outDirectives) *outDirectives = directives;
    return !directives.isEmpty();
}

bool KisAiStrokeProgramCodec::parseResponse(const QByteArray &responseBytes,
                                            KisAiStrokeProgram *outProgram,
                                            QString *errorMessage,
                                            KisAiJsonDiagnostic *diagnostic,
                                            KisAiStrokeQualityReport *qualityReport)
{
    constexpr int MAX_RESPONSE_BYTES = 32 * 1024 * 1024;
    if (responseBytes.size() > MAX_RESPONSE_BYTES) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LLM応答が安全上限を超えています。");
        }
        if (diagnostic) {
            diagnostic->hasError = true;
            diagnostic->errorMessage = QStringLiteral("Response exceeded MAX_RESPONSE_BYTES limit");
        }
        return false;
    }

    if (diagnostic) {
        diagnostic->hasError = false;
        diagnostic->errorMessage.clear();
        diagnostic->errorSnippet.clear();
        diagnostic->appliedRepairs.clear();
    }

    const auto parseAndRefine = [outProgram, errorMessage, diagnostic, qualityReport](const QJsonObject &programObject) {
        QJsonObject mutableRoot = programObject;
        KisAiStrokeTypeCheckReport typeReport;
        KisAiStrokeTypeChecker::checkAndCoerceProgram(&mutableRoot, &typeReport);
        if (diagnostic && !typeReport.warnings.isEmpty()) {
            diagnostic->appliedRepairs.append(typeReport.summary());
        }

        if (!KisAiStrokeProgramCodec::parseProgramJson(mutableRoot, outProgram, errorMessage)) {
            if (diagnostic) {
                diagnostic->hasError = true;
                diagnostic->errorMessage = errorMessage ? *errorMessage : QStringLiteral("parseProgramJson failed");
            }
            return false;
        }

        KisAiStrokeQualityReport localQualityReport;
        *outProgram = KisAiStrokeProgramCodec::refineForRendering(*outProgram, &localQualityReport);
        if (qualityReport) {
            *qualityReport = localQualityReport;
        }
        if (outProgram->operations.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("描画可能なストローク操作が1件も含まれていません。");
            }
            if (diagnostic) {
                diagnostic->hasError = true;
                diagnostic->errorMessage = QStringLiteral("No renderable stroke operations after refineForRendering");
            }
            return false;
        }
        return true;
    };

    // Helper: search recursively for an object that contains operations/strokes or is a StrokeProgram
    std::function<QJsonObject(const QJsonObject &, int)> findProgramEnvelope;
    findProgramEnvelope = [&findProgramEnvelope](const QJsonObject &obj, int depth) -> QJsonObject {
        if (depth > 4) return QJsonObject();
        if (obj.contains(QStringLiteral("operations")) || obj.contains(QStringLiteral("strokes"))) {
            return obj;
        }
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QString k = it.key().trimmed().toLower();
            if (it.value().isArray() && (k.contains(QLatin1String("operation")) || k.contains(QLatin1String("stroke")))) {
                return obj;
            }
        }
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (it.value().isObject()) {
                const QJsonObject child = it.value().toObject();
                const QJsonObject found = findProgramEnvelope(child, depth + 1);
                if (!found.isEmpty()) {
                    return found;
                }
            }
        }
        return QJsonObject();
    };

    const QString rawText = QString::fromUtf8(responseBytes).trimmed();

    // 1. Try direct parsing as JSON
    QJsonParseError directDocErr;
    const QJsonDocument doc = QJsonDocument::fromJson(responseBytes, &directDocErr);
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.contains(QStringLiteral("error"))) {
            const QString err = root.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
            if (errorMessage) {
                *errorMessage = QStringLiteral("APIエラー: ") + (err.isEmpty() ? QStringLiteral("不明なエラー") : err);
            }
            if (diagnostic) {
                diagnostic->hasError = true;
                diagnostic->errorMessage = *errorMessage;
            }
            return false;
        }

        // Direct StrokeProgram check: if root explicitly has operations or strokes, parse directly
        if (root.contains(QStringLiteral("operations")) || root.contains(QStringLiteral("strokes")) || root.contains(QStringLiteral("schema_version"))) {
            return parseAndRefine(root);
        }

        // Nested StrokeProgram envelope check (handles wrapper objects like {"result": {...}})
        const QJsonObject envelope = findProgramEnvelope(root, 0);
        if (!envelope.isEmpty() && parseAndRefine(envelope)) {
            return true;
        }

        // OpenAI Chat Completions choices[0].message.content
        const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
        if (!choices.isEmpty()) {
            const QJsonObject firstChoice = choices.at(0).toObject();
            const QJsonObject messageObj = firstChoice.value(QStringLiteral("message")).toObject();
            const QString content = messageObj.value(QStringLiteral("content")).toString();

            if (!content.isEmpty()) {
                const QString cleanJson = sanitizeAndExtractJson(content, diagnostic);
                QJsonParseError parseErr;
                const QJsonDocument programDoc = QJsonDocument::fromJson(cleanJson.toUtf8(), &parseErr);
                if (!programDoc.isNull() && programDoc.isObject()) {
                    const QJsonObject innerEnv = findProgramEnvelope(programDoc.object(), 0);
                    if (!innerEnv.isEmpty() && parseAndRefine(innerEnv)) {
                        return true;
                    }
                }
                // Fallback extraction on choices content
                if (extractOperationsFromRawText(content, outProgram, errorMessage, diagnostic)) {
                    if (qualityReport) {
                        KisAiStrokeQualityReport rep;
                        KisAiStrokeProgramCodec::refineForRendering(*outProgram, &rep);
                        *qualityReport = rep;
                    }
                    return true;
                }
            }
        }

        // Gemini native format: candidates[0].content.parts[0].text
        const QJsonArray candidates = root.value(QStringLiteral("candidates")).toArray();
        if (!candidates.isEmpty()) {
            const QJsonObject cand0 = candidates.at(0).toObject();
            const QJsonObject contentObj = cand0.value(QStringLiteral("content")).toObject();
            const QJsonArray parts = contentObj.value(QStringLiteral("parts")).toArray();
            if (!parts.isEmpty()) {
                const QString partText = parts.at(0).toObject().value(QStringLiteral("text")).toString();
                if (!partText.isEmpty()) {
                    const QString cleanJson = sanitizeAndExtractJson(partText, diagnostic);
                    const QJsonDocument programDoc = QJsonDocument::fromJson(cleanJson.toUtf8());
                    if (programDoc.isObject()) {
                        const QJsonObject innerEnv = findProgramEnvelope(programDoc.object(), 0);
                        if (!innerEnv.isEmpty() && parseAndRefine(innerEnv)) {
                            return true;
                        }
                    }
                    if (extractOperationsFromRawText(partText, outProgram, errorMessage, diagnostic)) {
                        if (qualityReport) {
                            KisAiStrokeQualityReport rep;
                            KisAiStrokeProgramCodec::refineForRendering(*outProgram, &rep);
                            *qualityReport = rep;
                        }
                        return true;
                    }
                }
            }
        }
    }

    // 2. Streamed response or raw model output: run sanitizeAndExtractJson
    const QString extractedJson = sanitizeAndExtractJson(rawText, diagnostic);
    QJsonParseError extDocErr;
    const QJsonDocument extractedDoc = QJsonDocument::fromJson(extractedJson.toUtf8(), &extDocErr);
    if (extractedDoc.isObject()) {
        const QJsonObject root = extractedDoc.object();
        const QJsonObject env = findProgramEnvelope(root, 0);
        if (!env.isEmpty() && parseAndRefine(env)) {
            return true;
        }
        // If choices present in extracted JSON
        const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
        if (!choices.isEmpty()) {
            const QString content = choices.at(0).toObject().value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
            if (!content.isEmpty()) {
                const QString cleanContent = sanitizeAndExtractJson(content, diagnostic);
                const QJsonDocument cDoc = QJsonDocument::fromJson(cleanContent.toUtf8());
                if (cDoc.isObject()) {
                    const QJsonObject cEnv = findProgramEnvelope(cDoc.object(), 0);
                    if (!cEnv.isEmpty() && parseAndRefine(cEnv)) {
                        return true;
                    }
                }
            }
        }
    }

    // 3. Ultimate Fallback: operation-by-operation extraction from raw text
    if (extractOperationsFromRawText(rawText, outProgram, errorMessage, diagnostic)) {
        if (qualityReport) {
            KisAiStrokeQualityReport rep;
            KisAiStrokeProgramCodec::refineForRendering(*outProgram, &rep);
            *qualityReport = rep;
        }
        return true;
    }

    if (diagnostic) {
        diagnostic->hasError = true;
        diagnostic->errorOffset = extDocErr.offset;
        diagnostic->errorMessage = extDocErr.errorString();
        // Compute line and column
        int line = 1;
        int col = 1;
        for (int i = 0; i < qMin(extDocErr.offset, extractedJson.size()); ++i) {
            if (extractedJson.at(i) == QLatin1Char('\n')) {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        diagnostic->errorLine = line;
        diagnostic->errorColumn = col;
        const int snippetStart = qMax(0, extDocErr.offset - 40);
        const int snippetLen = qMin(80, extractedJson.size() - snippetStart);
        diagnostic->errorSnippet = extractedJson.mid(snippetStart, snippetLen);
    }

    if (errorMessage && errorMessage->isEmpty()) {
        *errorMessage = QStringLiteral("LLM応答がJSONオブジェクトではありません。修復にも失敗しました。");
    }
    return false;
}

QString KisAiStrokeProgramCodec::normalizeLayerName(const QString &name)
{
    const QString lower = name.trimmed().toLower();
    if (lower.isEmpty()) {
        return QStringLiteral("Lineart");
    }

    // Static lookup table for layer name aliases.
    static const QHash<QString, QString> aliases = {
        // Background aliases
        {QStringLiteral("background"), QStringLiteral("Background")},
        {QStringLiteral("bg"), QStringLiteral("Background")},
        {QStringLiteral("backdrop"), QStringLiteral("Background")},
        // Flats aliases
        {QStringLiteral("flat"), QStringLiteral("Flats")},
        {QStringLiteral("flats"), QStringLiteral("Flats")},
        {QStringLiteral("base"), QStringLiteral("Flats")},
        {QStringLiteral("color"), QStringLiteral("Flats")},
        {QStringLiteral("colors"), QStringLiteral("Flats")},
        // Shading aliases
        {QStringLiteral("shading"), QStringLiteral("Shading")},
        {QStringLiteral("shade"), QStringLiteral("Shading")},
        {QStringLiteral("shadow"), QStringLiteral("Shading")},
        {QStringLiteral("shadows"), QStringLiteral("Shading")},
        // Lineart aliases
        {QStringLiteral("lineart"), QStringLiteral("Lineart")},
        {QStringLiteral("line_art"), QStringLiteral("Lineart")},
        {QStringLiteral("line art"), QStringLiteral("Lineart")},
        {QStringLiteral("lines"), QStringLiteral("Lineart")},
        {QStringLiteral("line"), QStringLiteral("Lineart")},
        {QStringLiteral("ink"), QStringLiteral("Lineart")},
        // Highlights aliases
        {QStringLiteral("highlight"), QStringLiteral("Highlights")},
        {QStringLiteral("highlights"), QStringLiteral("Highlights")},
        {QStringLiteral("specular"), QStringLiteral("Highlights")},
        {QStringLiteral("glint"), QStringLiteral("Highlights")},
        // FX aliases
        {QStringLiteral("fx"), QStringLiteral("FX")},
        {QStringLiteral("effects"), QStringLiteral("FX")},
        {QStringLiteral("effect"), QStringLiteral("FX")},
        {QStringLiteral("particles"), QStringLiteral("FX")},
        {QStringLiteral("manga_lines"), QStringLiteral("FX")},
    };

    const auto it = aliases.constFind(lower);
    if (it != aliases.constEnd()) {
        return it.value();
    }

    if (lower.contains(QLatin1String("back")) || lower.contains(QLatin1String("bg"))) return QStringLiteral("Background");
    if (lower.contains(QLatin1String("flat")) || lower.contains(QLatin1String("base"))) return QStringLiteral("Flats");
    if (lower.contains(QLatin1String("shad")) || lower.contains(QLatin1String("dark"))) return QStringLiteral("Shading");
    if (lower.contains(QLatin1String("line")) || lower.contains(QLatin1String("ink"))) return QStringLiteral("Lineart");
    if (lower.contains(QLatin1String("light")) || lower.contains(QLatin1String("specular"))) return QStringLiteral("Highlights");
    if (lower.contains(QLatin1String("fx")) || lower.contains(QLatin1String("effect")) || lower.contains(QLatin1String("particle"))) return QStringLiteral("FX");

    return name.trimmed();
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
    QStringList parts;
    if (counts.value(QStringLiteral("Background"), 0) > 0) {
        parts << QStringLiteral("Background: %1").arg(counts.value(QStringLiteral("Background"), 0));
    }
    parts << QStringLiteral("Flats: %1").arg(counts.value(QStringLiteral("Flats"), 0));
    parts << QStringLiteral("Shading: %1").arg(counts.value(QStringLiteral("Shading"), 0));
    parts << QStringLiteral("Lineart: %1").arg(counts.value(QStringLiteral("Lineart"), 0));
    parts << QStringLiteral("Highlights: %1").arg(counts.value(QStringLiteral("Highlights"), 0));
    parts << QStringLiteral("FX: %1").arg(counts.value(QStringLiteral("FX"), 0));
    return parts.join(QStringLiteral(", "));
}

bool KisAiStrokeProgramCodec::parseProgramJson(const QJsonObject &rootObj,
                                                KisAiStrokeProgram *outProgram,
                                                QString *errorMessage)
{
    if (!outProgram) {
        return false;
    }

    // Validate schema version: v1 (strokes array) and v2 (operations array) are supported.
    // Reject clearly unsupported future versions to avoid misinterpretation.
    const auto findField = [](const QJsonObject &o, const QStringList &names, const QJsonValue &defaultVal = QJsonValue()) -> QJsonValue {
        for (const auto &name : names) {
            if (o.contains(name)) {
                return o.value(name);
            }
        }
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            const QString k = it.key().trimmed().toLower();
            for (const auto &name : names) {
                const QString n = name.toLower();
                if (k == n || k.contains(n) || n.contains(k)) {
                    return it.value();
                }
            }
        }
        return defaultVal;
    };

    const auto toDoubleField = [](const QJsonValue &val, qreal defaultVal) -> qreal {
        if (val.isDouble()) {
            const qreal v = val.toDouble(defaultVal);
            return std::isfinite(v) ? v : defaultVal;
        }
        if (val.isString()) {
            bool ok = false;
            QString s = val.toString().trimmed();
            QString clean;
            for (const QChar &ch : s) {
                if (ch.isDigit() || ch == QLatin1Char('.') || ch == QLatin1Char('-') || ch == QLatin1Char('+')) {
                    clean.append(ch);
                }
            }
            const qreal v = clean.toDouble(&ok);
            // A long digit run overflows to +/-inf with ok == true; such a value
            // must never reach the renderer as geometry.
            if (ok && std::isfinite(v)) return v;
        }
        bool ok = false;
        const qreal v = val.toVariant().toDouble(&ok);
        return (ok && std::isfinite(v)) ? v : defaultVal;
    };

    const auto toIntField = [](const QJsonValue &val, int defaultVal) -> int {
        if (val.isDouble()) return val.toInt(defaultVal);
        if (val.isString()) {
            bool ok = false;
            QString s = val.toString().trimmed();
            QString clean;
            for (const QChar &ch : s) {
                if (ch.isDigit() || ch == QLatin1Char('-') || ch == QLatin1Char('+')) {
                    clean.append(ch);
                }
            }
            const int v = clean.toInt(&ok);
            if (ok) return v;
        }
        bool ok = false;
        const int v = val.toVariant().toInt(&ok);
        return ok ? v : defaultVal;
    };

    const auto toBoolField = [](const QJsonValue &val, bool defaultVal) -> bool {
        if (val.isBool()) return val.toBool(defaultVal);
        if (val.isString()) {
            const QString s = val.toString().trimmed().toLower();
            if (s.startsWith(QLatin1String("t")) || s == QLatin1String("1") || s == QLatin1String("yes")) return true;
            if (s.startsWith(QLatin1String("f")) || s == QLatin1String("0") || s == QLatin1String("no")) return false;
        }
        if (val.isDouble()) {
            return val.toDouble() != 0.0;
        }
        return defaultVal;
    };

    if (rootObj.contains(QStringLiteral("schema_version")) || !findField(rootObj, {QStringLiteral("schema_version")}).isUndefined()) {
        const int version = toIntField(findField(rootObj, {QStringLiteral("schema_version")}), 2);
        if (version < 1 || version > 2) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("サポートされていないスキーマバージョンです (v%1)。v1 または v2 が必要です。").arg(version);
            }
            return false;
        }
    }

    outProgram->schemaVersion = toIntField(findField(rootObj, {QStringLiteral("schema_version")}), 2);
    outProgram->prompt = findField(rootObj, {QStringLiteral("prompt")}).toString();
    outProgram->title = findField(rootObj, {QStringLiteral("title")}, QStringLiteral("AI Artwork")).toString(QStringLiteral("AI Artwork"));
    outProgram->seed = toIntField(findField(rootObj, {QStringLiteral("seed")}), 42);
    outProgram->visualCritique = findField(rootObj, {QStringLiteral("visual_critique"), QStringLiteral("critique")}).toString();
    outProgram->agentCritique = findField(rootObj, {QStringLiteral("agent_critique"), QStringLiteral("visual_critique"), QStringLiteral("critique")}).toString();
    outProgram->targetFocusArea = findField(rootObj, {QStringLiteral("target_focus_area"), QStringLiteral("focus_area"), QStringLiteral("focus")}).toString();
    outProgram->stepPhase = findField(rootObj, {QStringLiteral("step_phase")}, QStringLiteral("complete")).toString(QStringLiteral("complete"));
    outProgram->currentStep = toIntField(findField(rootObj, {QStringLiteral("current_step")}), 1);
    outProgram->totalSteps = toIntField(findField(rootObj, {QStringLiteral("total_steps")}), 1);
    outProgram->goalReached = toBoolField(findField(rootObj, {QStringLiteral("goal_reached")}), true);
    outProgram->completionScore = toDoubleField(findField(rootObj, {QStringLiteral("completion_score")}), 1.0);
    outProgram->readinessScore = clamp01(toDoubleField(findField(rootObj, {QStringLiteral("readiness_score"), QStringLiteral("readiness"), QStringLiteral("completion_score")}), 1.0));
    outProgram->recommendedAction = findField(rootObj, {QStringLiteral("recommended_action"), QStringLiteral("action")}).toString();

    const QJsonValue cVal = findField(rootObj, {QStringLiteral("canvas_size"), QStringLiteral("canvas")});
    if (!cVal.isUndefined()) {
        int cw = 0;
        int ch = 0;
        if (cVal.isObject()) {
            const QJsonObject cObj = cVal.toObject();
            cw = toIntField(findField(cObj, {QStringLiteral("width"), QStringLiteral("w")}), 0);
            ch = toIntField(findField(cObj, {QStringLiteral("height"), QStringLiteral("h")}), 0);
        } else if (cVal.isArray()) {
            const QJsonArray cArr = cVal.toArray();
            if (cArr.size() >= 2) {
                cw = toIntField(cArr.at(0), 0);
                ch = toIntField(cArr.at(1), 0);
            }
        }
        if (cw > 0 && ch > 0) {
            outProgram->canvasSize = QSize(cw, ch);
        }
    }

    outProgram->operations.clear();

    const auto parseBrush = [&findField, &toDoubleField, &toBoolField](const QJsonObject &bObj) -> KisAiStrokeBrush {
        KisAiStrokeBrush b;
        b.profile = findField(bObj, {QStringLiteral("profile")}, QStringLiteral("auto")).toString(QStringLiteral("auto"));
        b.color = parseColor(findField(bObj, {QStringLiteral("color")}, QStringLiteral("#232323")).toString(QStringLiteral("#232323")));
        b.size = toDoubleField(findField(bObj, {QStringLiteral("size")}), 0.008);
        b.sizeMode = findField(bObj, {QStringLiteral("size_mode")}, QStringLiteral("ratio")).toString(QStringLiteral("ratio"));
        b.opacity = clamp01(toDoubleField(findField(bObj, {QStringLiteral("opacity")}), 1.0));
        b.isEraser = toBoolField(findField(bObj, {QStringLiteral("is_eraser"), QStringLiteral("is_eraster")}), false);
        b.presetHint = findField(bObj, {QStringLiteral("preset_hint")}).toString();

        // Safety: a ratio size >= 1.0 would cover 100%+ of the canvas; treat as pixels
        if (b.sizeMode.compare(QLatin1String("ratio"), Qt::CaseInsensitive) == 0 && b.size >= 1.0) {
            b.sizeMode = QStringLiteral("px");
        }
        return b;
    };

    const auto normalizeKind = [](const QString &rawKind) -> KisAiStrokeOperation::Kind {
        const QString k = rawKind.trimmed().toLower();
        if (k.contains(QLatin1String("gradient"))) {
            return KisAiStrokeOperation::Kind::GradientFill;
        }
        if (k.contains(QLatin1String("manga")) || k.contains(QLatin1String("speed")) || k.contains(QLatin1String("focus"))) {
            return KisAiStrokeOperation::Kind::MangaLines;
        }
        if (k.contains(QLatin1String("ribbon")) || k.contains(QLatin1String("band")) || k.contains(QLatin1String("taper"))) {
            return KisAiStrokeOperation::Kind::Ribbon;
        }
        if (k.contains(QLatin1String("particle")) || k.contains(QLatin1String("scatter")) || k.contains(QLatin1String("sparkle"))) {
            return KisAiStrokeOperation::Kind::Particles;
        }
        if (k.contains(QLatin1String("hatch"))) {
            return KisAiStrokeOperation::Kind::Hatch;
        }
        if (k.contains(QLatin1String("fill")) || k.contains(QLatin1String("polygon")) || k.contains(QLatin1String("color_fill"))
            || k.contains(QLatin1String("solid_fill"))) {
            return KisAiStrokeOperation::Kind::Fill;
        }
        if (k.contains(QLatin1String("path")) || k.contains(QLatin1String("stroke")) || k.contains(QLatin1String("line"))
            || k.contains(QLatin1String("contour"))) {
            return KisAiStrokeOperation::Kind::Path;
        }
        return KisAiStrokeOperation::Kind::Unknown;
    };

    const auto parsePoint = [](const QJsonValue &pv, qreal defaultPressure = 0.8) -> QPair<QPointF, qreal> {
        const auto toDoubleVal = [](const QJsonValue &val, bool *ok) -> qreal {
            if (val.isDouble()) {
                if (ok) *ok = true;
                return val.toDouble();
            }
            if (val.isString()) {
                QString s = val.toString().trimmed();
                QString clean;
                for (const QChar &ch : s) {
                    if (ch.isDigit() || ch == QLatin1Char('.') || ch == QLatin1Char('-') || ch == QLatin1Char('+')) {
                        clean.append(ch);
                    }
                }
                return clean.toDouble(ok);
            }
            return val.toVariant().toDouble(ok);
        };

        if (pv.isArray()) {
            const QJsonArray pa = pv.toArray();
            if (pa.size() >= 2) {
                bool okX = false, okY = false;
                const qreal x = toDoubleVal(pa.at(0), &okX);
                const qreal y = toDoubleVal(pa.at(1), &okY);
                if (!okX || !okY || !std::isfinite(x) || !std::isfinite(y)) {
                    return {QPointF(), -1.0};
                }
                bool okP = false;
                const qreal p = (pa.size() >= 3)
                    ? toDoubleVal(pa.at(2), &okP)
                    : defaultPressure;
                if (!std::isfinite(p)) {
                    return {QPointF(), -1.0};
                }
                return {QPointF(x, y), okP ? p : defaultPressure};
            }
        } else if (pv.isObject()) {
            const QJsonObject po = pv.toObject();
            bool okX = false, okY = false;
            const qreal x = toDoubleVal(po.value(QStringLiteral("x")), &okX);
            const qreal y = toDoubleVal(po.value(QStringLiteral("y")), &okY);
            if (!okX || !okY || !std::isfinite(x) || !std::isfinite(y)) {
                return {QPointF(), -1.0};
            }
            bool okP = false;
            const qreal p = po.contains(QStringLiteral("pressure"))
                ? toDoubleVal(po.value(QStringLiteral("pressure")), &okP)
                : defaultPressure;
            if (!std::isfinite(p)) {
                return {QPointF(), -1.0};
            }
            return {QPointF(x, y), okP ? p : defaultPressure};
        }
        return {QPointF(), -1.0};
    };

    constexpr int MAX_OPERATIONS = 160;
    constexpr int MAX_POINTS_PER_OPERATION = 256;
    constexpr int MAX_TOTAL_CONTROL_POINTS = 8192;
    constexpr int MAX_PARTICLES_PER_OPERATION = 200;
    constexpr int MAX_TOTAL_PARTICLES = 4096;

    int totalControlPoints = 0;
    int totalParticles = 0;
    const auto reserveControlPoints = [&](const QJsonArray &points) {
        if (points.size() > MAX_POINTS_PER_OPERATION) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("1 操作あたりの制御点数が上限を超えています。");
            }
            return false;
        }
        if (points.size() > MAX_TOTAL_CONTROL_POINTS - totalControlPoints) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("ストロークプログラム全体の制御点数が上限を超えています。");
            }
            return false;
        }
        totalControlPoints += points.size();
        return true;
    };
    // A hostile or confused particle count must not abort an otherwise valid
    // program: the renderer clamps again at rasterization time, so clamping here
    // keeps the total work bounded while preserving the remaining operations.
    const auto reserveParticles = [&](int count) {
        int safeCount = qBound(1, count, MAX_PARTICLES_PER_OPERATION);
        const int remaining = MAX_TOTAL_PARTICLES - totalParticles;
        if (safeCount > remaining) {
            safeCount = qMax(0, remaining);
        }
        totalParticles += safeCount;
        return safeCount;
    };

    const qreal canvasW = outProgram->canvasSize.width() > 0 ? outProgram->canvasSize.width() : 1024.0;
    const qreal canvasH = outProgram->canvasSize.height() > 0 ? outProgram->canvasSize.height() : 1024.0;

    // v2: operations array
    QJsonArray opArray;
    if (rootObj.contains(QStringLiteral("operations"))) {
        opArray = rootObj.value(QStringLiteral("operations")).toArray();
    } else {
        // Tolerant candidate key search (e.g. "operationst", "strokes", "ops", "layers", "data")
        for (auto it = rootObj.constBegin(); it != rootObj.constEnd(); ++it) {
            const QString k = it.key().trimmed().toLower();
            if (it.value().isArray() && (k.contains(QLatin1String("operation")) ||
                                         k.contains(QLatin1String("stroke")) ||
                                         k == QLatin1String("ops") ||
                                         k == QLatin1String("layers") ||
                                         k == QLatin1String("data") ||
                                         k == QLatin1String("items"))) {
                opArray = it.value().toArray();
                break;
            }
        }
    }

    if (!opArray.isEmpty()) {
        if (opArray.size() > MAX_OPERATIONS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("ストローク操作数が上限を超えています。");
            }
            return false;
        }
        const int count = opArray.size();
        for (int oi = 0; oi < count; ++oi) {
            const QJsonValue &v = opArray.at(oi);
            if (!v.isObject())
                continue;
            const QJsonObject o = v.toObject();
            KisAiStrokeOperation op;
            op.kind = normalizeKind(findField(o, {QStringLiteral("kind"), QStringLiteral("type")}).toString());
            op.id = findField(o, {QStringLiteral("id"), QStringLiteral("name")}).toString();
            op.layer = normalizeLayerName(findField(o, {QStringLiteral("layer"), QStringLiteral("layer_name")}, QStringLiteral("Lineart")).toString(QStringLiteral("Lineart")));
            op.brush = parseBrush(findField(o, {QStringLiteral("brush")}).toObject());

            if (op.kind == KisAiStrokeOperation::Kind::Path) {
                op.closed = toBoolField(findField(o, {QStringLiteral("closed")}), false);
                op.smooth = toBoolField(findField(o, {QStringLiteral("smooth")}), true);
                op.role = findField(o, {QStringLiteral("role")}, QStringLiteral("auto")).toString(QStringLiteral("auto"));
                const QJsonArray pts = findField(o, {QStringLiteral("points"), QStringLiteral("pts")}).toArray();
                if (!reserveControlPoints(pts))
                    return false;
                const int ptCount = pts.size();
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < ptCount; ++pi) {
                    const auto pt = parsePoint(pts.at(pi), 0.8);
                    if (pt.second >= 0.0) {
                        op.points.append(KisAiStrokePoint(pt.first.x(), pt.first.y(), pt.second));
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                // Auto-normalize if coordinates were provided in pixel values instead of [0.0, 1.0]
                if (maxCoord > kPixelCoordinateThreshold) {
                    for (KisAiStrokePoint &p : op.points) {
                        p.pos = QPointF(p.pos.x() / canvasW, p.pos.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Fill) {
                op.fillStyle = findField(o, {QStringLiteral("style"), QStringLiteral("fill_style")}, QStringLiteral("wash")).toString(QStringLiteral("wash"));
                op.smooth = toBoolField(findField(o, {QStringLiteral("smooth")}), op.fillStyle.compare(QLatin1String("contour"), Qt::CaseInsensitive) == 0);
                op.angleDeg = toDoubleField(findField(o, {QStringLiteral("angle_deg"), QStringLiteral("angle")}), 0.0);
                op.spacing = toDoubleField(findField(o, {QStringLiteral("spacing")}), 0.5);
                const QJsonArray poly = findField(o, {QStringLiteral("polygon"), QStringLiteral("poly"), QStringLiteral("points")}).toArray();
                if (!reserveControlPoints(poly))
                    return false;
                const int polyCount = poly.size();
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < polyCount; ++pi) {
                    const auto pt = parsePoint(poly.at(pi));
                    if (pt.second >= 0.0) {
                        op.polygon.append(pt.first);
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                if (maxCoord > kPixelCoordinateThreshold) {
                    for (QPointF &p : op.polygon) {
                        p = QPointF(p.x() / canvasW, p.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::GradientFill) {
                op.fillStyle = findField(o, {QStringLiteral("style"), QStringLiteral("fill_style")}, QStringLiteral("linear")).toString(QStringLiteral("linear"));
                op.smooth = toBoolField(findField(o, {QStringLiteral("smooth")}), false);
                op.angleDeg = toDoubleField(findField(o, {QStringLiteral("angle_deg"), QStringLiteral("angle")}), 90.0);
                op.isRadial = toBoolField(findField(o, {QStringLiteral("is_radial"), QStringLiteral("radial")}), op.fillStyle.compare(QLatin1String("radial"), Qt::CaseInsensitive) == 0);
                const QJsonValue centerVal = findField(o, {QStringLiteral("center"), QStringLiteral("center_pt")});
                if (!centerVal.isUndefined() && !centerVal.isNull()) {
                    const auto cp = parsePoint(centerVal);
                    if (cp.second >= 0.0) {
                        op.gradientCenter = cp.first;
                        if (qMax(op.gradientCenter.x(), op.gradientCenter.y()) > kPixelCoordinateThreshold) {
                            op.gradientCenter =
                                QPointF(op.gradientCenter.x() / canvasW, op.gradientCenter.y() / canvasH);
                        }
                    }
                }
                op.gradientRadius = toDoubleField(findField(o, {QStringLiteral("radius"), QStringLiteral("gradient_radius")}), 0.5);
                if (op.gradientRadius > kPixelCoordinateThreshold) {
                    op.gradientRadius /= qMin(canvasW, canvasH);
                }
                const QJsonArray colors = findField(o, {QStringLiteral("colors"), QStringLiteral("gradient_colors")}).toArray();
                // A gradient only needs a handful of stops; an unbounded list from
                // the model must not translate into unbounded work per operation.
                constexpr int MAX_GRADIENT_COLORS = 64;
                for (const QJsonValue &cv : colors) {
                    if (op.gradientColors.size() >= MAX_GRADIENT_COLORS) {
                        break;
                    }
                    op.gradientColors.append(parseColor(cv.toString()));
                }
                const QJsonArray pts = findField(o, {QStringLiteral("points"), QStringLiteral("pts")}).toArray();
                if (!pts.isEmpty()) {
                    if (!reserveControlPoints(pts))
                        return false;
                    for (const QJsonValue &pv : pts) {
                        const auto pt = parsePoint(pv);
                        if (pt.second >= 0.0) {
                            op.points.append(KisAiStrokePoint(pt.first.x(), pt.first.y(), pt.second));
                        }
                    }
                    if (!op.points.isEmpty()) {
                        qreal maxCoord = 0.0;
                        for (const KisAiStrokePoint &point : op.points) {
                            maxCoord = qMax(maxCoord, qMax(qAbs(point.pos.x()), qAbs(point.pos.y())));
                        }
                        if (maxCoord > kPixelCoordinateThreshold) {
                            for (KisAiStrokePoint &point : op.points) {
                                point.pos = QPointF(point.pos.x() / canvasW, point.pos.y() / canvasH);
                            }
                        }
                    }
                }
                const QJsonArray poly = findField(o, {QStringLiteral("polygon"), QStringLiteral("poly")}).toArray();
                if (!poly.isEmpty()) {
                    if (!reserveControlPoints(poly))
                        return false;
                    const int polyCount = poly.size();
                    qreal maxCoord = 0.0;
                    for (int pi = 0; pi < polyCount; ++pi) {
                        const auto pt = parsePoint(poly.at(pi));
                        if (pt.second >= 0.0) {
                            op.polygon.append(pt.first);
                            maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                        }
                    }
                    if (maxCoord > kPixelCoordinateThreshold) {
                        for (QPointF &p : op.polygon) {
                            p = QPointF(p.x() / canvasW, p.y() / canvasH);
                        }
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Hatch) {
                op.smooth = toBoolField(findField(o, {QStringLiteral("smooth")}), false);
                op.angleDeg = toDoubleField(findField(o, {QStringLiteral("angle_deg"), QStringLiteral("angle")}), 45.0);
                op.spacing = toDoubleField(findField(o, {QStringLiteral("spacing")}), 0.015);
                op.crossHatch = toBoolField(findField(o, {QStringLiteral("cross_hatch"), QStringLiteral("crosshatch")}), false);
                const QJsonArray poly = findField(o, {QStringLiteral("polygon"), QStringLiteral("poly"), QStringLiteral("points")}).toArray();
                if (!reserveControlPoints(poly))
                    return false;
                const int polyCount = poly.size();
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < polyCount; ++pi) {
                    const auto pt = parsePoint(poly.at(pi));
                    if (pt.second >= 0.0) {
                        op.polygon.append(pt.first);
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                if (maxCoord > kPixelCoordinateThreshold) {
                    for (QPointF &p : op.polygon) {
                        p = QPointF(p.x() / canvasW, p.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Ribbon) {
                op.widthStart = toDoubleField(findField(o, {QStringLiteral("width_start"), QStringLiteral("start_width")}), 0.02);
                op.widthMid = toDoubleField(findField(o, {QStringLiteral("width_mid"), QStringLiteral("mid_width")}), 0.015);
                op.widthEnd = toDoubleField(findField(o, {QStringLiteral("width_end"), QStringLiteral("end_width")}), 0.005);
                const qreal minCanvasDim = qMin(canvasW, canvasH);
                if (qMax(qMax(op.widthStart, op.widthMid), op.widthEnd) > kPixelCoordinateThreshold) {
                    op.widthStart /= minCanvasDim;
                    op.widthMid /= minCanvasDim;
                    op.widthEnd /= minCanvasDim;
                }
                const QJsonArray spine = findField(o, {QStringLiteral("spine"), QStringLiteral("points"), QStringLiteral("pts")}).toArray();
                if (!reserveControlPoints(spine))
                    return false;
                const int spineCount = spine.size();
                qreal maxCoord = 0.0;
                for (int pi = 0; pi < spineCount; ++pi) {
                    const auto pt = parsePoint(spine.at(pi));
                    if (pt.second >= 0.0) {
                        op.spine.append(pt.first);
                        maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                    }
                }
                if (maxCoord > kPixelCoordinateThreshold) {
                    for (QPointF &p : op.spine) {
                        p = QPointF(p.x() / canvasW, p.y() / canvasH);
                    }
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Particles) {
                op.particleShape = findField(o, {QStringLiteral("shape"), QStringLiteral("particle_shape")}, QStringLiteral("petal")).toString(QStringLiteral("petal"));
                op.particleCount = toIntField(findField(o, {QStringLiteral("count"), QStringLiteral("particle_count")}), 16);
                op.particleCount = reserveParticles(op.particleCount);
                const QJsonArray b = findField(o, {QStringLiteral("bounds"), QStringLiteral("rect"), QStringLiteral("box")}).toArray();
                if (b.size() >= 4) {
                    qreal x1 = toDoubleField(b.at(0), 0.1);
                    qreal y1 = toDoubleField(b.at(1), 0.1);
                    qreal x2 = toDoubleField(b.at(2), 0.9);
                    qreal y2 = toDoubleField(b.at(3), 0.9);
                    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) {
                        op.bounds = QRectF(0.1, 0.1, 0.8, 0.8);
                    } else {
                        if (qMax(qMax(x1, y1), qMax(x2, y2)) > kPixelCoordinateThreshold) {
                            x1 /= canvasW;
                            y1 /= canvasH;
                            x2 /= canvasW;
                            y2 /= canvasH;
                        }
                        op.bounds = QRectF(QPointF(x1, y1), QPointF(x2, y2)).normalized();
                    }
                } else {
                    op.bounds = QRectF(0.1, 0.1, 0.8, 0.8);
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::MangaLines) {
                const QJsonValue centerVal = findField(o, {QStringLiteral("center"), QStringLiteral("center_pt")});
                if (!centerVal.isUndefined() && !centerVal.isNull()) {
                    const auto cp = parsePoint(centerVal);
                    if (cp.second >= 0.0) {
                        op.gradientCenter = cp.first;
                        if (qMax(op.gradientCenter.x(), op.gradientCenter.y()) > kPixelCoordinateThreshold) {
                            op.gradientCenter = QPointF(op.gradientCenter.x() / canvasW, op.gradientCenter.y() / canvasH);
                        }
                    }
                }
                op.innerRadius = toDoubleField(findField(o, {QStringLiteral("inner_radius"), QStringLiteral("inner")}), 0.15);
                op.outerRadius = toDoubleField(findField(o, {QStringLiteral("outer_radius"), QStringLiteral("outer")}), 0.70);
                const qreal minCanvasDim = qMin(canvasW, canvasH);
                if (qMax(op.innerRadius, op.outerRadius) > kPixelCoordinateThreshold) {
                    op.innerRadius /= minCanvasDim;
                    op.outerRadius /= minCanvasDim;
                }
                op.density = qBound(4, toIntField(findField(o, {QStringLiteral("density")}), 48), 120);
                op.lineLengthJitter = toDoubleField(findField(o, {QStringLiteral("line_length_jitter"), QStringLiteral("jitter")}), 0.20);
            }

            if (op.kind != KisAiStrokeOperation::Kind::Unknown) {
                outProgram->operations.append(op);
            }
        }
    }

    // v1 fallback: strokes array
    if (outProgram->operations.isEmpty() && rootObj.contains(QStringLiteral("strokes"))) {
        const QJsonArray strokeArray = rootObj.value(QStringLiteral("strokes")).toArray();
        if (strokeArray.size() > MAX_OPERATIONS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("ストローク操作数が上限を超えています。");
            }
            return false;
        }
        const int count = strokeArray.size();
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
            if (!reserveControlPoints(pts))
                return false;
            const int ptCount = pts.size();
            qreal maxCoord = 0.0;
            for (int pi = 0; pi < ptCount; ++pi) {
                const auto pt = parsePoint(pts.at(pi), 0.8);
                if (pt.second >= 0.0) {
                    op.points.append(KisAiStrokePoint(pt.first.x(), pt.first.y(), pt.second));
                    maxCoord = qMax(maxCoord, qMax(qAbs(pt.first.x()), qAbs(pt.first.y())));
                }
            }
            if (maxCoord > kPixelCoordinateThreshold) {
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

KisAiStrokeProgram KisAiStrokeProgramCodec::refineForRendering(const KisAiStrokeProgram &program,
                                                               KisAiStrokeQualityReport *report)
{
    KisAiStrokeQualityReport localReport;
    localReport.inputOperations = program.operations.size();

    KisAiStrokeProgram refined = program;
    refined.operations.clear();
    refined.operations.reserve(program.operations.size());

    // canvas_size is model-supplied metadata. Clamp it here so a hostile or
    // confused value can never size a downstream QImage allocation.
    constexpr int MAX_CANVAS_EDGE = 4096;
    const QSize declaredCanvas = refined.canvasSize;
    refined.canvasSize =
        QSize(qBound(1, declaredCanvas.width(), MAX_CANVAS_EDGE), qBound(1, declaredCanvas.height(), MAX_CANVAS_EDGE));
    if (refined.canvasSize != declaredCanvas) {
        ++localReport.repairedValues;
    }

    const qreal minCanvasDimension = qMax<qreal>(64.0, qMin(refined.canvasSize.width(), refined.canvasSize.height()));
    int generatedId = 1;

    for (const KisAiStrokeOperation &source : program.operations) {
        KisAiStrokeOperation op = source;
        op.layer = normalizeLayerName(op.layer);

        // Hatch screens and gradient directions are rasterized through
        // QPainter::rotate(), so a non-finite or unwrapped angle must not
        // survive this pass.
        if (!std::isfinite(op.angleDeg)) {
            op.angleDeg = 0.0;
            ++localReport.repairedValues;
        } else {
            const qreal wrapped = std::fmod(op.angleDeg, 360.0);
            const qreal normalized = wrapped < 0.0 ? wrapped + 360.0 : wrapped;
            if (normalized != op.angleDeg)
                ++localReport.repairedValues;
            op.angleDeg = normalized;
        }

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
        if (profile == QLatin1String("felt") || profile == QLatin1String("copic"))
            profile = QStringLiteral("marker");
        if (profile == QLatin1String("pastel") || profile == QLatin1String("chalk") || profile == QLatin1String("oil_pastel"))
            profile = QStringLiteral("crayon");
        if (profile == QLatin1String("glow") || profile == QLatin1String("laser") || profile == QLatin1String("light"))
            profile = QStringLiteral("neon");
        if (profile == QLatin1String("spatter") || profile == QLatin1String("blot") || profile == QLatin1String("fleck"))
            profile = QStringLiteral("splatter");
        if (profile == QLatin1String("chisel") || profile == QLatin1String("flat_pen") || profile == QLatin1String("ribbon_pen"))
            profile = QStringLiteral("calligraphy");
        if (profile == QLatin1String("carbon") || profile == QLatin1String("conte"))
            profile = QStringLiteral("charcoal");
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

        // A5: Shading hatch rescue - convert coarse/mechanical hatch to smooth watercolor/brush fill
        if (op.layer == QLatin1String("Shading") && op.kind == KisAiStrokeOperation::Kind::Hatch) {
            if (!op.crossHatch && (op.spacing >= 0.02 || op.spacing <= 0.002)) {
                op.kind = KisAiStrokeOperation::Kind::Fill;
                op.brush.profile = QStringLiteral("watercolor");
                op.brush.opacity = qBound<qreal>(0.05, op.brush.opacity * 0.75, 0.40);
                ++localReport.repairedValues;
            }
        }

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

        // B3: If shading fill has pure black or near-zero saturation, enrich with hue-shifted shadow tone
        if (op.layer == QLatin1String("Shading") && (op.kind == KisAiStrokeOperation::Kind::Fill || op.kind == KisAiStrokeOperation::Kind::GradientFill)) {
            if (op.brush.color.isValid() && op.brush.color.value() < 40 && op.brush.color.saturation() < 25) {
                op.brush.color = calculateHueShiftedShadow(op.brush.color, true);
                ++localReport.repairedValues;
            }
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

                // Angle Spike Suppression: smooth severe zigzag noise
                for (int i = 1; i + 1 < points.size(); ++i) {
                    const QPointF &pPrev = points.at(i - 1).pos;
                    const QPointF &pCurr = points.at(i).pos;
                    const QPointF &pNext = points.at(i + 1).pos;
                    const QPointF v1 = pCurr - pPrev;
                    const QPointF v2 = pNext - pCurr;
                    const qreal len1 = std::hypot(v1.x(), v1.y());
                    const qreal len2 = std::hypot(v2.x(), v2.y());
                    if (len1 > 1.0e-5 && len2 > 1.0e-5 && (len1 + len2) < 0.15) {
                        const qreal dot = (v1.x() * v2.x() + v1.y() * v2.y()) / (len1 * len2);
                        if (dot < -0.85) { // sharp fold-back > ~150 deg
                            points[i].pos = (pPrev + pNext) * 0.5;
                            ++localReport.repairedValues;
                        }
                    }
                }
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
                const qreal area = polygonArea(op.polygon);
                renderable = op.polygon.size() >= 3 && area >= 1.0e-4;
                if (renderable && op.polygon.boundingRect().width() < 0.005 && op.polygon.boundingRect().height() < 0.005) {
                    renderable = false;
                }
            }
            if (!std::isfinite(op.spacing))
                op.spacing = 0.015;
            op.spacing = qBound<qreal>(0.002, op.spacing, 0.2);
            op.gradientCenter = clampedPoint(op.gradientCenter, &localReport.repairedValues);
            op.gradientRadius = qBound<qreal>(0.01, std::isfinite(op.gradientRadius) ? op.gradientRadius : 0.5, 2.0);
            if (op.kind == KisAiStrokeOperation::Kind::GradientFill && !op.points.isEmpty()) {
                QVector<KisAiStrokePoint> pts;
                pts.reserve(op.points.size());
                for (KisAiStrokePoint pt : op.points) {
                    pt.pos = clampedPoint(pt.pos, &localReport.repairedValues);
                    const qreal pressure = std::isfinite(pt.pressure) ? qBound<qreal>(0.05, pt.pressure, 1.0) : 0.8;
                    if (pressure != pt.pressure)
                        ++localReport.repairedValues;
                    pt.pressure = pressure;
                    pts.append(pt);
                }
                op.points = pts;
            }
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
            // 0 is allowed: parse-time budgeting zeroes operations that exceeded
            // MAX_TOTAL_PARTICLES, and re-inflating them to 1 would break that
            // total-work bound. A zero-count op simply draws nothing.
            op.particleCount = qBound(0, op.particleCount, 300);
            renderable = op.bounds.width() > 1.0e-4 && op.bounds.height() > 1.0e-4;
            break;
        }
        case KisAiStrokeOperation::Kind::MangaLines: {
            // A5: Manga lines must only be in the FX layer
            if (op.layer != QLatin1String("FX")) {
                renderable = false;
                break;
            }
            // A5: Discard manga focus lines if prompt context is static / calm
            const QString promptLower = program.prompt.toLower();
            const bool hasActionContext = promptLower.contains(QLatin1String("action")) ||
                                          promptLower.contains(QLatin1String("speed")) ||
                                          promptLower.contains(QLatin1String("battle")) ||
                                          promptLower.contains(QLatin1String("impact")) ||
                                          promptLower.contains(QLatin1String("manga")) ||
                                          promptLower.contains(QLatin1String("comic")) ||
                                          promptLower.contains(QLatin1String("burst")) ||
                                          promptLower.contains(QLatin1String("dynamic"));
            const bool hasStaticContext = promptLower.contains(QLatin1String("portrait")) ||
                                          promptLower.contains(QLatin1String("landscape")) ||
                                          promptLower.contains(QLatin1String("scenery")) ||
                                          promptLower.contains(QLatin1String("peaceful")) ||
                                          promptLower.contains(QLatin1String("calm")) ||
                                          promptLower.contains(QLatin1String("sunset")) ||
                                          promptLower.contains(QLatin1String("sleep"));
            if (!hasActionContext && hasStaticContext) {
                renderable = false;
                break;
            }

            op.gradientCenter = clampedPoint(op.gradientCenter, &localReport.repairedValues);
            op.innerRadius = qBound<qreal>(0.01, std::isfinite(op.innerRadius) ? op.innerRadius : 0.15, 0.8);
            op.outerRadius = qBound<qreal>(op.innerRadius + 0.05, std::isfinite(op.outerRadius) ? op.outerRadius : 0.70, 1.5);
            op.density = qBound(4, op.density, 180);
            op.lineLengthJitter = qBound<qreal>(0.0, std::isfinite(op.lineLengthJitter) ? op.lineLengthJitter : 0.20, 0.9);
            renderable = op.density > 0 && op.outerRadius > op.innerRadius;
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

    // Ensure deterministic back-to-front layer ordering: Background -> Flats -> Shading -> Lineart -> Highlights -> FX
    const auto layerOrder = [](const QString &layer) -> int {
        if (layer == QLatin1String("Background")) return 0;
        if (layer == QLatin1String("Flats")) return 1;
        if (layer == QLatin1String("Shading")) return 2;
        if (layer == QLatin1String("Lineart")) return 3;
        if (layer == QLatin1String("Highlights")) return 4;
        if (layer == QLatin1String("FX")) return 5;
        return 6;
    };
    std::stable_sort(refined.operations.begin(), refined.operations.end(),
                     [&layerOrder](const KisAiStrokeOperation &a, const KisAiStrokeOperation &b) {
                         return layerOrder(a.layer) < layerOrder(b.layer);
                     });

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
    if (program.totalSteps > 1) {
        // Goal Mode is orchestrated locally. A model response is untrusted
        // metadata and must not terminate a user-selected multi-step run
        // before the configured final step is reached.
        refined.goalReached = program.currentStep >= program.totalSteps;
    } else {
        refined.goalReached = localReport.score >= 0.6;
    }
    if (report)
        *report = localReport;
    return refined;
}

qreal KisAiStrokeProgramCodec::qualityScore(const KisAiStrokeProgram &program)
{
    if (program.operations.isEmpty())
        return 0.0;

    QSet<QString> layers;
    QSet<QString> uniqueColors;
    qreal totalPolygonArea = 0.0;
    int continuousStrokes = 0;
    int totalStrokes = 0;
    int geometryPoints = 0;

    for (const KisAiStrokeOperation &op : program.operations) {
        const QString normLayer = normalizeLayerName(op.layer);
        layers.insert(normLayer);

        if (op.brush.color.isValid()) {
            uniqueColors.insert(op.brush.color.name(QColor::HexRgb).toLower());
        }
        for (const QColor &gc : op.gradientColors) {
            if (gc.isValid()) {
                uniqueColors.insert(gc.name(QColor::HexRgb).toLower());
            }
        }

        if (op.kind == KisAiStrokeOperation::Kind::Path) {
            ++totalStrokes;
            if (op.points.size() >= 3) {
                ++continuousStrokes;
            }
            geometryPoints += op.points.size();
        } else if (op.kind == KisAiStrokeOperation::Kind::Fill || op.kind == KisAiStrokeOperation::Kind::GradientFill) {
            if (op.kind == KisAiStrokeOperation::Kind::GradientFill && op.polygon.isEmpty()) {
                totalPolygonArea += 1.0;
                geometryPoints += 4;
            } else {
                totalPolygonArea += polygonArea(op.polygon);
                geometryPoints += op.polygon.size();
            }
        } else if (op.kind == KisAiStrokeOperation::Kind::Ribbon) {
            ++totalStrokes;
            if (op.spine.size() >= 3) {
                ++continuousStrokes;
            }
            geometryPoints += op.spine.size();
        } else if (op.kind == KisAiStrokeOperation::Kind::Particles) {
            geometryPoints += op.particleCount;
        } else if (op.kind == KisAiStrokeOperation::Kind::MangaLines) {
            geometryPoints += op.density * 2;
        }
    }

    // 1. Layer hierarchy score (0.30)
    qreal layerHierarchyScore = 0.0;
    if (layers.contains(QStringLiteral("Flats"))) layerHierarchyScore += 0.35;
    if (layers.contains(QStringLiteral("Lineart"))) layerHierarchyScore += 0.30;
    if (layers.contains(QStringLiteral("Shading"))) layerHierarchyScore += 0.20;
    if (layers.contains(QStringLiteral("Highlights"))) layerHierarchyScore += 0.10;
    if (layers.contains(QStringLiteral("Background"))) layerHierarchyScore += 0.05;
    layerHierarchyScore = qBound<qreal>(0.0, layerHierarchyScore, 1.0);

    // 2. Real polygon silhouette area score (0.20)
    const qreal silhouetteAreaScore = qBound<qreal>(0.0, totalPolygonArea / 0.35, 1.0);

    // 3. Stroke continuity score (0.20): ratio of continuous 3+ point strokes
    const qreal strokeContinuityScore = (totalStrokes > 0)
        ? (qreal(continuousStrokes) / qreal(totalStrokes))
        : (layers.contains(QStringLiteral("Flats")) ? 0.8 : 0.0);

    // 4. Color diversity and harmony score (0.15): 4-16 colors optimal
    const int colorCount = uniqueColors.size();
    qreal colorDiversityScore = 0.0;
    if (colorCount >= 4 && colorCount <= 16) {
        colorDiversityScore = 1.0;
    } else if (colorCount > 0 && colorCount < 4) {
        colorDiversityScore = qreal(colorCount) / 4.0;
    } else if (colorCount > 16) {
        colorDiversityScore = qMax<qreal>(0.5, 1.0 - qreal(colorCount - 16) * 0.03);
    }

    // 5. Operation count and geometric detail score (0.15)
    const qreal operationCountScore = qBound<qreal>(0.0, qreal(program.operations.size()) / 20.0, 1.0);
    const qreal detailPointScore = qBound<qreal>(0.0, qreal(geometryPoints) / 100.0, 1.0);
    const qreal operationScore = 0.5 * operationCountScore + 0.5 * detailPointScore;

    const qreal finalScore = 0.30 * layerHierarchyScore
                           + 0.20 * silhouetteAreaScore
                           + 0.20 * strokeContinuityScore
                           + 0.15 * colorDiversityScore
                           + 0.15 * operationScore;

    return qBound<qreal>(0.0, finalScore, 1.0);
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

        // 0. Background: Subtle vignette / atmosphere
        {
            KisAiStrokeOperation bg;
            bg.kind = KisAiStrokeOperation::Kind::GradientFill;
            bg.id = QStringLiteral("bg_vignette");
            bg.layer = QStringLiteral("Background");
            bg.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
            bg.gradientColors << makeHslColor(baseHue + 20, 40, 245) << makeHslColor(baseHue + 40, 60, 220);
            bg.isRadial = true;
            bg.gradientCenter = QPointF(0.50, 0.50);
            bg.gradientRadius = 0.75;
            program.operations.append(bg);
        }

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

        return refineForRendering(program);
    }
    else if (spec.domain == KisAiPromptAnalyzer::DomainType::Cyberpunk) {
        // 0. Background: Dark neon skyline gradient
        {
            KisAiStrokeOperation bg;
            bg.kind = KisAiStrokeOperation::Kind::GradientFill;
            bg.id = QStringLiteral("cyber_sky");
            bg.layer = QStringLiteral("Background");
            bg.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 0.70) << QPointF(0.0, 0.70);
            bg.gradientColors << QColor(QStringLiteral("#0b0c16")) << QColor(QStringLiteral("#1e0836")) << QColor(QStringLiteral("#380e4a"));
            bg.angleDeg = 90.0;
            program.operations.append(bg);
        }

        // 1. Flats: Skyscraper silhouettes & Ground highway
        {
            KisAiStrokeOperation city;
            city.kind = KisAiStrokeOperation::Kind::Fill;
            city.id = QStringLiteral("skyscrapers");
            city.layer = QStringLiteral("Flats");
            city.brush.color = QColor(QStringLiteral("#101726"));
            city.polygon << QPointF(0.05, 0.65) << QPointF(0.05, 0.28) << QPointF(0.22, 0.28) << QPointF(0.22, 0.38)
                         << QPointF(0.35, 0.38) << QPointF(0.35, 0.20) << QPointF(0.48, 0.20) << QPointF(0.48, 0.32)
                         << QPointF(0.62, 0.32) << QPointF(0.62, 0.16) << QPointF(0.78, 0.16) << QPointF(0.78, 0.35)
                         << QPointF(0.95, 0.35) << QPointF(0.95, 0.65);
            program.operations.append(city);

            KisAiStrokeOperation highway;
            highway.kind = KisAiStrokeOperation::Kind::Fill;
            highway.id = QStringLiteral("highway_ground");
            highway.layer = QStringLiteral("Flats");
            highway.brush.color = QColor(QStringLiteral("#0c0d14"));
            highway.polygon << QPointF(0.0, 0.62) << QPointF(1.0, 0.62) << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
            program.operations.append(highway);
        }

        // 2. Shading: Building shadow panels
        {
            KisAiStrokeOperation shade;
            shade.kind = KisAiStrokeOperation::Kind::Hatch;
            shade.id = QStringLiteral("building_shading");
            shade.layer = QStringLiteral("Shading");
            shade.brush.color = QColor(QStringLiteral("#060910"));
            shade.angleDeg = 90.0;
            shade.spacing = 0.012;
            shade.crossHatch = false;
            shade.polygon << QPointF(0.35, 0.20) << QPointF(0.48, 0.20) << QPointF(0.48, 0.62) << QPointF(0.35, 0.62);
            program.operations.append(shade);
        }

        // 3. Lineart: Architectural edges & road grid
        {
            KisAiStrokeOperation grid;
            grid.kind = KisAiStrokeOperation::Kind::Path;
            grid.id = QStringLiteral("city_lineart");
            grid.layer = QStringLiteral("Lineart");
            grid.brush.profile = QStringLiteral("gpen");
            grid.brush.color = QColor(QStringLiteral("#1e2b40"));
            grid.brush.size = 0.003;
            grid.points << KisAiStrokePoint(0.0, 0.62, 0.8) << KisAiStrokePoint(0.50, 0.62, 0.9) << KisAiStrokePoint(1.0, 0.62, 0.8);
            program.operations.append(grid);

            KisAiStrokeOperation antenna;
            antenna.kind = KisAiStrokeOperation::Kind::Ribbon;
            antenna.id = QStringLiteral("spire");
            antenna.layer = QStringLiteral("Lineart");
            antenna.brush.color = QColor(QStringLiteral("#283e5c"));
            antenna.widthStart = 0.012;
            antenna.widthMid = 0.006;
            antenna.widthEnd = 0.001;
            antenna.spine << QPointF(0.62, 0.25) << QPointF(0.62, 0.08);
            program.operations.append(antenna);
        }

        // 4. Highlights: Neon glows (Cyan & Magenta)
        {
            KisAiStrokeOperation neonCyan;
            neonCyan.kind = KisAiStrokeOperation::Kind::Path;
            neonCyan.id = QStringLiteral("neon_cyan");
            neonCyan.layer = QStringLiteral("Highlights");
            neonCyan.brush.profile = QStringLiteral("neon");
            neonCyan.brush.color = QColor(QStringLiteral("#00f0ff"));
            neonCyan.brush.size = 0.004;
            neonCyan.points << KisAiStrokePoint(0.08, 0.32, 0.9) << KisAiStrokePoint(0.18, 0.32, 0.9);
            program.operations.append(neonCyan);

            KisAiStrokeOperation neonMagenta;
            neonMagenta.kind = KisAiStrokeOperation::Kind::Path;
            neonMagenta.id = QStringLiteral("neon_magenta");
            neonMagenta.layer = QStringLiteral("Highlights");
            neonMagenta.brush.profile = QStringLiteral("neon");
            neonMagenta.brush.color = QColor(QStringLiteral("#ff007f"));
            neonMagenta.brush.size = 0.005;
            neonMagenta.points << KisAiStrokePoint(0.38, 0.25, 0.9) << KisAiStrokePoint(0.38, 0.45, 0.9);
            program.operations.append(neonMagenta);
        }

        // 5. FX: Manga Lines (Perspective speed) & cyber particles
        {
            KisAiStrokeOperation speed;
            speed.kind = KisAiStrokeOperation::Kind::MangaLines;
            speed.id = QStringLiteral("cyber_perspective_lines");
            speed.layer = QStringLiteral("FX");
            speed.brush.profile = QStringLiteral("neon");
            speed.brush.color = QColor(QStringLiteral("#00e5ff"));
            speed.brush.size = 0.002;
            speed.gradientCenter = QPointF(0.50, 0.62);
            speed.innerRadius = 0.10;
            speed.outerRadius = 0.75;
            speed.density = 36;
            speed.lineLengthJitter = 0.25;
            program.operations.append(speed);

            KisAiStrokeOperation particles;
            particles.kind = KisAiStrokeOperation::Kind::Particles;
            particles.id = QStringLiteral("cyber_sparks");
            particles.layer = QStringLiteral("FX");
            particles.brush.color = QColor(QStringLiteral("#ff00a0"));
            particles.bounds = QRectF(0.05, 0.20, 0.90, 0.75);
            particles.particleCount = 28;
            particles.particleShape = QStringLiteral("sparkle");
            program.operations.append(particles);
        }

        return refineForRendering(program);
    }
    else if (spec.domain == KisAiPromptAnalyzer::DomainType::Botanical) {
        // 0. Background: Soft watercolor wash
        {
            KisAiStrokeOperation bg;
            bg.kind = KisAiStrokeOperation::Kind::GradientFill;
            bg.id = QStringLiteral("botanical_bg");
            bg.layer = QStringLiteral("Background");
            bg.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
            bg.gradientColors << QColor(QStringLiteral("#fcfaf6")) << QColor(QStringLiteral("#f4eee1"));
            bg.angleDeg = 45.0;
            program.operations.append(bg);
        }

        // 1. Flats: Leaves & Petals
        {
            KisAiStrokeOperation leaf;
            leaf.kind = KisAiStrokeOperation::Kind::Fill;
            leaf.id = QStringLiteral("broad_leaf");
            leaf.layer = QStringLiteral("Flats");
            leaf.brush.profile = QStringLiteral("watercolor");
            leaf.brush.color = QColor(QStringLiteral("#4a7a53"));
            leaf.polygon << QPointF(0.35, 0.65) << QPointF(0.18, 0.58) << QPointF(0.12, 0.72) << QPointF(0.32, 0.76);
            program.operations.append(leaf);

            KisAiStrokeOperation blossom;
            blossom.kind = KisAiStrokeOperation::Kind::Fill;
            blossom.id = QStringLiteral("flower_petals");
            blossom.layer = QStringLiteral("Flats");
            blossom.brush.profile = QStringLiteral("watercolor");
            blossom.brush.color = QColor(QStringLiteral("#e64966"));
            blossom.polygon << QPointF(0.40, 0.35) << QPointF(0.50, 0.22) << QPointF(0.60, 0.35)
                            << QPointF(0.68, 0.48) << QPointF(0.50, 0.60) << QPointF(0.32, 0.48);
            program.operations.append(blossom);
        }

        // 2. Shading: Inner petal depth & leaf vein hatch
        {
            KisAiStrokeOperation petalShade;
            petalShade.kind = KisAiStrokeOperation::Kind::Fill;
            petalShade.id = QStringLiteral("inner_petal_shade");
            petalShade.layer = QStringLiteral("Shading");
            petalShade.brush.profile = QStringLiteral("brush");
            petalShade.brush.color = QColor(QStringLiteral("#b02542"));
            petalShade.polygon << QPointF(0.44, 0.40) << QPointF(0.50, 0.32) << QPointF(0.56, 0.40)
                               << QPointF(0.53, 0.52) << QPointF(0.47, 0.52);
            program.operations.append(petalShade);

            KisAiStrokeOperation veinHatch;
            veinHatch.kind = KisAiStrokeOperation::Kind::Hatch;
            veinHatch.id = QStringLiteral("leaf_veins");
            veinHatch.layer = QStringLiteral("Shading");
            veinHatch.brush.color = QColor(QStringLiteral("#2f5236"));
            veinHatch.angleDeg = 30.0;
            veinHatch.spacing = 0.015;
            veinHatch.polygon << QPointF(0.35, 0.65) << QPointF(0.18, 0.58) << QPointF(0.12, 0.72) << QPointF(0.32, 0.76);
            program.operations.append(veinHatch);
        }

        // 3. Lineart: Organic stem ribbon & petal contours
        {
            KisAiStrokeOperation stem;
            stem.kind = KisAiStrokeOperation::Kind::Ribbon;
            stem.id = QStringLiteral("flower_stem");
            stem.layer = QStringLiteral("Lineart");
            stem.brush.color = QColor(QStringLiteral("#334d36"));
            stem.widthStart = 0.024;
            stem.widthMid = 0.018;
            stem.widthEnd = 0.010;
            stem.spine << QPointF(0.48, 0.95) << QPointF(0.49, 0.75) << QPointF(0.50, 0.58);
            program.operations.append(stem);

            KisAiStrokeOperation contour;
            contour.kind = KisAiStrokeOperation::Kind::Path;
            contour.id = QStringLiteral("petal_contour");
            contour.layer = QStringLiteral("Lineart");
            contour.brush.profile = QStringLiteral("gpen");
            contour.brush.color = QColor(QStringLiteral("#5c1422"));
            contour.brush.size = 0.003;
            contour.points << KisAiStrokePoint(0.40, 0.35, 0.8) << KisAiStrokePoint(0.50, 0.22, 0.9)
                           << KisAiStrokePoint(0.60, 0.35, 0.8) << KisAiStrokePoint(0.68, 0.48, 0.7)
                           << KisAiStrokePoint(0.50, 0.60, 0.8) << KisAiStrokePoint(0.32, 0.48, 0.7);
            contour.closed = true;
            program.operations.append(contour);
        }

        // 4. Highlights: Dewdrops
        {
            KisAiStrokeOperation dew;
            dew.kind = KisAiStrokeOperation::Kind::Path;
            dew.id = QStringLiteral("dewdrop");
            dew.layer = QStringLiteral("Highlights");
            dew.brush.profile = QStringLiteral("gpen");
            dew.brush.color = QColor(QStringLiteral("#ffffff"));
            dew.brush.size = 0.003;
            dew.points << KisAiStrokePoint(0.45, 0.30, 1.0) << KisAiStrokePoint(0.46, 0.31, 1.0);
            program.operations.append(dew);
        }

        // 5. FX: Floating pollen particles
        {
            KisAiStrokeOperation pollen;
            pollen.kind = KisAiStrokeOperation::Kind::Particles;
            pollen.id = QStringLiteral("pollen");
            pollen.layer = QStringLiteral("FX");
            pollen.brush.color = QColor(QStringLiteral("#ffe5b4"));
            pollen.bounds = QRectF(0.15, 0.15, 0.70, 0.70);
            pollen.particleCount = 20;
            pollen.particleShape = QStringLiteral("petal");
            program.operations.append(pollen);
        }

        return refineForRendering(program);
    }
    else if (spec.domain == KisAiPromptAnalyzer::DomainType::Creature) {
        // 0. Background: Misty dragon cavern
        {
            KisAiStrokeOperation bg;
            bg.kind = KisAiStrokeOperation::Kind::GradientFill;
            bg.id = QStringLiteral("creature_bg");
            bg.layer = QStringLiteral("Background");
            bg.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
            bg.gradientColors << QColor(QStringLiteral("#18141c")) << QColor(QStringLiteral("#2d1822"));
            bg.angleDeg = 90.0;
            program.operations.append(bg);
        }

        // 1. Flats: Creature body & wing silhouette
        {
            KisAiStrokeOperation wing;
            wing.kind = KisAiStrokeOperation::Kind::Fill;
            wing.id = QStringLiteral("creature_wing");
            wing.layer = QStringLiteral("Flats");
            wing.brush.color = QColor(QStringLiteral("#381822"));
            wing.polygon << QPointF(0.48, 0.45) << QPointF(0.18, 0.22) << QPointF(0.08, 0.38) << QPointF(0.24, 0.52);
            program.operations.append(wing);

            KisAiStrokeOperation body;
            body.kind = KisAiStrokeOperation::Kind::Fill;
            body.id = QStringLiteral("creature_body");
            body.layer = QStringLiteral("Flats");
            body.brush.color = QColor(QStringLiteral("#4a1d28"));
            body.polygon << QPointF(0.42, 0.32) << QPointF(0.58, 0.32) << QPointF(0.68, 0.52)
                         << QPointF(0.55, 0.78) << QPointF(0.40, 0.78) << QPointF(0.35, 0.52);
            program.operations.append(body);
        }

        // 2. Shading: Scale cross-hatch shading
        {
            KisAiStrokeOperation scaleHatch;
            scaleHatch.kind = KisAiStrokeOperation::Kind::Hatch;
            scaleHatch.id = QStringLiteral("scale_hatch");
            scaleHatch.layer = QStringLiteral("Shading");
            scaleHatch.brush.color = QColor(QStringLiteral("#1e0a12"));
            scaleHatch.angleDeg = 45.0;
            scaleHatch.spacing = 0.012;
            scaleHatch.crossHatch = true;
            scaleHatch.polygon << QPointF(0.42, 0.38) << QPointF(0.58, 0.38) << QPointF(0.55, 0.78) << QPointF(0.40, 0.78);
            program.operations.append(scaleHatch);
        }

        // 3. Lineart: Horns ribbon & claw paths
        {
            KisAiStrokeOperation horn;
            horn.kind = KisAiStrokeOperation::Kind::Ribbon;
            horn.id = QStringLiteral("creature_horn");
            horn.layer = QStringLiteral("Lineart");
            horn.brush.color = QColor(QStringLiteral("#1a080e"));
            horn.widthStart = 0.030;
            horn.widthMid = 0.016;
            horn.widthEnd = 0.002;
            horn.spine << QPointF(0.46, 0.34) << QPointF(0.38, 0.22) << QPointF(0.32, 0.12);
            program.operations.append(horn);

            KisAiStrokeOperation spineContour;
            spineContour.kind = KisAiStrokeOperation::Kind::Path;
            spineContour.id = QStringLiteral("spine_contour");
            spineContour.layer = QStringLiteral("Lineart");
            spineContour.brush.profile = QStringLiteral("gpen");
            spineContour.brush.color = QColor(QStringLiteral("#1a080e"));
            spineContour.brush.size = 0.004;
            spineContour.points << KisAiStrokePoint(0.32, 0.12, 0.4) << KisAiStrokePoint(0.42, 0.32, 0.9)
                                << KisAiStrokePoint(0.35, 0.52, 0.8) << KisAiStrokePoint(0.40, 0.78, 0.9);
            program.operations.append(spineContour);
        }

        // 4. Highlights: Glowing creature eye & horn tip
        {
            KisAiStrokeOperation eyeGlow;
            eyeGlow.kind = KisAiStrokeOperation::Kind::Path;
            eyeGlow.id = QStringLiteral("creature_eye");
            eyeGlow.layer = QStringLiteral("Highlights");
            eyeGlow.brush.profile = QStringLiteral("neon");
            eyeGlow.brush.color = QColor(QStringLiteral("#ffaa00"));
            eyeGlow.brush.size = 0.005;
            eyeGlow.points << KisAiStrokePoint(0.48, 0.36, 1.0) << KisAiStrokePoint(0.52, 0.36, 1.0);
            program.operations.append(eyeGlow);
        }

        // 5. FX: Dragon fire sparks
        {
            KisAiStrokeOperation sparks;
            sparks.kind = KisAiStrokeOperation::Kind::Particles;
            sparks.id = QStringLiteral("fire_sparks");
            sparks.layer = QStringLiteral("FX");
            sparks.brush.color = QColor(QStringLiteral("#ff5500"));
            sparks.bounds = QRectF(0.20, 0.20, 0.60, 0.60);
            sparks.particleCount = 26;
            sparks.particleShape = QStringLiteral("sparkle");
            program.operations.append(sparks);
        }

        return refineForRendering(program);
    }
    else if (spec.domain == KisAiPromptAnalyzer::DomainType::MangaFx) {
        // 0. Background: Dynamic comic halftone backdrop
        {
            KisAiStrokeOperation bg;
            bg.kind = KisAiStrokeOperation::Kind::GradientFill;
            bg.id = QStringLiteral("manga_bg");
            bg.layer = QStringLiteral("Background");
            bg.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
            bg.gradientColors << QColor(QStringLiteral("#ffffff")) << QColor(QStringLiteral("#d8d8d8"));
            bg.isRadial = true;
            bg.gradientCenter = QPointF(0.50, 0.50);
            bg.gradientRadius = 0.85;
            program.operations.append(bg);
        }

        // 1. Flats: Impact center silhouette
        {
            KisAiStrokeOperation impact;
            impact.kind = KisAiStrokeOperation::Kind::Fill;
            impact.id = QStringLiteral("impact_base");
            impact.layer = QStringLiteral("Flats");
            impact.brush.color = QColor(QStringLiteral("#181818"));
            impact.polygon << QPointF(0.42, 0.44) << QPointF(0.50, 0.38) << QPointF(0.58, 0.44)
                           << QPointF(0.56, 0.56) << QPointF(0.44, 0.56);
            program.operations.append(impact);
        }

        // 2. Shading: Intense cross-hatch shading
        {
            KisAiStrokeOperation hatch;
            hatch.kind = KisAiStrokeOperation::Kind::Hatch;
            hatch.id = QStringLiteral("manga_shading");
            hatch.layer = QStringLiteral("Shading");
            hatch.brush.color = QColor(QStringLiteral("#0f0f0f"));
            hatch.angleDeg = 45.0;
            hatch.spacing = 0.008;
            hatch.crossHatch = true;
            hatch.polygon << QPointF(0.40, 0.42) << QPointF(0.60, 0.42) << QPointF(0.58, 0.58) << QPointF(0.42, 0.58);
            program.operations.append(hatch);
        }

        // 3. Lineart: Bold dynamic G-pen action lines
        {
            KisAiStrokeOperation actionRibbon;
            actionRibbon.kind = KisAiStrokeOperation::Kind::Ribbon;
            actionRibbon.id = QStringLiteral("impact_ribbon");
            actionRibbon.layer = QStringLiteral("Lineart");
            actionRibbon.brush.color = QColor(QStringLiteral("#000000"));
            actionRibbon.widthStart = 0.035;
            actionRibbon.widthMid = 0.020;
            actionRibbon.widthEnd = 0.003;
            actionRibbon.spine << QPointF(0.15, 0.85) << QPointF(0.38, 0.62) << QPointF(0.50, 0.50);
            program.operations.append(actionRibbon);

            KisAiStrokeOperation slash;
            slash.kind = KisAiStrokeOperation::Kind::Path;
            slash.id = QStringLiteral("slash_contour");
            slash.layer = QStringLiteral("Lineart");
            slash.brush.profile = QStringLiteral("gpen");
            slash.brush.color = QColor(QStringLiteral("#050505"));
            slash.brush.size = 0.005;
            slash.points << KisAiStrokePoint(0.20, 0.25, 0.9) << KisAiStrokePoint(0.50, 0.50, 1.0) << KisAiStrokePoint(0.80, 0.75, 0.9);
            program.operations.append(slash);
        }

        // 4. Highlights: Pure white flash cut
        {
            KisAiStrokeOperation flash;
            flash.kind = KisAiStrokeOperation::Kind::Path;
            flash.id = QStringLiteral("impact_flash");
            flash.layer = QStringLiteral("Highlights");
            flash.brush.profile = QStringLiteral("gpen");
            flash.brush.color = QColor(QStringLiteral("#ffffff"));
            flash.brush.size = 0.006;
            flash.points << KisAiStrokePoint(0.48, 0.50, 1.0) << KisAiStrokePoint(0.52, 0.50, 1.0);
            program.operations.append(flash);
        }

        // 5. FX: Full radial Manga speed lines
        {
            KisAiStrokeOperation mangaLines;
            mangaLines.kind = KisAiStrokeOperation::Kind::MangaLines;
            mangaLines.id = QStringLiteral("radial_speed_lines");
            mangaLines.layer = QStringLiteral("FX");
            mangaLines.brush.profile = QStringLiteral("gpen");
            mangaLines.brush.color = QColor(QStringLiteral("#111111"));
            mangaLines.brush.size = 0.0025;
            mangaLines.gradientCenter = QPointF(0.50, 0.50);
            mangaLines.innerRadius = 0.15;
            mangaLines.outerRadius = 0.85;
            mangaLines.density = 64;
            mangaLines.lineLengthJitter = 0.30;
            program.operations.append(mangaLines);
        }

        return refineForRendering(program);
    }

    // =========================================================================
    // LANDSCAPE / NATURE / GENERAL PROCEDURAL SCENERY
    // =========================================================================

    // 1. Flats: Sky wash with time-of-day gradient
    {
        KisAiStrokeOperation sky;
        sky.kind = KisAiStrokeOperation::Kind::GradientFill;
        sky.id = QStringLiteral("sky_gradient");
        sky.layer = QStringLiteral("Background");
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

bool KisAiStrokeProgramCodec::isVisionModel(const QString &model)
{
    return !model.trimmed().isEmpty();
}

QJsonObject KisAiStrokeProgramCodec::buildGeometryDigest(const KisAiStrokeProgram &program)
{
    QJsonObject digest;
    digest[QStringLiteral("total_operations")] = program.operations.size();

    const QMap<QString, int> layerCounts = countLayerOperations(program);
    QJsonObject layersObj;
    for (auto it = layerCounts.constBegin(); it != layerCounts.constEnd(); ++it) {
        layersObj[it.key()] = it.value();
    }
    digest[QStringLiteral("layer_counts")] = layersObj;

    qreal flatsAreaEst = 0.0;
    QSet<QString> colorSet;
    qreal minX = 1.0, minY = 1.0, maxX = 0.0, maxY = 0.0;
    bool hasGeometry = false;

    for (const KisAiStrokeOperation &op : program.operations) {
        if (op.brush.color.isValid()) {
            colorSet.insert(op.brush.color.name(QColor::HexRgb).toLower());
        }
        for (const QColor &gc : op.gradientColors) {
            if (gc.isValid()) {
                colorSet.insert(gc.name(QColor::HexRgb).toLower());
            }
        }
        const QString normLayer = normalizeLayerName(op.layer);

        if (!op.points.isEmpty()) {
            hasGeometry = true;
            qreal opMinX = 1.0, opMinY = 1.0, opMaxX = 0.0, opMaxY = 0.0;
            for (const KisAiStrokePoint &pt : op.points) {
                opMinX = qMin(opMinX, pt.pos.x());
                opMaxX = qMax(opMaxX, pt.pos.x());
                opMinY = qMin(opMinY, pt.pos.y());
                opMaxY = qMax(opMaxY, pt.pos.y());
            }
            minX = qMin(minX, opMinX);
            minY = qMin(minY, opMinY);
            maxX = qMax(maxX, opMaxX);
            maxY = qMax(maxY, opMaxY);
        }
        if (!op.polygon.isEmpty()) {
            hasGeometry = true;
            qreal opMinX = 1.0, opMinY = 1.0, opMaxX = 0.0, opMaxY = 0.0;
            for (const QPointF &pt : op.polygon) {
                opMinX = qMin(opMinX, pt.x());
                opMaxX = qMax(opMaxX, pt.x());
                opMinY = qMin(opMinY, pt.y());
                opMaxY = qMax(opMaxY, pt.y());
            }
            minX = qMin(minX, opMinX);
            minY = qMin(minY, opMinY);
            maxX = qMax(maxX, opMaxX);
            maxY = qMax(maxY, opMaxY);

            if (normLayer == QLatin1String("Flats") || normLayer == QLatin1String("Background")) {
                const qreal w = qMax<qreal>(0.0, opMaxX - opMinX);
                const qreal h = qMax<qreal>(0.0, opMaxY - opMinY);
                flatsAreaEst += (w * h * 0.7);
            }
        }
    }

    digest[QStringLiteral("flats_coverage_estimated")] = qBound<qreal>(0.0, flatsAreaEst, 1.0);

    QJsonArray palArr;
    int count = 0;
    for (const QString &c : colorSet) {
        palArr.append(c);
        if (++count >= 12) break;
    }
    digest[QStringLiteral("active_palette")] = palArr;

    if (hasGeometry) {
        QJsonObject bbox;
        bbox[QStringLiteral("min_x")] = qBound<qreal>(0.0, minX, 1.0);
        bbox[QStringLiteral("min_y")] = qBound<qreal>(0.0, minY, 1.0);
        bbox[QStringLiteral("max_x")] = qBound<qreal>(0.0, maxX, 1.0);
        bbox[QStringLiteral("max_y")] = qBound<qreal>(0.0, maxY, 1.0);
        digest[QStringLiteral("bounding_box")] = bbox;
    }

    return digest;
}

QJsonObject KisAiStrokeProgramCodec::buildGoalStepPayload(
    const QString &model,
    const QString &prompt,
    const QSize &canvasSize,
    int step,
    int totalSteps,
    const QString &imageBase64,
    const QString &additionalInstruction,
    int strokeBudget,
    const QString &reasoningEffort,
    bool includeVision,
    bool enableStreaming,
    bool enforceJsonFormat,
    qreal temperature,
    qreal topP,
    int maxTokensOverride,
    int artStyle,
    const KisAiStrokeProgram *accumulatedProgram,
    const QString &previousCritique,
    const QString &visionDetail)
{
    const bool reasoning = isReasoningModel(model);
    const bool vision = includeVision && isVisionModel(model) && !imageBase64.trimmed().isEmpty();
    auto spec = KisAiPromptAnalyzer::analyze(prompt, canvasSize);
    if (artStyle > 0 && artStyle <= 5) {
        spec.style = static_cast<KisAiPromptAnalyzer::ArtStyle>(artStyle);
    }
    const QString phaseGuidance = KisAiPromptAnalyzer::generateGoalPhaseGuidance(step, spec, canvasSize, totalSteps);

    QString combinedInstructions = phaseGuidance;
    if (!additionalInstruction.trimmed().isEmpty()) {
        if (!additionalInstruction.contains(phaseGuidance.trimmed())) {
            combinedInstructions += QStringLiteral("\n\n[USER ADDITIONAL FEEDBACK]\n") + additionalInstruction.trimmed();
        } else {
            combinedInstructions = additionalInstruction.trimmed();
        }
    }

    const QString systemText = buildSystemPrompt(canvasSize, prompt, combinedInstructions, artStyle);

    const int geometryBudget = qBound(20, strokeBudget, 2000);
    const int operationTarget = qBound(12, geometryBudget / (totalSteps > 0 ? totalSteps * 3 : 12), 60);

    QString phaseName;
    if (totalSteps <= 2) {
        phaseName = (step == 1) ? QStringLiteral("Flats & Shading Foundation")
                                : QStringLiteral("Lineart, Highlights & Polish");
    } else if (totalSteps == 3) {
        phaseName = (step == 1) ? QStringLiteral("Flats & Background")
                    : (step == 2) ? QStringLiteral("Shading & Lineart")
                                  : QStringLiteral("Highlights & FX Polish");
    } else if (totalSteps == 4) {
        phaseName = (step == 1) ? QStringLiteral("Flats & Background")
                    : (step == 2) ? QStringLiteral("Shading & Ambient Occlusion")
                    : (step == 3) ? QStringLiteral("Lineart & Details")
                                  : QStringLiteral("Highlights & FX Polish");
    } else if (totalSteps == 5) {
        phaseName = (step == 1) ? QStringLiteral("Flats & Background")
                    : (step == 2) ? QStringLiteral("Shading & Ambient Occlusion")
                    : (step == 3) ? QStringLiteral("Lineart & Details")
                    : (step == 4) ? QStringLiteral("Specular Highlights")
                                  : QStringLiteral("FX & Final Polish");
    } else {
        phaseName = (step == 1) ? QStringLiteral("Background Atmosphere")
                    : (step == 2) ? QStringLiteral("Flats & Silhouettes")
                    : (step == 3) ? QStringLiteral("Shading & Ambient Occlusion")
                    : (step == 4) ? QStringLiteral("Lineart & Details")
                    : (step == 5) ? QStringLiteral("Specular Highlights")
                                  : QStringLiteral("FX & Final Polish");
    }

    QJsonObject userObj;
    userObj[QStringLiteral("restated_goal")] = prompt; // Prompt-First principle
    userObj[QStringLiteral("prompt")] = prompt;
    userObj[QStringLiteral("canvas_width")] = canvasSize.width();
    userObj[QStringLiteral("canvas_height")] = canvasSize.height();
    userObj[QStringLiteral("current_step")] = step;
    userObj[QStringLiteral("total_steps")] = totalSteps;
    userObj[QStringLiteral("step_phase")] = phaseName;
    userObj[QStringLiteral("operation_target")] = operationTarget;

    if (accumulatedProgram && !accumulatedProgram->operations.isEmpty()) {
        userObj[QStringLiteral("accumulated_context")] = buildGeometryDigest(*accumulatedProgram);
    }
    if (!previousCritique.trimmed().isEmpty()) {
        userObj[QStringLiteral("previous_step_critique")] = previousCritique.trimmed();
    }

    userObj[QStringLiteral("directive")] = QStringLiteral(
        "Execute Step %1 of %2 in Goal Mode for prompt: '%5'. "
        "Perform your artistic cognitive cycle: "
        "1. [OBSERVE & CRITIQUE]: Inspect the canvas screenshot (if attached) and accumulated geometry. Provide a concise 1-2 sentence 'agent_critique' analyzing depth, silhouettes, anatomical harmony, and missing elements. "
        "2. [FOCUS]: Specify 'target_focus_area' (e.g. 'Face & Expression', 'Hair Strands & Volume', 'Form Shading & Ambient Occlusion', 'Specular Highlights & Atmosphere'). "
        "3. [READINESS EVALUATION]: Provide 'readiness_score' from 0.0 (bare outline) to 1.0 (finished presentation). If >= 0.85 and presentation-ready, set 'goal_reached' to true. "
        "4. [ACT]: Generate only the necessary, high-precision operations for phase '%3'. Set 'step_phase' to '%3', 'current_step' to %1, and 'goal_reached' to %4. "
        "Output strictly valid RFC 8259 JSON without markdown fences.")
        .arg(step)
        .arg(totalSteps)
        .arg(phaseName)
        .arg(step >= totalSteps ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(prompt);

    const QString userText = QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact));

    QJsonArray messages;
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});

    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    if (vision && !imageBase64.trimmed().isEmpty()) {
        QJsonArray contentArray;
        contentArray.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), userText}
        });
        QString imageUrl = imageBase64.trimmed();
        if (!imageUrl.startsWith(QLatin1String("data:image/"))) {
            imageUrl = QStringLiteral("data:image/jpeg;base64,") + imageUrl;
        }

        QString detail = visionDetail.trimmed().toLower();
        if (detail.isEmpty() || detail == QLatin1String("auto")) {
            detail = (step >= totalSteps) ? QStringLiteral("high") : QStringLiteral("low");
        }

        contentArray.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("image_url")},
            {QStringLiteral("image_url"), QJsonObject{
                {QStringLiteral("url"), imageUrl},
                {QStringLiteral("detail"), detail}
            }}
        });
        userMsg[QStringLiteral("content")] = contentArray;
    } else {
        userMsg[QStringLiteral("content")] = userText;
    }
    messages.append(userMsg);

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.trimmed();
    payload[QStringLiteral("messages")] = messages;
    payload[QStringLiteral("seed")] = static_cast<int>(stableSeed(prompt.simplified()));

    if (enableStreaming) {
        payload[QStringLiteral("stream")] = true;
    }

    if (enforceJsonFormat) {
        if (supportsJsonSchema(model)) {
            QJsonObject schemaObj;
            schemaObj[QStringLiteral("name")] = QStringLiteral("stroke_program");
            schemaObj[QStringLiteral("strict")] = true;
            schemaObj[QStringLiteral("schema")] = strokeProgramJsonSchema();

            QJsonObject responseFormat;
            responseFormat[QStringLiteral("type")] = QStringLiteral("json_schema");
            responseFormat[QStringLiteral("json_schema")] = schemaObj;
            payload[QStringLiteral("response_format")] = responseFormat;
        } else {
            QJsonObject responseFormat;
            responseFormat[QStringLiteral("type")] = QStringLiteral("json_object");
            payload[QStringLiteral("response_format")] = responseFormat;
        }
    }

    const int calculatedTokens = maxTokensOverride > 0
        ? maxTokensOverride
        : qBound(4096, operationTarget * 160 + (reasoning ? 12288 : 2560), reasoning ? 32768 : 16384);
    if (reasoning) {
        payload[QStringLiteral("max_completion_tokens")] = calculatedTokens;
        if (!reasoningEffort.isEmpty() && reasoningEffort.toLower() != QLatin1String("none")) {
            payload[QStringLiteral("reasoning_effort")] = reasoningEffort.toLower();
        }
    } else {
        payload[QStringLiteral("max_tokens")] = calculatedTokens;
        payload[QStringLiteral("temperature")] = qBound<qreal>(0.0, temperature, 2.0);
        if (topP < 1.0) {
            payload[QStringLiteral("top_p")] = qBound<qreal>(0.05, topP, 1.0);
        }
    }

    return payload;
}

KisAiStrokeProgram KisAiStrokeProgramCodec::createDeterministicProgramStep(
    const QString &prompt,
    const QSize &canvasSize,
    int step,
    int totalSteps)
{
    const KisAiStrokeProgram full = createDeterministicProgram(prompt, canvasSize);
    KisAiStrokeProgram stepProg;
    stepProg.prompt = prompt;
    stepProg.title = QStringLiteral("%1 [Step %2/%3]").arg(full.title).arg(step).arg(totalSteps);
    stepProg.canvasSize = canvasSize;
    stepProg.seed = full.seed;
    stepProg.currentStep = step;
    stepProg.totalSteps = totalSteps;
    stepProg.goalReached = (step >= totalSteps);

    for (const KisAiStrokeOperation &op : full.operations) {
        const QString l = normalizeLayerName(op.layer);
        bool match = false;
        if (totalSteps <= 2) {
            if (step == 1) {
                match = (l == QLatin1String("Background") || l == QLatin1String("Flats") || l == QLatin1String("Shading"));
            } else {
                match = (l == QLatin1String("Lineart") || l == QLatin1String("Highlights") || l == QLatin1String("FX"));
            }
        } else if (totalSteps == 3) {
            if (step == 1) {
                match = (l == QLatin1String("Background") || l == QLatin1String("Flats"));
            } else if (step == 2) {
                match = (l == QLatin1String("Shading") || l == QLatin1String("Lineart"));
            } else {
                match = (l == QLatin1String("Highlights") || l == QLatin1String("FX"));
            }
        } else if (totalSteps == 4) {
            if (step == 1) {
                match = (l == QLatin1String("Background") || l == QLatin1String("Flats"));
            } else if (step == 2) {
                match = (l == QLatin1String("Shading"));
            } else if (step == 3) {
                match = (l == QLatin1String("Lineart"));
            } else {
                match = (l == QLatin1String("Highlights") || l == QLatin1String("FX"));
            }
        } else if (totalSteps == 5) {
            if (step == 1) {
                match = (l == QLatin1String("Background") || l == QLatin1String("Flats"));
            } else if (step == 2) {
                match = (l == QLatin1String("Shading"));
            } else if (step == 3) {
                match = (l == QLatin1String("Lineart"));
            } else if (step == 4) {
                match = (l == QLatin1String("Highlights"));
            } else {
                match = (l == QLatin1String("FX"));
            }
        } else { // 6 or more
            if (step == 1) {
                match = (l == QLatin1String("Background"));
            } else if (step == 2) {
                match = (l == QLatin1String("Flats"));
            } else if (step == 3) {
                match = (l == QLatin1String("Shading"));
            } else if (step == 4) {
                match = (l == QLatin1String("Lineart"));
            } else if (step == 5) {
                match = (l == QLatin1String("Highlights"));
            } else {
                match = (l == QLatin1String("FX"));
            }
        }
        if (match) {
            stepProg.operations.append(op);
        }
    }

    if (totalSteps <= 2) {
        if (step == 1) {
            stepProg.stepPhase = QStringLiteral("Flats & Shading Foundation");
            stepProg.visualCritique = QStringLiteral("Base silhouettes, flats, and volume blocking are established.");
        } else {
            stepProg.stepPhase = QStringLiteral("Lineart, Highlights & Final FX");
            stepProg.visualCritique = QStringLiteral("Contour lineart, highlights, and final FX polish completed. Goal reached.");
        }
    } else if (totalSteps == 3) {
        if (step == 1) {
            stepProg.stepPhase = QStringLiteral("Flats & Background");
            stepProg.visualCritique = QStringLiteral("Base silhouettes and background wash are established.");
        } else if (step == 2) {
            stepProg.stepPhase = QStringLiteral("Shading & Lineart");
            stepProg.visualCritique = QStringLiteral("Volumetric shading and contour lineart are rendered.");
        } else {
            stepProg.stepPhase = QStringLiteral("Highlights & FX Polish");
            stepProg.visualCritique = QStringLiteral("Specular highlights and FX particles applied. Goal reached.");
        }
    } else if (totalSteps == 4) {
        if (step == 1) {
            stepProg.stepPhase = QStringLiteral("Flats & Background");
            stepProg.visualCritique = QStringLiteral("Base silhouettes and background wash are established.");
        } else if (step == 2) {
            stepProg.stepPhase = QStringLiteral("Shading & Ambient Occlusion");
            stepProg.visualCritique = QStringLiteral("Volumetric shading and cast shadows are rendered.");
        } else if (step == 3) {
            stepProg.stepPhase = QStringLiteral("Lineart & Details");
            stepProg.visualCritique = QStringLiteral("Contour lineart and anatomical details are completed.");
        } else {
            stepProg.stepPhase = QStringLiteral("Highlights & FX Polish");
            stepProg.visualCritique = QStringLiteral("Specular highlights and FX particles applied. Goal reached.");
        }
    } else if (totalSteps == 5) {
        if (step == 1) {
            stepProg.stepPhase = QStringLiteral("Flats & Background");
            stepProg.visualCritique = QStringLiteral("Base silhouettes and background wash are established.");
        } else if (step == 2) {
            stepProg.stepPhase = QStringLiteral("Shading & Ambient Occlusion");
            stepProg.visualCritique = QStringLiteral("Volumetric shading and cast shadows are rendered.");
        } else if (step == 3) {
            stepProg.stepPhase = QStringLiteral("Lineart & Details");
            stepProg.visualCritique = QStringLiteral("Contour lineart and anatomical details are completed.");
        } else if (step == 4) {
            stepProg.stepPhase = QStringLiteral("Specular Highlights");
            stepProg.visualCritique = QStringLiteral("Catchlights, rim lighting, and specular glints are rendered.");
        } else {
            stepProg.stepPhase = QStringLiteral("FX & Final Polish");
            stepProg.visualCritique = QStringLiteral("Floating particles, atmospheric effects, and polish applied. Goal reached.");
        }
    } else { // 6 or more
        if (step == 1) {
            stepProg.stepPhase = QStringLiteral("Background Atmosphere");
            stepProg.visualCritique = QStringLiteral("Atmospheric backdrop and sky gradient established.");
        } else if (step == 2) {
            stepProg.stepPhase = QStringLiteral("Flats & Silhouettes");
            stepProg.visualCritique = QStringLiteral("Base color silhouettes established without white canvas gaps.");
        } else if (step == 3) {
            stepProg.stepPhase = QStringLiteral("Shading & Ambient Occlusion");
            stepProg.visualCritique = QStringLiteral("Volumetric shading and cast shadows are rendered.");
        } else if (step == 4) {
            stepProg.stepPhase = QStringLiteral("Lineart & Details");
            stepProg.visualCritique = QStringLiteral("Contour lineart and anatomical details are completed.");
        } else if (step == 5) {
            stepProg.stepPhase = QStringLiteral("Specular Highlights");
            stepProg.visualCritique = QStringLiteral("Catchlights, rim lighting, and specular glints are rendered.");
        } else {
            stepProg.stepPhase = QStringLiteral("FX & Final Polish");
            stepProg.visualCritique = QStringLiteral("Manga lines, particles, and polish applied. Goal reached.");
        }
    }

    if (stepProg.operations.isEmpty() && !full.operations.isEmpty()) {
        stepProg.operations.append(full.operations.first());
    }

    KisAiStrokeQualityReport report;
    return refineForRendering(stepProg, &report);
}

KisAiStrokeProgram KisAiStrokeProgramCodec::mergePrograms(
    const KisAiStrokeProgram &base,
    const KisAiStrokeProgram &extension)
{
    KisAiStrokeProgram merged = base;
    if (merged.canvasSize.isEmpty() || !merged.canvasSize.isValid()) {
        merged.canvasSize = extension.canvasSize;
    }
    if (merged.prompt.isEmpty()) {
        merged.prompt = extension.prompt;
    }
    if (merged.title.isEmpty() || merged.title == QLatin1String("AI Artwork")) {
        merged.title = extension.title;
    }

    merged.operations.append(extension.operations);
    merged.currentStep = qMax(base.currentStep, extension.currentStep);
    merged.totalSteps = qMax(base.totalSteps, extension.totalSteps);
    if (!extension.stepPhase.isEmpty()) {
        merged.stepPhase = extension.stepPhase;
    }
    if (!extension.visualCritique.isEmpty()) {
        merged.visualCritique = extension.visualCritique;
    }
    merged.goalReached = merged.totalSteps > 1 ? merged.currentStep >= merged.totalSteps : extension.goalReached;

    KisAiStrokeQualityReport report;
    return refineForRendering(merged, &report);
}
