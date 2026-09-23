/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiSceneSpec.h"
#include "KisAiStrokeProgram.h"
#include "KisAiAbstractOntology.h"

#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace
{
QString normalizeEnum(const QString &value, const QStringList &allowed, const QString &fallback)
{
    QString lowerVal = value.trimmed().toLower();
    if (allowed.contains(lowerVal))
        return lowerVal;
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
    if (!okX || !okY || !std::isfinite(x) || !std::isfinite(y))
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
        o.insert(QStringLiteral("description"), QStringLiteral("CSS hex color like #2b3a67"));
        return o;
    };

    QJsonObject props;
    QJsonObject subject;
    subject.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject subjectProps;
    subjectProps.insert(QStringLiteral("type"),
                        strEnum({QStringLiteral("character"),
                                 QStringLiteral("landscape"),
                                 QStringLiteral("creature"),
                                 QStringLiteral("object")}));
    subjectProps.insert(QStringLiteral("pose_id"),
                        strEnum({QStringLiteral("three_quarter_bust"),
                                 QStringLiteral("front_bust"),
                                 QStringLiteral("profile_bust"),
                                 QStringLiteral("upper_body"),
                                 QStringLiteral("full_body"),
                                 QStringLiteral("wide_scene")}));
    subjectProps.insert(QStringLiteral("facing"),
                        strEnum({QStringLiteral("front"),
                                 QStringLiteral("front-right"),
                                 QStringLiteral("front-left"),
                                 QStringLiteral("profile")}));
    subject.insert(QStringLiteral("properties"), subjectProps);
    subject.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("subject"), subject);

    QJsonObject head;
    head.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject headProps;
    headProps.insert(QStringLiteral("expression"),
                     strEnum({QStringLiteral("smile_open"),
                              QStringLiteral("smile_closed"),
                              QStringLiteral("neutral"),
                              QStringLiteral("half"),
                              QStringLiteral("closed"),
                              QStringLiteral("wink_left"),
                              QStringLiteral("wink_right"),
                              QStringLiteral("blush_shy"),
                              QStringLiteral("confident_smug")}));
    headProps.insert(
        QStringLiteral("gaze"),
        strEnum({QStringLiteral("front"), QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("up")}));
    headProps.insert(QStringLiteral("hair_style"),
                     strEnum({QStringLiteral("long_hime"),
                              QStringLiteral("long_wavy"),
                              QStringLiteral("bob"),
                              QStringLiteral("twin_tails"),
                              QStringLiteral("short_messy"),
                              QStringLiteral("short_straight"),
                              QStringLiteral("pony_tail"),
                              QStringLiteral("half_up"),
                              QStringLiteral("wolf_cut"),
                              QStringLiteral("braided")}));
    headProps.insert(QStringLiteral("hair_bangs"),
                     strEnum({QStringLiteral("m_fringe"),
                              QStringLiteral("straight_cut"),
                              QStringLiteral("swept_left"),
                              QStringLiteral("swept_right"),
                              QStringLiteral("see_through"),
                              QStringLiteral("blunt_bangs"),
                              QStringLiteral("center_part")}));
    headProps.insert(QStringLiteral("hair_color"), color());
    headProps.insert(QStringLiteral("eye_color"), color());
    headProps.insert(QStringLiteral("skin_tone"), color());
    {
        QJsonObject num01;
        num01.insert(QStringLiteral("type"), QStringLiteral("number"));
        num01.insert(QStringLiteral("minimum"), 0.0);
        num01.insert(QStringLiteral("maximum"), 1.0);
        headProps.insert(QStringLiteral("hair_volume"), num01);
        headProps.insert(QStringLiteral("hair_flyaway"), num01);
        headProps.insert(QStringLiteral("blush_intensity"), num01);
    }
    headProps.insert(QStringLiteral("eye_highlight_style"),
                     strEnum({QStringLiteral("twin_dot"),
                              QStringLiteral("radiant_sparkle"),
                              QStringLiteral("soft_diffuse"),
                              QStringLiteral("crescent")}));
    head.insert(QStringLiteral("properties"), headProps);
    head.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("head"), head);

    QJsonObject clothing;
    clothing.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject clothProps;
    clothProps.insert(QStringLiteral("style"),
                      strEnum({QStringLiteral("school_uniform"),
                               QStringLiteral("sailor"),
                               QStringLiteral("hoodie"),
                               QStringLiteral("casual"),
                               QStringLiteral("dress"),
                               QStringLiteral("kimono")}));
    clothProps.insert(QStringLiteral("color"), color());
    clothProps.insert(QStringLiteral("secondary_color"), color());
    clothProps.insert(QStringLiteral("accent_color"), color());
    clothing.insert(QStringLiteral("properties"), clothProps);
    clothing.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("clothing"), clothing);

    QJsonObject composition;
    composition.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject compProps;
    compProps.insert(QStringLiteral("framing"),
                     strEnum({QStringLiteral("face_closeup"),
                              QStringLiteral("bust_up"),
                              QStringLiteral("upper_body"),
                              QStringLiteral("full_body"),
                              QStringLiteral("wide")}));
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
    lightProps.insert(QStringLiteral("time"),
                      strEnum({QStringLiteral("day"), QStringLiteral("sunset"), QStringLiteral("night")}));
    lightProps.insert(
        QStringLiteral("warmth"),
        strEnum(
            {QStringLiteral("warm_key_cool_fill"), QStringLiteral("cool_key_warm_fill"), QStringLiteral("neutral")}));
    {
        QJsonObject num01;
        num01.insert(QStringLiteral("type"), QStringLiteral("number"));
        num01.insert(QStringLiteral("minimum"), 0.0);
        num01.insert(QStringLiteral("maximum"), 1.0);
        lightProps.insert(QStringLiteral("rim_intensity"), num01);
        lightProps.insert(QStringLiteral("sss_strength"), num01);
    }
    lightProps.insert(QStringLiteral("lighting_style"),
                      strEnum({QStringLiteral("soft_studio"),
                               QStringLiteral("dramatic_backlight"),
                               QStringLiteral("komorebi_dappled"),
                               QStringLiteral("sunset_golden"),
                               QStringLiteral("neon_rim")}));
    light.insert(QStringLiteral("properties"), lightProps);
    light.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("light"), light);

    // V10: finish block
    QJsonObject finish;
    finish.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject finishProps;
    {
        QJsonObject num01;
        num01.insert(QStringLiteral("type"), QStringLiteral("number"));
        num01.insert(QStringLiteral("minimum"), 0.0);
        num01.insert(QStringLiteral("maximum"), 1.0);
        finishProps.insert(QStringLiteral("bloom_strength"), num01);
        finishProps.insert(QStringLiteral("grain_intensity"), num01);
        finishProps.insert(QStringLiteral("vignette_strength"), num01);
    }
    finishProps.insert(QStringLiteral("tone_mood"),
                       strEnum({QStringLiteral("anime_vibrant"),
                                QStringLiteral("cinematic_warm"),
                                QStringLiteral("pastel_dreamy"),
                                QStringLiteral("dark_noir")}));
    finish.insert(QStringLiteral("properties"), finishProps);
    finish.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("finish"), finish);

    QJsonObject negative;
    negative.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject negProps;
    for (const QString &k :
         {QStringLiteral("no_particles_on_face"), QStringLiteral("no_text"), QStringLiteral("no_extra_limbs")}) {
        QJsonObject b;
        b.insert(QStringLiteral("type"), QStringLiteral("boolean"));
        negProps.insert(k, b);
    }
    negative.insert(QStringLiteral("properties"), negProps);
    props.insert(QStringLiteral("negative"), negative);

    // ---- V5 R1: style / camera / color_script / narrative ----
    QJsonObject style;
    style.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject styleProps;
    styleProps.insert(QStringLiteral("art_style"),
                      strEnum({QStringLiteral("anime_cel"),
                               QStringLiteral("watercolor"),
                               QStringLiteral("impasto"),
                               QStringLiteral("ink_sketch"),
                               QStringLiteral("cyber_neon"),
                               QStringLiteral("fine_line")}));
    QJsonArray tags;
    tags.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
    QJsonObject tagsArr;
    tagsArr.insert(QStringLiteral("type"), QStringLiteral("array"));
    tagsArr.insert(QStringLiteral("items"), tags.at(0));
    tagsArr.insert(QStringLiteral("maxItems"), 8);
    styleProps.insert(QStringLiteral("custom_tags"), tagsArr);
    styleProps.insert(QStringLiteral("line_weight"),
                      strEnum({QStringLiteral("delicate"), QStringLiteral("standard"), QStringLiteral("bold")}));
    QJsonObject detail;
    detail.insert(QStringLiteral("type"), QStringLiteral("number"));
    detail.insert(QStringLiteral("minimum"), 0.0);
    detail.insert(QStringLiteral("maximum"), 1.0);
    styleProps.insert(QStringLiteral("detail_level"), detail);
    style.insert(QStringLiteral("properties"), styleProps);
    style.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("style"), style);

    QJsonObject camera;
    camera.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject camProps;
    camProps.insert(QStringLiteral("focal"),
                    strEnum({QStringLiteral("short"), QStringLiteral("normal"), QStringLiteral("long")}));
    camProps.insert(QStringLiteral("tilt"),
                    strEnum({QStringLiteral("level"), QStringLiteral("high_angle"), QStringLiteral("low_angle")}));
    camera.insert(QStringLiteral("properties"), camProps);
    camera.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("camera"), camera);

    QJsonObject colorScript;
    colorScript.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject csProps;
    csProps.insert(QStringLiteral("shadow"), color());
    csProps.insert(QStringLiteral("midtone"), color());
    csProps.insert(QStringLiteral("highlight"), color());
    QJsonObject accentW;
    accentW.insert(QStringLiteral("type"), QStringLiteral("number"));
    accentW.insert(QStringLiteral("minimum"), 0.0);
    accentW.insert(QStringLiteral("maximum"), 1.0);
    csProps.insert(QStringLiteral("accent_weight"), accentW);
    colorScript.insert(QStringLiteral("properties"), csProps);
    colorScript.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("color_script"), colorScript);

    QJsonObject narrative;
    narrative.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject narProps;
    narProps.insert(QStringLiteral("time"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
    narProps.insert(
        QStringLiteral("weather"),
        strEnum({QStringLiteral("clear"), QStringLiteral("cloudy"), QStringLiteral("rain"), QStringLiteral("snow")}));
    QJsonObject propsArr;
    propsArr.insert(QStringLiteral("type"), QStringLiteral("array"));
    propsArr.insert(QStringLiteral("items"), tags.at(0));
    propsArr.insert(QStringLiteral("maxItems"), 6);
    narProps.insert(QStringLiteral("props"), propsArr);
    narrative.insert(QStringLiteral("properties"), narProps);
    narrative.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("narrative"), narrative);

    // ---- V5 R2: rig parameter tuning ----
    QJsonObject rigBlock;
    rigBlock.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject rigProps;
    QJsonObject aperture;
    aperture.insert(QStringLiteral("type"), QStringLiteral("number"));
    aperture.insert(QStringLiteral("minimum"), 0.0);
    aperture.insert(QStringLiteral("maximum"), 1.0);
    rigProps.insert(QStringLiteral("eye_aperture"), aperture);
    QJsonObject iris;
    iris.insert(QStringLiteral("type"), QStringLiteral("number"));
    iris.insert(QStringLiteral("minimum"), 0.35);
    iris.insert(QStringLiteral("maximum"), 0.85);
    rigProps.insert(QStringLiteral("iris_ratio"), iris);
    rigProps.insert(QStringLiteral("eye_highlight"),
                    strEnum({QStringLiteral("twin_dot"), QStringLiteral("streak"), QStringLiteral("soft")}));
    rigProps.insert(QStringLiteral("double_lid"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}});
    QJsonObject density;
    density.insert(QStringLiteral("type"), QStringLiteral("number"));
    density.insert(QStringLiteral("minimum"), 0.0);
    density.insert(QStringLiteral("maximum"), 1.0);
    rigProps.insert(QStringLiteral("hair_strand_density"), density);
    QJsonObject flyaway;
    flyaway.insert(QStringLiteral("type"), QStringLiteral("number"));
    flyaway.insert(QStringLiteral("minimum"), 0.0);
    flyaway.insert(QStringLiteral("maximum"), 1.0);
    rigProps.insert(QStringLiteral("hair_flyaway"), flyaway);
    QJsonObject bands;
    bands.insert(QStringLiteral("type"), QStringLiteral("number"));
    bands.insert(QStringLiteral("minimum"), 0.0);
    bands.insert(QStringLiteral("maximum"), 3.0);
    rigProps.insert(QStringLiteral("hair_highlight_bands"), bands);
    QJsonObject mouth;
    mouth.insert(QStringLiteral("type"), QStringLiteral("number"));
    mouth.insert(QStringLiteral("minimum"), 0.6);
    mouth.insert(QStringLiteral("maximum"), 1.4);
    rigProps.insert(QStringLiteral("mouth_width_scale"), mouth);
    rigProps.insert(QStringLiteral("has_brows"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}});
    rigBlock.insert(QStringLiteral("properties"), rigProps);
    rigBlock.insert(QStringLiteral("additionalProperties"), false);
    props.insert(QStringLiteral("rig"), rigBlock);

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), props);
    schema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("subject"), QStringLiteral("head")});
    schema.insert(QStringLiteral("additionalProperties"), false);
    return schema;
}

bool KisAiSceneSpecCodec::parseSceneSpec(const QByteArray &responseBytes,
                                         KisAiSceneSpec *outSpec,
                                         QString *errorMessage,
                                         QStringList *warnings)
{
    if (!outSpec) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Null output spec.");
        return false;
    }
    // Mirror the MAX_RESPONSE_BYTES guard used by parseResponse and the
    // MAX_COMPOSITION_PLAN_BYTES guard used by parseCompositionPlan: this entry
    // point also sanitizes attacker-controlled model output, and the sanitizer's
    // regex work grows with the input.
    constexpr int MAX_SCENE_SPEC_BYTES = 32 * 1024 * 1024;
    if (responseBytes.size() > MAX_SCENE_SPEC_BYTES) {
        if (errorMessage)
            *errorMessage = QStringLiteral("SceneSpec response exceeded the size limit.");
        return false;
    }
    // Reuse the hardened envelope extractor (markdown fences, <think> tokens).
    const QString jsonText = KisAiStrokeProgramCodec::sanitizeAndExtractJson(QString::fromUtf8(responseBytes));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("SceneSpec is not valid JSON: %1").arg(parseError.errorString());
        return false;
    }
    return parseSceneSpecObject(doc.object(), outSpec, warnings);
}

bool KisAiSceneSpecCodec::parseSceneSpecObject(const QJsonObject &rootObj,
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
                                          {QStringLiteral("character"),
                                           QStringLiteral("landscape"),
                                           QStringLiteral("creature"),
                                           QStringLiteral("object")},
                                          QStringLiteral("character"));
        spec.subject.poseId = normalizeEnum(subject.value(QStringLiteral("pose_id")).toString(spec.subject.poseId),
                                            {QStringLiteral("three_quarter_bust"),
                                             QStringLiteral("front_bust"),
                                             QStringLiteral("profile_bust"),
                                             QStringLiteral("upper_body"),
                                             QStringLiteral("full_body"),
                                             QStringLiteral("wide_scene")},
                                            QStringLiteral("three_quarter_bust"));
        spec.subject.facing = normalizeEnum(subject.value(QStringLiteral("facing")).toString(spec.subject.facing),
                                            {QStringLiteral("front"),
                                             QStringLiteral("front-right"),
                                             QStringLiteral("front-left"),
                                             QStringLiteral("profile")},
                                            QStringLiteral("front"));
    }

    const QJsonObject head = rootObj.value(QStringLiteral("head")).toObject();
    if (!head.isEmpty()) {
        spec.head.expression =
            normalizeEnum(head.value(QStringLiteral("expression")).toString(spec.head.expression),
                          {QStringLiteral("smile_open"),
                           QStringLiteral("smile_closed"),
                           QStringLiteral("neutral"),
                           QStringLiteral("half"),
                           QStringLiteral("closed"),
                           QStringLiteral("wink_left"),
                           QStringLiteral("wink_right"),
                           QStringLiteral("blush_shy"),
                           QStringLiteral("confident_smug")},
                          QStringLiteral("smile_open"));
        spec.head.gaze = normalizeEnum(
            head.value(QStringLiteral("gaze")).toString(spec.head.gaze),
            {QStringLiteral("front"), QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("up")},
            QStringLiteral("front"));
        spec.head.hairStyle = normalizeEnum(head.value(QStringLiteral("hair_style")).toString(spec.head.hairStyle),
                                            {QStringLiteral("long_hime"),
                                             QStringLiteral("long_wavy"),
                                             QStringLiteral("bob"),
                                             QStringLiteral("twin_tails"),
                                             QStringLiteral("short_messy"),
                                             QStringLiteral("short_straight"),
                                             QStringLiteral("pony_tail"),
                                             QStringLiteral("half_up"),
                                             QStringLiteral("wolf_cut"),
                                             QStringLiteral("braided")},
                                            QStringLiteral("long_hime"));
        spec.head.hairBangs = normalizeEnum(head.value(QStringLiteral("hair_bangs")).toString(spec.head.hairBangs),
                                            {QStringLiteral("m_fringe"),
                                             QStringLiteral("straight_cut"),
                                             QStringLiteral("swept_left"),
                                             QStringLiteral("swept_right"),
                                             QStringLiteral("see_through"),
                                             QStringLiteral("blunt_bangs"),
                                             QStringLiteral("center_part")},
                                            QStringLiteral("m_fringe"));
        spec.head.hairColor = parseColorField(head, QStringLiteral("hair_color"), spec.head.hairColor);
        spec.head.eyeColor = parseColorField(head, QStringLiteral("eye_color"), spec.head.eyeColor);
        spec.head.skinTone = parseColorField(head, QStringLiteral("skin_tone"), spec.head.skinTone);
        if (head.contains(QStringLiteral("hair_volume"))) {
            spec.head.hairVolume = qBound<qreal>(0.0, head.value(QStringLiteral("hair_volume")).toDouble(spec.head.hairVolume), 1.0);
        }
        if (head.contains(QStringLiteral("hair_flyaway"))) {
            spec.head.hairFlyaway = qBound<qreal>(0.0, head.value(QStringLiteral("hair_flyaway")).toDouble(spec.head.hairFlyaway), 1.0);
        }
        if (head.contains(QStringLiteral("blush_intensity"))) {
            spec.head.blushIntensity = qBound<qreal>(0.0, head.value(QStringLiteral("blush_intensity")).toDouble(spec.head.blushIntensity), 1.0);
        }
        spec.head.eyeHighlightStyle = normalizeEnum(head.value(QStringLiteral("eye_highlight_style")).toString(spec.head.eyeHighlightStyle),
                                                   {QStringLiteral("twin_dot"),
                                                    QStringLiteral("radiant_sparkle"),
                                                    QStringLiteral("soft_diffuse"),
                                                    QStringLiteral("crescent")},
                                                   QStringLiteral("twin_dot"));
    } else if (spec.isCharacter()) {
        localWarnings.append(QStringLiteral("head block missing; canonical anime head defaults applied."));
    }

    const QJsonObject cloth = rootObj.value(QStringLiteral("clothing")).toObject();
    if (!cloth.isEmpty()) {
        spec.clothing.style = normalizeEnum(cloth.value(QStringLiteral("style")).toString(spec.clothing.style),
                                            {QStringLiteral("school_uniform"),
                                             QStringLiteral("sailor"),
                                             QStringLiteral("hoodie"),
                                             QStringLiteral("casual"),
                                             QStringLiteral("dress"),
                                             QStringLiteral("kimono")},
                                            QStringLiteral("school_uniform"));
        spec.clothing.color = parseColorField(cloth, QStringLiteral("color"), spec.clothing.color);
        spec.clothing.secondaryColor =
            parseColorField(cloth, QStringLiteral("secondary_color"), spec.clothing.secondaryColor);
        spec.clothing.accentColor = parseColorField(cloth, QStringLiteral("accent_color"), spec.clothing.accentColor);
    }

    const QJsonObject comp = rootObj.value(QStringLiteral("composition")).toObject();
    if (!comp.isEmpty()) {
        spec.composition.framing =
            normalizeEnum(comp.value(QStringLiteral("framing")).toString(spec.composition.framing),
                          {QStringLiteral("face_closeup"),
                           QStringLiteral("bust_up"),
                           QStringLiteral("upper_body"),
                           QStringLiteral("full_body"),
                           QStringLiteral("wide")},
                          QStringLiteral("bust_up"));
        spec.composition.headCenter =
            parseUnitPoint(comp.value(QStringLiteral("head_center")), spec.composition.headCenter);
        const double hh = comp.value(QStringLiteral("head_height")).toDouble(-1.0);
        if (hh >= 0.15 && hh <= 0.80)
            spec.composition.headHeight = hh;
        else if (comp.contains(QStringLiteral("head_height")))
            localWarnings.append(QStringLiteral("head_height out of range; canonical 0.42 applied."));
        spec.composition.depth =
            normalizeEnum(comp.value(QStringLiteral("depth")).toString(spec.composition.depth),
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
        spec.light.warmth = normalizeEnum(
            light.value(QStringLiteral("warmth")).toString(spec.light.warmth),
            {QStringLiteral("warm_key_cool_fill"), QStringLiteral("cool_key_warm_fill"), QStringLiteral("neutral")},
            QStringLiteral("warm_key_cool_fill"));
        spec.light.timeOfDay = normalizeEnum(light.value(QStringLiteral("time")).toString(spec.light.timeOfDay),
                                             {QStringLiteral("day"), QStringLiteral("sunset"), QStringLiteral("night")},
                                             QStringLiteral("day"));
        if (light.contains(QStringLiteral("rim_intensity"))) {
            spec.light.rimIntensity = qBound<qreal>(0.0, light.value(QStringLiteral("rim_intensity")).toDouble(spec.light.rimIntensity), 1.0);
        }
        if (light.contains(QStringLiteral("sss_strength"))) {
            spec.light.sssStrength = qBound<qreal>(0.0, light.value(QStringLiteral("sss_strength")).toDouble(spec.light.sssStrength), 1.0);
        }
        spec.light.lightingStyle = normalizeEnum(
            light.value(QStringLiteral("lighting_style")).toString(spec.light.lightingStyle),
            {QStringLiteral("soft_studio"),
             QStringLiteral("dramatic_backlight"),
             QStringLiteral("komorebi_dappled"),
             QStringLiteral("sunset_golden"),
             QStringLiteral("neon_rim")},
            QStringLiteral("soft_studio"));
    }

    const QJsonObject finishObj = rootObj.value(QStringLiteral("finish")).toObject();
    if (!finishObj.isEmpty()) {
        if (finishObj.contains(QStringLiteral("bloom_strength"))) {
            spec.finish.bloomStrength = qBound<qreal>(0.0, finishObj.value(QStringLiteral("bloom_strength")).toDouble(spec.finish.bloomStrength), 1.0);
        }
        if (finishObj.contains(QStringLiteral("grain_intensity"))) {
            spec.finish.grainIntensity = qBound<qreal>(0.0, finishObj.value(QStringLiteral("grain_intensity")).toDouble(spec.finish.grainIntensity), 1.0);
        }
        if (finishObj.contains(QStringLiteral("vignette_strength"))) {
            spec.finish.vignetteStrength = qBound<qreal>(0.0, finishObj.value(QStringLiteral("vignette_strength")).toDouble(spec.finish.vignetteStrength), 1.0);
        }
        spec.finish.toneMood = normalizeEnum(
            finishObj.value(QStringLiteral("tone_mood")).toString(spec.finish.toneMood),
            {QStringLiteral("anime_vibrant"),
             QStringLiteral("cinematic_warm"),
             QStringLiteral("pastel_dreamy"),
             QStringLiteral("dark_noir")},
            QStringLiteral("anime_vibrant"));
    }

    const QJsonObject bg = rootObj.value(QStringLiteral("background")).toObject();
    if (!bg.isEmpty()) {
        spec.background.type = normalizeEnum(bg.value(QStringLiteral("type")).toString(spec.background.type),
                                             {QStringLiteral("simple_gradient"),
                                              QStringLiteral("night_sky_town"),
                                              QStringLiteral("sky_meadow"),
                                              QStringLiteral("interior"),
                                              QStringLiteral("abstract")},
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

    // ---- V5 R1/R2: v2 blocks (optional; defaults preserve v3 behaviour) ----
    const QJsonObject styleObj = rootObj.value(QStringLiteral("style")).toObject();
    if (!styleObj.isEmpty()) {
        spec.style.artStyleId =
            normalizeEnum(styleObj.value(QStringLiteral("art_style")).toString(spec.style.artStyleId),
                          {QStringLiteral("anime_cel"),
                           QStringLiteral("watercolor"),
                           QStringLiteral("impasto"),
                           QStringLiteral("ink_sketch"),
                           QStringLiteral("cyber_neon"),
                           QStringLiteral("fine_line")},
                          QStringLiteral("anime_cel"));
        const QJsonArray tags = styleObj.value(QStringLiteral("custom_tags")).toArray();
        for (const QJsonValue &v : tags) {
            if (v.isString() && spec.style.customTags.size() < 8) {
                const QString t = v.toString().trimmed().toLower();
                if (!t.isEmpty())
                    spec.style.customTags.append(t);
            }
        }
        spec.style.lineWeight =
            normalizeEnum(styleObj.value(QStringLiteral("line_weight")).toString(spec.style.lineWeight),
                          {QStringLiteral("delicate"), QStringLiteral("standard"), QStringLiteral("bold")},
                          QStringLiteral("standard"));
        if (styleObj.contains(QStringLiteral("detail_level"))) {
            const double dl = styleObj.value(QStringLiteral("detail_level")).toDouble(spec.style.detailLevel);
            if (std::isfinite(dl)) {
                spec.style.detailLevel = qBound<qreal>(0.0, dl, 1.0);
            }
        }
    }

    const QJsonObject camObj = rootObj.value(QStringLiteral("camera")).toObject();
    if (!camObj.isEmpty()) {
        spec.camera.focal = normalizeEnum(camObj.value(QStringLiteral("focal")).toString(spec.camera.focal),
                                          {QStringLiteral("short"), QStringLiteral("normal"), QStringLiteral("long")},
                                          QStringLiteral("normal"));
        spec.camera.tilt =
            normalizeEnum(camObj.value(QStringLiteral("tilt")).toString(spec.camera.tilt),
                          {QStringLiteral("level"), QStringLiteral("high_angle"), QStringLiteral("low_angle")},
                          QStringLiteral("level"));
    }

    const QJsonObject csObj = rootObj.value(QStringLiteral("color_script")).toObject();
    if (!csObj.isEmpty()) {
        spec.colorScript.shadow = parseColorField(csObj, QStringLiteral("shadow"), spec.colorScript.shadow);
        spec.colorScript.midtone = parseColorField(csObj, QStringLiteral("midtone"), spec.colorScript.midtone);
        spec.colorScript.highlight = parseColorField(csObj, QStringLiteral("highlight"), spec.colorScript.highlight);
        if (csObj.contains(QStringLiteral("accent_weight"))) {
            const double aw = csObj.value(QStringLiteral("accent_weight")).toDouble(spec.colorScript.accentWeight);
            if (std::isfinite(aw)) {
                spec.colorScript.accentWeight = qBound<qreal>(0.0, aw, 1.0);
            }
        }
    }

    const QJsonObject narObj = rootObj.value(QStringLiteral("narrative")).toObject();
    if (!narObj.isEmpty()) {
        if (narObj.contains(QStringLiteral("time"))) {
            QString rawTime = narObj.value(QStringLiteral("time")).toString().trimmed().toLower();
            constexpr int kMaxNarrativeTimeChars = 64;
            if (rawTime.size() > kMaxNarrativeTimeChars)
                rawTime.truncate(kMaxNarrativeTimeChars);
            spec.narrative.time = rawTime;
        }
        spec.narrative.weather = normalizeEnum(
            narObj.value(QStringLiteral("weather")).toString(spec.narrative.weather),
            {QStringLiteral("clear"), QStringLiteral("cloudy"), QStringLiteral("rain"), QStringLiteral("snow")},
            QStringLiteral("clear"));
        const QJsonArray narProps = narObj.value(QStringLiteral("props")).toArray();
        for (const QJsonValue &v : narProps) {
            if (v.isString() && spec.narrative.props.size() < 6) {
                const QString p = v.toString().trimmed().toLower();
                if (!p.isEmpty())
                    spec.narrative.props.append(p);
            }
        }
    }

    const QJsonObject rigObj = rootObj.value(QStringLiteral("rig")).toObject();
    if (!rigObj.isEmpty()) {
        if (rigObj.contains(QStringLiteral("eye_aperture"))) {
            const double ea = rigObj.value(QStringLiteral("eye_aperture")).toDouble(spec.rig.eyeAperture);
            if (std::isfinite(ea)) {
                spec.rig.eyeAperture = qBound<qreal>(0.0, ea, 1.0);
            }
        }
        if (rigObj.contains(QStringLiteral("iris_ratio"))) {
            const double ir = rigObj.value(QStringLiteral("iris_ratio")).toDouble(spec.rig.irisRatio);
            if (std::isfinite(ir)) {
                spec.rig.irisRatio = qBound<qreal>(0.35, ir, 0.85);
            }
        }
        spec.rig.eyeHighlight =
            normalizeEnum(rigObj.value(QStringLiteral("eye_highlight")).toString(spec.rig.eyeHighlight),
                          {QStringLiteral("twin_dot"), QStringLiteral("streak"), QStringLiteral("soft")},
                          spec.rig.eyeHighlight);
        if (rigObj.contains(QStringLiteral("double_lid")))
            spec.rig.doubleLid = rigObj.value(QStringLiteral("double_lid")).toBool(spec.rig.doubleLid);
        if (rigObj.contains(QStringLiteral("hair_strand_density"))) {
            const double hsd =
                rigObj.value(QStringLiteral("hair_strand_density")).toDouble(spec.rig.hairStrandDensity);
            if (std::isfinite(hsd)) {
                spec.rig.hairStrandDensity = qBound<qreal>(0.0, hsd, 1.0);
            }
        }
        if (rigObj.contains(QStringLiteral("hair_flyaway"))) {
            const double hf = rigObj.value(QStringLiteral("hair_flyaway")).toDouble(spec.rig.hairFlyaway);
            if (std::isfinite(hf)) {
                spec.rig.hairFlyaway = qBound<qreal>(0.0, hf, 1.0);
            }
        }
        if (rigObj.contains(QStringLiteral("hair_highlight_bands"))) {
            const double hhb =
                rigObj.value(QStringLiteral("hair_highlight_bands")).toDouble(spec.rig.hairHighlightBands);
            if (std::isfinite(hhb)) {
                spec.rig.hairHighlightBands = qBound(0, static_cast<int>(std::round(hhb)), 3);
            }
        }
        if (rigObj.contains(QStringLiteral("mouth_width_scale"))) {
            const double mws =
                rigObj.value(QStringLiteral("mouth_width_scale")).toDouble(spec.rig.mouthWidthScale);
            if (std::isfinite(mws)) {
                spec.rig.mouthWidthScale = qBound<qreal>(0.6, mws, 1.4);
            }
        }
        if (rigObj.contains(QStringLiteral("has_brows")))
            spec.rig.hasBrows = rigObj.value(QStringLiteral("has_brows")).toBool(spec.rig.hasBrows);
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
            "\"style\": {\"art_style\": \"anime_cel\", \"line_weight\": \"standard\", \"detail_level\": 0.6}, "
            "\"camera\": {\"focal\": \"normal\", \"tilt\": \"level\"}, "
            "\"narrative\": {\"time\": \"sunset\", \"weather\": \"clear\", \"props\": []}, "
            "\"rig\": {\"eye_aperture\": 0.85, \"hair_highlight_bands\": 1}, "
            "\"negative\": {\"no_particles_on_face\": true, \"no_text\": true, \"no_extra_limbs\": true}}");
    }

    if (d.contains(QStringLiteral("action")) || d.contains(QStringLiteral("dynamic"))
        || d.contains(QStringLiteral("jump")) || d.contains(QStringLiteral("battle"))) {
        return QStringLiteral(
            "=== CANONICAL SPEC EXAMPLE (dynamic action character; nudge values, keep keys) ===\n"
            "{\"subject\": {\"type\": \"character\", \"pose_id\": \"dynamic_lean\", \"facing\": \"front-right\"}, "
            "\"head\": {\"expression\": \"confident_smug\", \"gaze\": \"front\", \"hair_style\": \"long_wavy\", "
            "\"hair_color\": \"#1e293b\", \"eye_color\": \"#06b6d4\", \"skin_tone\": \"#ffe0c0\"}, "
            "\"composition\": {\"framing\": \"upper_body\", \"head_center\": [0.48, 0.35], \"head_height\": 0.38}, "
            "\"palette\": {\"mood\": \"cinematic_warm\", \"key\": \"#0f172a\", \"accents\": [\"#38bdf8\", \"#f43f5e\"]}, "
            "\"light\": {\"direction\": [-0.7, -0.5], \"warmth\": \"warm_key_cool_fill\", \"time\": \"day\"}, "
            "\"background\": {\"type\": \"abstract\", \"elements\": [\"speed_lines\"], \"forbid\": [\"tree\"]}, "
            "\"style\": {\"art_style\": \"anime_cel\", \"line_weight\": \"bold\", \"detail_level\": 0.75}, "
            "\"camera\": {\"focal\": \"short\", \"tilt\": \"low_angle\"}, "
            "\"narrative\": {\"time\": \"day\", \"weather\": \"clear\", \"props\": []}, "
            "\"rig\": {\"eye_aperture\": 0.90, \"iris_ratio\": 0.60, \"eye_highlight\": \"radiant_sparkle\", "
            "\"double_lid\": true, \"hair_highlight_bands\": 2, \"mouth_width_scale\": 1.1, \"has_brows\": true}, "
            "\"negative\": {\"no_particles_on_face\": true, \"no_text\": true, \"no_extra_limbs\": true}}");
    }

    return QStringLiteral(
        "=== CANONICAL SPEC EXAMPLE (expressive character; nudge values, keep keys) ===\n"
        "{\"subject\": {\"type\": \"character\", \"pose_id\": \"three_quarter_bust\", \"facing\": \"front-right\"}, "
        "\"head\": {\"expression\": \"smile_open\", \"gaze\": \"front\", \"hair_style\": \"long_hime\", "
        "\"hair_color\": \"#2b3a67\", \"eye_color\": \"#3b82f6\", \"skin_tone\": \"#ffe0c0\"}, "
        "\"composition\": {\"framing\": \"bust_up\", \"head_center\": [0.49, 0.37], \"head_height\": 0.42}, "
        "\"palette\": {\"mood\": \"soft_daylight\", \"key\": \"#64748b\", \"accents\": [\"#ff9fb2\"]}, "
        "\"light\": {\"direction\": [-0.5, -0.7], \"warmth\": \"warm_key_cool_fill\", \"time\": \"day\"}, "
        "\"background\": {\"type\": \"simple_gradient\", \"elements\": [], \"forbid\": [\"tree\", "
        "\"stars_over_face\"]}, "
        "\"style\": {\"art_style\": \"anime_cel\", \"line_weight\": \"standard\", \"detail_level\": 0.6}, "
        "\"camera\": {\"focal\": \"normal\", \"tilt\": \"level\"}, "
        "\"narrative\": {\"time\": \"day\", \"weather\": \"clear\", \"props\": []}, "
        "\"rig\": {\"eye_aperture\": 0.85, \"iris_ratio\": 0.62, \"eye_highlight\": \"twin_dot\", "
        "\"double_lid\": true, \"hair_highlight_bands\": 1, \"mouth_width_scale\": 1.0, \"has_brows\": true}, "
        "\"negative\": {\"no_particles_on_face\": true, \"no_text\": true, \"no_extra_limbs\": true}}");
}

QJsonObject KisAiSceneSpecCodec::buildSceneSpecPayload(const QString &model,
                                                       const QString &prompt,
                                                       const QSize &canvasSize,
                                                       int artStyle,
                                                       const QString &reasoningEffort,
                                                       const QString &customInstructions,
                                                       bool enableStreaming,
                                                       bool enforceJsonFormat,
                                                       qreal temperature,
                                                       qreal topP,
                                                       int maxTokensOverride,
                                                       bool forceJsonObjectOnly,
                                                       const QString &referenceImageBase64,
                                                       qint64 seed)
{
    // V6 W5: the Docker art-style combo finally reaches the Spec.
    // The caller passes KisAiPromptAnalyzer::ArtStyle enum values (NOT combo
    // indexes): General=0 leaves the style block untouched, 1..6 map onto the
    // SceneSpec art_style vocabulary, PureLineart=7 has no vocabulary entry and
    // intentionally stays empty.
    auto artStyleIdFor = [](int artStyle) -> QString {
        switch (artStyle) {
        case 1:
            return QStringLiteral("anime_cel");
        case 2:
            return QStringLiteral("watercolor");
        case 3:
            return QStringLiteral("impasto");
        case 4: // ArtStyle::InkSketch
            return QStringLiteral("ink_sketch");
        case 5: // ArtStyle::CyberNeon
            return QStringLiteral("cyber_neon");
        case 6: // ArtStyle::FineLineart
            return QStringLiteral("fine_line");
        default:
            return QString();
        }
    };
    const QString artStyleId = artStyleIdFor(artStyle);
    const bool reasoning = KisAiStrokeProgramCodec::isReasoningModel(model);

    QString systemText = QStringLiteral(
                             "You are an Art Director for a deterministic painting engine. "
                             "Output a SceneSpec JSON object describing WHAT to paint (subject, expression, hairstyle, "
                             "colors, light, framing). "
                             "CRITICAL: Output MEANING ONLY. There are no coordinate fields; the engine owns all "
                             "geometry and guarantees symmetry. "
                             "Use values from the schema enums. Keep every color harmonious with the palette mood.\n\n"
                             "=== USER REQUEST (ABSOLUTE HIGHEST PRIORITY) ===\n\"%1\"\n"
                             "If any example below conflicts with the USER REQUEST, follow the USER REQUEST.\n\n%2")
                             .arg(prompt.trimmed(), canonicalSpecExample(prompt));

    if (!customInstructions.trimmed().isEmpty()) {
        systemText += QStringLiteral("\n\n[USER ADDITIONAL FEEDBACK]\n") + customInstructions.trimmed();
    }
    if (!artStyleId.isEmpty()) {
        systemText += QStringLiteral("\n\n[ART STYLE OVERRIDE]\nUse art_style \"") + artStyleId
            + QStringLiteral("\" in the style block unless the USER REQUEST demands otherwise.");
    }
    const bool hasReferenceImage = !referenceImageBase64.trimmed().isEmpty();
    if (hasReferenceImage) {
        systemText += QStringLiteral(
            "\n\n[REFERENCE IMAGE GUIDANCE]\n"
            "An attached reference image is provided. Faithfully inspect and analyze the character design, "
            "costume/outfit, hairstyle, color palette, lighting atmosphere, and key visual motifs in the reference image. "
            "Reflect these visual traits accurately into the SceneSpec JSON object while honoring the user's prompt.");
    }

    const QJsonObject userObj{
        {QStringLiteral("directive"), QStringLiteral("Return the SceneSpec JSON object for this request.")},
        {QStringLiteral("canvas"),
         QJsonObject{
             {QStringLiteral("width"), canvasSize.width()},
             {QStringLiteral("height"), canvasSize.height()},
         }},
    };

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), model.trimmed());
    if (seed >= 0) {
        payload.insert(QStringLiteral("seed"), static_cast<qint64>(seed & 0x7FFFFFFF));
    }

    QJsonArray messages;
    messages.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), systemText}});

    QJsonObject userMsg;
    userMsg.insert(QStringLiteral("role"), QStringLiteral("user"));
    const QString userText = QString::fromUtf8(QJsonDocument(userObj).toJson(QJsonDocument::Compact));
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
    payload.insert(QStringLiteral("messages"), messages);

    if (enableStreaming) {
        payload.insert(QStringLiteral("stream"), true);
    }

    if (enforceJsonFormat) {
        if (!forceJsonObjectOnly && KisAiStrokeProgramCodec::supportsJsonSchema(model)) {
            QJsonObject schemaObj;
            schemaObj.insert(QStringLiteral("name"), QStringLiteral("scene_spec"));
            schemaObj.insert(QStringLiteral("strict"), true);
            schemaObj.insert(QStringLiteral("schema"), sceneSpecJsonSchema());

            QJsonObject responseFormat;
            responseFormat.insert(QStringLiteral("type"), QStringLiteral("json_schema"));
            responseFormat.insert(QStringLiteral("json_schema"), schemaObj);
            payload.insert(QStringLiteral("response_format"), responseFormat);
        } else {
            QJsonObject responseFormat;
            responseFormat.insert(QStringLiteral("type"), QStringLiteral("json_object"));
            payload.insert(QStringLiteral("response_format"), responseFormat);
        }
    }

    const int calculatedTokens = maxTokensOverride > 0 ? maxTokensOverride : (reasoning ? 8192 : 2048);

    if (reasoning) {
        payload.insert(QStringLiteral("max_completion_tokens"), calculatedTokens);
        if (!reasoningEffort.isEmpty() && reasoningEffort.toLower() != QLatin1String("none")) {
            payload.insert(QStringLiteral("reasoning_effort"), reasoningEffort.toLower());
        }
    } else {
        payload.insert(QStringLiteral("max_tokens"), calculatedTokens);
        payload.insert(QStringLiteral("temperature"), qBound<qreal>(0.0, temperature, 2.0));
        if (topP > 0.0 && topP < 1.0) {
            payload.insert(QStringLiteral("top_p"), qBound<qreal>(0.01, topP, 1.0));
        }
    }

    return payload;
}

KisAiSceneSpec KisAiSceneSpecCodec::defaultSpecForPrompt(const QString &prompt, const QSize &canvasSize)
{
    KisAiSceneSpec spec;
    spec.prompt = prompt;
    spec.canvasSize = canvasSize.isValid() ? canvasSize : QSize(1024, 1024);
    const QString lower = prompt.toLower();

    if (lower.contains(QStringLiteral("night")) || lower.contains(QStringLiteral("starry"))
        || lower.contains(QStringLiteral("moon"))) {
        spec.light.timeOfDay = QStringLiteral("night");
        spec.background.type = QStringLiteral("night_sky_town");
        spec.palette.mood = QStringLiteral("night_festival");
        spec.palette.keyColor = QColor(30, 41, 59);
    } else if (lower.contains(QStringLiteral("sunset")) || lower.contains(QStringLiteral("dusk"))
               || lower.contains(QStringLiteral("evening"))) {
        spec.light.timeOfDay = QStringLiteral("sunset");
        spec.palette.mood = QStringLiteral("quiet_dusk");
        spec.palette.keyColor = QColor(120, 70, 60);
    }
    if (lower.contains(QStringLiteral("landscape")) || lower.contains(QStringLiteral("mountain"))
        || lower.contains(QStringLiteral("scenery")) || lower.contains(QStringLiteral("sea"))) {
        spec.subject.type = QStringLiteral("landscape");
        spec.subject.poseId = QStringLiteral("wide_scene");
        spec.composition.framing = QStringLiteral("wide");
        spec.background.type = spec.light.timeOfDay == QLatin1String("night") ? QStringLiteral("night_sky_town")
                                                                              : QStringLiteral("sky_meadow");
    }
    if (lower.contains(QStringLiteral("silver")) || lower.contains(QStringLiteral("white hair"))) {
        spec.head.hairColor = QColor(226, 232, 240);
    } else if (lower.contains(QStringLiteral("blonde")) || lower.contains(QStringLiteral("blond"))
               || lower.contains(QStringLiteral("gold"))) {
        spec.head.hairColor = QColor(250, 214, 90);
    } else if (lower.contains(QStringLiteral("pink"))
               && (lower.contains(QStringLiteral("hair")) || lower.contains(QStringLiteral("twin")))) {
        spec.head.hairColor = QColor(255, 160, 185);
    } else if (lower.contains(QStringLiteral("red")) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairColor = QColor(195, 55, 60);
    } else if (lower.contains(QStringLiteral("black")) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairColor = QColor(24, 24, 32);
    }
    if (lower.contains(QStringLiteral("twin")) || lower.contains(QStringLiteral("twintail"))) {
        spec.head.hairStyle = QStringLiteral("twin_tails");
    } else if (lower.contains(QStringLiteral("pony")) || lower.contains(QStringLiteral("ponytail"))) {
        spec.head.hairStyle = QStringLiteral("pony_tail");
    } else if (lower.contains(QStringLiteral("half up")) || lower.contains(QStringLiteral("half-up"))) {
        spec.head.hairStyle = QStringLiteral("half_up");
    } else if (lower.contains(QStringLiteral("wolf"))) {
        spec.head.hairStyle = QStringLiteral("wolf_cut");
    } else if (lower.contains(QStringLiteral("braid")) || lower.contains(QStringLiteral("braided"))) {
        spec.head.hairStyle = QStringLiteral("braided");
    } else if (lower.contains(QStringLiteral("bob"))) {
        spec.head.hairStyle = QStringLiteral("bob");
    } else if (lower.contains(QStringLiteral("short")) && lower.contains(QStringLiteral("hair"))) {
        spec.head.hairStyle = QStringLiteral("short_messy");
    }

    if (lower.contains(QStringLiteral("see through")) || lower.contains(QStringLiteral("see_through"))
        || lower.contains(QStringLiteral("see-through"))) {
        spec.head.hairBangs = QStringLiteral("see_through");
    } else if (lower.contains(QStringLiteral("blunt")) || lower.contains(QStringLiteral("straight cut"))) {
        spec.head.hairBangs = QStringLiteral("blunt_bangs");
    }

    if (lower.contains(QStringLiteral("wink"))) {
        spec.head.expression = QStringLiteral("wink_left");
    } else if (lower.contains(QStringLiteral("blush")) || lower.contains(QStringLiteral("shy"))) {
        spec.head.expression = QStringLiteral("blush_shy");
        spec.head.blushIntensity = 0.85;
    } else if (lower.contains(QStringLiteral("smug")) || lower.contains(QStringLiteral("confident"))) {
        spec.head.expression = QStringLiteral("confident_smug");
    }

    if (lower.contains(QStringLiteral("backlight")) || lower.contains(QStringLiteral("rim"))) {
        spec.light.lightingStyle = QStringLiteral("dramatic_backlight");
        spec.light.rimIntensity = 0.75;
    } else if (lower.contains(QStringLiteral("komorebi")) || lower.contains(QStringLiteral("dappled"))) {
        spec.light.lightingStyle = QStringLiteral("komorebi_dappled");
    }

    if (lower.contains(QStringLiteral("sparkle")) || lower.contains(QStringLiteral("radiant"))) {
        spec.head.eyeHighlightStyle = QStringLiteral("radiant_sparkle");
        spec.rig.eyeHighlight = QStringLiteral("radiant_sparkle");
    } else if (lower.contains(QStringLiteral("crescent"))) {
        spec.head.eyeHighlightStyle = QStringLiteral("crescent");
        spec.rig.eyeHighlight = QStringLiteral("crescent");
    }
    if (lower.contains(QStringLiteral("green")) && lower.contains(QStringLiteral("eye"))) {
        spec.head.eyeColor = QColor(34, 197, 94);
    } else if (lower.contains(QStringLiteral("red")) && lower.contains(QStringLiteral("eye"))) {
        spec.head.eyeColor = QColor(239, 68, 68);
    }
    if (lower.contains(QStringLiteral("close"))
        && (lower.contains(QStringLiteral("face")) || lower.contains(QStringLiteral("portrait")))) {
        spec.composition.framing = QStringLiteral("face_closeup");
        spec.composition.headHeight = 0.60;
    } else if (lower.contains(QStringLiteral("full")) && lower.contains(QStringLiteral("body"))) {
        spec.composition.framing = QStringLiteral("full_body");
        spec.composition.headHeight = 0.22;
        spec.composition.headCenter = QPointF(0.5, 0.24);
    }
    if (lower.contains(QStringLiteral("hoodie")) || lower.contains(QStringLiteral("parka"))) {
        spec.clothing.style = QStringLiteral("hoodie");
        spec.clothing.color = QColor(64, 72, 90);
    } else if (lower.contains(QStringLiteral("sailor")) || lower.contains(QStringLiteral("school"))
               || lower.contains(QStringLiteral("uniform"))) {
        spec.clothing.style = QStringLiteral("school_uniform");
        spec.clothing.color = QColor(36, 44, 70);
    } else if (lower.contains(QStringLiteral("dress")) || lower.contains(QStringLiteral("goth"))) {
        spec.clothing.style = QStringLiteral("dress");
        spec.clothing.color = QColor(48, 32, 54);
    } else if (lower.contains(QStringLiteral("kimono")) || lower.contains(QStringLiteral("yukata"))) {
        spec.clothing.style = QStringLiteral("kimono");
        spec.clothing.color = QColor(160, 48, 64);
    }
    KisAi::OntologyApplier::apply(prompt, &spec);
    return spec;
}

// ========================================================================
// V5 R5: deterministic N-best SceneSpec scoring
// ========================================================================
namespace
{
qreal paletteHarmonyScore(const KisAiSceneSpec &spec)
{
    // Harmony = complementary-but-not-clashing key vs accent hues and a sane
    // key-light value. Deterministic, no randomness.
    const QColor key = spec.palette.keyColor;
    if (!key.isValid() || spec.palette.accents.isEmpty())
        return 0.6;
    const QColor accent = spec.palette.accents.first();
    if (!accent.isValid())
        return 0.6;

    int keyH = key.hue();
    int accH = accent.hue();
    if (keyH < 0)
        keyH = 0;
    if (accH < 0)
        accH = 0;
    int hueDist = qAbs(keyH - accH);
    if (hueDist > 180)
        hueDist = 360 - hueDist;

    // Sweet spots: analogous (<=60) or split-complement (~120..150).
    qreal score = 0.55;
    if (hueDist <= 60)
        score = 0.85;
    else if (hueDist >= 110 && hueDist <= 160)
        score = 0.90;
    else if (hueDist <= 90)
        score = 0.70;

    // Very dark key with very dark accents reads as mud.
    if (key.value() < 60 && accent.value() < 60)
        score -= 0.25;
    return qBound<qreal>(0.0, score, 1.0);
}

qreal rigFeasibilityScore(const KisAiSceneSpec &spec, QStringList *notes)
{
    // The parser already clamps into invariant ranges; feasibility measures
    // how far the requested values sit from canonical rig defaults.
    const KisAiSceneRigOverrides &r = spec.rig;
    qreal deviation = 0.0;
    deviation += qAbs(r.eyeAperture - 0.85) / 0.85;
    deviation += qAbs(r.irisRatio - 0.62) / 0.27;
    deviation += qAbs(r.hairStrandDensity - 0.55) / 0.55;
    deviation += qAbs(r.hairFlyaway - 0.35) / 0.35;
    deviation += qAbs(r.mouthWidthScale - 1.0) / 0.4;
    deviation += qAbs(r.hairHighlightBands - 1) / 2.0;
    const qreal normalized = deviation / 6.0; // average over the 6 terms
    if (normalized > 0.5 && notes)
        notes->append(QStringLiteral("rig params deviate strongly from canonical defaults"));
    return qBound<qreal>(0.0, 1.0 - normalized * 0.5, 1.0);
}

qreal intentMatchScore(const KisAiSceneSpec &spec)
{
    // Deterministic keyword coverage: does the spec vocabulary answer the
    // prompt? Reuses the same tokenization style as defaultSpecForPrompt.
    const QString lower = spec.prompt.toLower();
    if (lower.trimmed().isEmpty())
        return 0.5;

    QStringList evidence;
    if (lower.contains(QStringLiteral("night")) || lower.contains(QStringLiteral("moon"))
        || lower.contains(QStringLiteral("starry")))
        evidence.append(spec.light.timeOfDay == QLatin1String("night") ? QStringLiteral("t_night") : QString());
    if (lower.contains(QStringLiteral("sunset")) || lower.contains(QStringLiteral("dusk"))
        || lower.contains(QStringLiteral("evening")))
        evidence.append(spec.light.timeOfDay == QLatin1String("sunset") ? QStringLiteral("t_sunset") : QString());
    if (lower.contains(QStringLiteral("twin")))
        evidence.append(spec.head.hairStyle == QLatin1String("twin_tails") ? QStringLiteral("t_twin") : QString());
    if (lower.contains(QStringLiteral("bob")))
        evidence.append(spec.head.hairStyle == QLatin1String("bob") ? QStringLiteral("t_bob") : QString());
    if (lower.contains(QStringLiteral("hoodie")) || lower.contains(QStringLiteral("parka")))
        evidence.append(spec.clothing.style == QLatin1String("hoodie") ? QStringLiteral("t_hoodie") : QString());
    if (lower.contains(QStringLiteral("uniform")) || lower.contains(QStringLiteral("school"))
        || lower.contains(QStringLiteral("sailor")))
        evidence.append(spec.clothing.style == QLatin1String("school_uniform") ? QStringLiteral("t_uniform")
                                                                               : QString());
    if (lower.contains(QStringLiteral("kimono")) || lower.contains(QStringLiteral("yukata")))
        evidence.append(spec.clothing.style == QLatin1String("kimono") ? QStringLiteral("t_kimono") : QString());
    if (lower.contains(QStringLiteral("close")) && lower.contains(QStringLiteral("face")))
        evidence.append(spec.composition.framing == QLatin1String("face_closeup") ? QStringLiteral("t_closeup")
                                                                                  : QString());
    if (lower.contains(QStringLiteral("full")) && lower.contains(QStringLiteral("body")))
        evidence.append(spec.composition.framing == QLatin1String("full_body") ? QStringLiteral("t_fullbody")
                                                                               : QString());
    if (lower.contains(QStringLiteral("watercolor")))
        evidence.append(spec.style.artStyleId == QLatin1String("watercolor") ? QStringLiteral("t_wc") : QString());
    if (lower.contains(QStringLiteral("neon")) || lower.contains(QStringLiteral("cyber")))
        evidence.append(spec.style.artStyleId == QLatin1String("cyber_neon") ? QStringLiteral("t_neon") : QString());

    int checked = 0;
    int matched = 0;
    for (const QString &e : evidence) {
        checked++;
        if (!e.isEmpty())
            matched++;
    }
    if (checked == 0)
        return 0.7; // nothing checkable; neutral
    return qBound<qreal>(0.0, qreal(matched) / checked, 1.0);
}
} // namespace

KisAiSceneSpecScore KisAiSceneSpecCodec::scoreSceneSpec(const KisAiSceneSpec &spec, const QStringList &candidateSpecs)
{
    Q_UNUSED(candidateSpecs);
    KisAiSceneSpecScore s;
    s.paletteHarmony = paletteHarmonyScore(spec);
    s.rigFeasibility = rigFeasibilityScore(spec, &s.notes);
    s.intentMatch = intentMatchScore(spec);

    qreal negativeCompliance = 1.0;
    if (!spec.negative.noParticlesOnFace)
        negativeCompliance -= 0.2;
    if (!spec.negative.noText)
        negativeCompliance -= 0.2;
    if (!spec.negative.noExtraLimbs)
        negativeCompliance -= 0.3;
    s.negativeCompliance = qBound<qreal>(0.0, negativeCompliance, 1.0);

    s.total = 0.30 * s.intentMatch + 0.25 * s.paletteHarmony + 0.25 * s.rigFeasibility + 0.20 * s.negativeCompliance;
    return s;
}

KisAiSceneSpec KisAiSceneSpecCodec::selectBestSpec(const QString &prompt,
                                                   const QSize &canvasSize,
                                                   const QVector<KisAiSceneSpec> &candidates)
{
    if (candidates.isEmpty())
        return defaultSpecForPrompt(prompt, canvasSize);

    const KisAiSceneSpec *best = nullptr;
    KisAiSceneSpecScore bestScore;
    for (const KisAiSceneSpec &c : candidates) {
        KisAiSceneSpec spec = c;
        if (spec.prompt.trimmed().isEmpty())
            spec.prompt = prompt;
        spec.canvasSize = canvasSize.isValid() ? canvasSize : spec.canvasSize;
        const KisAiSceneSpecScore s = scoreSceneSpec(spec);
        if (!best || s.total > bestScore.total) {
            best = &c;
            bestScore = s;
        }
    }
    KisAiSceneSpec result = *best;
    result.prompt = result.prompt.trimmed().isEmpty() ? prompt : result.prompt;
    result.canvasSize = canvasSize.isValid() ? canvasSize : result.canvasSize;
    return result;
}
