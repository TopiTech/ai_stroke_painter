/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeProgram.h"
#include "KisAiLayoutEngine.h"
#include "KisAiModelRouter.h"
#include "KisAiPromptAnalyzer.h"
#include "KisAiSceneSpec.h"
#include "KisAiStrokeTypeChecker.h"
#include "KisAiStrokeQualityUtils.h"
#include "KisAiRigLibrary.h"


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
#include <atomic>
#include <cmath>
#include <limits>

namespace
{
// Threshold above which coordinates are interpreted as pixel values
// rather than normalized [0.0, 1.0] values.
constexpr qreal kPixelCoordinateThreshold = 1.5;

// V3 Phase 0.1: Particle suppression policy state. Enabled by default so
// headless / test pipelines also benefit; the Docker checkbox toggles it.
// Atomic because the Docker writes it from UI slots while refineForRendering()
// and mergePrograms() read it from render/worker paths.
std::atomic<bool> g_particleSuppressionEnabled{true};
constexpr int kMaxParticlesOperations = 3;
constexpr int kMaxMergedParticlesOperations = 2;

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
QString extractContentStringFromMessage(const QJsonObject &messageObj, const QJsonObject &choiceObj = QJsonObject())
{
    const QJsonValue contentVal = messageObj.value(QStringLiteral("content"));
    if (contentVal.isString()) {
        const QString s = contentVal.toString();
        if (!s.trimmed().isEmpty()) {
            return s;
        }
    } else if (contentVal.isArray()) {
        QString combined;
        const QJsonArray arr = contentVal.toArray();
        for (const QJsonValue &item : arr) {
            if (item.isObject()) {
                const QJsonObject obj = item.toObject();
                const QString type = obj.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("text") || type.isEmpty()) {
                    combined.append(obj.value(QStringLiteral("text")).toString());
                }
            } else if (item.isString()) {
                combined.append(item.toString());
            }
        }
        if (!combined.trimmed().isEmpty()) {
            return combined;
        }
    }

    // Tool calls fallback (e.g. function calling responses where model outputs the json payload as arguments)
    if (messageObj.contains(QStringLiteral("tool_calls"))) {
        const QJsonArray toolCalls = messageObj.value(QStringLiteral("tool_calls")).toArray();
        for (const QJsonValue &tcVal : toolCalls) {
            const QJsonObject tc = tcVal.toObject();
            const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
            const QString args = fn.value(QStringLiteral("arguments")).toString().trimmed();
            if (!args.isEmpty()) {
                return args;
            }
        }
    }

    // Legacy completions text field fallback
    if (choiceObj.contains(QStringLiteral("text"))) {
        const QString text = choiceObj.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) {
            return text;
        }
    }

    return QString();
}
} // namespace

bool KisAiStrokeProgramCodec::isReasoningModel(const QString &model)
{
    const QString lower = model.toLower().trimmed();
    // "o1"/"o3"/"o4" must match as a family prefix (o1, o1-mini, o3-mini, ...) but
    // not as an arbitrary substring (proto1, radio3). "dots"/"note" name a
    // specific provider family (e.g. dots-3-note-preview), so they must match
    // as hyphen-delimited tokens — not as substrings of "denotes"/"notebook".
    const auto matchesFamily = [&lower](const char *family) {
        const QString f = QLatin1String(family);
        if (lower == f || lower.startsWith(f + QLatin1Char('-')) || lower.startsWith(f + QLatin1Char('/'))) {
            return true;
        }
        int idx = lower.indexOf(f + QLatin1Char('-'));
        while (idx > 0) {
            const QChar prev = lower.at(idx - 1);
            if (prev == QLatin1Char('/') || prev == QLatin1Char('-') || prev == QLatin1Char(':')
                || prev == QLatin1Char('_')) {
                return true;
            }
            idx = lower.indexOf(f + QLatin1Char('-'), idx + 1);
        }
        return false;
    };
    return matchesFamily("o1") || matchesFamily("o3") || matchesFamily("o4")
        || matchesFamily("gpt-5")
        || lower.contains(QLatin1String("claude-3-7"))
        || lower.contains(QLatin1String("claude-3.7"))
        || lower.contains(QLatin1String("claude-5"))
        || lower.contains(QLatin1String("deepseek-r1"))
        || lower.contains(QLatin1String("deepseek-reasoner"))
        || lower.contains(QLatin1String("deepseek-v4"))
        || lower.contains(QLatin1String("thinking"))
        || lower.contains(QLatin1String("reasoner"))
        || lower.contains(QLatin1String("qwq"))
        || matchesFamily("dots") || matchesFamily("note")
        || lower.contains(QLatin1String("r1-distill"));
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

QJsonObject KisAiStrokeProgramCodec::sceneSpecJsonSchema()
{
    return KisAiSceneSpecCodec::sceneSpecJsonSchema();
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
                                                              QStringLiteral("manga_lines"),
                                                              QStringLiteral("anime_eye"),
                                                              QStringLiteral("anime_mouth")}}};
    opProps[QStringLiteral("id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("layer")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("blend_mode")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                        {QStringLiteral("enum"),
                                                         QJsonArray{QStringLiteral("normal"),
                                                                    QStringLiteral("multiply"),
                                                                    QStringLiteral("screen"),
                                                                    QStringLiteral("color_dodge"),
                                                                    QStringLiteral("overlay"),
                                                                    QStringLiteral("linear_burn"),
                                                                    QStringLiteral("add")}}};
    opProps[QStringLiteral("clip_to_id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
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
    opProps[QStringLiteral("fill_profile")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                    {QStringLiteral("enum"),
                     QJsonArray{QStringLiteral("flat"), QStringLiteral("watercolor"), QStringLiteral("gradient")}}};
    opProps[QStringLiteral("is_shading")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    opProps[QStringLiteral("shading_type")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("shading_intensity")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                                                               {QStringLiteral("minimum"), 0.0},
                                                               {QStringLiteral("maximum"), 1.0}};
    opProps[QStringLiteral("size")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                  {QStringLiteral("items"), pointItem},
                                                  {QStringLiteral("minItems"), 2},
                                                  {QStringLiteral("maxItems"), 2}};
    opProps[QStringLiteral("iris_color")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("secondary_color")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("lip_color")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    opProps[QStringLiteral("has_highlight")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
    opProps[QStringLiteral("expression")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                        {QStringLiteral("enum"),
                                                         QJsonArray{QStringLiteral("open"),
                                                                    QStringLiteral("smile"),
                                                                    QStringLiteral("half"),
                                                                    QStringLiteral("closed"),
                                                                    QStringLiteral("wink"),
                                                                    QStringLiteral("open_smile"),
                                                                    QStringLiteral("small_open"),
                                                                    QStringLiteral("closed_line"),
                                                                    QStringLiteral("cat_mouth"),
                                                                    QStringLiteral("pout")}}};
    opProps[QStringLiteral("is_right")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
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
    rootProps[QStringLiteral("agent_critique")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("target_focus_area")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    rootProps[QStringLiteral("readiness_score")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                                                               {QStringLiteral("minimum"), 0.0},
                                                               {QStringLiteral("maximum"), 1.0}};

    QJsonObject regionItem;
    regionItem[QStringLiteral("type")] = QStringLiteral("object");
    QJsonObject regProps;
    regProps[QStringLiteral("area")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    regProps[QStringLiteral("issue")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    regProps[QStringLiteral("action")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
    regProps[QStringLiteral("priority")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                                       {QStringLiteral("minimum"), 1},
                                                       {QStringLiteral("maximum"), 5}};
    regionItem[QStringLiteral("properties")] = regProps;
    rootProps[QStringLiteral("critique_regions")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), regionItem}};
    rootProps[QStringLiteral("regions")] =
        QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), regionItem}};
    rootProps[QStringLiteral("operations")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                          {QStringLiteral("items"), opItem},
                                                          {QStringLiteral("minItems"), 1},
                                                          {QStringLiteral("maxItems"), 500}};

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
        "2. Double quotes only: all object keys and string values must use standard double quotes (\"), never single "
        "quotes or backticks.\n"
        "3. No comments: never include JavaScript comments (// or /* */) anywhere in the output.\n"
        "4. No Python literals: use true, false, and null (all lowercase) instead of True, False, None.\n"
        "5. No ellipses or placeholders: never write '...' or placeholder entries; emit complete geometry only.\n"
        "6. Valid numbers only: coordinates must be standard decimal numbers (e.g. 0.5, -0.1). Never output NaN, "
        "Infinity, or unit suffixes (no 'px', 'deg', '%').\n"
        "7. Complete JSON: budget your points and operations so your output completes fully before reaching token "
        "limits.\n"
        "8. Brush size is in (0.0, 1.0]: values outside this range are invalid and will cause type-check failures.");
}

QString KisAiStrokeProgramCodec::buildCoordinateSection(const QSize &canvasSize)
{
    const qreal aspect = canvasSize.height() > 0 ? qreal(canvasSize.width()) / canvasSize.height() : 1.0;
    return QStringLiteral(
               "=== COORDINATE SYSTEM & RESOLUTION ===\n"
               "Coordinates are normalized float numbers strictly in [0.0, 1.0]. (0.0, 0.0) is top-left, (1.0, 1.0) is "
               "bottom-right.\n"
               "Canvas size: %1x%2 (Aspect %3:1).")
        .arg(canvasSize.width())
        .arg(canvasSize.height())
        .arg(QString::number(aspect, 'f', 2));
}

QString KisAiStrokeProgramCodec::buildLayerSemanticsSection()
{
    return QStringLiteral(
        "=== LAYER ARCHITECTURE & COMPOSITION (Back-to-Front) ===\n"
        "1. 'Background': Far distance, atmosphere, environment washes, and depth setting (rendered behind subjects; "
        "NOT clipped).\n"
        "2. 'Flats': Volumetric subject mass & local color blocking (skin, hair, clothing, props). Establish solid 3D "
        "plane volumes, NOT flat paper silhouettes.\n"
        "3. 'Shading': True 3D volumetric shadows (Multiply blend, clipped to Flats).\n"
        "   - Tier 1 Form Shadows: Soft curvature transitions across rounded forms (face planes, torso, fabric folds) "
        "using watercolor/brush with wash/directional style.\n"
        "   - Tier 2 Cast Shadows: Crisp occlusion shadow edges under hair bangs, jawline, collar, and deep drapery.\n"
        "   - Tier 3 Ambient Occlusion (AO): Deep crevice shading in overlapping corners and contact seams.\n"
        "4. 'Lineart': Exquisite master inking. Every line drawn with deliberate care, natural S/C-curve flow, and "
        "pressure nuance.\n"
        "   - Facial micro-contours (eyelashes, double eyelids, iris rims, subtle nose bridge, delicate lip "
        "contours).\n"
        "   - Flowing hair strands, tapered locks, dynamic clothing seams, and anatomical contours.\n"
        "5. 'Highlights': Specular glints, vital eye catchlights, hair halo luster, and rim lighting (Screen or Color "
        "Dodge blend, clipped to Flats).\n"
        "   - Use 'blend_mode': 'color_dodge' for intense luminous specular accents, magical glows, eye glints, and "
        "hair luster rings.\n"
        "   - Use 'clip_to_id': '<target_op_id>' to strictly clip a shadow or highlight to an underlying silhouette "
        "(e.g., hair shadow cast strictly onto face skin).\n"
        "6. 'FX': Atmospheric depth, lighting bloom accents, floating motes/petals, or manga energy lines.");
}

QString KisAiStrokeProgramCodec::buildDrawingWorkflowSection()
{
    return QStringLiteral(
        "=== MASTER DRAWING WORKFLOW (MANDATORY) ===\n"
        "Direct your drawing like a master digital illustrator with artistic autonomy. "
        "Silently audit the target composition, gesture dynamics, and focal hierarchy before laying down strokes:\n"
        "1. Dynamic Staging & Cinematic Framing: Never lock into a stiff, centered passport-photo bust! "
        "Embrace expressive camera angles (subtle dramatic tilt, 3/4 dynamic view, high/low angle, rule of thirds offset). "
        "Incorporate organic gestures (hand touching face, hair fluttering in wind, dynamic shoulder lean).\n"
        "2. Sculpting 3D Form Masses: Establish confident, continuous anatomical silhouettes on 'Flats' (skin, sweeping hair masses, clothing folds). "
        "Assign clean IDs ('face_skin', 'body_base', 'hair_back', 'hair_bangs') to enable strict silhouette clipping (clip_to_id).\n"
        "3. Multi-Tier Shading Depth (Multiply blend): Pair soft curvature form shadows (wash/directional) with crisp occlusion cast shadows "
        "under hair fringe, chin, and drapery seams. Let shadows breathe with warm peach/coral subsurface scattering (SSS) transitions.\n"
        "4. Exquisite Deliberate Inking: Inscribe lineart with organic calligraphic weight hierarchy! Outer structural contours use bold strokes (0.0035-0.0055), "
        "while facial features (eyelashes, double eyelids, lip creases) and tapered hair tips use delicate micro-lines (0.0015-0.0025).\n"
        "5. Vital Highlights & Specular Luminescence: Finish with radiant catchlights in pupils, hair halo luster (screen/color_dodge blend), and rim lighting accents.");
}

QString KisAiStrokeProgramCodec::buildArtisticGuidelinesSection()
{
    return QStringLiteral(
        "=== ARTISTIC & ANATOMICAL GUIDELINES ===\n"
        "- 3D Volume & Form: Treat surfaces as curved planes. Never leave large areas as a single flat unshaded "
        "color.\n"
        "- Anime Shading Harmony: Avoid scattering multiple disconnected polygons across smooth face planes. Use "
        "clean, cohesive cel-shading masses or delicate soft blush. Keep face skin centers clear, smooth, and "
        "luminous.\n"
        "- Dual Shadow Separation: Combine soft 'fill' (style: wash/directional) for facial curvature with sharp "
        "'fill' (style: contour) for hard cast shadows under hair and chin.\n"
        "- Subsurface Color Warmth: Avoid muddy grey/black shading. On skin and warm surfaces, shift shadow hues "
        "toward rich peach, rose, or warm violet to convey blood flow and translucency.\n"
        "- Master Linework Craftsmanship: Never draw coarse 2-3 point zigzags. Use smooth 4-8 point Catmull-Rom "
        "curves. Vary pressure from 0.2 (light flick entry/exit) to 0.9 (heavy grounded crest).\n"
        "- Eye & Mouth Fidelity: For anime characters and portraits, prefer using 'anime_eye' and 'anime_mouth' "
        "operations for facial hero features to achieve sparkling anime irises and gracefully sculpted lips with "
        "corner ink pooling.\n"
        "- Colored Lineart Harmony: Lineart naturally blends with underlying colors (warm coral-brown for skin, deep "
        "harmonic hues for hair), creating soft professional unity.\n"
        "- Solid Hair Masses: Always establish opaque foundational hair volumes on 'Flats' first before drawing "
        "individual strands, preventing transparent or wireframe hair.\n"
        "- Exquisite Facial Landmarks: Dedicate delicate individual strokes for upper lash arcs, double eyelids, iris "
        "rings, pupil cores, and subtle lip creases.\n"
        "- Hair Volume & Strands: Group hair into primary masses, sculpt shadow planes beneath them, and finish with "
        "flowing ribbon strands and tapered flyaways.");
}

QString KisAiStrokeProgramCodec::buildOperationKindsSection()
{
    return QStringLiteral(
        "=== OPERATION KINDS ===\n"
        "- 'gradient_fill': Atmospheric sky, environment, or broad directional light washes. Polygon [ [x, y], ... ], "
        "colors [ '#hex', ... ], "
        "angle_deg (0=horizontal, 90=vertical), is_radial (true/false), center [cx, cy], radius.\n"
        "- 'fill': Volumetric color masses, plane blocking, and form/cast shadows. Polygon [ [x, y], ... ], brush { "
        "'profile': 'watercolor'/'brush'/'marker'/'airbrush', 'color': '#hex' }, style "
        "('wash'/'directional'/'contour'), angle_deg. "
        "Use 'fill_profile': 'watercolor' for genuine wet-edge pigmentation and paper grain texture! When "
        "brush.profile is 'foliage'/'petals' or id contains 'sakura'/'foliage', the engine automatically synthesizes "
        "billowing petal/leaf clusters.\n"
        "- 'path': Exquisite linework, anatomical contours, facial features, hair strands. Points [ [x, y, pressure], "
        "... ] (pressure: 0.1-1.0). "
        "brush { 'profile': 'gpen'/'pencil'/'fineliner'/'maru_pen'/'airbrush'/'watercolor'/'brush', 'color': '#hex', "
        "'size': 0.0015-0.008, opacity: 0.0-1.0 }.\n"
        "- 'ribbon': Tapered organic strokes (hair locks, drapery folds, limbs). Spine [ [x, y], ... ], width_start, "
        "width_mid, width_end (0.004-0.04). "
        "IMPORTANT: When brush.profile is 'hair' or id contains 'hair', the engine automatically procedurally "
        "synthesizes realistic multi-strand hair clumps, flyaways, and luminous halo accents!\n"
        "- 'hatch': Fine technical cross-hatching or manga screentone. Polygon [ [x, y], ... ], angle_deg (0-180), "
        "spacing (0.005-0.02), cross_hatch (true/false).\n"
        "- 'anime_eye': Modern high-fidelity procedural eye assembly (multi-layer iris, limbal ring, emission "
        "crescent, catchlights & bloom). center [cx, cy], size [w, h], iris_color '#hex', secondary_color '#hex', "
        "style ('sparkle'/'dual_dot'/'gradient'), expression ('open'/'smile'/'half'), is_right (true/false).\n"
        "- 'anime_mouth': Modern procedural anime mouth/lip assembly (graceful upper lip inking, corner pooling dots, "
        "subtle teeth/tongue layers, specular lip shine). center [cx, cy], size [w, h], lip_color '#hex', expression "
        "('smile'/'open_smile'/'small_open'/'closed_line'/'cat_mouth'/'pout'), has_highlight (true/false).\n"
        "- 'particles': Atmospheric particles (ONLY when theme calls for it: petals, stars, embers). Bounds [x1, y1, "
        "x2, y2], count (8-24), shape ('petal'/'sparkle'/'star'/'dot').\n"
        "- 'manga_lines': Dynamic focus/speed lines. center [cx, cy], inner_radius, outer_radius, density (16-64).\n"
        "- 'hair_flow_cluster': Flowing hair ribbon cluster with automated multi-strand synthesis. spine [ [x, y], ... ], "
        "brush { 'color': '#hex' }, width_start, width_mid, width_end. Synthesizes main mass, parallel flowing strands, and tapered flyaways.\n"
        "- 'cloth_drapery': Natural cloth tension folds and drapery. origin [x0, y0], target [x1, y1], brush { 'color': '#hex' }, "
        "width (1.0-3.0). Synthesizes delicate lineart crease curves and soft shadow fold washes.\n"
        "- 'hand_gesture': Expressive anime hand assembly. center [cx, cy], size [w, h], gesture "
        "('reach'/'peace'/'open_palm'/'fist'/'touch_face'), brush { 'color': '#hex' }. Synthesizes anatomical palm planes, delicate finger lineart, and cast shadows.\n"
        "- 'dynamic_pose': Cinematic gesture dynamics and speed/air flow lines. center [cx, cy], "
        "pose ('dynamic_lean'/'action'/'floating'/'contrapposto'), brush { 'color': '#hex' }.\n"
        "- 'clip_to_id': Assign to any operation (e.g. shadow or highlight) to strictly clip its rasterization to the "
        "silhouette of a base part (e.g. clip_to_id: 'face_skin').\n"
        "- 'blend_mode': 'color_dodge' for vivid specular luminescence, 'multiply' for true shadows, 'screen' for soft "
        "fog, 'normal' for default.");
}

QString KisAiStrokeProgramCodec::buildOutputSchemaExampleSection()
{
    return QStringLiteral(
        "=== OUTPUT SCHEMA EXAMPLE ===\n"
        "{\n"
        "  \"schema_version\": 2,\n"
        "  \"prompt\": \"masterpiece illustration\",\n"
        "  \"title\": \"Harmonious Artwork\",\n"
        "  \"operations\": [\n"
        "    {\n"
        "      \"kind\": \"gradient_fill\",\n"
        "      \"id\": \"bg_ambience\",\n"
        "      \"layer\": \"Background\",\n"
        "      \"polygon\": [[0.0,0.0],[1.0,0.0],[1.0,1.0],[0.0,1.0]],\n"
        "      \"colors\": [\"#1e293b\", \"#0f172a\"],\n"
        "      \"angle_deg\": 90,\n"
        "      \"brush\": {\"profile\": \"brush\", \"color\": \"#1e293b\", \"size\": 0.05, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"subject_silhouette\",\n"
        "      \"layer\": \"Flats\",\n"
        "      \"polygon\": "
        "[[0.50,0.18],[0.66,0.28],[0.70,0.50],[0.65,0.74],[0.50,0.80],[0.35,0.74],[0.30,0.50],[0.34,0.28]],\n"
        "      \"brush\": {\"profile\": \"brush\", \"color\": \"#f2dcd0\", \"size\": 0.04, \"is_eraser\": false},\n"
        "      \"style\": \"wash\"\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"fill\",\n"
        "      \"id\": \"subject_shadow\",\n"
        "      \"layer\": \"Shading\",\n"
        "      \"clip_to_id\": \"subject_silhouette\",\n"
        "      \"blend_mode\": \"multiply\",\n"
        "      \"polygon\": [[0.50,0.18],[0.66,0.28],[0.70,0.50],[0.65,0.74],[0.50,0.80],[0.52,0.48]],\n"
        "      \"brush\": {\"profile\": \"brush\", \"color\": \"#c89280\", \"size\": 0.03, \"is_eraser\": false},\n"
        "      \"style\": \"wash\"\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"primary_contour\",\n"
        "      \"layer\": \"Lineart\",\n"
        "      \"points\": [[0.34,0.28,0.5],[0.30,0.50,0.85],[0.35,0.74,0.6]],\n"
        "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#1e1428\", \"size\": 0.0035, \"is_eraser\": false}\n"
        "    },\n"
        "    {\n"
        "      \"kind\": \"path\",\n"
        "      \"id\": \"specular_highlight\",\n"
        "      \"layer\": \"Highlights\",\n"
        "      \"clip_to_id\": \"subject_silhouette\",\n"
        "      \"blend_mode\": \"color_dodge\",\n"
        "      \"points\": [[0.36,0.30,0.7],[0.39,0.32,0.9],[0.43,0.31,0.6]],\n"
        "      \"brush\": {\"profile\": \"airbrush\", \"color\": \"#ffffff\", \"size\": 0.008, \"opacity\": 0.85, "
        "\"is_eraser\": false}\n"
        "    }\n"
        "  ]\n"
        "}");
}

QString KisAiStrokeProgramCodec::buildFlagshipDirectives()
{
    return QStringLiteral(
        "=== ADVANCED FLAGSHIP & DELIBERATE ART DIRECTION ===\n"
        "You are operating in High-Precision Flagship Mode. Maximize geometric nuance and painterly depth:\n"
        "1. Master Deliberate Inking & Catmull-Rom Curvature (Single-Stroke Craftsmanship):\n"
        "   - Treat every single stroke as an indispensable work of art. Never rush or output coarse approximations.\n"
        "   - Supply smooth, multi-point coordinate sequences (5 to 12 points) for organic curves rather than coarse 2-point lines.\n"
        "   - Continuous line-weight modulation: feathered entry (pressure 0.10-0.25), confident grounded core (0.70-0.95), and delicate tapered exit (0.05-0.20).\n"
        "   - Corner inking fillets: naturally thicken lines at acute junctions and intersections to simulate physical nib ink pooling.\n"
        "   - STRICTLY FORBIDDEN: Random scribble clusters, jittery jagged zigzag lines, or rough repetitive hatch noise.\n"
        "2. 4-Tier Volumetric Shading Architecture:\n"
        "   - Ambient Environment Wash: Background & broad tone washes setting light atmosphere.\n"
        "   - Tier 1 Form Shading: Smoothly rounded curvature on cheeks, neck, arms, and drapery folds (style: wash/directional).\n"
        "   - Tier 2 Occlusion Cast Shadows: Crisp shadow edges under bangs, nose, jawline, and collar folds (style: contour with clip_to_id).\n"
        "   - Tier 3 Ambient Occlusion (AO): Deep crevices where geometric forms contact, anchor, or overlap.\n"
        "3. Subsurface Scattering (SSS) & Terminator Warmth:\n"
        "   - Along the terminator between light and shadow on skin and warm organic surfaces, introduce warm transition accents (coral, peach, or rose).\n"
        "   - Avoid dead neutral gray or muddy black shading.\n"
        "4. Facial Micro-Anatomy Mastery:\n"
        "   - Use micro-detail precision strokes for eye corners, eyelashes, double eyelids, iris limbal rings, and lips.\n"
        "   - Place eye catchlights precisely on the pupil/iris boundary without blurry spill.\n"
        "5. Hair Clump Architecture:\n"
        "   - Foundation mass on 'Flats' -> underside occlusion shading on 'Shading' -> ribbon spine clumps ('ribbon' with width_start/mid/end) -> delicate flyaways on 'Lineart'.\n"
        "6. Accurate Silhouette Anchoring:\n"
        "   - Consistently specify 'clip_to_id' referencing base silhouette operations so shadows and highlights never bleed outside the target subject.");
}

QVector<KisAiStrokeOperation> KisAiStrokeProgramCodec::expandMacroOperation(const QJsonObject &o,
                                                                          const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> ops;
    const QString kindStr = o.value(QStringLiteral("kind")).toString().trimmed().toLower();
    const QString idStr = o.value(QStringLiteral("id")).toString().trimmed();
    const QString baseId = idStr.isEmpty() ? QStringLiteral("macro") : idStr;
    const qreal canvasW = canvasSize.isValid() && canvasSize.width() > 0 ? canvasSize.width() : 1024.0;
    const qreal canvasH = canvasSize.isValid() && canvasSize.height() > 0 ? canvasSize.height() : 1024.0;

    auto parseCol = [](const QString &str, const QColor &fallback) -> QColor {
        if (str.trimmed().isEmpty()) return fallback;
        QColor c(str.trimmed());
        return c.isValid() ? c : fallback;
    };

    auto parsePt = [canvasW, canvasH](const QJsonValue &pv) -> QPointF {
        if (pv.isArray()) {
            const QJsonArray arr = pv.toArray();
            if (arr.size() >= 2) {
                qreal x = arr.at(0).toDouble();
                qreal y = arr.at(1).toDouble();
                if (qMax(qAbs(x), qAbs(y)) > 2.0) {
                    x /= canvasW;
                    y /= canvasH;
                }
                return QPointF(qBound(0.0, x, 1.0), qBound(0.0, y, 1.0));
            }
        } else if (pv.isObject()) {
            const QJsonObject obj = pv.toObject();
            qreal x = obj.value(QStringLiteral("x")).toDouble();
            qreal y = obj.value(QStringLiteral("y")).toDouble();
            if (qMax(qAbs(x), qAbs(y)) > 2.0) {
                x /= canvasW;
                y /= canvasH;
            }
            return QPointF(qBound(0.0, x, 1.0), qBound(0.0, y, 1.0));
        }
        return QPointF(0.5, 0.5);
    };

    // 1. CLOTH DRAPERY
    if (kindStr.contains(QLatin1String("drapery")) || kindStr.contains(QLatin1String("fold"))) {
        const QPointF origin = parsePt(o.value(QStringLiteral("origin")));
        const QPointF target = parsePt(o.value(QStringLiteral("target")));
        const QJsonObject brushObj = o.value(QStringLiteral("brush")).toObject();
        const QColor clothColor = parseCol(brushObj.value(QStringLiteral("color")).toString(), QColor(50, 60, 80));
        const QColor shadowColor = calculateHueShiftedShadow(clothColor, true);
        const qreal width = qBound<qreal>(1.0, o.value(QStringLiteral("width")).toDouble(2.0), 5.0);
        return KisAiRigLibrary::draperyFoldOps(origin, target, width, clothColor, shadowColor, baseId);
    }

    // 2. HAIR FLOW CLUSTER
    if (kindStr.contains(QLatin1String("hair_flow")) || kindStr.contains(QLatin1String("hair_cluster"))) {
        const QJsonObject brushObj = o.value(QStringLiteral("brush")).toObject();
        const QColor hairColor = parseCol(brushObj.value(QStringLiteral("color")).toString(), QColor(43, 58, 103));
        const QColor inkColor = parseCol(QStringLiteral("#232328"), QColor(35, 35, 40));

        QVector<QPointF> spinePts;
        const QJsonArray spineArr = o.value(QStringLiteral("spine")).toArray();
        for (const auto &sp : spineArr) {
            spinePts.append(parsePt(sp));
        }
        if (spinePts.size() < 2) {
            const QJsonArray ptsArr = o.value(QStringLiteral("points")).toArray();
            for (const auto &sp : ptsArr) {
                spinePts.append(parsePt(sp));
            }
        }
        if (spinePts.size() < 2) {
            const QPointF c = parsePt(o.value(QStringLiteral("center")));
            spinePts.append(QPointF(c.x() - 0.05, c.y() - 0.10));
            spinePts.append(QPointF(c.x(), c.y()));
            spinePts.append(QPointF(c.x() + 0.04, c.y() + 0.12));
        }

        // Main Ribbon mass
        KisAiStrokeOperation mainMass;
        mainMass.kind = KisAiStrokeOperation::Kind::Ribbon;
        mainMass.id = baseId + QStringLiteral("_main");
        mainMass.layer = QStringLiteral("Flats");
        mainMass.brush.profile = QStringLiteral("hair");
        mainMass.brush.color = hairColor;
        mainMass.spine = spinePts;
        mainMass.widthStart = qBound(0.005, o.value(QStringLiteral("width_start")).toDouble(0.028), 0.08);
        mainMass.widthMid = qBound(0.004, o.value(QStringLiteral("width_mid")).toDouble(0.018), 0.06);
        mainMass.widthEnd = qBound(0.002, o.value(QStringLiteral("width_end")).toDouble(0.005), 0.03);
        ops.append(mainMass);

        // Side flowing lineart strokes
        for (int side = -1; side <= 1; side += 2) {
            KisAiStrokeOperation strand;
            strand.kind = KisAiStrokeOperation::Kind::Path;
            strand.id = QStringLiteral("%1_strand_%2").arg(baseId, side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
            strand.layer = QStringLiteral("Lineart");
            strand.brush.profile = QStringLiteral("gpen");
            strand.brush.color = inkColor;
            strand.brush.size = 0.0025;
            for (int i = 0; i < spinePts.size(); ++i) {
                const qreal t = qreal(i) / qreal(qMax(1, spinePts.size() - 1));
                const qreal offset = side * (0.006 * (1.0 - t * 0.5));
                const qreal pressure = (i == 0 || i == spinePts.size() - 1) ? 0.3 : 0.8;
                strand.points.append(KisAiStrokePoint(spinePts[i].x() + offset, spinePts[i].y(), pressure));
            }
            ops.append(strand);
        }

        // Specular highlight band
        if (spinePts.size() >= 3) {
            KisAiStrokeOperation halo;
            halo.kind = KisAiStrokeOperation::Kind::Path;
            halo.id = baseId + QStringLiteral("_halo");
            halo.layer = QStringLiteral("Highlights");
            halo.brush.profile = QStringLiteral("airbrush");
            halo.brush.color = QColor(255, 255, 255, 180);
            halo.brush.size = 0.006;
            const int midIdx = spinePts.size() / 2;
            halo.points.append(KisAiStrokePoint(spinePts[qMax(0, midIdx - 1)].x(), spinePts[qMax(0, midIdx - 1)].y(), 0.3));
            halo.points.append(KisAiStrokePoint(spinePts[midIdx].x(), spinePts[midIdx].y(), 0.9));
            halo.points.append(KisAiStrokePoint(spinePts[qMin(spinePts.size() - 1, midIdx + 1)].x(), spinePts[qMin(spinePts.size() - 1, midIdx + 1)].y(), 0.3));
            ops.append(halo);
        }
        return ops;
    }

    // 3. HAND GESTURE
    if (kindStr.contains(QLatin1String("hand")) || kindStr.contains(QLatin1String("gesture"))) {
        const QPointF center = parsePt(o.value(QStringLiteral("center")));
        const QJsonArray szArr = o.value(QStringLiteral("size")).toArray();
        const qreal w = szArr.size() >= 1 ? qBound(0.03, szArr.at(0).toDouble(0.08), 0.25) : 0.08;
        const qreal h = szArr.size() >= 2 ? qBound(0.03, szArr.at(1).toDouble(0.10), 0.25) : 0.10;
        const QString gesture = o.value(QStringLiteral("gesture")).toString(QStringLiteral("reach")).toLower();
        const QJsonObject brushObj = o.value(QStringLiteral("brush")).toObject();
        const QColor skinColor = parseCol(brushObj.value(QStringLiteral("color")).toString(), QColor(255, 224, 192));
        const QColor inkColor = parseCol(QStringLiteral("#4a3728"), QColor(74, 55, 40));

        // Palm base fill
        QPolygonF palm;
        palm.append(QPointF(center.x() - w * 0.40, center.y() - h * 0.20));
        palm.append(QPointF(center.x() + w * 0.40, center.y() - h * 0.20));
        palm.append(QPointF(center.x() + w * 0.35, center.y() + h * 0.30));
        palm.append(QPointF(center.x() - w * 0.35, center.y() + h * 0.30));
        KisAiStrokeOperation palmFill;
        palmFill.kind = KisAiStrokeOperation::Kind::Fill;
        palmFill.id = baseId + QStringLiteral("_palm");
        palmFill.layer = QStringLiteral("Flats");
        palmFill.polygon = palm;
        palmFill.brush.color = skinColor;
        palmFill.brush.profile = QStringLiteral("brush");
        ops.append(palmFill);

        // 4 Finger strokes (reach / open / peace gesture lines)
        for (int f = 0; f < 4; ++f) {
            KisAiStrokeOperation finger;
            finger.kind = KisAiStrokeOperation::Kind::Path;
            finger.id = QStringLiteral("%1_finger_%2").arg(baseId).arg(f);
            finger.layer = QStringLiteral("Lineart");
            finger.brush.profile = QStringLiteral("gpen");
            finger.brush.color = inkColor;
            finger.brush.size = 0.0028;

            const qreal fx = center.x() - w * 0.30 + f * (w * 0.20);
            const qreal fy = center.y() - h * 0.20;
            const qreal fingerLen = (f == 1 || f == 2) ? h * 0.50 : h * 0.40;
            const qreal spread = (gesture == QLatin1String("peace") && f >= 2) ? 0.3 : 1.0;

            finger.points.append(KisAiStrokePoint(fx, fy, 0.7));
            finger.points.append(KisAiStrokePoint(fx + (fx - center.x()) * 0.3 * spread, fy - fingerLen * 0.55, 0.8));
            finger.points.append(KisAiStrokePoint(fx + (fx - center.x()) * 0.45 * spread, fy - fingerLen, 0.3));
            ops.append(finger);
        }

        // Thumb stroke
        KisAiStrokeOperation thumb;
        thumb.kind = KisAiStrokeOperation::Kind::Path;
        thumb.id = baseId + QStringLiteral("_thumb");
        thumb.layer = QStringLiteral("Lineart");
        thumb.brush.profile = QStringLiteral("gpen");
        thumb.brush.color = inkColor;
        thumb.brush.size = 0.0028;
        thumb.points.append(KisAiStrokePoint(center.x() - w * 0.38, center.y() + h * 0.05, 0.6));
        thumb.points.append(KisAiStrokePoint(center.x() - w * 0.55, center.y() - h * 0.08, 0.7));
        thumb.points.append(KisAiStrokePoint(center.x() - w * 0.50, center.y() - h * 0.22, 0.3));
        ops.append(thumb);

        return ops;
    }

    // 4. DYNAMIC POSE (Cinematic gesture / airflow lines)
    if (kindStr.contains(QLatin1String("dynamic_pose")) || kindStr.contains(QLatin1String("flow_line"))) {
        const QPointF center = parsePt(o.value(QStringLiteral("center")));
        const QJsonObject brushObj = o.value(QStringLiteral("brush")).toObject();
        const QColor flowColor = parseCol(brushObj.value(QStringLiteral("color")).toString(), QColor(255, 255, 255, 200));

        for (int l = 0; l < 3; ++l) {
            KisAiStrokeOperation flow;
            flow.kind = KisAiStrokeOperation::Kind::Path;
            flow.id = QStringLiteral("%1_flow_%2").arg(baseId).arg(l);
            flow.layer = QStringLiteral("FX");
            flow.brush.profile = QStringLiteral("airbrush");
            flow.brush.color = flowColor;
            flow.brush.size = 0.0035;

            const qreal offY = (l - 1) * 0.06;
            flow.points.append(KisAiStrokePoint(center.x() - 0.25, center.y() + offY - 0.05, 0.2));
            flow.points.append(KisAiStrokePoint(center.x(), center.y() + offY, 0.7));
            flow.points.append(KisAiStrokePoint(center.x() + 0.25, center.y() + offY + 0.05, 0.1));
            ops.append(flow);
        }
        return ops;
    }

    return ops;
}

QString KisAiStrokeProgramCodec::buildSystemPrompt(const QSize &canvasSize,
                                                   const QString &prompt,
                                                   const QString &customInstructions,
                                                   int artStyle,
                                                   bool enableAdvancedDirectives)
{
    auto spec = KisAiPromptAnalyzer::analyze(prompt, canvasSize);
    if (artStyle > 0 && artStyle <= 7) {
        spec.style = static_cast<KisAiPromptAnalyzer::ArtStyle>(artStyle);
    }
    const QString artDirection = KisAiPromptAnalyzer::generateArtDirection(spec, canvasSize);

    // A0: User Request is placed at the absolute top with strict priority rule
    QString systemText;
    systemText += QStringLiteral(
                      "=== USER REQUEST (ABSOLUTE HIGHEST PRIORITY) ===\n"
                      "\"%1\"\n"
                      "PRIORITY RULE: If any artistic guideline, example, or default suggestion below conflicts with "
                      "the USER REQUEST, you MUST follow the USER REQUEST. Every subject, character, mood, color "
                      "palette, and detail MUST be derived strictly from the USER REQUEST.\n\n")
                      .arg(prompt.trimmed());

    systemText += QStringLiteral(
        "You are an autonomous AI master digital painter directing layer-by-layer drawing plans for Krita.\n"
        "Generate a rich, cohesive, painterly illustration by specifying coordinate-directed strokes in StrokeProgram "
        "JSON format.\n"
        "Output ONLY valid, parseable RFC 8259 JSON starting with '{' and ending with '}'.\n"
        "Do NOT include markdown explanations, thought text, or conversational chatter outside the JSON.\n\n");

    systemText += buildJsonContractSection() + QStringLiteral("\n\n");
    systemText += buildCoordinateSection(canvasSize) + QStringLiteral("\n\n");
    systemText += buildLayerSemanticsSection() + QStringLiteral("\n\n");
    systemText += buildDrawingWorkflowSection() + QStringLiteral("\n\n");
    systemText += buildArtisticGuidelinesSection() + QStringLiteral("\n\n");
    systemText += buildOperationKindsSection() + QStringLiteral("\n\n");
    systemText += artDirection + QStringLiteral("\n\n");
    if (enableAdvancedDirectives) {
        systemText += buildFlagshipDirectives() + QStringLiteral("\n\n");
    }
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
                                                                  int artStyle,
                                                                  bool forceJsonObjectOnly,
                                                                  const QString &referenceImageBase64,
                                                                  qint64 seed)
{
    const bool reasoning = isReasoningModel(model);
    const bool advancedStroke = KisAiModelRouter::shouldUseAdvancedStrokeLogic(model, KisAiModelRouter::qualityMode());
    QString effectiveInstructions = customInstructions;
    const bool hasReferenceImage = !referenceImageBase64.trimmed().isEmpty();
    if (hasReferenceImage) {
        if (!effectiveInstructions.isEmpty()) {
            effectiveInstructions += QStringLiteral("\n\n");
        }
        effectiveInstructions += QStringLiteral(
            "[REFERENCE IMAGE GUIDANCE]\n"
            "An attached reference image is provided. Faithfully inspect and analyze the character design, "
            "costume/outfit, hairstyle, color palette, lighting atmosphere, and key visual motifs in the reference image. "
            "Reflect these visual traits accurately into the generated strokes while honoring the user prompt.");
    }
    const QString systemText = buildSystemPrompt(canvasSize, prompt, effectiveInstructions, artStyle, advancedStroke);

    QJsonObject userObj;
    userObj[QStringLiteral("prompt")] = prompt;
    userObj[QStringLiteral("canvas_width")] = canvasSize.width();
    userObj[QStringLiteral("canvas_height")] = canvasSize.height();
    const int geometryBudget = qBound(20, strokeBudget, 4000);
    // Backwards-compatible for test budgets (300 -> 20), but scales up to 250 operations for rich inking and volumetric
    const int maxOpLimit = advancedStroke ? 300 : 250;
    const int operationTarget =
        (geometryBudget <= 300) ? qBound(16, geometryBudget / 15, 60) : qBound(20, geometryBudget / 12, maxOpLimit);
    userObj[QStringLiteral("geometry_budget")] = geometryBudget;
    userObj[QStringLiteral("operation_target")] = operationTarget;
    userObj[QStringLiteral("budget_allocation")] =
        QJsonObject{{QStringLiteral("Flats"), QStringLiteral("20-25%")},
                    {QStringLiteral("Shading"), QStringLiteral("25-35%")},
                    {QStringLiteral("Lineart"), QStringLiteral("35-45%")},
                    {QStringLiteral("Highlights_FX"), QStringLiteral("10-15%")}};
    userObj[QStringLiteral("directive")] = QStringLiteral(
        "Create a master-level, presentation-ready illustration directly manifesting the user prompt. "
        "Avoid flat coloring-book fills: build 3D rounded volumes with soft form shading and crisp cast shadows. "
        "Draw lineart with deliberate care, varying line weights from bold contours to fine facial micro-details. "
        "Spend geometry on volumetric shapes, deep multi-tier shadows, and exquisite inking. Output strictly valid RFC "
        "8259 JSON.");
    if (hasReferenceImage) {
        userObj[QStringLiteral("reference_image_guidance")] = QStringLiteral(
            "Faithfully inspect the attached reference image for character appearance, styling, and color harmony.");
    }

    const QString userText = QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact));

    QJsonArray messages;
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});

    QJsonObject userMsg;
    userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
    if (hasReferenceImage) {
        QJsonArray contentArray;
        contentArray.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), userText}
        });
        QString imageUrl = referenceImageBase64.trimmed();
        if (!imageUrl.startsWith(QLatin1String("data:image/"))) {
            imageUrl = QStringLiteral("data:image/jpeg;base64,") + imageUrl;
        }
        contentArray.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("image_url")},
            {QStringLiteral("image_url"), QJsonObject{
                {QStringLiteral("url"), imageUrl},
                {QStringLiteral("detail"), QStringLiteral("auto")}
            }}
        });
        userMsg.insert(QStringLiteral("content"), contentArray);
    } else {
        userMsg.insert(QStringLiteral("content"), userText);
    }
    messages.append(userMsg);

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.trimmed();
    payload[QStringLiteral("messages")] = messages;
    if (seed >= 0) {
        payload[QStringLiteral("seed")] = static_cast<qint64>(seed & 0x7FFFFFFF);
    }

    if (enableStreaming) {
        payload[QStringLiteral("stream")] = true;
    }

    // Structured output via json_object or json_schema (optional, only when explicitly enforced or native OpenAI)
    if (enforceJsonFormat) {
        if (!forceJsonObjectOnly && supportsJsonSchema(model)) {
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
        : qBound(4096, operationTarget * 180 + (reasoning ? 16384 : 4096), reasoning ? 32768 : 16384);
    if (advancedStroke && maxTokensOverride <= 0 && calculatedTokens < 12288) {
        calculatedTokens = qBound(12288, operationTarget * 200 + 4096, 32768);
    }

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

    // A hostile/truncated body made of `"a""a""a"...` produces one masked entry
    // per literal. Each entry is stored twice (the vector plus the ~30 replace()
    // passes over the rewritten text), so the default QVector growth would
    // amplify a 32 MB response into gigabytes. Past the budget we stop masking
    // and let the sanitizer operate on the raw text instead; a valid program
    // never carries more than a few hundred literals.
    constexpr int kMaxMaskedStrings = 4096;

    {
        QString masked;
        masked.reserve(text.size());
        bool inStr = false;
        bool esc = false;
        QString currentStr;
        bool maskingBudgetExhausted = false;

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
                    if (maskedStrings.size() >= kMaxMaskedStrings) {
                        maskingBudgetExhausted = true;
                        break;
                    }
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

        if (maskingBudgetExhausted) {
            maskedStrings.clear();
        } else {
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
    }

    if (diagnostic && !maskedStrings.isEmpty()) {
        diagnostic->appliedRepairs.append(
            QStringLiteral("TokenMasking(%1 strings preserved)").arg(maskedStrings.size()));
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
    static const QRegularExpression strayTokenBeforeQuote(QStringLiteral(R"((?<=[,\{\[\s])([a-zA-Z_]{1,3})\s+(?="))"));
    text.replace(strayTokenBeforeQuote, QStringLiteral(""));

    static const QRegularExpression strayTokenBeforeOpen(
        QStringLiteral(R"((?<=[,\{\[\s])([a-zA-Z_]{1,3})\s+(?=[\{\[]))"));
    text.replace(strayTokenBeforeOpen, QStringLiteral(""));

    // 9. Quote unquoted object keys (supports alphanumeric, underscores, and hyphens)
    static const QRegularExpression unquotedKey(QStringLiteral(R"((?<=[,\{\s])([a-zA-Z_][a-zA-Z0-9_\-]*)\s*:)"));
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
    static const QRegularExpression numUnitPx(QStringLiteral(R"((?<=[,\:\[\s])-?(\d+(?:\.\d+)?)\s*px(?=[,\:\]\}\s]))"),
                                              QRegularExpression::CaseInsensitiveOption);
    text.replace(numUnitPx, QStringLiteral("\\1"));

    static const QRegularExpression numUnitDeg(
        QStringLiteral(R"((?<=[,\:\[\s])-?(\d+(?:\.\d+)?)\s*deg(?=[,\:\]\}\s]))"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(numUnitDeg, QStringLiteral("\\1"));

    // NaN / Infinity
    static const QRegularExpression nanRe(QStringLiteral(R"((?<=[,\:\[\s])NaN(?=[,\:\]\}\s]))"),
                                          QRegularExpression::CaseInsensitiveOption);
    text.replace(nanRe, QStringLiteral("0.0"));

    static const QRegularExpression infRe(QStringLiteral(R"((?<=[,\:\[\s])\+?Infinity(?=[,\:\]\}\s]))"),
                                          QRegularExpression::CaseInsensitiveOption);
    text.replace(infRe, QStringLiteral("1.0"));

    static const QRegularExpression negInfRe(QStringLiteral(R"((?<=[,\:\[\s])-Infinity(?=[,\:\]\}\s]))"),
                                             QRegularExpression::CaseInsensitiveOption);
    text.replace(negInfRe, QStringLiteral("-1.0"));

    // Corrupted / noisy numbers in coordinates or values (e.g. 0t.05, 0.t3, t0, 1t, 0.08t)
    static const QRegularExpression numLetterBeforeDot(QStringLiteral(R"((?<=[,\:\[\s])-?(\d+)[a-zA-Z]+(\.\d+))"));
    text.replace(numLetterBeforeDot, QStringLiteral("\\1\\2"));

    static const QRegularExpression numLetterAfterDot(QStringLiteral(R"((?<=[,\:\[\s])-?(\d+\.)[a-zA-Z]+(\d+))"));
    text.replace(numLetterAfterDot, QStringLiteral("\\1\\2"));

    static const QRegularExpression numLetterPrefix(
        QStringLiteral(R"((?<=[,\:\[\s])-?[a-zA-Z]+(\d+(?:\.\d+)?)(?=[,\:\]\}\s]))"));
    text.replace(numLetterPrefix, QStringLiteral("\\1"));

    static const QRegularExpression numLetterSuffix(
        QStringLiteral(R"((?<=[,\:\[\s])-?(\d+(?:\.\d+)?)[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(numLetterSuffix, QStringLiteral("\\1"));

    // 11. Corrupted booleans and null (e.g. falset -> false, truet -> true, nullt -> null)
    static const QRegularExpression boolFalse(QStringLiteral(R"((?<=[,\:\[\s])false[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(boolFalse, QStringLiteral("false"));

    static const QRegularExpression boolTrue(QStringLiteral(R"((?<=[,\:\[\s])true[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(boolTrue, QStringLiteral("true"));

    static const QRegularExpression valNull(QStringLiteral(R"((?<=[,\:\[\s])null[a-zA-Z]+(?=[,\:\]\}\s]))"));
    text.replace(valNull, QStringLiteral("null"));

    // 12. Fix missing commas between object properties. The lookbehind is
    // split into fixed-length alternatives because PCRE2 (< 10.43) rejects
    // variable-length lookbehinds; Qt would silently disable the whole repair.
    {
        static const QRegularExpression missingCommaPropChar(
            QStringLiteral(R"re((?<=["\}\]])\s+(?="(?:[a-zA-Z_][a-zA-Z0-9_\-]*|__AI_STR_MASK_\d+__)"\s*:))re"));
        static const QRegularExpression missingCommaPropDigit(
            QStringLiteral(R"re((?<=\d)\s+(?="(?:[a-zA-Z_][a-zA-Z0-9_\-]*|__AI_STR_MASK_\d+__)"\s*:))re"));
        static const QRegularExpression missingCommaPropWord(QStringLiteral(
            R"re((?<=(?:true|false|null))\s+(?="(?:[a-zA-Z_][a-zA-Z0-9_\-]*|__AI_STR_MASK_\d+__)"\s*:))re"));
        text.replace(missingCommaPropChar, QStringLiteral(", "));
        text.replace(missingCommaPropDigit, QStringLiteral(", "));
        text.replace(missingCommaPropWord, QStringLiteral(", "));
    }

    // 13. Fix missing commas between numbers in coordinate arrays (e.g. [0.1 0.2 0.8] -> [0.1, 0.2, 0.8])
    static const QRegularExpression missingCommaNum(QStringLiteral(R"((?<=\d)\s+(?=-?\d+\.?\d*))"));
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
    static const QStringList arrayKeys = {QStringLiteral("\"operations\""),
                                          QStringLiteral("\"strokes\""),
                                          QStringLiteral("\"ops\""),
                                          QStringLiteral("\"layers\""),
                                          QStringLiteral("\"data\"")};

    int targetIdx = -1;
    for (const auto &k : arrayKeys) {
        targetIdx = text.indexOf(k, 0, Qt::CaseInsensitive);
        if (targetIdx >= 0)
            break;
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
                    if (rbEsc) {
                        rbEsc = false;
                        continue;
                    }
                    if (sc == QLatin1Char('\\')) {
                        rbEsc = true;
                        continue;
                    }
                    if (sc == QLatin1Char('"')) {
                        rbInStr = !rbInStr;
                        continue;
                    }
                    if (rbInStr)
                        continue;
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
                    if (open == QLatin1Char('{'))
                        repaired.append(QLatin1Char('}'));
                    else if (open == QLatin1Char('['))
                        repaired.append(QLatin1Char(']'));
                }
                repaired = repairJsonSyntax(repaired, diagnostic);
                QJsonParseError repErr;
                const QJsonDocument testDoc = QJsonDocument::fromJson(repaired.toUtf8(), &repErr);
                if (!testDoc.isNull() && testDoc.isObject()) {
                    if (diagnostic)
                        diagnostic->appliedRepairs.append(QStringLiteral("ArrayRollbackClosure"));
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
    if (diagnostic)
        diagnostic->appliedRepairs.append(QStringLiteral("StackBasedClosure"));
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
            if (block.contains(QLatin1String("operations"), Qt::CaseInsensitive))
                score += 10;
            if (block.contains(QLatin1String("strokes"), Qt::CaseInsensitive))
                score += 8;
            if (block.contains(QLatin1String("schema_version"), Qt::CaseInsensitive))
                score += 5;
            if (block.contains(QLatin1String("kind"), Qt::CaseInsensitive))
                score += 3;
            if (score > bestScore) {
                bestScore = score;
                bestBlock = block;
            }
        }
    }

    if (!bestBlock.isEmpty()) {
        text = bestBlock;
        if (diagnostic)
            diagnostic->appliedRepairs.append(QStringLiteral("BestCodeBlockExtracted"));
    }

    // 3. Find outermost { ... } or [ ... ]
    int startPos = -1;
    QChar closeChar;

    const int firstBrace = text.indexOf(QLatin1Char('{'));
    const int firstFullBrace = text.indexOf(QChar(0xFF5B)); // 全角 ｛
    const int effectiveFirstBrace = (firstBrace >= 0 && firstFullBrace >= 0)
        ? std::min(firstBrace, firstFullBrace)
        : (firstBrace >= 0 ? firstBrace : firstFullBrace);

    const int firstBracket = text.indexOf(QLatin1Char('['));

    if (effectiveFirstBrace >= 0 && firstBracket >= 0) {
        if (effectiveFirstBrace <= firstBracket) {
            startPos = effectiveFirstBrace;
            closeChar = (effectiveFirstBrace == firstBrace) ? QLatin1Char('}') : QChar(0xFF5D);
        } else if (text.left(firstBracket).trimmed().isEmpty()) {
            startPos = firstBracket;
            closeChar = QLatin1Char(']');
        }
    } else if (effectiveFirstBrace >= 0) {
        startPos = effectiveFirstBrace;
        closeChar = (effectiveFirstBrace == firstBrace) ? QLatin1Char('}') : QChar(0xFF5D);
    } else if (firstBracket >= 0) {
        if (text.left(firstBracket).trimmed().isEmpty()) {
            startPos = firstBracket;
            closeChar = QLatin1Char(']');
        }
    }

    if (startPos >= 0) {
        int lastPos = -1;
        if (closeChar == QLatin1Char(']')) {
            const int lastBracket = text.lastIndexOf(QLatin1Char(']'));
            const int lastFullBracket = text.lastIndexOf(QChar(0xFF3D)); // 全角 ］
            lastPos = std::max(lastBracket, lastFullBracket);
        } else {
            const int lastBrace = text.lastIndexOf(QLatin1Char('}'));
            const int lastFullBrace = text.lastIndexOf(QChar(0xFF5D)); // 全角 ｝
            lastPos = std::max(lastBrace, lastFullBrace);
        }
        if (lastPos > startPos) {
            const QString candidate = text.mid(startPos, lastPos - startPos + 1).trimmed();
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
                    text = text.mid(startPos).trimmed();
                }
            }
        } else {
            text = text.mid(startPos).trimmed();
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

bool KisAiStrokeProgramCodec::parseSseStreamChunk(const QByteArray &chunk,
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

    // The accumulated content is the decoded stream body; it must obey the same
    // overall budget as a buffered response so a hostile/buggy peer streaming
    // endless deltas cannot exhaust memory.
    constexpr int MAX_SSE_CONTENT_BYTES = 32 * 1024 * 1024;
    if (accumulatedContent->size() > MAX_SSE_CONTENT_BYTES) {
        unprocessedBuffer->clear();
        return false;
    }

    unprocessedBuffer->append(chunk);
    bool anyDeltaExtracted = false;

    // Consume complete lines in a single forward scan and rewrite the carry-over
    // buffer once per chunk. Calling remove(0, n) per line makes a large chunk
    // with many lines quadratic (each remove memmoves the whole remainder).
    int scanPos = 0;
    while (true) {
        const int newlineIdx = unprocessedBuffer->indexOf('\n', scanPos);
        if (newlineIdx < 0) {
            break;
        }

        QByteArray line = unprocessedBuffer->mid(scanPos, newlineIdx - scanPos).trimmed();
        scanPos = newlineIdx + 1;

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
                const QString deltaText = extractContentStringFromMessage(delta, choice0);
                if (!deltaText.isEmpty()) {
                    accumulatedContent->append(deltaText);
                    anyDeltaExtracted = true;
                }
            }
        }
    }

    // Drop the consumed prefix in one move, keeping any partial trailing line.
    if (scanPos > 0) {
        unprocessedBuffer->remove(0, scanPos);
    }

    return anyDeltaExtracted;
}

bool KisAiStrokeProgramCodec::extractOperationsFromRawText(const QString &rawText,
                                                           KisAiStrokeProgram *outProgram,
                                                           QString *errorMessage,
                                                           KisAiJsonDiagnostic *diagnostic,
                                                           KisAiStrokeQualityReport *qualityReport)
{
    Q_UNUSED(diagnostic);
    if (!outProgram) {
        return false;
    }

    outProgram->operations.clear();

    // Try to extract schema_version, title, prompt if present
    static const QRegularExpression schemaVerRe(QStringLiteral(R"("schema_version"\s*:\s*(\d+))"));
    const auto svMatch = schemaVerRe.match(rawText);
    bool versionOk = false;
    if (svMatch.hasMatch()) {
        // Match the v1/v2 gate that parseProgramJson enforces: toInt() alone would
        // accept an overflowing digit run (silently becoming 0) and bypass it.
        const int parsed = svMatch.captured(1).toInt(&versionOk);
        if (versionOk && parsed >= 1 && parsed <= 2) {
            outProgram->schemaVersion = parsed;
        } else {
            versionOk = false;
        }
    }
    if (!versionOk) {
        outProgram->schemaVersion = 2;
    }

    static const QRegularExpression titleRe(
        QStringLiteral(R"re("[a-z]*title[a-z]*"\s*:\s*"([^"\\]*(?:\\.[^"\\]*)*)")re"));
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
    constexpr int MAX_RECOVERED_OPERATIONS = 500;
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
            // A hostile response of pure '{' would otherwise push one entry per
            // byte; stop tracking once no further range can be recorded.
            if (openBraces.size() < MAX_OBJECT_RANGES) {
                openBraces.append(i);
            }
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

    KisAiStrokeQualityReport rep;
    *outProgram = KisAiStrokeProgramCodec::refineForRendering(*outProgram, &rep);
    if (qualityReport) {
        *qualityReport = rep;
    }
    return !outProgram->operations.isEmpty();
}

bool KisAiStrokeProgramCodec::supportsJsonFormat(const QString &endpoint)
{
    const QString ep = endpoint.trimmed().toLower();
    if (ep.isEmpty()) {
        return false;
    }
    return ep.contains(QLatin1String("api.openai.com")) || ep.contains(QLatin1String("openrouter.ai"))
        || ep.contains(QLatin1String("deepseek.com")) || ep.contains(QLatin1String("groq.com"))
        || ep.contains(QLatin1String("googleapis.com")) || ep.contains(QLatin1String("mistral.ai"))
        || ep.contains(QLatin1String("together.xyz")) || ep.contains(QLatin1String("together.ai"))
        || ep.contains(QLatin1String("fireworks.ai")) || ep.contains(QLatin1String("perplexity.ai"))
        || ep.contains(QLatin1String("x.ai")) || ep.contains(QLatin1String("cerebras.ai"))
        || ep.contains(QLatin1String("anthropic.com")) || ep.contains(QLatin1String("cohere.com"))
        || ep.contains(QLatin1String("11434")) // Ollama default port
        || ep.contains(QLatin1String("1234")); // LM Studio default port
}

bool KisAiStrokeProgramCodec::supportsJsonSchema(const QString &model)
{
    const QString m = model.trimmed().toLower();
    if (m.isEmpty() || m.contains(QLatin1String("vision-preview")) || m.contains(QLatin1String("anthropic"))
        || m.contains(QLatin1String("claude")) || m.contains(QLatin1String("deepseek-chat"))) {
        return false;
    }
    return m.contains(QLatin1String("gpt-4o")) || m.contains(QLatin1String("gpt-4.5"))
        || m.startsWith(QLatin1String("gpt-5")) || m.startsWith(QLatin1String("gpt-6"))
        || m.startsWith(QLatin1String("o1")) || m.startsWith(QLatin1String("o3"))
        || m.startsWith(QLatin1String("o4")) || m.contains(QLatin1String("deepseek-v4"));
}

QColor KisAiStrokeProgramCodec::calculateHueShiftedShadow(const QColor &baseColor, bool warmLight)
{
    if (!baseColor.isValid())
        return QColor(30, 24, 45, 120);

    int h, s, v, a;
    baseColor.getHsv(&h, &s, &v, &a);
    if (h < 0)
        h = 240;

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

KisAiStrokeProgramCodec::IntentAdherenceResult
KisAiStrokeProgramCodec::checkIntentAdherence(const KisAiStrokeProgram &program, const QString &prompt)
{
    IntentAdherenceResult result;
    const QString pLower = prompt.toLower();

    bool expectDark = pLower.contains(QLatin1String("night")) || pLower.contains(QLatin1String("dark"))
        || pLower.contains(QLatin1String("evening")) || pLower.contains(QLatin1String("starry"))
        || pLower.contains(QLatin1String("midnight"));
    bool expectSunset = pLower.contains(QLatin1String("sunset")) || pLower.contains(QLatin1String("dusk"))
        || pLower.contains(QLatin1String("golden hour"));

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

KisAiStrokeProgram KisAiStrokeProgramCodec::trimOperationsToBudget(const KisAiStrokeProgram &program, int maxOperations)
{
    if (program.operations.size() <= maxOperations || maxOperations <= 0) {
        return program;
    }

    auto calcScore = [](const KisAiStrokeOperation &op) -> qreal {
        if (op.polygon.size() > 0) {
            return polygonArea(op.polygon);
        }
        if (!op.points.isEmpty()) {
            return op.points.size() * 0.01;
        }
        if (!op.spine.isEmpty()) {
            return op.spine.size() * 0.01;
        }
        if (op.kind == KisAiStrokeOperation::Kind::Particles) {
            return op.bounds.isValid() ? (op.bounds.width() * op.bounds.height() + op.particleCount * 0.001) : 0.05;
        }
        return 0.01;
    };

    QMap<QString, QVector<QPair<KisAiStrokeOperation, int>>> byLayer;
    for (int i = 0; i < program.operations.size(); ++i) {
        const KisAiStrokeOperation &op = program.operations.at(i);
        byLayer[normalizeLayerName(op.layer)].append(qMakePair(op, i));
    }
    for (auto it = byLayer.begin(); it != byLayer.end(); ++it) {
        std::stable_sort(it.value().begin(),
                         it.value().end(),
                         [&](const QPair<KisAiStrokeOperation, int> &a, const QPair<KisAiStrokeOperation, int> &b) {
                             return calcScore(a.first) > calcScore(b.first);
                         });
    }

    QVector<KisAiStrokeOperation> selected;
    int remainingBudget = maxOperations;

    auto selectLayerOps = [](const QVector<QPair<KisAiStrokeOperation, int>> &ops,
                             int count) -> QVector<KisAiStrokeOperation> {
        QVector<QPair<KisAiStrokeOperation, int>> chosen = ops.mid(0, count);
        std::sort(chosen.begin(),
                  chosen.end(),
                  [](const QPair<KisAiStrokeOperation, int> &a, const QPair<KisAiStrokeOperation, int> &b) {
                      return a.second < b.second;
                  });
        QVector<KisAiStrokeOperation> res;
        res.reserve(chosen.size());
        for (const auto &item : chosen) {
            res.append(item.first);
        }
        return res;
    };

    // 1. Background layer: preserve up to 2 operations
    if (byLayer.contains(QStringLiteral("Background"))) {
        const auto &bgOps = byLayer[QStringLiteral("Background")];
        const int bgCount = qMin(bgOps.size(), qMin(2, remainingBudget));
        selected.append(selectLayerOps(bgOps, bgCount));
        remainingBudget -= bgCount;
    }

    // 2. Dynamic layer budget allocation
    static const QStringList standardLayers = {QStringLiteral("Flats"),
                                               QStringLiteral("Lineart"),
                                               QStringLiteral("Shading"),
                                               QStringLiteral("Highlights"),
                                               QStringLiteral("FX")};

    auto getLayerWeight = [](const QString &layer) -> qreal {
        if (layer == QLatin1String("Flats"))
            return 0.35;
        if (layer == QLatin1String("Lineart"))
            return 0.30;
        if (layer == QLatin1String("Shading"))
            return 0.20;
        if (layer == QLatin1String("Highlights"))
            return 0.10;
        if (layer == QLatin1String("FX"))
            return 0.10;
        return 0.05;
    };

    QMap<QString, int> layerAllocations;

    // Guarantee at least 1 op to each present standard layer if budget permits
    for (const QString &l : standardLayers) {
        if (byLayer.contains(l) && !byLayer[l].isEmpty() && remainingBudget > 0) {
            layerAllocations[l] = 1;
            remainingBudget--;
        }
    }

    // Allocate remaining budget iteratively to the layer with highest-scoring next candidate operation
    while (remainingBudget > 0) {
        QString bestLayer;
        qreal bestMetric = -1.0;
        for (auto it = byLayer.cbegin(); it != byLayer.cend(); ++it) {
            const QString &l = it.key();
            if (l == QLatin1String("Background"))
                continue;
            const int currentAlloc = layerAllocations.value(l, 0);
            const int available = it.value().size();
            if (currentAlloc < available) {
                const auto &candidateOp = it.value().at(currentAlloc).first;
                const qreal metric = calcScore(candidateOp) * getLayerWeight(l);
                if (metric > bestMetric) {
                    bestMetric = metric;
                    bestLayer = l;
                }
            }
        }
        if (bestLayer.isEmpty()) {
            break;
        }
        layerAllocations[bestLayer]++;
        remainingBudget--;
    }

    auto appendLayerOps = [&](const QString &l) {
        if (layerAllocations.contains(l) && byLayer.contains(l)) {
            const int count = layerAllocations[l];
            selected.append(selectLayerOps(byLayer[l], count));
        }
    };

    for (const QString &l : standardLayers) {
        appendLayerOps(l);
    }
    for (auto it = byLayer.cbegin(); it != byLayer.cend(); ++it) {
        if (it.key() != QLatin1String("Background") && !standardLayers.contains(it.key())) {
            appendLayerOps(it.key());
        }
    }

    KisAiStrokeProgram trimmed = program;
    trimmed.operations = selected;
    return refineForRendering(trimmed);
}

QJsonObject KisAiStrokeProgramCodec::buildCompositionPlanPayload(const QString &model,
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
        "  \"light_source\": {\"direction\": \"top_left\" | \"top_right\" | \"rim\" | \"ambient\", \"temperature\": "
        "\"warm\" | \"cool\"},\n"
        "  \"depth_planes\": {\"background\": \"...\", \"midground\": \"...\", \"foreground\": \"...\"},\n"
        "  \"artistic_directives\": \"Concise 2-sentence directive for stroke generation.\"\n"
        "}");

    QJsonObject userObj;
    userObj[QStringLiteral("prompt")] = prompt;
    userObj[QStringLiteral("canvas_width")] = canvasSize.width();
    userObj[QStringLiteral("canvas_height")] = canvasSize.height();
    userObj[QStringLiteral("aspect_ratio")] = aspect;

    QJsonArray messages;
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact))}});

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

bool KisAiStrokeProgramCodec::parseCompositionPlan(const QByteArray &responseBytes,
                                                   QString *outDirectives,
                                                   QString *errorMessage)
{
    if (outDirectives)
        outDirectives->clear();
    // Mirror the MAX_RESPONSE_BYTES guard used by parseResponse: this entry point
    // also sanitizes attacker-controlled model output, and the sanitizer's work
    // grows with the input.
    constexpr int MAX_COMPOSITION_PLAN_BYTES = 32 * 1024 * 1024;
    if (responseBytes.size() > MAX_COMPOSITION_PLAN_BYTES) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Composition plan response exceeded the size limit.");
        return false;
    }
    const QString raw = QString::fromUtf8(responseBytes).trimmed();

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(responseBytes, &parseErr);
    if (!doc.isObject()) {
        const QString jsonStr = sanitizeAndExtractJson(raw);
        doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &parseErr);
    }
    if (!doc.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to parse composition plan JSON: %1").arg(parseErr.errorString());
        return false;
    }

    QJsonObject target = doc.object();
    if (target.contains(QStringLiteral("choices"))) {
        const QJsonArray choices = target.value(QStringLiteral("choices")).toArray();
        if (!choices.isEmpty()) {
            const QJsonObject firstChoice = choices.at(0).toObject();
            const QString content =
                extractContentStringFromMessage(firstChoice.value(QStringLiteral("message")).toObject(), firstChoice);
            if (!content.isEmpty()) {
                const QString innerJson = sanitizeAndExtractJson(content);
                QJsonParseError innerErr;
                const QJsonDocument innerDoc = QJsonDocument::fromJson(innerJson.toUtf8(), &innerErr);
                if (innerDoc.isObject()) {
                    target = innerDoc.object();
                }
            }
        }
    }

    QString directives;
    if (target.contains(QStringLiteral("artistic_directives"))) {
        directives = target.value(QStringLiteral("artistic_directives")).toString().trimmed();
    }
    if (target.contains(QStringLiteral("composition_type"))) {
        const QString compType = target.value(QStringLiteral("composition_type")).toString().trimmed();
        if (!compType.isEmpty()) {
            if (!directives.isEmpty())
                directives += QStringLiteral(" ");
            directives += QStringLiteral("[%1]").arg(compType);
        }
    }
    const QJsonObject focal = target.value(QStringLiteral("focal_point")).toObject();
    if (!focal.isEmpty()) {
        directives += QStringLiteral(" [Focal point at (%1, %2)]")
                          .arg(QString::number(focal.value(QStringLiteral("x")).toDouble(0.5), 'f', 2))
                          .arg(QString::number(focal.value(QStringLiteral("y")).toDouble(0.5), 'f', 2));
    }

    if (outDirectives)
        *outDirectives = directives;
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

    const auto parseAndRefine = [outProgram, errorMessage, diagnostic, qualityReport](
                                    const QJsonObject &programObject) {
        QJsonObject mutableRoot = programObject;
        KisAiStrokeTypeCheckReport typeReport;
        KisAiStrokeTypeChecker::checkAndCoerceProgram(&mutableRoot, &typeReport);
        // 検証落ちした op は validatedOps から外されて黙って消えるため、
        // エラーが1件でもあれば診断に必ず残す (warnings のみでは拾えない)。
        if (diagnostic && (typeReport.typeErrors > 0 || !typeReport.warnings.isEmpty())) {
            diagnostic->appliedRepairs.append(typeReport.summary());
            diagnostic->appliedRepairs.append(typeReport.errorMessages);
            diagnostic->appliedRepairs.append(typeReport.warnings);
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

    const auto tryParseSceneSpec =
        [outProgram, errorMessage, diagnostic, qualityReport](const QJsonObject &obj) -> bool {
        if (obj.contains(QStringLiteral("subject")) || obj.contains(QStringLiteral("head"))) {
            KisAiSceneSpec spec;
            QStringList warnings;
            if (KisAiSceneSpecCodec::parseSceneSpecObject(obj, &spec, &warnings)) {
                const QSize canvas = outProgram->canvasSize.isValid() ? outProgram->canvasSize : QSize(1024, 1024);
                *outProgram = KisAiLayoutEngine::generateProgram(spec, canvas);
                if (diagnostic && !warnings.isEmpty()) {
                    diagnostic->appliedRepairs.append(warnings.join(QStringLiteral("; ")));
                }
                if (qualityReport) {
                    qualityReport->score = outProgram->completionScore;
                }
                return !outProgram->operations.isEmpty();
            }
        }
        return false;
    };

    // Helper: search recursively for an object that contains operations/strokes or is a StrokeProgram
    std::function<QJsonObject(const QJsonObject &, int)> findProgramEnvelope;
    findProgramEnvelope = [&findProgramEnvelope](const QJsonObject &obj, int depth) -> QJsonObject {
        if (depth > 4)
            return QJsonObject();
        if (obj.contains(QStringLiteral("operations")) || obj.contains(QStringLiteral("strokes"))) {
            return obj;
        }
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QString k = it.key().trimmed().toLower();
            if (it.value().isArray()
                && (k.contains(QLatin1String("operation")) || k.contains(QLatin1String("stroke")))) {
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
            const QString err =
                root.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
            if (errorMessage) {
                *errorMessage = QStringLiteral("APIエラー: ") + (err.isEmpty() ? QStringLiteral("不明なエラー") : err);
            }
            if (diagnostic) {
                diagnostic->hasError = true;
                // errorMessage は任意引数なので nullptr でも成立させる。
                diagnostic->errorMessage =
                    errorMessage ? *errorMessage : (err.isEmpty() ? QStringLiteral("不明なエラー") : err);
            }
            return false;
        }

        // Direct SceneSpec check: meaning-only specification without explicit operations
        if (tryParseSceneSpec(root)) {
            return true;
        }

        // Direct StrokeProgram check: if root explicitly has operations or strokes, parse directly
        if (root.contains(QStringLiteral("operations")) || root.contains(QStringLiteral("strokes"))
            || root.contains(QStringLiteral("schema_version"))) {
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
            const QString content = extractContentStringFromMessage(messageObj, firstChoice);

            if (!content.isEmpty()) {
                const QString cleanJson = sanitizeAndExtractJson(content, diagnostic);
                QJsonParseError parseErr;
                const QJsonDocument programDoc = QJsonDocument::fromJson(cleanJson.toUtf8(), &parseErr);
                if (!programDoc.isNull() && programDoc.isObject()) {
                    if (tryParseSceneSpec(programDoc.object())) {
                        return true;
                    }
                    const QJsonObject innerEnv = findProgramEnvelope(programDoc.object(), 0);
                    if (!innerEnv.isEmpty() && parseAndRefine(innerEnv)) {
                        return true;
                    }
                }
                // Fallback extraction on choices content
                if (extractOperationsFromRawText(content, outProgram, errorMessage, diagnostic, qualityReport)) {
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
                    if (extractOperationsFromRawText(partText, outProgram, errorMessage, diagnostic, qualityReport)) {
                        return true;
                    }
                }
            }
        }

        // Anthropic Claude native format: content[0].text or content array
        if (root.contains(QStringLiteral("content"))) {
            const QJsonValue contentVal = root.value(QStringLiteral("content"));
            QString claudeText;
            if (contentVal.isString()) {
                claudeText = contentVal.toString();
            } else if (contentVal.isArray()) {
                for (const QJsonValue &pVal : contentVal.toArray()) {
                    if (pVal.isObject() && pVal.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                        claudeText.append(pVal.toObject().value(QStringLiteral("text")).toString());
                    }
                }
            }
            if (!claudeText.isEmpty()) {
                const QString cleanJson = sanitizeAndExtractJson(claudeText, diagnostic);
                const QJsonDocument programDoc = QJsonDocument::fromJson(cleanJson.toUtf8());
                if (programDoc.isObject()) {
                    const QJsonObject innerEnv = findProgramEnvelope(programDoc.object(), 0);
                    if (!innerEnv.isEmpty() && parseAndRefine(innerEnv)) {
                        return true;
                    }
                }
                if (extractOperationsFromRawText(claudeText, outProgram, errorMessage, diagnostic, qualityReport)) {
                    return true;
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
            const QJsonObject firstChoice = choices.at(0).toObject();
            const QString content = extractContentStringFromMessage(
                firstChoice.value(QStringLiteral("message")).toObject(), firstChoice);
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
    if (extractOperationsFromRawText(rawText, outProgram, errorMessage, diagnostic, qualityReport)) {
        return true;
    }

    if (diagnostic && extDocErr.error != QJsonParseError::NoError) {
        // Only report a JSON syntax error when the extracted JSON actually
        // failed to parse; a clean-but-empty document must not fabricate an
        // "error at line 1, column 1" diagnostic.
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

    if (lower.contains(QLatin1String("back")) || lower.contains(QLatin1String("bg")))
        return QStringLiteral("Background");
    if (lower.contains(QLatin1String("flat")) || lower.contains(QLatin1String("base")))
        return QStringLiteral("Flats");
    if (lower.contains(QLatin1String("shad")) || lower.contains(QLatin1String("dark")))
        return QStringLiteral("Shading");
    if (lower.contains(QLatin1String("line")) || lower.contains(QLatin1String("ink")))
        return QStringLiteral("Lineart");
    if (lower.contains(QLatin1String("light")) || lower.contains(QLatin1String("specular")))
        return QStringLiteral("Highlights");
    if (lower.contains(QLatin1String("fx")) || lower.contains(QLatin1String("effect"))
        || lower.contains(QLatin1String("particle")))
        return QStringLiteral("FX");

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
    const auto findField =
        [](const QJsonObject &o, const QStringList &names, const QJsonValue &defaultVal = QJsonValue()) -> QJsonValue {
        for (const auto &name : names) {
            if (o.contains(name)) {
                return o.value(name);
            }
        }
        // Tolerant fallback for near-miss keys ("layer_name" for "layer"). The
        // old n.contains(k) direction let a stray 1-char key such as "s" match
        // "size"/"points", and k.contains(n) let "kind" match "id"; both hijack
        // unrelated fields, so only containment of the longer key inside the
        // candidate is allowed and only when the candidate is at least 4
        // characters long.
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            const QString k = it.key().trimmed().toLower();
            for (const auto &name : names) {
                const QString n = name.toLower();
                if (k == n) {
                    return it.value();
                }
                if (n.size() >= 4 && k.size() > n.size() && k.contains(n)) {
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
            // Parse the string as-is first: a whitelist scrub would silently turn
            // exponent notation into a different number ("1e3" -> "13", "-1.5e-3"
            // -> "-1.53"), which is a wrong value rather than a rejection.
            QString s = val.toString().trimmed();
            {
                bool ok = false;
                const qreal v = s.toDouble(&ok);
                if (ok && std::isfinite(v))
                    return v;
            }
            // Fall back to stripping incidental decoration (currency signs,
            // percent, units) only when a direct parse is impossible.
            QString clean;
            for (const QChar &ch : s) {
                if (ch.isDigit() || ch == QLatin1Char('.') || ch == QLatin1Char('-') || ch == QLatin1Char('+')
                    || ch == QLatin1Char('e') || ch == QLatin1Char('E')) {
                    clean.append(ch);
                }
            }
            bool ok = false;
            const qreal v = clean.toDouble(&ok);
            // A long digit run overflows to +/-inf with ok == true; such a value
            // must never reach the renderer as geometry.
            if (ok && std::isfinite(v))
                return v;
        }
        bool ok = false;
        const qreal v = val.toVariant().toDouble(&ok);
        return (ok && std::isfinite(v)) ? v : defaultVal;
    };

    const auto toIntField = [](const QJsonValue &val, int defaultVal) -> int {
        if (val.isDouble())
            return val.toInt(defaultVal);
        if (val.isString()) {
            // Direct parse first so exponent notation survives ("1e3" is 1000, not
            // 13). Fall back to a bounded double parse before the digit scrub.
            const QString s = val.toString().trimmed();
            {
                bool ok = false;
                const int v = s.toInt(&ok);
                if (ok)
                    return v;
            }
            {
                bool ok = false;
                const qreal d = s.toDouble(&ok);
                // Guard the range: casting an out-of-range or non-finite double to
                // int is undefined behaviour, and a hostile "1e300" must not slip
                // past the schema-version gate as a wrapped value.
                if (ok && std::isfinite(d) && d >= qreal(std::numeric_limits<int>::min())
                    && d <= qreal(std::numeric_limits<int>::max())) {
                    return static_cast<int>(d);
                }
            }
            bool ok = false;
            QString clean;
            for (const QChar &ch : s) {
                if (ch.isDigit() || ch == QLatin1Char('-') || ch == QLatin1Char('+')) {
                    clean.append(ch);
                }
            }
            const int v = clean.toInt(&ok);
            if (ok)
                return v;
        }
        bool ok = false;
        const int v = val.toVariant().toInt(&ok);
        return ok ? v : defaultVal;
    };

    const auto toBoolField = [](const QJsonValue &val, bool defaultVal) -> bool {
        if (val.isBool())
            return val.toBool(defaultVal);
        if (val.isString()) {
            const QString s = val.toString().trimmed().toLower();
            if (s.startsWith(QLatin1String("t")) || s == QLatin1String("1") || s == QLatin1String("yes")
                || s == QLatin1String("on"))
                return true;
            if (s.startsWith(QLatin1String("f")) || s == QLatin1String("0") || s == QLatin1String("no")
                || s == QLatin1String("off"))
                return false;
        }
        if (val.isDouble()) {
            return val.toDouble() != 0.0;
        }
        return defaultVal;
    };

    if (rootObj.contains(QStringLiteral("schema_version"))
        || !findField(rootObj, {QStringLiteral("schema_version")}).isUndefined()) {
        const int version = toIntField(findField(rootObj, {QStringLiteral("schema_version")}), 2);
        if (version < 1 || version > 2) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("サポートされていないスキーマバージョンです (v%1)。v1 または v2 が必要です。")
                        .arg(version);
            }
            return false;
        }
    }

    outProgram->schemaVersion = toIntField(findField(rootObj, {QStringLiteral("schema_version")}), 2);
    outProgram->prompt = findField(rootObj, {QStringLiteral("prompt")}).toString();
    outProgram->title = findField(rootObj, {QStringLiteral("title")}, QStringLiteral("AI Artwork"))
                            .toString(QStringLiteral("AI Artwork"));
    outProgram->seed = toIntField(findField(rootObj, {QStringLiteral("seed")}), 42);
    outProgram->visualCritique =
        findField(rootObj, {QStringLiteral("visual_critique"), QStringLiteral("critique")}).toString();
    outProgram->agentCritique =
        findField(rootObj,
                  {QStringLiteral("agent_critique"), QStringLiteral("visual_critique"), QStringLiteral("critique")})
            .toString();
    outProgram->critiqueRegions.clear();
    const QJsonValue regVal =
        findField(rootObj,
                  {QStringLiteral("regions"), QStringLiteral("critique_regions"), QStringLiteral("region_actions")});
    if (regVal.isArray()) {
        const QJsonArray regArr = regVal.toArray();
        // Critique regions are diagnostics shown to the user, not geometry, but
        // they are still attacker-controlled. Every other collection in this
        // parser is budgeted, so cap this one too.
        constexpr int kMaxCritiqueRegions = 32;
        for (const QJsonValue &item : regArr) {
            if (outProgram->critiqueRegions.size() >= kMaxCritiqueRegions) {
                break;
            }
            if (!item.isObject())
                continue;
            const QJsonObject rObj = item.toObject();
            KisAiCritiqueRegion reg;
            reg.area = rObj.value(QStringLiteral("area")).toString().trimmed().toLower();
            reg.issue = rObj.value(QStringLiteral("issue")).toString().trimmed();
            reg.action = rObj.value(QStringLiteral("action")).toString().trimmed().toLower();
            reg.priority = qBound(1, rObj.value(QStringLiteral("priority")).toInt(1), 5);
            if (!reg.area.isEmpty()) {
                outProgram->critiqueRegions.append(reg);
            }
        }
    }
    outProgram->targetFocusArea =
        findField(rootObj, {QStringLiteral("target_focus_area"), QStringLiteral("focus_area"), QStringLiteral("focus")})
            .toString();
    outProgram->stepPhase = findField(rootObj, {QStringLiteral("step_phase")}, QStringLiteral("complete"))
                                .toString(QStringLiteral("complete"));
    outProgram->currentStep = toIntField(findField(rootObj, {QStringLiteral("current_step")}), 1);
    outProgram->totalSteps = toIntField(findField(rootObj, {QStringLiteral("total_steps")}), 1);
    outProgram->goalReached = toBoolField(findField(rootObj, {QStringLiteral("goal_reached")}), true);
    outProgram->completionScore = toDoubleField(findField(rootObj, {QStringLiteral("completion_score")}), 1.0);
    outProgram->readinessScore = clamp01(toDoubleField(
        findField(rootObj,
                  {QStringLiteral("readiness_score"), QStringLiteral("readiness"), QStringLiteral("completion_score")}),
        1.0));
    outProgram->recommendedAction =
        findField(rootObj, {QStringLiteral("recommended_action"), QStringLiteral("action")}).toString();

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
            // Clamp at parse time: the pixel-vs-normalized auto-scaling below
            // divides by these values, so a hostile 2e9 canvas would shrink
            // every pixel coordinate to ~0 before refineForRendering clamps.
            constexpr int PARSE_CANVAS_EDGE_MAX = 4096;
            outProgram->canvasSize = QSize(qBound(1, cw, PARSE_CANVAS_EDGE_MAX), qBound(1, ch, PARSE_CANVAS_EDGE_MAX));
        }
    }

    outProgram->operations.clear();

    const auto parseBrush = [&findField, &toDoubleField, &toBoolField](const QJsonObject &bObj) -> KisAiStrokeBrush {
        KisAiStrokeBrush b;
        b.profile =
            findField(bObj, {QStringLiteral("profile")}, QStringLiteral("auto")).toString(QStringLiteral("auto"));
        b.color = parseColor(
            findField(bObj, {QStringLiteral("color")}, QStringLiteral("#232323")).toString(QStringLiteral("#232323")));
        b.size = toDoubleField(findField(bObj, {QStringLiteral("size")}), 0.008);
        b.sizeMode =
            findField(bObj, {QStringLiteral("size_mode")}, QStringLiteral("ratio")).toString(QStringLiteral("ratio"));
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
        if (k.contains(QLatin1String("manga")) || k.contains(QLatin1String("speed"))
            || k.contains(QLatin1String("focus"))) {
            return KisAiStrokeOperation::Kind::MangaLines;
        }
        if (k.contains(QLatin1String("ribbon")) || k.contains(QLatin1String("band"))
            || k.contains(QLatin1String("taper"))) {
            return KisAiStrokeOperation::Kind::Ribbon;
        }
        if (k.contains(QLatin1String("particle")) || k.contains(QLatin1String("scatter"))
            || k.contains(QLatin1String("sparkle"))) {
            return KisAiStrokeOperation::Kind::Particles;
        }
        if (k.contains(QLatin1String("hatch"))) {
            return KisAiStrokeOperation::Kind::Hatch;
        }
        if (k.contains(QLatin1String("fill")) || k.contains(QLatin1String("polygon"))
            || k.contains(QLatin1String("color_fill")) || k.contains(QLatin1String("solid_fill"))) {
            return KisAiStrokeOperation::Kind::Fill;
        }
        // The eye check must precede the path check: ids like "eye_outline",
        // "eyeliner" and "eye_lineart" contain "line", so a later eye test would
        // never be reached and the eye would render as a plain path.
        if (k.contains(QLatin1String("anime_eye")) || k.contains(QLatin1String("eye"))) {
            return KisAiStrokeOperation::Kind::AnimeEye;
        }
        if (k.contains(QLatin1String("anime_mouth")) || k.contains(QLatin1String("mouth"))
            || k.contains(QLatin1String("lip"))) {
            return KisAiStrokeOperation::Kind::AnimeMouth;
        }
        if (k.contains(QLatin1String("path")) || k.contains(QLatin1String("stroke"))
            || k.contains(QLatin1String("line")) || k.contains(QLatin1String("contour"))) {
            return KisAiStrokeOperation::Kind::Path;
        }
        return KisAiStrokeOperation::Kind::Unknown;
    };

    const auto parsePoint = [](const QJsonValue &pv, qreal defaultPressure = 0.8) -> QPair<QPointF, qreal> {
        const auto toDoubleVal = [](const QJsonValue &val, bool *ok) -> qreal {
            if (val.isDouble()) {
                if (ok)
                    *ok = true;
                return val.toDouble();
            }
            if (val.isString()) {
                // Direct parse first so exponent notation survives; the scrub below
                // is only a fallback for decorated values like "0.5px".
                const QString s = val.toString().trimmed();
                {
                    bool directOk = false;
                    const qreal direct = s.toDouble(&directOk);
                    if (directOk) {
                        if (ok)
                            *ok = true;
                        return direct;
                    }
                }
                QString clean;
                for (const QChar &ch : s) {
                    if (ch.isDigit() || ch == QLatin1Char('.') || ch == QLatin1Char('-') || ch == QLatin1Char('+')
                        || ch == QLatin1Char('e') || ch == QLatin1Char('E')) {
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
                const qreal p = (pa.size() >= 3) ? toDoubleVal(pa.at(2), &okP) : defaultPressure;
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

    constexpr int MAX_OPERATIONS = 500;
    constexpr int MAX_POINTS_PER_OPERATION = 256;
    constexpr int MAX_TOTAL_CONTROL_POINTS = 16384;
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
            if (it.value().isArray()
                && (k.contains(QLatin1String("operation")) || k.contains(QLatin1String("stroke"))
                    || k == QLatin1String("ops") || k == QLatin1String("layers") || k == QLatin1String("data")
                    || k == QLatin1String("items"))) {
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
            op.layer = normalizeLayerName(
                findField(o, {QStringLiteral("layer"), QStringLiteral("layer_name")}, QStringLiteral("Lineart"))
                    .toString(QStringLiteral("Lineart")));
            op.blendMode = findField(o,
                                     {QStringLiteral("blend_mode"),
                                      QStringLiteral("blendMode"),
                                      QStringLiteral("blend-mode"),
                                      QStringLiteral("blend"),
                                      QStringLiteral("composite")},
                                     QStringLiteral("normal"))
                               .toString(QStringLiteral("normal"))
                               .toLower()
                               .replace(QLatin1Char('-'), QLatin1Char('_'));
            op.clipToId = findField(o,
                                    {QStringLiteral("clip_to_id"),
                                     QStringLiteral("clip_to"),
                                     QStringLiteral("clipToId"),
                                     QStringLiteral("clip-to-id"),
                                     QStringLiteral("clip")})
                              .toString();
            op.fillProfile =
                findField(
                    o,
                    {QStringLiteral("fill_profile"), QStringLiteral("fillProfile"), QStringLiteral("fill-profile")},
                    QStringLiteral("flat"))
                    .toString(QStringLiteral("flat"))
                    .toLower()
                    .replace(QLatin1Char('-'), QLatin1Char('_'));
            op.brush = parseBrush(findField(o, {QStringLiteral("brush")}).toObject());
            if (op.fillProfile == QLatin1String("watercolor")) {
                if (op.brush.profile.isEmpty() || op.brush.profile == QLatin1String("brush")
                    || op.brush.profile == QLatin1String("pen")) {
                    op.brush.profile = QStringLiteral("watercolor");
                }
                if (op.fillStyle.isEmpty() || op.fillStyle == QLatin1String("flat")) {
                    op.fillStyle = QStringLiteral("wash");
                }
            }

            if (op.kind == KisAiStrokeOperation::Kind::Path) {
                op.closed = toBoolField(findField(o, {QStringLiteral("closed")}), false);
                op.smooth = toBoolField(findField(o, {QStringLiteral("smooth")}), true);
                op.role =
                    findField(o, {QStringLiteral("role")}, QStringLiteral("auto")).toString(QStringLiteral("auto"));
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
                op.fillStyle =
                    findField(o, {QStringLiteral("style"), QStringLiteral("fill_style")}, QStringLiteral("wash"))
                        .toString(QStringLiteral("wash"));
                op.smooth = toBoolField(findField(o, {QStringLiteral("smooth")}),
                                        op.fillStyle.compare(QLatin1String("contour"), Qt::CaseInsensitive) == 0);
                op.angleDeg = toDoubleField(findField(o, {QStringLiteral("angle_deg"), QStringLiteral("angle")}), 0.0);
                op.spacing = toDoubleField(findField(o, {QStringLiteral("spacing")}), 0.5);
                const QJsonArray poly =
                    findField(o, {QStringLiteral("polygon"), QStringLiteral("poly"), QStringLiteral("points")})
                        .toArray();
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
                op.fillStyle =
                    findField(o, {QStringLiteral("style"), QStringLiteral("fill_style")}, QStringLiteral("linear"))
                        .toString(QStringLiteral("linear"));
                op.smooth = toBoolField(findField(o, {QStringLiteral("smooth")}), false);
                op.angleDeg = toDoubleField(findField(o, {QStringLiteral("angle_deg"), QStringLiteral("angle")}), 90.0);
                op.isRadial = toBoolField(findField(o, {QStringLiteral("is_radial"), QStringLiteral("radial")}),
                                          op.fillStyle.compare(QLatin1String("radial"), Qt::CaseInsensitive) == 0);
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
                op.gradientRadius =
                    toDoubleField(findField(o, {QStringLiteral("radius"), QStringLiteral("gradient_radius")}), 0.5);
                if (op.gradientRadius > kPixelCoordinateThreshold) {
                    op.gradientRadius /= qMin(canvasW, canvasH);
                }
                const QJsonArray colors =
                    findField(o, {QStringLiteral("colors"), QStringLiteral("gradient_colors")}).toArray();
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
                op.crossHatch =
                    toBoolField(findField(o, {QStringLiteral("cross_hatch"), QStringLiteral("crosshatch")}), false);
                const QJsonArray poly =
                    findField(o, {QStringLiteral("polygon"), QStringLiteral("poly"), QStringLiteral("points")})
                        .toArray();
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
                op.widthStart =
                    toDoubleField(findField(o, {QStringLiteral("width_start"), QStringLiteral("start_width")}), 0.02);
                op.widthMid =
                    toDoubleField(findField(o, {QStringLiteral("width_mid"), QStringLiteral("mid_width")}), 0.015);
                op.widthEnd =
                    toDoubleField(findField(o, {QStringLiteral("width_end"), QStringLiteral("end_width")}), 0.005);
                const qreal minCanvasDim = qMin(canvasW, canvasH);
                if (qMax(qMax(op.widthStart, op.widthMid), op.widthEnd) > kPixelCoordinateThreshold) {
                    op.widthStart /= minCanvasDim;
                    op.widthMid /= minCanvasDim;
                    op.widthEnd /= minCanvasDim;
                }
                const QJsonArray spine =
                    findField(o, {QStringLiteral("spine"), QStringLiteral("points"), QStringLiteral("pts")}).toArray();
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
                op.particleShape =
                    findField(o, {QStringLiteral("shape"), QStringLiteral("particle_shape")}, QStringLiteral("petal"))
                        .toString(QStringLiteral("petal"));
                op.particleCount =
                    toIntField(findField(o, {QStringLiteral("count"), QStringLiteral("particle_count")}), 16);
                op.particleCount = reserveParticles(op.particleCount);
                const QJsonArray b =
                    findField(o, {QStringLiteral("bounds"), QStringLiteral("rect"), QStringLiteral("box")}).toArray();
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
                            op.gradientCenter =
                                QPointF(op.gradientCenter.x() / canvasW, op.gradientCenter.y() / canvasH);
                        }
                    }
                }
                op.innerRadius =
                    toDoubleField(findField(o, {QStringLiteral("inner_radius"), QStringLiteral("inner")}), 0.15);
                op.outerRadius =
                    toDoubleField(findField(o, {QStringLiteral("outer_radius"), QStringLiteral("outer")}), 0.70);
                const qreal minCanvasDim = qMin(canvasW, canvasH);
                if (qMax(op.innerRadius, op.outerRadius) > kPixelCoordinateThreshold) {
                    op.innerRadius /= minCanvasDim;
                    op.outerRadius /= minCanvasDim;
                }
                op.density = qBound(4, toIntField(findField(o, {QStringLiteral("density")}), 48), 120);
                op.lineLengthJitter =
                    toDoubleField(findField(o, {QStringLiteral("line_length_jitter"), QStringLiteral("jitter")}), 0.20);
            } else if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
                const QJsonValue centerVal = findField(o, {QStringLiteral("center"), QStringLiteral("eye_center")});
                if (!centerVal.isUndefined() && !centerVal.isNull()) {
                    const auto cp = parsePoint(centerVal);
                    if (cp.second >= 0.0) {
                        op.eyeCenter = cp.first;
                        if (qMax(op.eyeCenter.x(), op.eyeCenter.y()) > kPixelCoordinateThreshold) {
                            op.eyeCenter = QPointF(op.eyeCenter.x() / canvasW, op.eyeCenter.y() / canvasH);
                        }
                    }
                }
                const QJsonArray szArr = findField(o, {QStringLiteral("size"), QStringLiteral("eye_size")}).toArray();
                if (szArr.size() >= 2) {
                    qreal ew = toDoubleField(szArr.at(0), 0.10);
                    qreal eh = toDoubleField(szArr.at(1), 0.12);
                    if (qMax(ew, eh) > kPixelCoordinateThreshold) {
                        ew /= canvasW;
                        eh /= canvasH;
                    }
                    op.eyeSize = QSizeF(qBound(0.01, ew, 0.50), qBound(0.01, eh, 0.50));
                } else {
                    op.eyeSize = QSizeF(0.10, 0.12);
                }
                op.eyeIrisColor =
                    parseColor(findField(o, {QStringLiteral("iris_color"), QStringLiteral("color")}).toString(),
                               QColor(60, 120, 240));
                op.eyeSecondaryColor = parseColor(
                    findField(o, {QStringLiteral("secondary_color"), QStringLiteral("secondary")}).toString(),
                    QColor(160, 210, 255));
                op.eyeStyle =
                    findField(o, {QStringLiteral("style"), QStringLiteral("eye_style")}, QStringLiteral("sparkle"))
                        .toString(QStringLiteral("sparkle"));
                op.eyeExpression = findField(o,
                                             {QStringLiteral("expression"), QStringLiteral("eye_expression")},
                                             QStringLiteral("open"))
                                       .toString(QStringLiteral("open"));
                op.eyeIsRight =
                    toBoolField(findField(o, {QStringLiteral("is_right"), QStringLiteral("right")}, false), false);
            } else if (op.kind == KisAiStrokeOperation::Kind::AnimeMouth) {
                const QJsonValue centerVal = findField(o, {QStringLiteral("center"), QStringLiteral("mouth_center")});
                if (!centerVal.isUndefined() && !centerVal.isNull()) {
                    const auto cp = parsePoint(centerVal);
                    if (cp.second >= 0.0) {
                        op.mouthCenter = cp.first;
                        if (qMax(op.mouthCenter.x(), op.mouthCenter.y()) > kPixelCoordinateThreshold) {
                            op.mouthCenter = QPointF(op.mouthCenter.x() / canvasW, op.mouthCenter.y() / canvasH);
                        }
                    }
                }
                const QJsonArray szArr = findField(o, {QStringLiteral("size"), QStringLiteral("mouth_size")}).toArray();
                if (szArr.size() >= 2) {
                    qreal mw = toDoubleField(szArr.at(0), 0.06);
                    qreal mh = toDoubleField(szArr.at(1), 0.03);
                    if (qMax(mw, mh) > kPixelCoordinateThreshold) {
                        mw /= canvasW;
                        mh /= canvasH;
                    }
                    op.mouthSize = QSizeF(qBound(0.01, mw, 0.40), qBound(0.005, mh, 0.30));
                } else {
                    op.mouthSize = QSizeF(0.06, 0.03);
                }
                op.mouthLipColor =
                    parseColor(findField(o, {QStringLiteral("lip_color"), QStringLiteral("color")}).toString(),
                               QColor(225, 115, 125));
                op.mouthExpression = findField(o,
                                               {QStringLiteral("expression"), QStringLiteral("mouth_expression")},
                                               QStringLiteral("smile"))
                                         .toString(QStringLiteral("smile"));
                op.mouthHasHighlight =
                    toBoolField(findField(o, {QStringLiteral("has_highlight"), QStringLiteral("highlight")}, true),
                                true);
            }

            if (op.kind != KisAiStrokeOperation::Kind::Unknown) {
                outProgram->operations.append(op);
            } else {
                const QVector<KisAiStrokeOperation> macroOps = expandMacroOperation(o, outProgram->canvasSize);
                if (!macroOps.isEmpty()) {
                    outProgram->operations.append(macroOps);
                }
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
        if (profile == QLatin1String("pastel") || profile == QLatin1String("chalk")
            || profile == QLatin1String("oil_pastel"))
            profile = QStringLiteral("crayon");
        if (profile == QLatin1String("glow") || profile == QLatin1String("laser") || profile == QLatin1String("light"))
            profile = QStringLiteral("neon");
        if (profile == QLatin1String("spatter") || profile == QLatin1String("blot")
            || profile == QLatin1String("fleck"))
            profile = QStringLiteral("splatter");
        if (profile == QLatin1String("chisel") || profile == QLatin1String("flat_pen")
            || profile == QLatin1String("ribbon_pen"))
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

        // Bound spacing before any policy test below reads it. The A5 hatch
        // rescue compares spacing against thresholds, so a hostile 1e300 (or NaN)
        // must not reach that comparison as an unbounded value.
        if (!std::isfinite(op.spacing))
            op.spacing = 0.015;
        op.spacing = qBound<qreal>(0.002, op.spacing, 0.2);

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
        if (op.layer == QLatin1String("Shading")
            && (op.kind == KisAiStrokeOperation::Kind::Fill || op.kind == KisAiStrokeOperation::Kind::GradientFill)) {
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
            // Phase 1: Suppress isolated stippling / tiny dot noise
            if (op.points.size() == 1) {
                const QString lowerId = op.id.toLower();
                const bool isIntentionalCatchlight = lowerId.contains(QLatin1String("glint"))
                    || lowerId.contains(QLatin1String("catchlight")) || lowerId.contains(QLatin1String("highlight"))
                    || lowerId.contains(QLatin1String("pupil")) || lowerId.contains(QLatin1String("eye"))
                    || lowerId.contains(QLatin1String("star")) || op.layer == QLatin1String("Highlights");
                if (!isIntentionalCatchlight) {
                    renderable = false;
                }
            } else if (op.points.size() == 2) {
                const qreal dist = std::hypot(op.points[0].pos.x() - op.points[1].pos.x(),
                                              op.points[0].pos.y() - op.points[1].pos.y());
                if (dist < 0.005) {
                    renderable = false;
                }
            }
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
                if (renderable && op.polygon.boundingRect().width() < 0.005
                    && op.polygon.boundingRect().height() < 0.005) {
                    renderable = false;
                }
            }
            // spacing was already bounded at the top of the loop (see the A5 hatch rescue).
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

            // Clamp particle count to prevent dense blizzard overcrowding, while
            // preserving 0 for parse-time budgeting. A zero count renders nothing,
            // so it must not be reported as renderable (the renderer returns
            // immediately and the op would only inflate counts/summaries).
            if (op.particleCount > 0) {
                op.particleCount = qBound(1, op.particleCount, 64);
            } else {
                op.particleCount = 0;
            }
            renderable = op.particleCount > 0 && op.bounds.width() > 1.0e-4 && op.bounds.height() > 1.0e-4;
            break;
        }
        case KisAiStrokeOperation::Kind::AnimeEye: {
            op.eyeCenter = clampedPoint(op.eyeCenter, &localReport.repairedValues);
            const qreal w = qBound<qreal>(0.02, op.eyeSize.width(), 0.40);
            const qreal h = qBound<qreal>(0.02, op.eyeSize.height(), 0.40);
            op.eyeSize = QSizeF(w, h);
            renderable = true;
            break;
        }
        case KisAiStrokeOperation::Kind::AnimeMouth: {
            op.mouthCenter = clampedPoint(op.mouthCenter, &localReport.repairedValues);
            const qreal w = qBound<qreal>(0.01, op.mouthSize.width(), 0.40);
            const qreal h = qBound<qreal>(0.005, op.mouthSize.height(), 0.30);
            op.mouthSize = QSizeF(w, h);
            renderable = true;
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
            const bool hasActionContext = promptLower.contains(QLatin1String("action"))
                || promptLower.contains(QLatin1String("speed")) || promptLower.contains(QLatin1String("battle"))
                || promptLower.contains(QLatin1String("impact")) || promptLower.contains(QLatin1String("manga"))
                || promptLower.contains(QLatin1String("comic")) || promptLower.contains(QLatin1String("burst"))
                || promptLower.contains(QLatin1String("dynamic"));
            const bool hasStaticContext = promptLower.contains(QLatin1String("portrait"))
                || promptLower.contains(QLatin1String("landscape")) || promptLower.contains(QLatin1String("scenery"))
                || promptLower.contains(QLatin1String("peaceful")) || promptLower.contains(QLatin1String("calm"))
                || promptLower.contains(QLatin1String("sunset")) || promptLower.contains(QLatin1String("sleep"));
            if (!hasActionContext && hasStaticContext) {
                renderable = false;
                break;
            }

            op.gradientCenter = clampedPoint(op.gradientCenter, &localReport.repairedValues);
            op.innerRadius = qBound<qreal>(0.01, std::isfinite(op.innerRadius) ? op.innerRadius : 0.15, 0.8);
            op.outerRadius =
                qBound<qreal>(op.innerRadius + 0.05, std::isfinite(op.outerRadius) ? op.outerRadius : 0.70, 1.5);
            op.density = qBound(4, op.density, 180);
            op.lineLengthJitter =
                qBound<qreal>(0.0, std::isfinite(op.lineLengthJitter) ? op.lineLengthJitter : 0.20, 0.9);
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

    // V3 Phase 0.1: Cap 'particles' operations per program so a single model
    // response can never blanket the canvas with stipple-noise layers.
    if (g_particleSuppressionEnabled) {
        int keptParticles = 0;
        QVector<KisAiStrokeOperation> capped;
        capped.reserve(refined.operations.size());
        for (const KisAiStrokeOperation &op : refined.operations) {
            if (op.kind != KisAiStrokeOperation::Kind::Particles) {
                capped.append(op);
                continue;
            }
            if (keptParticles < kMaxParticlesOperations) {
                capped.append(op);
                ++keptParticles;
            } else {
                ++localReport.droppedOperations;
            }
        }
        if (capped.size() != refined.operations.size()) {
            localReport.warnings.append(
                QStringLiteral("Excess 'particles' operations were dropped to prevent stipple-noise accumulation."));
        }
        refined.operations = capped;
    }

    // V3 Phase 0.3: Eye-pair symmetry lint. A single asymmetric eye pair is the
    // most visible face defect, so flag it for the quality self-correction loop.
    {
        QVector<const KisAiStrokeOperation *> eyes;
        for (const KisAiStrokeOperation &op : refined.operations) {
            if (op.kind == KisAiStrokeOperation::Kind::AnimeEye)
                eyes.append(&op);
        }
        if (eyes.size() == 2) {
            const qreal mirrorError = qAbs((eyes.at(0)->eyeCenter.x() + eyes.at(1)->eyeCenter.x()) * 0.5 - 0.5);
            const qreal yError = qAbs(eyes.at(0)->eyeCenter.y() - eyes.at(1)->eyeCenter.y());
            const qreal sizeError = qAbs(eyes.at(0)->eyeSize.width() - eyes.at(1)->eyeSize.width())
                + qAbs(eyes.at(0)->eyeSize.height() - eyes.at(1)->eyeSize.height());
            if (mirrorError > 0.08 || yError > 0.06 || sizeError > 0.06) {
                localReport.warnings.append(
                    QStringLiteral("Eye pair is asymmetric; check gaze alignment and matching eye sizes."));
            }
        } else if (eyes.size() > 2) {
            localReport.warnings.append(
                QStringLiteral("Unexpected eye count (%1); expected exactly 2 AnimeEye operations.")
                    .arg(eyes.size()));
        }
    }

    // Ensure deterministic back-to-front layer ordering: Background -> Flats -> Shading -> Lineart -> Highlights -> FX
    const auto layerOrder = [](const QString &layer) -> int {
        if (layer == QLatin1String("Background"))
            return 0;
        if (layer == QLatin1String("Flats"))
            return 1;
        if (layer == QLatin1String("Shading"))
            return 2;
        if (layer == QLatin1String("Lineart"))
            return 3;
        if (layer == QLatin1String("Highlights"))
            return 4;
        if (layer == QLatin1String("FX"))
            return 5;
        return 6;
    };
    std::stable_sort(refined.operations.begin(),
                     refined.operations.end(),
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

// V8 Phase 1.5: Deprecated wrapper. New code should use
// KisAi::QualityVectorEvaluator::evaluate() + KisAi::QualityVector::aggregate() to obtain
// a multi-dimensional quality vector (16 axes). This function is kept for backward
// compatibility with existing callers and tests (returns the same 5-axis weighted sum
// in [0,1] as before). Will be removed in a future Phase 6 cleanup.
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
        } else if (op.kind == KisAiStrokeOperation::Kind::Hatch) {
            if (!op.polygon.isEmpty()) {
                totalPolygonArea += polygonArea(op.polygon);
                geometryPoints += op.polygon.size();
            } else if (!op.points.isEmpty()) {
                geometryPoints += op.points.size();
            }
        } else if (op.kind == KisAiStrokeOperation::Kind::MangaLines) {
            geometryPoints += op.density * 2;
        } else if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
            geometryPoints += 16;
            ++continuousStrokes;
            totalPolygonArea += 0.05;
        } else if (op.kind == KisAiStrokeOperation::Kind::AnimeMouth) {
            geometryPoints += 8;
            ++continuousStrokes;
            totalPolygonArea += 0.02;
        }
    }

    // 1. Layer hierarchy score (0.30)
    qreal layerHierarchyScore = 0.0;
    if (layers.contains(QStringLiteral("Flats")))
        layerHierarchyScore += 0.35;
    if (layers.contains(QStringLiteral("Lineart")))
        layerHierarchyScore += 0.30;
    if (layers.contains(QStringLiteral("Shading")))
        layerHierarchyScore += 0.20;
    if (layers.contains(QStringLiteral("Highlights")))
        layerHierarchyScore += 0.10;
    if (layers.contains(QStringLiteral("Background")))
        layerHierarchyScore += 0.05;
    layerHierarchyScore = qBound<qreal>(0.0, layerHierarchyScore, 1.0);

    // 2. Real polygon silhouette area score (0.20)
    const qreal silhouetteAreaScore = qBound<qreal>(0.0, totalPolygonArea / 0.35, 1.0);

    // 3. Stroke continuity score (0.20): ratio of continuous 3+ point strokes
    const qreal strokeContinuityScore = (totalStrokes > 0) ? (qreal(continuousStrokes) / qreal(totalStrokes))
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

    const qreal finalScore = 0.30 * layerHierarchyScore + 0.20 * silhouetteAreaScore + 0.20 * strokeContinuityScore
        + 0.15 * colorDiversityScore + 0.15 * operationScore;

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
        // CHARACTER PORTRAIT (Dynamic 3D Geometry: Angles, Perspectives & Luster)
        // =========================================================================
        const QString pLower = prompt.toLower();
        const bool isProfile = pLower.contains(QLatin1String("profile")) || pLower.contains(QLatin1String("side view"))
            || prompt.contains(QStringLiteral("横顔"));
        const bool isThreeQuarter = pLower.contains(QLatin1String("three quarter"))
            || pLower.contains(QLatin1String("3/4")) || prompt.contains(QStringLiteral("斜め"))
            || pLower.contains(QLatin1String("looking left")) || pLower.contains(QLatin1String("looking right"))
            || (!isProfile && rng.bounded(100) < 65);
        const int facingSign = (pLower.contains(QLatin1String("looking left"))
                                || (!pLower.contains(QLatin1String("looking right")) && rng.bounded(100) < 50))
            ? -1
            : 1;

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

        // Center offsets based on perspective
        const qreal headX =
            isProfile ? (0.50 + facingSign * 0.08) : (isThreeQuarter ? (0.50 + facingSign * 0.035) : 0.50);
        const qreal headY = 0.46;

        // 1. Flats: Back hair mass
        {
            KisAiStrokeOperation backHair;
            backHair.kind = KisAiStrokeOperation::Kind::Fill;
            backHair.id = QStringLiteral("back_hair");
            backHair.layer = QStringLiteral("Flats");
            backHair.brush.color = hairColor.darker(130);
            if (isProfile) {
                const qreal backX = headX - facingSign * 0.22;
                backHair.polygon << QPointF(backX, 0.25) << QPointF(headX, 0.15)
                                 << QPointF(headX + facingSign * 0.12, 0.32) << QPointF(headX + facingSign * 0.15, 0.70)
                                 << QPointF(backX - facingSign * 0.08, 0.85) << QPointF(backX, 0.65);
            } else if (isThreeQuarter) {
                backHair.polygon << QPointF(headX - facingSign * 0.25, 0.35) << QPointF(headX, 0.15)
                                 << QPointF(headX + facingSign * 0.32, 0.35) << QPointF(headX + facingSign * 0.38, 0.75)
                                 << QPointF(headX + facingSign * 0.18, 0.86) << QPointF(headX - facingSign * 0.18, 0.86)
                                 << QPointF(headX - facingSign * 0.32, 0.75);
            } else {
                backHair.polygon << QPointF(0.20, 0.35) << QPointF(0.50, 0.15) << QPointF(0.80, 0.35)
                                 << QPointF(0.88, 0.75) << QPointF(0.68, 0.85) << QPointF(0.32, 0.85)
                                 << QPointF(0.12, 0.75);
            }
            program.operations.append(backHair);
        }

        // 2. Flats: Face & Neck Skin Base
        {
            KisAiStrokeOperation skin;
            skin.kind = KisAiStrokeOperation::Kind::Fill;
            skin.id = QStringLiteral("skin_base");
            skin.layer = QStringLiteral("Flats");
            skin.brush.color = skinColor;

            if (isProfile) {
                // Profile E-line silhouette
                skin.polygon << QPointF(headX - facingSign * 0.12, 0.30) << QPointF(headX + facingSign * 0.08, 0.26)
                             << QPointF(headX + facingSign * 0.18, 0.44) // Nose tip
                             << QPointF(headX + facingSign * 0.14, 0.50) // Lip philtrum
                             << QPointF(headX + facingSign * 0.16, 0.53) // Lower lip
                             << QPointF(headX + facingSign * 0.14, 0.66) // Chin tip
                             << QPointF(headX - facingSign * 0.04, 0.62) // Jaw angle
                             << QPointF(headX - facingSign * 0.12, 0.48); // Ear position
            } else if (isThreeQuarter) {
                skin.polygon << QPointF(headX - facingSign * 0.20, 0.32) << QPointF(headX, 0.27)
                             << QPointF(headX + facingSign * 0.18, 0.32)
                             << QPointF(headX + facingSign * 0.22, 0.48) // Near cheekbone
                             << QPointF(headX + facingSign * 0.06, 0.69) // Chin
                             << QPointF(headX - facingSign * 0.18, 0.54); // Far jaw
            } else {
                skin.polygon << QPointF(0.30, 0.32) << QPointF(0.50, 0.28) << QPointF(0.70, 0.32) << QPointF(0.72, 0.52)
                             << QPointF(0.50, 0.70) << QPointF(0.28, 0.52);
            }
            program.operations.append(skin);

            KisAiStrokeOperation neck;
            neck.kind = KisAiStrokeOperation::Kind::Fill;
            neck.id = QStringLiteral("neck_base");
            neck.layer = QStringLiteral("Flats");
            neck.brush.color = skinColor.darker(105);
            if (isProfile) {
                neck.polygon << QPointF(headX - facingSign * 0.02, 0.60) << QPointF(headX + facingSign * 0.08, 0.65)
                             << QPointF(headX + facingSign * 0.12, 0.85) << QPointF(headX - facingSign * 0.06, 0.85);
            } else if (isThreeQuarter) {
                neck.polygon << QPointF(headX - facingSign * 0.08, 0.64) << QPointF(headX + facingSign * 0.10, 0.65)
                             << QPointF(headX + facingSign * 0.14, 0.85) << QPointF(headX - facingSign * 0.12, 0.85);
            } else {
                neck.polygon << QPointF(0.42, 0.65) << QPointF(0.58, 0.65) << QPointF(0.62, 0.85)
                             << QPointF(0.38, 0.85);
            }
            program.operations.append(neck);
        }

        // 3. Flats: Sclera & Irises (Perspective scaled)
        QVector<int> sides = isProfile ? QVector<int>{1} : QVector<int>{-1, 1};
        for (int side : sides) {
            const bool isNear = (side == facingSign);
            const qreal eyeScale = (isThreeQuarter && !isNear) ? 0.72 : 1.0;
            const qreal ecx = isProfile ? (headX + facingSign * 0.07)
                                        : (headX + side * (isThreeQuarter ? (isNear ? 0.13 : 0.08) : 0.13));
            const qreal ecy = headY;

            KisAiStrokeOperation sclera;
            sclera.kind = KisAiStrokeOperation::Kind::Fill;
            sclera.id = QStringLiteral("sclera_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
            sclera.layer = QStringLiteral("Flats");
            sclera.brush.color = QColor(QStringLiteral("#f8f9fa"));
            const qreal sw = 0.055 * eyeScale;
            const qreal sh = 0.035 * eyeScale;
            sclera.polygon << QPointF(ecx - sw, ecy) << QPointF(ecx, ecy - sh) << QPointF(ecx + sw, ecy)
                           << QPointF(ecx, ecy + sh);
            program.operations.append(sclera);

            KisAiStrokeOperation iris;
            iris.kind = KisAiStrokeOperation::Kind::GradientFill;
            iris.id = QStringLiteral("iris_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
            iris.layer = QStringLiteral("Flats");
            iris.isRadial = true;
            iris.gradientCenter = QPointF(ecx, ecy);
            iris.gradientRadius = 0.035 * eyeScale;
            iris.gradientColors << eyeColor.lighter(130) << eyeColor << eyeColor.darker(150);
            const qreal iw = 0.032 * eyeScale;
            iris.polygon << QPointF(ecx - iw, ecy - sh) << QPointF(ecx + iw, ecy - sh) << QPointF(ecx + iw, ecy + sh)
                         << QPointF(ecx - iw, ecy + sh);
            program.operations.append(iris);
        }

        // 4. Flats: Front Hair Clumps
        {
            KisAiStrokeOperation bangs;
            bangs.kind = KisAiStrokeOperation::Kind::Fill;
            bangs.id = QStringLiteral("bangs_mass");
            bangs.layer = QStringLiteral("Flats");
            bangs.brush.color = hairColor;
            if (isProfile) {
                bangs.polygon << QPointF(headX - facingSign * 0.10, 0.28) << QPointF(headX + facingSign * 0.08, 0.20)
                              << QPointF(headX + facingSign * 0.16, 0.35) << QPointF(headX + facingSign * 0.12, 0.44)
                              << QPointF(headX + facingSign * 0.02, 0.36);
            } else if (isThreeQuarter) {
                bangs.polygon << QPointF(headX - facingSign * 0.24, 0.30) << QPointF(headX, 0.18)
                              << QPointF(headX + facingSign * 0.24, 0.28) << QPointF(headX + facingSign * 0.26, 0.44)
                              << QPointF(headX + facingSign * 0.12, 0.38) << QPointF(headX, 0.45)
                              << QPointF(headX - facingSign * 0.10, 0.38) << QPointF(headX - facingSign * 0.20, 0.42);
            } else {
                bangs.polygon << QPointF(0.24, 0.30) << QPointF(0.50, 0.18) << QPointF(0.76, 0.30)
                              << QPointF(0.72, 0.42) << QPointF(0.58, 0.38) << QPointF(0.50, 0.44)
                              << QPointF(0.42, 0.38) << QPointF(0.28, 0.42);
            }
            program.operations.append(bangs);
        }

        // 5. Shading: Directional Form Shading, hair cast, blush
        {
            // Bangs cast shadow with directional falloff
            KisAiStrokeOperation bangsCast;
            bangsCast.kind = KisAiStrokeOperation::Kind::Fill;
            bangsCast.id = QStringLiteral("bangs_shadow");
            bangsCast.layer = QStringLiteral("Shading");
            bangsCast.brush.color = QColor(QStringLiteral("#c48b80"));
            bangsCast.brush.opacity = 0.45;
            bangsCast.fillStyle = QStringLiteral("directional");
            bangsCast.angleDeg = 115.0;
            bangsCast.blendMode = QStringLiteral("multiply");
            bangsCast.clipToId = QStringLiteral("skin_base");
            if (isProfile) {
                bangsCast.polygon << QPointF(headX, 0.36) << QPointF(headX + facingSign * 0.12, 0.36)
                                  << QPointF(headX + facingSign * 0.10, 0.42) << QPointF(headX, 0.40);
            } else if (isThreeQuarter) {
                bangsCast.polygon << QPointF(headX - facingSign * 0.18, 0.38) << QPointF(headX, 0.40)
                                  << QPointF(headX + facingSign * 0.20, 0.38)
                                  << QPointF(headX + facingSign * 0.18, 0.44) << QPointF(headX, 0.46)
                                  << QPointF(headX - facingSign * 0.16, 0.44);
            } else {
                bangsCast.polygon << QPointF(0.28, 0.38) << QPointF(0.50, 0.40) << QPointF(0.72, 0.38)
                                  << QPointF(0.70, 0.44) << QPointF(0.50, 0.46) << QPointF(0.30, 0.44);
            }
            program.operations.append(bangsCast);

            // Cheeks Soft Blush (Radial)
            for (int side : sides) {
                const qreal eyeScale = (isThreeQuarter && side != facingSign) ? 0.72 : 1.0;
                const qreal bcx = isProfile
                    ? (headX + facingSign * 0.08)
                    : (headX + side * (isThreeQuarter ? (side == facingSign ? 0.14 : 0.08) : 0.14));
                KisAiStrokeOperation blush;
                blush.kind = KisAiStrokeOperation::Kind::Fill;
                blush.id = QStringLiteral("blush_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                blush.layer = QStringLiteral("Shading");
                blush.brush.color = blushColor;
                blush.brush.opacity = 0.35;
                blush.fillStyle = QStringLiteral("radial");
                blush.blendMode = QStringLiteral("multiply");
                const qreal bw = 0.04 * eyeScale;
                blush.polygon << QPointF(bcx - bw, 0.52) << QPointF(bcx + bw, 0.52) << QPointF(bcx + bw, 0.56)
                              << QPointF(bcx - bw, 0.56);
                program.operations.append(blush);
            }

            // Neck Contact Hatch Shading
            KisAiStrokeOperation neckHatch;
            neckHatch.kind = KisAiStrokeOperation::Kind::Hatch;
            neckHatch.id = QStringLiteral("neck_hatch");
            neckHatch.layer = QStringLiteral("Shading");
            neckHatch.brush.color = QColor(QStringLiteral("#a36a60"));
            neckHatch.brush.opacity = 0.55;
            neckHatch.angleDeg = 45.0;
            neckHatch.spacing = 0.012;
            neckHatch.blendMode = QStringLiteral("multiply");
            if (isProfile) {
                neckHatch.polygon << QPointF(headX, 0.64) << QPointF(headX + facingSign * 0.08, 0.68)
                                  << QPointF(headX + facingSign * 0.10, 0.80)
                                  << QPointF(headX - facingSign * 0.02, 0.80);
            } else if (isThreeQuarter) {
                neckHatch.polygon << QPointF(headX - facingSign * 0.06, 0.66)
                                  << QPointF(headX + facingSign * 0.08, 0.66)
                                  << QPointF(headX + facingSign * 0.12, 0.82)
                                  << QPointF(headX - facingSign * 0.10, 0.82);
            } else {
                neckHatch.polygon << QPointF(0.40, 0.66) << QPointF(0.60, 0.66) << QPointF(0.62, 0.82)
                                  << QPointF(0.38, 0.82);
            }
            program.operations.append(neckHatch);
        }

        // 6. Lineart: Jawline, Eyelashes, Double Eyelids, Nose, Mouth
        {
            // Jawline / Profile line
            KisAiStrokeOperation jaw;
            jaw.kind = KisAiStrokeOperation::Kind::Path;
            jaw.id = QStringLiteral("jawline");
            jaw.layer = QStringLiteral("Lineart");
            jaw.brush.profile = QStringLiteral("gpen");
            jaw.brush.color = inkColor;
            jaw.brush.size = 0.0035;

            if (isProfile) {
                jaw.points << KisAiStrokePoint(headX + facingSign * 0.08, 0.32, 0.4)
                           << KisAiStrokePoint(headX + facingSign * 0.18, 0.44, 0.9) // Nose
                           << KisAiStrokePoint(headX + facingSign * 0.14, 0.50, 0.6)
                           << KisAiStrokePoint(headX + facingSign * 0.16, 0.53, 0.8) // Lips
                           << KisAiStrokePoint(headX + facingSign * 0.14, 0.66, 0.9) // Chin
                           << KisAiStrokePoint(headX - facingSign * 0.04, 0.62, 0.6); // Jaw
            } else if (isThreeQuarter) {
                jaw.points << KisAiStrokePoint(headX - facingSign * 0.18, 0.44, 0.4)
                           << KisAiStrokePoint(headX - facingSign * 0.14, 0.56, 0.7)
                           << KisAiStrokePoint(headX + facingSign * 0.06, 0.69, 0.9)
                           << KisAiStrokePoint(headX + facingSign * 0.20, 0.56, 0.8)
                           << KisAiStrokePoint(headX + facingSign * 0.22, 0.44, 0.4);
            } else {
                jaw.points << KisAiStrokePoint(0.28, 0.48, 0.4) << KisAiStrokePoint(0.32, 0.58, 0.8)
                           << KisAiStrokePoint(0.50, 0.70, 0.9) << KisAiStrokePoint(0.68, 0.58, 0.8)
                           << KisAiStrokePoint(0.72, 0.48, 0.4);
            }
            program.operations.append(jaw);

            // Eyes: Upper lashes with separate flicks and double eyelid
            for (int side : sides) {
                const bool isNear = (side == facingSign);
                const qreal eyeScale = (isThreeQuarter && !isNear) ? 0.72 : 1.0;
                const qreal ecx = isProfile ? (headX + facingSign * 0.07)
                                            : (headX + side * (isThreeQuarter ? (isNear ? 0.13 : 0.08) : 0.13));
                const qreal ecy = headY;

                KisAiStrokeOperation upperLash;
                upperLash.kind = KisAiStrokeOperation::Kind::Path;
                upperLash.id = QStringLiteral("upper_lash_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                upperLash.layer = QStringLiteral("Lineart");
                upperLash.brush.profile = QStringLiteral("gpen");
                upperLash.brush.color = inkColor;
                upperLash.brush.size = 0.0045 * eyeScale;
                const qreal lw = 0.055 * eyeScale;
                upperLash.points << KisAiStrokePoint(ecx - side * lw * 0.9, ecy + 0.005, 0.3)
                                 << KisAiStrokePoint(ecx, ecy - 0.025 * eyeScale, 1.0)
                                 << KisAiStrokePoint(ecx + side * lw, ecy - 0.015 * eyeScale, 0.7)
                                 << KisAiStrokePoint(ecx + side * lw * 1.25, ecy - 0.025 * eyeScale, 0.2);
                program.operations.append(upperLash);

                // Separate Lash Clump (flick)
                KisAiStrokeOperation lashFlick;
                lashFlick.kind = KisAiStrokeOperation::Kind::Path;
                lashFlick.id = QStringLiteral("lash_flick_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                lashFlick.layer = QStringLiteral("Lineart");
                lashFlick.brush.profile = QStringLiteral("gpen");
                lashFlick.brush.color = inkColor;
                lashFlick.brush.size = 0.0025 * eyeScale;
                lashFlick.points << KisAiStrokePoint(ecx + side * lw * 0.8, ecy - 0.020 * eyeScale, 0.6)
                                 << KisAiStrokePoint(ecx + side * lw * 1.35, ecy - 0.032 * eyeScale, 0.1);
                program.operations.append(lashFlick);

                // Double eyelid crease
                KisAiStrokeOperation doubleLid;
                doubleLid.kind = KisAiStrokeOperation::Kind::Path;
                doubleLid.id = QStringLiteral("double_lid_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                doubleLid.layer = QStringLiteral("Lineart");
                doubleLid.brush.profile = QStringLiteral("fineliner");
                doubleLid.brush.color = inkColor.lighter(130);
                doubleLid.brush.size = 0.0018 * eyeScale;
                doubleLid.points << KisAiStrokePoint(ecx - side * lw * 0.6, ecy - 0.035 * eyeScale, 0.2)
                                 << KisAiStrokePoint(ecx, ecy - 0.040 * eyeScale, 0.6)
                                 << KisAiStrokePoint(ecx + side * lw * 0.7, ecy - 0.035 * eyeScale, 0.2);
                program.operations.append(doubleLid);
            }

            // Nose
            if (!isProfile) {
                KisAiStrokeOperation nose;
                nose.kind = KisAiStrokeOperation::Kind::Path;
                nose.id = QStringLiteral("nose");
                nose.layer = QStringLiteral("Lineart");
                nose.brush.profile = QStringLiteral("fineliner");
                nose.brush.color = inkColor.lighter(120);
                nose.brush.size = 0.0022;
                const qreal nx = isThreeQuarter ? (headX + facingSign * 0.02) : 0.50;
                nose.points << KisAiStrokePoint(nx, 0.54, 0.6) << KisAiStrokePoint(nx + 0.005, 0.548, 0.4);
                program.operations.append(nose);

                KisAiStrokeOperation mouth;
                mouth.kind = KisAiStrokeOperation::Kind::Path;
                mouth.id = QStringLiteral("mouth");
                mouth.layer = QStringLiteral("Lineart");
                mouth.brush.profile = QStringLiteral("gpen");
                mouth.brush.color = inkColor;
                mouth.brush.size = 0.0025;
                const qreal mx = isThreeQuarter ? (headX + facingSign * 0.03) : 0.50;
                mouth.points << KisAiStrokePoint(mx - 0.04, 0.61, 0.3) << KisAiStrokePoint(mx, 0.616, 0.8)
                             << KisAiStrokePoint(mx + 0.04, 0.61, 0.3);
                program.operations.append(mouth);
            }

            // Hair Strands (Ribbons)
            KisAiStrokeOperation hairStrand;
            hairStrand.kind = KisAiStrokeOperation::Kind::Ribbon;
            hairStrand.id = QStringLiteral("hair_strand_main");
            hairStrand.layer = QStringLiteral("Lineart");
            hairStrand.brush.color = hairColor.darker(110);
            hairStrand.widthStart = 0.025;
            hairStrand.widthMid = 0.018;
            hairStrand.widthEnd = 0.004;
            if (isProfile) {
                hairStrand.spine << QPointF(headX - facingSign * 0.05, 0.28) << QPointF(headX + facingSign * 0.08, 0.50)
                                 << QPointF(headX + facingSign * 0.12, 0.72);
            } else if (isThreeQuarter) {
                hairStrand.spine << QPointF(headX + facingSign * 0.15, 0.26) << QPointF(headX + facingSign * 0.28, 0.48)
                                 << QPointF(headX + facingSign * 0.30, 0.72);
            } else {
                hairStrand.spine << QPointF(0.68, 0.28) << QPointF(0.76, 0.48) << QPointF(0.78, 0.70);
            }
            program.operations.append(hairStrand);
        }

        // 7. Highlights: Specular catchlights & angel halo with Color Dodge
        {
            for (int side : sides) {
                const bool isNear = (side == facingSign);
                const qreal eyeScale = (isThreeQuarter && !isNear) ? 0.72 : 1.0;
                const qreal ecx = isProfile ? (headX + facingSign * 0.07)
                                            : (headX + side * (isThreeQuarter ? (isNear ? 0.13 : 0.08) : 0.13));
                const qreal ecy = headY;

                // Main bright eye catchlight (Color Dodge)
                KisAiStrokeOperation catchlight;
                catchlight.kind = KisAiStrokeOperation::Kind::Path;
                catchlight.id = QStringLiteral("catchlight_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                catchlight.layer = QStringLiteral("Highlights");
                catchlight.brush.profile = QStringLiteral("gpen");
                catchlight.brush.color = QColor(QStringLiteral("#ffffff"));
                catchlight.brush.size = 0.0045 * eyeScale;
                catchlight.blendMode = QStringLiteral("color_dodge");
                catchlight.points << KisAiStrokePoint(ecx - 0.012 * eyeScale, ecy - 0.012 * eyeScale, 1.0)
                                  << KisAiStrokePoint(ecx - 0.008 * eyeScale, ecy - 0.008 * eyeScale, 1.0);
                program.operations.append(catchlight);

                // Crescent lower rim glow (Color Dodge)
                KisAiStrokeOperation crescent;
                crescent.kind = KisAiStrokeOperation::Kind::Path;
                crescent.id = QStringLiteral("crescent_") + (side < 0 ? QStringLiteral("l") : QStringLiteral("r"));
                crescent.layer = QStringLiteral("Highlights");
                crescent.brush.profile = QStringLiteral("gpen");
                crescent.brush.color = QColor(QStringLiteral("#aae0ff"));
                crescent.brush.size = 0.0022 * eyeScale;
                crescent.blendMode = QStringLiteral("color_dodge");
                const qreal cw = 0.018 * eyeScale;
                crescent.points << KisAiStrokePoint(ecx - cw, ecy + cw, 0.4)
                                << KisAiStrokePoint(ecx, ecy + 0.025 * eyeScale, 0.8)
                                << KisAiStrokePoint(ecx + cw, ecy + cw, 0.4);
                program.operations.append(crescent);
            }

            // Hair Angel Halo Rim Light (Color Dodge)
            KisAiStrokeOperation halo;
            halo.kind = KisAiStrokeOperation::Kind::Path;
            halo.id = QStringLiteral("hair_halo");
            halo.layer = QStringLiteral("Highlights");
            halo.brush.profile = QStringLiteral("airbrush");
            halo.brush.color = QColor(QStringLiteral("#ffffff"));
            halo.brush.opacity = 0.70;
            halo.brush.size = 0.012;
            halo.blendMode = QStringLiteral("color_dodge");
            halo.points << KisAiStrokePoint(headX - 0.18, 0.25, 0.2) << KisAiStrokePoint(headX, 0.21, 0.9)
                        << KisAiStrokePoint(headX + 0.18, 0.25, 0.2);
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
    } else if (spec.domain == KisAiPromptAnalyzer::DomainType::Cyberpunk) {
        // 0. Background: Dark neon skyline gradient
        {
            KisAiStrokeOperation bg;
            bg.kind = KisAiStrokeOperation::Kind::GradientFill;
            bg.id = QStringLiteral("cyber_sky");
            bg.layer = QStringLiteral("Background");
            bg.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 0.70) << QPointF(0.0, 0.70);
            bg.gradientColors << QColor(QStringLiteral("#0b0c16")) << QColor(QStringLiteral("#1e0836"))
                              << QColor(QStringLiteral("#380e4a"));
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
            grid.points << KisAiStrokePoint(0.0, 0.62, 0.8) << KisAiStrokePoint(0.50, 0.62, 0.9)
                        << KisAiStrokePoint(1.0, 0.62, 0.8);
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
    } else if (spec.domain == KisAiPromptAnalyzer::DomainType::Botanical) {
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
            blossom.polygon << QPointF(0.40, 0.35) << QPointF(0.50, 0.22) << QPointF(0.60, 0.35) << QPointF(0.68, 0.48)
                            << QPointF(0.50, 0.60) << QPointF(0.32, 0.48);
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
            veinHatch.polygon << QPointF(0.35, 0.65) << QPointF(0.18, 0.58) << QPointF(0.12, 0.72)
                              << QPointF(0.32, 0.76);
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
    } else if (spec.domain == KisAiPromptAnalyzer::DomainType::Creature) {
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
            body.polygon << QPointF(0.42, 0.32) << QPointF(0.58, 0.32) << QPointF(0.68, 0.52) << QPointF(0.55, 0.78)
                         << QPointF(0.40, 0.78) << QPointF(0.35, 0.52);
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
            scaleHatch.polygon << QPointF(0.42, 0.38) << QPointF(0.58, 0.38) << QPointF(0.55, 0.78)
                               << QPointF(0.40, 0.78);
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
    } else if (spec.domain == KisAiPromptAnalyzer::DomainType::MangaFx) {
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
            impact.polygon << QPointF(0.42, 0.44) << QPointF(0.50, 0.38) << QPointF(0.58, 0.44) << QPointF(0.56, 0.56)
                           << QPointF(0.44, 0.56);
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
            slash.points << KisAiStrokePoint(0.20, 0.25, 0.9) << KisAiStrokePoint(0.50, 0.50, 1.0)
                         << KisAiStrokePoint(0.80, 0.75, 0.9);
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
    // QSet iteration order is unspecified, so the digest built from it (and any
    // prompt derived from that digest) would vary between runs. Sort for a
    // stable, reproducible palette.
    QStringList paletteColors = colorSet.values();
    paletteColors.sort();
    int count = 0;
    for (const QString &c : paletteColors) {
        palArr.append(c);
        if (++count >= 12)
            break;
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

QJsonObject KisAiStrokeProgramCodec::buildGoalStepPayload(const QString &model,
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
                                                          const QString &visionDetail,
                                                          bool forceJsonObjectOnly,
                                                          bool isRefinementExtraStep,
                                                          qreal targetReadiness,
                                                          const QString &referenceImageBase64,
                                                          qint64 seed)
{
    const bool reasoning = isReasoningModel(model);
    const bool vision = includeVision && isVisionModel(model)
        && (!imageBase64.trimmed().isEmpty() || !referenceImageBase64.trimmed().isEmpty());
    auto spec = KisAiPromptAnalyzer::analyze(prompt, canvasSize);
    if (artStyle > 0 && artStyle <= 7) {
        spec.style = static_cast<KisAiPromptAnalyzer::ArtStyle>(artStyle);
    }
    const QString phaseGuidance = KisAiPromptAnalyzer::generateGoalPhaseGuidance(step, spec, canvasSize, totalSteps);

    QString combinedInstructions = phaseGuidance;
    if (!additionalInstruction.trimmed().isEmpty()) {
        if (!additionalInstruction.contains(phaseGuidance.trimmed())) {
            combinedInstructions +=
                QStringLiteral("\n\n[USER ADDITIONAL FEEDBACK]\n") + additionalInstruction.trimmed();
        } else {
            combinedInstructions = additionalInstruction.trimmed();
        }
    }

    const bool advancedStroke = KisAiModelRouter::shouldUseAdvancedStrokeLogic(model, KisAiModelRouter::qualityMode());
    const QString systemText = buildSystemPrompt(canvasSize, prompt, combinedInstructions, artStyle, advancedStroke);

    const int geometryBudget = qBound(50, strokeBudget, 3000);
    const int baseStepTarget = (totalSteps >= 18)
        ? qBound(15, geometryBudget / 18, 60)
        : (geometryBudget / (totalSteps > 0 ? qMax(1, totalSteps) : 4));
    const int operationTarget = qBound(12, baseStepTarget, 150);

    const bool isExtraRefine = (isRefinementExtraStep || step > totalSteps);
    QString phaseName;
    if (isExtraRefine) {
        phaseName = QStringLiteral("Autonomous Polish & Defect Correction (Refinement %1)").arg(qMax(1, step - totalSteps));
    } else if (totalSteps >= 18) {
        static const QString s_masterPhases[] = {
            QStringLiteral("Composition, Proportions & Canvas Layout"),
            QStringLiteral("Atmospheric Background Wash & Horizon Gradients"),
            QStringLiteral("Environment Structures & Midground Elements"),
            QStringLiteral("Character Silhouettes & Base Blocking"),
            QStringLiteral("Flat Coloring - Skin Base & Undergarments"),
            QStringLiteral("Flat Coloring - Hair Clusters & Costumes"),
            QStringLiteral("Primary Form Shading & Global Light Direction"),
            QStringLiteral("Secondary Cast Shadows & Ambient Occlusion"),
            QStringLiteral("Subsurface Scattering & Warmth Blush Wash"),
            QStringLiteral("Structural Rough Contours & Feature Registration"),
            QStringLiteral("Deliberate Micro-Inking - Eyes & Expression"),
            QStringLiteral("Deliberate Precision Inking - Silhouettes & Outer Contours"),
            QStringLiteral("Deliberate Precision Inking - Hair Strands & Flow Splines"),
            QStringLiteral("Deliberate Precision Inking - Cloth Folds, Seams & Drapery"),
            QStringLiteral("Delicate Form Hatching & Corner Inking Fillets"),
            QStringLiteral("Primary Diffuse Highlights & Hair Angel Halo"),
            QStringLiteral("Specular Glints, Lip Shine & Eye Catchlights"),
            QStringLiteral("Atmospheric Rim Light, Bloom & Masterwork Polish")
        };
        const int idx = qBound(0, step - 1, 17);
        phaseName = s_masterPhases[idx];
    } else if (totalSteps <= 2) {
        phaseName =
            (step == 1) ? QStringLiteral("Flats & Shading Foundation") : QStringLiteral("Lineart, Highlights & Polish");
    } else if (totalSteps == 3) {
        phaseName = (step == 1) ? QStringLiteral("Flats & Background")
            : (step == 2)       ? QStringLiteral("Shading & Lineart")
                                : QStringLiteral("Highlights & FX Polish");
    } else if (totalSteps == 4) {
        phaseName = (step == 1) ? QStringLiteral("Flats & Background")
            : (step == 2)       ? QStringLiteral("Shading & Ambient Occlusion")
            : (step == 3)       ? QStringLiteral("Lineart & Details")
                                : QStringLiteral("Highlights & FX Polish");
    } else if (totalSteps == 5) {
        phaseName = (step == 1) ? QStringLiteral("Flats & Background")
            : (step == 2)       ? QStringLiteral("Shading & Ambient Occlusion")
            : (step == 3)       ? QStringLiteral("Lineart & Details")
            : (step == 4)       ? QStringLiteral("Specular Highlights")
                                : QStringLiteral("FX & Final Polish");
    } else {
        phaseName = (step == 1) ? QStringLiteral("Background Atmosphere")
            : (step == 2)       ? QStringLiteral("Flats & Silhouettes")
            : (step == 3)       ? QStringLiteral("Shading & Ambient Occlusion")
            : (step == 4)       ? QStringLiteral("Lineart & Details")
            : (step == 5)       ? QStringLiteral("Specular Highlights")
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
    if (isExtraRefine) {
        userObj[QStringLiteral("is_refinement_mode")] = true;
        userObj[QStringLiteral("target_readiness")] = targetReadiness;
    }

    if (accumulatedProgram && !accumulatedProgram->operations.isEmpty()) {
        userObj[QStringLiteral("accumulated_context")] = buildGeometryDigest(*accumulatedProgram);
    }
    if (accumulatedProgram && !accumulatedProgram->critiqueRegions.isEmpty()) {
        QJsonArray regArr;
        for (const auto &reg : accumulatedProgram->critiqueRegions) {
            QJsonObject rObj;
            rObj[QStringLiteral("area")] = reg.area;
            rObj[QStringLiteral("issue")] = reg.issue;
            rObj[QStringLiteral("action")] = reg.action;
            rObj[QStringLiteral("priority")] = reg.priority;
            regArr.append(rObj);
        }
        userObj[QStringLiteral("previous_step_critique_regions")] = regArr;
    }
    if (!previousCritique.trimmed().isEmpty()) {
        userObj[QStringLiteral("previous_step_critique")] = previousCritique.trimmed();
    }

    if (isExtraRefine) {
        userObj[QStringLiteral("directive")] =
            QStringLiteral(
                "Execute Autonomous Refinement Step %1 (Refinement Round %2) in Goal Mode for prompt: '%3'. "
                "Baseline structural phases are complete, but quality is below target readiness (%4). "
                "Perform surgical refinement with masterly single-stroke care: "
                "1. [OBSERVE & CRITIQUE]: Inspect the canvas screenshot and accumulated geometry. "
                "Provide concise 'agent_critique' and structured 'regions' array: "
                "[{\"area\": \"left_eye|right_eye|hair|face_skin|shading|highlights|background|fx\", \"issue\": \"defect "
                "description\", \"action\": \"repaint|soften|remove|keep\", \"priority\": 1-5}]. "
                "2. [READINESS EVALUATION]: Accurately assess 'readiness_score' (0.0 to 1.0). If >= %4 and truly finished with no remaining defects, set 'goal_reached' to true. Otherwise keep 'goal_reached' false and state what needs work. "
                "3. [SURGICAL POLISH & DELIBERATE INKING]: Do NOT redraw whole silhouettes. Emit high-impact corrective strokes: exquisite micro-linework, occlusion shading, highlights, or eraser strokes (is_eraser: true) to clean stray lines. "
                "Set 'step_phase' to '%5', 'current_step' to %1. Output strictly valid RFC 8259 JSON.")
                .arg(step)
                .arg(qMax(1, step - totalSteps))
                .arg(prompt)
                .arg(QString::number(targetReadiness, 'f', 2))
                .arg(phaseName);
    } else {
        userObj[QStringLiteral("directive")] =
            QStringLiteral(
                "Execute Step %1 of %2 in Goal Mode for prompt: '%5'. "
                "Masterwork Directive (1-Hour Session): Treat this drawing step with profound care, patience, and deliberate craftsmanship. "
                "Every single stroke must be executed with intentional curve design, smooth Catmull-Rom curvature, and dynamic line-weight tapering. "
                "1. [OBSERVE & CRITIQUE]: Inspect the canvas screenshot (if attached) and accumulated geometry. "
                "Provide concise 'agent_critique' and structured 'regions' array: "
                "[{\"area\": \"left_eye|right_eye|hair|face_skin|shading|highlights|background|fx\", \"issue\": \"defect "
                "description\", \"action\": \"repaint|soften|remove|keep\", \"priority\": 1-5}]. "
                "2. [FOCUS]: Specify 'target_focus_area' (e.g. 'Face & Eyes Micro-Inking', 'Hair Strands & Curvature', 'Volumetric Shading & AO', 'Specular Accents'). "
                "3. [READINESS EVALUATION]: Provide 'readiness_score' from 0.0 (bare outline) to 1.0 (finished "
                "presentation). If >= %6 and presentation-ready, set 'goal_reached' to true. "
                "4. [ACT & REFINE WITH SINGLE STROKE CRAFTSMANSHIP]: Generate the necessary high-precision operations for phase '%3'. "
                "Dedicate your stroke budget to beautifully constructed, smooth strokes. Never rush or output coarse zigzag scribbles. "
                "If previous critique regions identified defects (e.g. weak facial lines, missing cast shadows, misaligned "
                "features), "
                "actively emit targeted correction operations: refine those specific features with exquisite linework, add "
                "localized directional shading, "
                "or use is_eraser: true to clean up errant strokes. Set 'step_phase' to '%3', 'current_step' to %1, and "
                "'goal_reached' to %4. "
                "Your operations are cumulatively merged onto the canvas; do NOT redraw base silhouettes from scratch "
                "unless correcting them. "
                "Output strictly valid RFC 8259 JSON without markdown fences.")
                .arg(step)
                .arg(totalSteps)
                .arg(phaseName)
                .arg(step >= totalSteps ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(prompt)
                .arg(QString::number(targetReadiness, 'f', 2));
    }

    const QString userText = QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact));

    QJsonArray messages;
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});

    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    if (vision && (!imageBase64.trimmed().isEmpty() || !referenceImageBase64.trimmed().isEmpty())) {
        QJsonArray contentArray;
        contentArray.append(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), userText}});

        if (!referenceImageBase64.trimmed().isEmpty()) {
            QString refUrl = referenceImageBase64.trimmed();
            if (!refUrl.startsWith(QLatin1String("data:image/"))) {
                refUrl = QStringLiteral("data:image/jpeg;base64,") + refUrl;
            }
            contentArray.append(
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                            {QStringLiteral("text"), QStringLiteral("[Reference Image / Target Style & Character]") }});
            contentArray.append(
                QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                            {QStringLiteral("image_url"),
                             QJsonObject{{QStringLiteral("url"), refUrl}, {QStringLiteral("detail"), QStringLiteral("high")}}}});
        }

        if (!imageBase64.trimmed().isEmpty()) {
            QString imageUrl = imageBase64.trimmed();
            if (!imageUrl.startsWith(QLatin1String("data:image/"))) {
                imageUrl = QStringLiteral("data:image/jpeg;base64,") + imageUrl;
            }

            QString detail = visionDetail.trimmed().toLower();
            if (detail.isEmpty() || detail == QLatin1String("auto")) {
                detail = (step >= totalSteps) ? QStringLiteral("high") : QStringLiteral("low");
            }

            if (!referenceImageBase64.trimmed().isEmpty()) {
                contentArray.append(
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("[Current Canvas Screenshot / Painting Progress]") }});
            }

            contentArray.append(
                QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                            {QStringLiteral("image_url"),
                             QJsonObject{{QStringLiteral("url"), imageUrl}, {QStringLiteral("detail"), detail}}}});
        }
        userMsg[QStringLiteral("content")] = contentArray;
    } else {
        userMsg[QStringLiteral("content")] = userText;
    }
    messages.append(userMsg);

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.trimmed();
    payload[QStringLiteral("messages")] = messages;
    const qint64 effectiveSeed = (seed >= 0)
        ? static_cast<qint64>(seed & 0x7FFFFFFF)
        : static_cast<qint64>(stableSeed(prompt.simplified()) & 0x7FFFFFFFU);
    payload[QStringLiteral("seed")] = effectiveSeed;

    if (enableStreaming) {
        payload[QStringLiteral("stream")] = true;
    }

    if (enforceJsonFormat) {
        if (!forceJsonObjectOnly && supportsJsonSchema(model)) {
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
    if (advancedStroke && maxTokensOverride <= 0) {
        calculatedTokens = qMax(calculatedTokens, reasoning ? 16384 : 8192);
    }
    if (reasoning) {
        payload[QStringLiteral("max_completion_tokens")] = calculatedTokens;
        if (!reasoningEffort.isEmpty() && reasoningEffort.toLower() != QLatin1String("none")) {
            payload[QStringLiteral("reasoning_effort")] = reasoningEffort.toLower();
        }
    } else {
        payload[QStringLiteral("max_tokens")] = calculatedTokens;
        payload[QStringLiteral("temperature")] = qBound<qreal>(0.0, temperature, 2.0);
        // Mirror buildChatCompletionsPayload(): omit top_p for the default 1.0
        // and for non-positive values instead of sending a fabricated 0.05.
        if (topP > 0.0 && topP < 1.0) {
            payload[QStringLiteral("top_p")] = qBound<qreal>(0.01, topP, 1.0);
        }
    }

    return payload;
}

KisAiStrokeProgram KisAiStrokeProgramCodec::createDeterministicProgramStep(const QString &prompt,
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
    stepProg.readinessScore = qBound(0.0, qreal(step) / qreal(qMax(1, totalSteps)), 1.0);
    stepProg.goalReached = (step >= totalSteps);

    for (const KisAiStrokeOperation &op : full.operations) {
        const QString l = normalizeLayerName(op.layer);
        bool match = false;
        if (totalSteps <= 2) {
            if (step == 1) {
                match =
                    (l == QLatin1String("Background") || l == QLatin1String("Flats") || l == QLatin1String("Shading"));
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
        } else if (totalSteps >= 18) {
            // Distribute operations across the 18 masterwork phases
            if (step <= 3) {
                match = (l == QLatin1String("Background"));
            } else if (step <= 6) {
                match = (l == QLatin1String("Flats"));
            } else if (step <= 9) {
                match = (l == QLatin1String("Shading"));
            } else if (step <= 15) {
                match = (l == QLatin1String("Lineart"));
            } else if (step <= 17) {
                match = (l == QLatin1String("Highlights"));
            } else {
                match = (l == QLatin1String("FX"));
            }
        } else { // 6 to 17
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

    if (totalSteps >= 18) {
        static const QString s_masterPhases[] = {
            QStringLiteral("Composition, Proportions & Canvas Layout"),
            QStringLiteral("Atmospheric Background Wash & Horizon Gradients"),
            QStringLiteral("Environment Structures & Midground Elements"),
            QStringLiteral("Character Silhouettes & Base Blocking"),
            QStringLiteral("Flat Coloring - Skin Base & Undergarments"),
            QStringLiteral("Flat Coloring - Hair Clusters & Costumes"),
            QStringLiteral("Primary Form Shading & Global Light Direction"),
            QStringLiteral("Secondary Cast Shadows & Ambient Occlusion"),
            QStringLiteral("Subsurface Scattering & Warmth Blush Wash"),
            QStringLiteral("Structural Rough Contours & Feature Registration"),
            QStringLiteral("Deliberate Micro-Inking - Eyes & Expression"),
            QStringLiteral("Deliberate Precision Inking - Silhouettes & Outer Contours"),
            QStringLiteral("Deliberate Precision Inking - Hair Strands & Flow Splines"),
            QStringLiteral("Deliberate Precision Inking - Cloth Folds, Seams & Drapery"),
            QStringLiteral("Delicate Form Hatching & Corner Inking Fillets"),
            QStringLiteral("Primary Diffuse Highlights & Hair Angel Halo"),
            QStringLiteral("Specular Glints, Lip Shine & Eye Catchlights"),
            QStringLiteral("Atmospheric Rim Light, Bloom & Masterwork Polish")
        };
        const int idx = qBound(0, step - 1, 17);
        stepProg.stepPhase = s_masterPhases[idx];
        stepProg.visualCritique = QStringLiteral("Masterwork phase %1 (%2) executed with deliberate stroke precision.")
                                      .arg(step)
                                      .arg(stepProg.stepPhase);
    } else if (totalSteps <= 2) {
        if (step == 1) {
            stepProg.stepPhase = QStringLiteral("Flats & Shading Foundation");
            stepProg.visualCritique = QStringLiteral("Base silhouettes, flats, and volume blocking are established.");
        } else {
            stepProg.stepPhase = QStringLiteral("Lineart, Highlights & Final FX");
            stepProg.visualCritique =
                QStringLiteral("Contour lineart, highlights, and final FX polish completed. Goal reached.");
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
            stepProg.visualCritique =
                QStringLiteral("Floating particles, atmospheric effects, and polish applied. Goal reached.");
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

void KisAiStrokeProgramCodec::setParticleSuppressionEnabled(bool enabled)
{
    g_particleSuppressionEnabled.store(enabled, std::memory_order_relaxed);
}

bool KisAiStrokeProgramCodec::isParticleSuppressionEnabled()
{
    return g_particleSuppressionEnabled.load(std::memory_order_relaxed);
}

int KisAiStrokeProgramCodec::maxParticlesOperations()
{
    return kMaxParticlesOperations;
}

KisAiStrokeProgram KisAiStrokeProgramCodec::mergePrograms(const KisAiStrokeProgram &base,
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

    // V3 Phase 0.1: Block Goal Mode particle accumulation. If the base already
    // carries atmospheric particles, further steps must not pile more of them
    // on top (blizzard-noise). Otherwise cap freshly merged particle ops.
    const bool baseHasParticles =
        std::any_of(base.operations.cbegin(), base.operations.cend(), [](const KisAiStrokeOperation &op) {
            return op.kind == KisAiStrokeOperation::Kind::Particles;
        });
    int mergedParticleCount = 0;
    for (const KisAiStrokeOperation &op : base.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::Particles)
            ++mergedParticleCount;
    }
    for (const KisAiStrokeOperation &op : extension.operations) {
        if (op.kind != KisAiStrokeOperation::Kind::Particles) {
            merged.operations.append(op);
            continue;
        }
        if (!g_particleSuppressionEnabled) {
            merged.operations.append(op);
            continue;
        }
        if (baseHasParticles)
            continue;
        if (mergedParticleCount >= kMaxMergedParticlesOperations)
            continue;
        merged.operations.append(op);
        ++mergedParticleCount;
    }
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
