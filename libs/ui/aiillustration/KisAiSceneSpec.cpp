/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiSceneSpec.h"
#include "KisAiStrokeProgram.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace
{
QString normalizeEnum(const QString &value, const QStringList &allowed, const QString &fallback)
{
    const QString v = value.trimmed().toLower();
    if (allowed.contains(v))
        return v;
    return fallback;
}

QPointF parsePoint(const QJsonValue &v, const QPointF &fallback)
{
    if (!v.isArray())
        return fallback;
    const QJsonArray a = v.toArray();
    if (a.size() < 2)
        return fallback;
    bool okX = false, okY = false;
    const qreal x = a.at(0).toVariant().toDouble(&okX);
    const qreal y = a.at(1).toVariant().toDouble(&okY);
    if (!okX || !okY)
        return fallback;
    return QPointF(qBound<qreal>(-1.0, x, 1.0), qBound<qreal>(-1.0, y, 1.0));
}

QPointF parseUnitPoint(const QJsonValue &v, const QPointF &fallback)
{
    const QPointF p = parsePoint(v, fallback);
    if (p == fallback)
        return fallback;
    return QPointF(qBound<qreal>(0.0, p.x(), 1.0), qBound<qreal>(0.0, p.y(), 1.0));
}

QColor parseColorField(const QJsonObject &obj, const QString &key, const QColor &fallback)
{
    if (!obj.contains(key))
        return fallback;
    const QJsonValue v = obj.value(key);
    if (!v.isString())
        return fallback;
    const QColor c = KisAiStrokeProgramCodec::parseColor(v.toString(), QColor());
    return c.isValid() ? c : fallback;
}
} // namespace

QJsonObject KisAiSceneSpecCodec::sceneSpecJsonSchema()
{
    // Strict Structured Outputs contract: meaning only, zero coordinate fields.
    auto strEnum = [](const QStringList &values) {
        QJsonObject o;
        o.insert(QStringLiteral("type"), QStringLiteral("string"));
        QJsonArray e;
        for (const QString &v : values)
            e.append(v);
        o.insert(QStringLiteral("enum"), e);
        return o;
    };
    auto color = [] {
        QJsonObject o;
        o.insert(QStringLiteral("type"), QStringLiteral("string"));
        o.insert(QStringLiteral("description"),
                 QStringLiteral("CSS hex color like #2b3a67"));
        return o;
    };

    QJsonObject props;
    QJsonObject subject;
    subject.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject subjectProps;
    subjectProps.insert(QStringLiteral("type"), strEnum({QStringLiteral("character"), QStringLiteral("landscape"), QStringLiteral("creature"), QStringLiteral("object")}));
    subjectProps.insert(QStringLiteral("pose_id"), strEnum({QStringLiteral("three_quarter_bust"), QStringLiteral("front_bust"), QStringLiteral("profile_bust"), QStringLiteral("upper_body"), QStringLiteral("full_body"), QStringLiteral("wide_scene")}));
    subjectProps.insert(QStringLiteral("facing"), strEnum({QStringLiteral("front"), QStringLiteral("front-right"), QStringLiteral("front-left"), QStringLiteral("profile")}));
    subject.insert(QStringLiteral("properties"), subjectProps);
    subject.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("subject"), subject);

    QJsonObject head;
    head.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject headProps;
    headProps.insert(QStringLiteral("expression"), strEnum({QStringLiteral("smile_open"), QStringLiteral("smile_closed"), QStringLiteral("neutral"), QStringLiteral("half"), QStringLiteral("closed")}));
    headProps.insert(QStringLiteral("gaze"), strEnum({QStringLiteral("front"), QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("up")}));
    headProps.insert(QStringLiteral("hair_style"), strEnum({QStringLiteral("long_hime"), QStringLiteral("long_wavy"), QStringLiteral("bob"), QStringLiteral("twin_tails"), QStringLiteral("short_messy"), QStringLiteral("short_straight")}));
    headProps.insert(QStringLiteral("hair_color"), color());
    headProps.insert(QStringLiteral("eye_color"), color());
    headProps.insert(QStringLiteral("skin_tone"), color());
    head.insert(QStringLiteral("properties"), headProps);
    head.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("head"), head);

    QJsonObject composition;
    composition.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject compProps;
    compProps.insert(QStringLiteral("framing"), strEnum({QStringLiteral("face_closeup"), QStringLiteral("bust_up"), QStringLiteral("upper_body"), QStringLiteral("full_body"), QStringLiteral("wide")}));
    QJsonObject center;
    center.insert(QStringLiteral("type"), QStringLiteral("array"));
    center.insert(QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}});
    center.insert(QStringLiteral("minItems"), 2);
    center.insert(QStringLiteral("maxItems"), 2);
    compProps.insert(QStringLiteral("head_center"), center);
    QJsonObject hh;
    hh.insert(QStringLiteral("type"), QStringLiteral("number"));
    hh.insert(QStringLiteral("minimum"), 0.15);
    hh.insert(QStringLiteral("maximum"), 0.80);
    compProps.insert(QStringLiteral("head_height"), hh);
    composition.insert(QStringLiteral("properties"), compProps);
    composition.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("composition"), composition);

    QJsonObject light;
    light.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject lightProps;
    lightProps.insert(QStringLiteral("time"), strEnum({QStringLiteral("day"), QStringLiteral("sunset"), QStringLiteral("night")}));
    lightProps.insert(QStringLiteral("warmth"), strEnum({QStringLiteral("warm_key_cool_fill"), QStringLiteral("cool_key_warm_fill"), QStringLiteral("neutral")}));
    light.insert(QStringLiteral("properties"), lightProps);
    light.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("light"), light);

    QJsonObject negative;
    negative.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject negProps;
    for (const QString &k : {QStringLiteral("no_particles_on_face"), QStringLiteral("no_text"), QStringLiteral("no_extra_limbs")}) {
        QJsonObject b;
        b.insert(QStringLiteral("type"), QStringLiteral("boolean"));
        negProps.insert(k, b);
    }
    negative.insert(QStringLiteral("properties"), negProps);
    props.insert(QStringLiteral("negative"), negative);

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), props);
    schema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("subject"), QStringLiteral("head")});
    schema.insert(QStringLiteral("additionalProperties"), false);
    return schema;
}

bool KisAiSceneSpecCodec::parseSceneSpec(
    const QByteArray &responseBytes,
    KisAiSceneSpec *outSpec,
    QString *errorMessage,
    QStringList *warnings)
{
    if (!outSpec) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Null output spec.");
        return false;
    }
    // Reuse the hardened envelope extractor (markdown fences, <think> tokens).
    const QString jsonText = KisAiStrokeProgramCodec::sanitizeAndExtractJson(
        QString::fromUtf8(responseBytes));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("SceneSpec is not valid JSON: %1").arg(parseError.errorString());
        return false;
    }
    return parseSceneSpecObject(doc.object(), outSpec, warnings);
}

bool KisAiSceneSpecCodec::parseSceneSpecObject(
    const QJsonObject &rootObj,
    KisAiSceneSpec *outSpec,
    QStringList *warnings)
{
    if (!outSpec)
        return false;
    KisAiSceneSpec spec; // defaults survive unknown/missing fields
    QStringList localWarnings;

    const QJsonObject subject = rootObj.value(QStringLiteral("subject")).toObject();
    if (!subject.isEmpty()) {
        spec.subject.type = normalizeEnum(subject.value(QStringLiteral("type")).toString(spec.subject.type),
                                          {QStringLiteral("character"), QStringLiteral("landscape"), QStringLiteral("creature"), QStringLiteral("object")},
                                          QStringLiteral("character"));
        spec.subject.poseId = normalizeEnum(subject.value(QStringLiteral("pose_id")).toString(spec.subject.poseId),
                                            {QStringLiteral("three_quarter_bust"), QStringLiteral("front_bust"), QStringLiteral("profile_bust"), QStringLiteral("upper_body"), QStringLiteral("full_body"), QStringLiteral("wide_scene")},
                                            QStringLiteral("three_quarter_bust"));
        spec.subject.facing = normalizeEnum(subject.value(QStringLiteral("facing")).toString(spec.subject.facing),
                                            {QStringLiteral("front"), QStringLiteral("front-right"), QStringLiteral("front-left"), QStringLiteral("profile")},
                                            QStringLiteral("front"));
    }

    const QJsonObject head = rootObj.value(QStringLiteral("head")).toObject();
    if (!head.isEmpty()) {
        spec.head.expression = normalizeEnum(head.value(QStringLiteral("expression")).toString(spec.head.expression),
                                             {QStringLiteral("smile_open"), QStringLiteral("smile_closed"), QStringLiteral("neutral"), QStringLiteral("half"), QStringLiteral("closed")},
                                             QStringLiteral("smile_open"));
        spec.head.gaze = normalizeEnum(head.value(QStringLiteral("gaze")).toString(spec.head.gaze),
                                       {QStringLiteral("front"), QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("up")},
                                       QStringLiteral("front"));
        spec.head.hairStyle = normalizeEnum(head.value(QStringLiteral("hair_style")).toString(spec.head.hairStyle),
                                            {QStringLiteral("long_hime"), QStringLiteral("long_wavy"), QStringLiteral("bob"), QStringLiteral("twin_tails"), QStringLiteral("short_messy"), QStringLiteral("short_straight")},
                                            QStringLiteral("long_hime"));
        spec.head.hairColor = parseColorField(head, QStringLiteral("hair_color"), spec.head.hairColor);
        spec.head.eyeColor = parseColorField(head, QStringLiteral("eye_color"), spec.head.eyeColor);
        spec.head.skinTone = parseColorField(head, QStringLiteral("skin_tone"), spec.head.skinTone);
    } else if (spec.isCharacter()) {
        localWarnings.append(QStringLiteral("head block missing; canonical anime head defaults applied."));
    }

    const QJsonObject comp = rootObj.value(QStringLiteral("composition")).toObject();
    if (!comp.isEmpty()) {
        spec.composition.framing = normalizeEnum(comp.value(QStringLiteral("framing")).toString(spec.composition.framing),
                                                 {QStringLiteral("face_closeup"), QStringLiteral("bust_up"), QStringLiteral("upper_body"), QStringLiteral("full_body"), QStringLiteral("wide")},
                                                 QStringLiteral("bust_up"));
        spec.composition.headCenter = parseUnitPoint(comp.value(QStringLiteral("head_center")), spec.composition.headCenter);
        const double hh = comp.value(QStringLiteral("head_height")).toDouble(-1.0);
        if (hh >= 0.15 && hh <= 0.80)
            spec.composition.headHeight = hh;
        else if (comp.contains(QStringLiteral("head_height")))
            localWarnings.append(QStringLiteral("head_height out of range; canonical 0.42 applied."));
        spec.composition.depth = normalizeEnum(comp.value(QStringLiteral("depth")).toString(spec.composition.depth),
                                               {QStringLiteral("flat"), QStringLiteral("shallow"), QStringLiteral("deep")},
                                               QStringLiteral("shallow"));
    }

    const QJsonObject pal = rootObj.value(QStringLiteral("palette")).toObject();
    if (!pal.isEmpty()) {
        if (pal.contains(QStringLiteral("mood")))
            spec.palette.mood = pal.value(QStringLiteral("mood")).toString(spec.palette.mood);
        spec.palette.keyColor = parseColorField(pal, QStringLiteral("key"), spec.palette.keyColor);
        const QJsonArray accents = pal.value(QStringLiteral("accents")).toArray();
        for (const QJsonValue &v : accents) {
            if (!v.isString())
                continue;
            const QColor c = KisAiStrokeProgramCodec::parseColor(v.toString(), QColor());
            if (c.isValid() && spec.palette.accents.size() < 4)
                spec.palette.accents.append(c);
        }
    }

    const QJsonObject light = rootObj.value(QStringLiteral("light")).toObject();
    if (!light.isEmpty()) {
        spec.light.direction = parsePoint(light.value(QStringLiteral("direction")), spec.light.direction);
        spec.light.warmth = normalizeEnum(light.value(QStringLiteral("warmth")).toString(spec.light.warmth),
                                          {QStringLiteral("warm_key_cool_fill"), QStringLiteral("cool_key_warm_fill"), QStringLiteral("neutral")},
                                          QStringLiteral("warm_key_cool_fill"));
        spec.light.timeOfDay = normalizeEnum(light.value(QStringLiteral("time")).toString(spec.light.timeOfDay),
                                             {QStringLiteral("day"), QStringLiteral("sunset"), QStringLiteral("night")},
                                             QStringLiteral("day"));
    }

    const QJsonObject bg = rootObj.value(QStringLiteral("background")).toObject();
    if (!bg.isEmpty()) {
        spec.background.type = normalizeEnum(bg.value(QStringLiteral("type")).toString(spec.background.type),
                                             {QStringLiteral("simple_gradient"), QStringLiteral("night_sky_town"), QStringLiteral("sky_meadow"), QStringLiteral("interior"), QStringLiteral("abstract")},
                                             QStringLiteral("simple_gradient"));
        const QJsonArray elements = bg.value(QStringLiteral("elements")).toArray();
        for (const QJsonValue &v : elements) {
            if (v.isString() && spec.background.elements.size() < 6)
                spec.background.elements.append(v.toString().trimmed().toLower());
        }
        const QJsonArray forbid = bg.value(QStringLiteral("forbid")).toArray();
        for (const QJsonValue &v : forbid) {
            if (v.isString() && spec.background.forbid.size() < 6)
                spec.background.forbid.append(v.toString().trimmed().toLower());
        }
    }

    const QJsonObject neg = rootObj.value(QStringLiteral("negative")).toObject();
    if (!neg.isEmpty()) {
        if (neg.contains(QStringLiteral("no_particles_on_face")))
            spec.negative.noParticlesOnFace = neg.value(QStringLiteral("no_particles_on_face")).toBool(true);
        if (neg.contains(QStringLiteral("no_text")))
            spec.negative.noText = neg.value(QStringLiteral("no_text")).toBool(true);
        if (neg.contains(QStringLiteral("no_extra_limbs")))
            spec.negative.noExtraLimbs = neg.value(QStringLiteral("no_extra_limbs")).toBool(true);
    }

    if (rootObj.contains(QStringLiteral("prompt")) && rootObj.value(QStringLiteral("prompt")).isString())
        spec.prompt = rootObj.value(QStringLiteral("prompt")).toString();

    *outSpec = spec;
    if (warnings)
        *warnings = localWarnings;
    return true;
}

QString KisAiSceneSpecCodec::canonicalSpecExample(const QString &domain)
{
    const QString d = domain.trimmed().toLower();
    if (d.contains(QStringLiteral("landscape")) || d.contains(QStringLiteral("scenery"))) {
        return QStringLiteral(
            "=== CANONICAL SPEC EXAMPLE (landscape; nudge values, keep keys) ===\n"
            "{\"subject\": {\"type\": \"landscape\", \"pose_id\": \"wide_scene\", \"facing\": \"front\"}, "
            "\"composition\": {\"framing\": \"wide\", \"head_center\": [0.5, 0.38], \"head_height\": 0.42}, "
            "\"palette\": {\"mood\": \"quiet_dusk\", \"key\": \"#334155\", \"accents\": [\"#f59e0b\"]}, "
            "\"light\": {\"direction\": [-0.5, -0.7], \"warmth\": \"warm_key_cool_fill\", \"time\": \"sunset\"}, "
            "\"background\": {\"type\": \"sky_meadow\", \"elements\": [\"sun\", \"ridge\"], \"forbid\": [\"tree\"]}, "
            "\"negative\": {\"no_particles_on_face\": true, \"no_text\": true, \"no_extra_limbs\": true}}");
    }
    return QStringLiteral(
        "=== CANONICAL SPEC EXAMPLE (character; nudge values, keep keys) ===\n"
        "{\"subject\": {\"type\": \"character\", \"pose_id\": \"three_quarter_bust\", \"facing\": \"front-right\"}, "
        "\"head\": {\"expression\": \"smile_open\", \"gaze\": \"front\", \"hair_style\": \"long_hime\", "
        "\"hair_color\": \"#2b3a67\", \"eye_color\": \"#3b82f6\", \"skin_tone\": \"#ffe0c0\"}, "
        "\"composition\": {\"framing\": \"bust_up\", \"head_center\": [0.5, 0.38], \"head_height\": 0.42}, "
        "\"palette\": {\"mood\": \"soft_daylight\", \"key\": \"#64748b\", \"accents\": [\"#ff9fb2\"]}, "
        "\"light\": {\"direction\": [-0.5, -0.7], \"warmth\": \"warm_key_cool_fill\", \"time\": \"day\"}, "
        "\"background\": {\"type\": \"simple_gradient\", \"elements\": [], \"forbid\": [\"tree\", \"stars_over_face\"]}, "
        "\"negative\": {\"no_particles_on_face\": true, \"no_text\": true, \"no_extra_limbs\": true}}");
}

QJsonObject KisAiSceneSpecCodec::buildSceneSpecPayload(
    const QString &model,
    const QString &prompt,
    const QSize &canvasSize,
    int artStyle)
{
    Q_UNUSED(artStyle);
    const QString systemText = QStringLiteral(
        "You are an Art Director for a deterministic painting engine. "
        "Output a SceneSpec JSON object describing WHAT to paint (subject, expression, hairstyle, colors, light, framing). "
        "CRITICAL: Output MEANING ONLY. There are no coordinate fields; the engine owns all geometry and guarantees symmetry. "
        "Use values from the schema enums. Keep every color harmonious with the palette mood.\n\n"
        "=== USER REQUEST (ABSOLUTE HIGHEST PRIORITY) ===\n\"%1\"\n"
        "If any example below conflicts with the USER REQUEST, follow the USER REQUEST.\n\n%2")
        .arg(prompt.trimmed(), canonicalSpecExample(prompt));

    const QJsonObject userObj{
        {QStringLiteral("directive"), QStringLiteral("Return the SceneSpec JSON object for this request.")},
        {QStringLiteral("canvas"), QJsonObject{
            {QStringLiteral("width"), canvasSize.width()},
            {QStringLiteral("height"), canvasSize.height()},
        }},
    };

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), model);
    payload.insert(QStringLiteral("temperature"), 0.5);
    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact))}});
    payload.insert(QStringLiteral("messages"), messages);
    QJsonObject format;
    format.insert(QStringLiteral("type"), QStringLiteral("json_object"));
    payload.insert(QStringLiteral("response_format"), format);
    return payload;
}

KisAiSceneSpec KisAiSceneSpecCodec::defaultSpecForPrompt(
    const QString &prompt,
    const QSize &canvasSize)
{
    KisAiSceneSpec spec;
    spec.prompt = prompt;
    spec.canvasSize = canvasSize.isValid() ? canvasSize : QSize(1024, 1024);
    const QString lower = prompt.toLower();

    if (lower.contains(QStringLiteral("night")) || lower.contains(QStringLiteral("starry")) || lower.contains(QStringLiteral("moon"))) {
        spec.light.timeOfDay = QStringLiteral("night");
        spec.background.type = QStringLiteral("night_sky_town");
        spec.palette.mood = QStringLiteral("night_festival");
        spec.palette.keyColor = QColor(30, 41, 59);
    } else if (lower.contains(QStringLiteral("sunset")) || lower.contains(QStringLiteral("dusk")) || lower.contains(QStringLiteral("evening"))) {
        spec.light.timeOfDay = QStringLiteral("sunset");
        spec.palette.mood = QStringLiteral("quiet_dusk");
        spec.palette.keyColor = QColor(120, 70, 60);
    }
    if (lower.contains(QStringLiteral("landscape")) || lower.contains(QStringLiteral("mountain")) || lower.contains(QStringLiteral("scenery")) || lower.contains(QStringLiteral("sea"))) {
        spec.subject.type = QStringLiteral("landscape");
        spec.subject.poseId = QStringLiteral("wide_scene");
        spec.composition.framing = QStringLiteral("wide");
        spec.background.type = spec.light.timeOfDay == QLatin1String("night")
            ? QStringLiteral("night_sky_town") : QStringLiteral("sky_meadow");
    }
    if ((lower.contains(QStringLiteral("silver")) || lower.contains(QStringLiteral("white"))) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairColor = QColor(226, 232, 240);
    } else if ((lower.contains(QStringLiteral("blonde")) || lower.contains(QStringLiteral("gold"))) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairColor = QColor(250, 204, 21);
    } else if (lower.contains(QStringLiteral("red")) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairColor = QColor(185, 60, 50);
    } else if (lower.contains(QStringLiteral("black")) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairColor = QColor(24, 24, 32);
    }
    if (lower.contains(QStringLiteral("twin")) || lower.contains(QStringLiteral("twintail"))) {
        spec.head.hairStyle = QStringLiteral("twin_tails");
    } else if (lower.contains(QStringLiteral("bob"))) {
        spec.head.hairStyle = QStringLiteral("bob");
    } else if (lower.contains(QStringLiteral("short")) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairStyle = QStringLiteral("short_messy");
    }
    if (lower.contains(QStringLiteral("green")) && lower.contains(QStringLiteral("eye"))) {
        spec.head.eyeColor = QColor(34, 197, 94);
    } else if (lower.contains(QStringLiteral("red")) && lower.contains(QStringLiteral("eye"))) {
        spec.head.eyeColor = QColor(239, 68, 68);
    }
    if (lower.contains(QStringLiteral("close")) && (lower.contains(QStringLiteral("face")) || lower.contains(QStringLiteral("portrait")))) {
        spec.composition.framing = QStringLiteral("face_closeup");
        spec.composition.headHeight = 0.60;
    } else if (lower.contains(QStringLiteral("full")) && lower.contains(QStringLiteral("body"))) {
        spec.composition.framing = QStringLiteral("full_body");
        spec.composition.headHeight = 0.22;
        spec.composition.headCenter = QPointF(0.5, 0.24);
    }
    return spec;
}
