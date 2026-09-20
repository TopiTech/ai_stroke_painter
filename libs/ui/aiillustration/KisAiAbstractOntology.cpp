/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiAbstractOntology.h"

#include <QColor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "KisAiSceneSpec.h"

namespace KisAi
{

// ===========================================================================
// デフォルトルール (30+ 件)
// ===========================================================================

namespace
{

OntologyRule make(const QStringList &enWords,
                  const QStringList &jaWords,
                  const QString &specPath,
                  OntologyRule::DeltaKind kind,
                  const QString &stringValue,
                  QColor colorValue,
                  qreal numberValue,
                  const QString &artStyleHint,
                  qreal weight,
                  const QString &category,
                  const QString &description)
{
    OntologyRule r;
    r.triggerWords = enWords;
    r.triggerWordsJa = jaWords;
    r.specPath = specPath;
    r.kind = kind;
    r.stringValue = stringValue;
    r.colorValue = colorValue;
    r.numberValue = numberValue;
    r.artStyleHint = artStyleHint;
    r.weight = weight;
    r.category = category;
    r.description = description;
    return r;
}

QList<OntologyRule> buildDefaultRules()
{
    QList<OntologyRule> rules;

    // -------- mood --------
    rules.append(make({QStringLiteral("jazzy"), QStringLiteral("vibrant"), QStringLiteral("energetic")},
                      {QStringLiteral("ジャズ"), QStringLiteral("鮮やか"), QStringLiteral("エネルギッシュ")},
                      QStringLiteral("colorScript.accentWeight"),
                      OntologyRule::AddNumber,
                      QString(),
                      QColor(),
                      0.15,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Jazzzy/Vibrant → accentWeight +0.15")));

    rules.append(make({QStringLiteral("melancholy"), QStringLiteral("gloomy"), QStringLiteral("sorrowful")},
                      {QStringLiteral("陰鬱"), QStringLiteral("憂鬱"), QStringLiteral("物悲しい")},
                      QStringLiteral("light.timeOfDay"),
                      OntologyRule::SetString,
                      QStringLiteral("night"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Melancholy → timeOfDay=night")));

    rules.append(make({QStringLiteral("melancholy"), QStringLiteral("gloomy")},
                      {QStringLiteral("陰鬱"), QStringLiteral("憂鬱")},
                      QStringLiteral("colorScript.shadow"),
                      OntologyRule::SetColor,
                      QString(),
                      QColor(20, 30, 60),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Melancholy → shadow tinted cool blue")));

    rules.append(make({QStringLiteral("ethereal"), QStringLiteral("dreamy")},
                      {QStringLiteral("幻想的"), QStringLiteral("夢のような")},
                      QStringLiteral("style.artStyleId"),
                      OntologyRule::SetString,
                      QStringLiteral("watercolor"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Ethereal/Dreamy → artStyle=watercolor")));

    rules.append(make({QStringLiteral("romantic"), QStringLiteral("lovely")},
                      {QStringLiteral("ロマンチック"), QStringLiteral("かわいい")},
                      QStringLiteral("colorScript.accentWeight"),
                      OntologyRule::AddNumber,
                      QString(),
                      QColor(),
                      0.10,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Romantic → accentWeight +0.10")));

    rules.append(make({QStringLiteral("mysterious"), QStringLiteral("enigmatic")},
                      {QStringLiteral("神秘的"), QStringLiteral("謎めいた")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("muted"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Mysterious → palette mood=muted")));

    rules.append(make({QStringLiteral("energetic"), QStringLiteral("powerful")},
                      {QStringLiteral("力強い"), QStringLiteral("元気")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("vibrant"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Energetic → palette mood=vibrant")));

    rules.append(make({QStringLiteral("calm"), QStringLiteral("peaceful"), QStringLiteral("serene")},
                      {QStringLiteral("穏やか"), QStringLiteral("静か"), QStringLiteral("平和")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("pastel"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("mood"),
                      QStringLiteral("Calm → palette mood=pastel")));

    // -------- time --------
    rules.append(make({QStringLiteral("dawn"), QStringLiteral("sunrise"), QStringLiteral("early morning")},
                      {QStringLiteral("夜明け"), QStringLiteral("早朝")},
                      QStringLiteral("light.timeOfDay"),
                      OntologyRule::SetString,
                      QStringLiteral("dawn"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("time"),
                      QStringLiteral("Dawn → timeOfDay=dawn")));

    rules.append(make({QStringLiteral("golden hour"), QStringLiteral("sunset"), QStringLiteral("dusk")},
                      {QStringLiteral("夕暮れ"), QStringLiteral("黄昏"), QStringLiteral("マジックアワー")},
                      QStringLiteral("light.timeOfDay"),
                      OntologyRule::SetString,
                      QStringLiteral("sunset"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("time"),
                      QStringLiteral("Golden hour → timeOfDay=sunset")));

    rules.append(make({QStringLiteral("midnight"), QStringLiteral("late night")},
                      {QStringLiteral("深夜"), QStringLiteral("真夜中")},
                      QStringLiteral("light.timeOfDay"),
                      OntologyRule::SetString,
                      QStringLiteral("night"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("time"),
                      QStringLiteral("Midnight → timeOfDay=night")));

    rules.append(make({QStringLiteral("bright daylight"), QStringLiteral("noon")},
                      {QStringLiteral("真昼"), QStringLiteral("正午")},
                      QStringLiteral("light.timeOfDay"),
                      OntologyRule::SetString,
                      QStringLiteral("day"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("time"),
                      QStringLiteral("Noon → timeOfDay=day")));

    // -------- weather --------
    rules.append(make({QStringLiteral("rainy"), QStringLiteral("raining"), QStringLiteral("wet")},
                      {QStringLiteral("雨"), QStringLiteral("雨模様")},
                      QStringLiteral("narrative.weather"),
                      OntologyRule::SetString,
                      QStringLiteral("rain"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("weather"),
                      QStringLiteral("Rainy → narrative.weather=rain")));

    rules.append(make({QStringLiteral("snowy"), QStringLiteral("snowing"), QStringLiteral("blizzard")},
                      {QStringLiteral("雪"), QStringLiteral("吹雪")},
                      QStringLiteral("narrative.weather"),
                      OntologyRule::SetString,
                      QStringLiteral("snow"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("weather"),
                      QStringLiteral("Snowy → narrative.weather=snow")));

    rules.append(make({QStringLiteral("foggy"), QStringLiteral("misty")},
                      {QStringLiteral("霧"), QStringLiteral("もや")},
                      QStringLiteral("narrative.weather"),
                      OntologyRule::SetString,
                      QStringLiteral("fog"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("weather"),
                      QStringLiteral("Foggy → narrative.weather=fog")));

    rules.append(make({QStringLiteral("cloudy"), QStringLiteral("overcast")},
                      {QStringLiteral("曇り"), QStringLiteral("どんより")},
                      QStringLiteral("narrative.weather"),
                      OntologyRule::SetString,
                      QStringLiteral("cloudy"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("weather"),
                      QStringLiteral("Cloudy → narrative.weather=cloudy")));

    rules.append(make({QStringLiteral("stormy"), QStringLiteral("thunderstorm")},
                      {QStringLiteral("嵐"), QStringLiteral("雷雨")},
                      QStringLiteral("narrative.weather"),
                      OntologyRule::SetString,
                      QStringLiteral("storm"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("weather"),
                      QStringLiteral("Stormy → narrative.weather=storm")));

    // -------- texture / art style --------
    rules.append(make({QStringLiteral("painterly"), QStringLiteral("impasto"), QStringLiteral("thick paint")},
                      {QStringLiteral("厚塗り"), QStringLiteral("ペインティング")},
                      QStringLiteral("style.artStyleId"),
                      OntologyRule::SetString,
                      QStringLiteral("impasto"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Painterly → artStyle=impasto")));

    rules.append(make({QStringLiteral("ink sketch"), QStringLiteral("ink"), QStringLiteral("pen and ink")},
                      {QStringLiteral("墨絵"), QStringLiteral("ペン画")},
                      QStringLiteral("style.artStyleId"),
                      OntologyRule::SetString,
                      QStringLiteral("ink_sketch"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Ink sketch → artStyle=ink_sketch")));

    rules.append(make({QStringLiteral("cyberpunk"), QStringLiteral("cyber"), QStringLiteral("neon")},
                      {QStringLiteral("サイバーパンク"), QStringLiteral("ネオン")},
                      QStringLiteral("style.artStyleId"),
                      OntologyRule::SetString,
                      QStringLiteral("cyber_neon"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Cyberpunk → artStyle=cyber_neon")));

    rules.append(make({QStringLiteral("delicate"), QStringLiteral("refined"), QStringLiteral("fine line")},
                      {QStringLiteral("繊細"), QStringLiteral("細密")},
                      QStringLiteral("style.lineWeight"),
                      OntologyRule::SetString,
                      QStringLiteral("delicate"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Delicate → lineWeight=delicate")));

    rules.append(make({QStringLiteral("bold"), QStringLiteral("strong lines")},
                      {QStringLiteral("太い線"), QStringLiteral("力強い線")},
                      QStringLiteral("style.lineWeight"),
                      OntologyRule::SetString,
                      QStringLiteral("bold"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Bold → lineWeight=bold")));

    rules.append(make({QStringLiteral("sketchy"), QStringLiteral("rough")},
                      {QStringLiteral("スケッチ風"), QStringLiteral("ラフ")},
                      QStringLiteral("style.detailLevel"),
                      OntologyRule::SetNumber,
                      QString(),
                      QColor(),
                      0.4,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Sketchy → detailLevel=0.4")));

    rules.append(make({QStringLiteral("highly detailed"), QStringLiteral("intricate")},
                      {QStringLiteral("詳細"), QStringLiteral("精巧")},
                      QStringLiteral("style.detailLevel"),
                      OntologyRule::SetNumber,
                      QString(),
                      QColor(),
                      0.85,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Highly detailed → detailLevel=0.85")));

    // -------- palette --------
    rules.append(make({QStringLiteral("pastel"), QStringLiteral("soft colors")},
                      {QStringLiteral("パステル"), QStringLiteral("淡い色")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("pastel"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Pastel → palette.mood=pastel")));

    rules.append(make({QStringLiteral("vibrant"), QStringLiteral("saturated")},
                      {QStringLiteral("ビビッド"), QStringLiteral("高彩度")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("vibrant"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Vibrant → palette.mood=vibrant")));

    rules.append(make({QStringLiteral("monochrome"), QStringLiteral("monochromatic"), QStringLiteral("grayscale")},
                      {QStringLiteral("モノクロ"), QStringLiteral("白黒")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("monochrome"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Monochrome → palette.mood=monochrome")));

    rules.append(make({QStringLiteral("warm"), QStringLiteral("warm tones")},
                      {QStringLiteral("暖色"), QStringLiteral("暖かい")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("warm"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Warm tones → palette.mood=warm")));

    rules.append(make({QStringLiteral("cool"), QStringLiteral("cool tones")},
                      {QStringLiteral("寒色"), QStringLiteral("冷たい")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("cool"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Cool tones → palette.mood=cool")));

    rules.append(make({QStringLiteral("muted"), QStringLiteral("desaturated")},
                      {QStringLiteral("くすんだ"), QStringLiteral("控えめ")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("muted"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Muted → palette.mood=muted")));

    rules.append(make({QStringLiteral("neon"), QStringLiteral("fluorescent")},
                      {QStringLiteral("ネオン"), QStringLiteral("蛍光色")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("neon"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Neon → palette.mood=neon")));

    rules.append(make({QStringLiteral("noir"), QStringLiteral("dark"), QStringLiteral("shadowy")},
                      {QStringLiteral("ノワール"), QStringLiteral("暗黒")},
                      QStringLiteral("palette.mood"),
                      OntologyRule::SetString,
                      QStringLiteral("noir"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("palette"),
                      QStringLiteral("Noir → palette.mood=noir")));

    rules.append(make({QStringLiteral("fine line"), QStringLiteral("thin line")},
                      {QStringLiteral("細線"), QStringLiteral("細い線")},
                      QStringLiteral("style.artStyleId"),
                      OntologyRule::SetString,
                      QStringLiteral("fine_line"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Fine line → artStyle=fine_line")));

    rules.append(make({QStringLiteral("watercolor"), QStringLiteral("aqua")},
                      {QStringLiteral("水彩"), QStringLiteral(" watercolor ")}, // 先頭/末尾スペースで誤マッチ防止
                      QStringLiteral("style.artStyleId"),
                      OntologyRule::SetString,
                      QStringLiteral("watercolor"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Watercolor → artStyle=watercolor")));

    rules.append(make({QStringLiteral("anime"), QStringLiteral("cel shading")},
                      {QStringLiteral("アニメ風"), QStringLiteral("セルシェーディング")},
                      QStringLiteral("style.artStyleId"),
                      OntologyRule::SetString,
                      QStringLiteral("anime_cel"),
                      QColor(),
                      0.0,
                      QString(),
                      1.0,
                      QStringLiteral("texture"),
                      QStringLiteral("Anime/cel shading → artStyle=anime_cel")));

    return rules;
}

} // namespace

OntologyRuleset OntologyRuleset::defaultRules()
{
    OntologyRuleset r;
    r.rules = buildDefaultRules();
    return r;
}

QJsonArray OntologyRuleset::toJson() const
{
    QJsonArray array;
    for (int i = 0; i < rules.size(); ++i) {
        const OntologyRule &r = rules.at(i);
        QJsonObject obj;
        QJsonArray enWords;
        for (int w = 0; w < r.triggerWords.size(); ++w)
            enWords.append(r.triggerWords.at(w));
        QJsonArray jaWords;
        for (int w = 0; w < r.triggerWordsJa.size(); ++w)
            jaWords.append(r.triggerWordsJa.at(w));
        obj.insert(QStringLiteral("triggerWords"), enWords);
        obj.insert(QStringLiteral("triggerWordsJa"), jaWords);
        obj.insert(QStringLiteral("specPath"), r.specPath);
        QString kindStr;
        switch (r.kind) {
        case OntologyRule::SetString:
            kindStr = QStringLiteral("SetString");
            break;
        case OntologyRule::SetColor:
            kindStr = QStringLiteral("SetColor");
            break;
        case OntologyRule::AddNumber:
            kindStr = QStringLiteral("AddNumber");
            break;
        case OntologyRule::SetNumber:
            kindStr = QStringLiteral("SetNumber");
            break;
        }
        obj.insert(QStringLiteral("kind"), kindStr);
        if (!r.stringValue.isEmpty())
            obj.insert(QStringLiteral("stringValue"), r.stringValue);
        if (r.colorValue.isValid())
            obj.insert(QStringLiteral("colorValue"), r.colorValue.name());
        obj.insert(QStringLiteral("numberValue"), r.numberValue);
        if (!r.artStyleHint.isEmpty())
            obj.insert(QStringLiteral("artStyleHint"), r.artStyleHint);
        obj.insert(QStringLiteral("weight"), r.weight);
        obj.insert(QStringLiteral("category"), r.category);
        obj.insert(QStringLiteral("description"), r.description);
        array.append(obj);
    }
    return array;
}

OntologyRuleset OntologyRuleset::fromJson(const QJsonArray &array)
{
    OntologyRuleset r;
    for (int i = 0; i < array.size(); ++i) {
        const QJsonObject obj = array.at(i).toObject();
        OntologyRule rule;
        const QJsonArray enWords = obj.value(QStringLiteral("triggerWords")).toArray();
        for (int w = 0; w < enWords.size(); ++w) {
            rule.triggerWords.append(enWords.at(w).toString());
        }
        const QJsonArray jaWords = obj.value(QStringLiteral("triggerWordsJa")).toArray();
        for (int w = 0; w < jaWords.size(); ++w) {
            rule.triggerWordsJa.append(jaWords.at(w).toString());
        }
        rule.specPath = obj.value(QStringLiteral("specPath")).toString();
        const QString kindStr = obj.value(QStringLiteral("kind")).toString();
        if (kindStr == QStringLiteral("SetColor"))
            rule.kind = OntologyRule::SetColor;
        else if (kindStr == QStringLiteral("AddNumber"))
            rule.kind = OntologyRule::AddNumber;
        else if (kindStr == QStringLiteral("SetNumber"))
            rule.kind = OntologyRule::SetNumber;
        else
            rule.kind = OntologyRule::SetString;
        rule.stringValue = obj.value(QStringLiteral("stringValue")).toString();
        const QString colorStr = obj.value(QStringLiteral("colorValue")).toString();
        if (!colorStr.isEmpty())
            rule.colorValue = QColor(colorStr);
        rule.numberValue = obj.value(QStringLiteral("numberValue")).toDouble();
        rule.artStyleHint = obj.value(QStringLiteral("artStyleHint")).toString();
        rule.weight = obj.value(QStringLiteral("weight")).toDouble(1.0);
        rule.category = obj.value(QStringLiteral("category")).toString();
        rule.description = obj.value(QStringLiteral("description")).toString();
        r.rules.append(rule);
    }
    return r;
}

OntologyRuleset OntologyRuleset::loadCustom(const QString &path)
{
    OntologyRuleset r = defaultRules();
    if (path.isEmpty())
        return r;
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return r;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &perr);
    file.close();
    if (perr.error != QJsonParseError::NoError || !doc.isArray())
        return r;
    const OntologyRuleset custom = fromJson(doc.array());
    // カスタムルールを末尾に append (デフォルトより優先したい場合は別途ロジック追加可能)
    for (int i = 0; i < custom.rules.size(); ++i) {
        r.rules.append(custom.rules.at(i));
    }
    return r;
}

// ===========================================================================
// OntologyApplier
// ===========================================================================

namespace
{

bool promptMatchesRule(const QString &prompt, const OntologyRule &rule)
{
    // 英語トリガーは単語境界で照合する ("ink" が "pink"/"link" に誤爆しないよう)。
    // 日本語は分かち書きしないため従来どおり部分一致とする。
    static auto containsWordEn = [](const QString &text, const QString &word) {
        if (word.isEmpty())
            return false;
        QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(word) + QStringLiteral("\\b"),
                              QRegularExpression::CaseInsensitiveOption);
        return text.contains(re);
    };
    for (int i = 0; i < rule.triggerWords.size(); ++i) {
        if (containsWordEn(prompt, rule.triggerWords.at(i)))
            return true;
    }
    for (int i = 0; i < rule.triggerWordsJa.size(); ++i) {
        if (prompt.contains(rule.triggerWordsJa.at(i)))
            return true;
    }
    return false;
}

void applyRuleToSpec(const OntologyRule &rule, KisAiSceneSpec *spec)
{
    if (!spec)
        return;
    const QString path = rule.specPath;
    if (path == QStringLiteral("colorScript.accentWeight")) {
        if (rule.kind == OntologyRule::AddNumber) {
            const qreal weight = (rule.weight > 0.0 && std::isfinite(rule.weight)) ? rule.weight : 1.0;
            spec->colorScript.accentWeight =
                qBound<qreal>(0.0, spec->colorScript.accentWeight + rule.numberValue * weight, 1.0);
        } else if (rule.kind == OntologyRule::SetNumber) {
            spec->colorScript.accentWeight = qBound<qreal>(0.0, rule.numberValue, 1.0);
        }
    } else if (path == QStringLiteral("colorScript.shadow")) {
        if (rule.kind == OntologyRule::SetColor && rule.colorValue.isValid()) {
            spec->colorScript.shadow = rule.colorValue;
        }
    } else if (path == QStringLiteral("light.timeOfDay")) {
        if (rule.kind == OntologyRule::SetString) {
            spec->light.timeOfDay = rule.stringValue;
            spec->narrative.time = rule.stringValue;
        }
    } else if (path == QStringLiteral("palette.mood")) {
        if (rule.kind == OntologyRule::SetString) {
            spec->palette.mood = rule.stringValue;
        }
    } else if (path == QStringLiteral("style.artStyleId")) {
        if (rule.kind == OntologyRule::SetString) {
            spec->style.artStyleId = rule.stringValue;
        }
    } else if (path == QStringLiteral("style.lineWeight")) {
        if (rule.kind == OntologyRule::SetString) {
            spec->style.lineWeight = rule.stringValue;
        }
    } else if (path == QStringLiteral("style.detailLevel")) {
        if (rule.kind == OntologyRule::SetNumber) {
            spec->style.detailLevel = qBound<qreal>(0.0, rule.numberValue, 1.0);
        }
    } else if (path == QStringLiteral("narrative.weather")) {
        if (rule.kind == OntologyRule::SetString) {
            spec->narrative.weather = rule.stringValue;
        }
    }
}

} // namespace

int OntologyApplier::apply(const QString &prompt,
                           KisAiSceneSpec *spec,
                           const OntologyRuleset &ruleset,
                           QStringList *appliedDescriptions)
{
    int applied = 0;
    if (!spec)
        return 0;

    // 簡略化のため、各 specPath を 1 度だけ処理する (集約は同一種別内で実施)

    for (int i = 0; i < ruleset.rules.size(); ++i) {
        const OntologyRule &rule = ruleset.rules.at(i);
        if (!promptMatchesRule(prompt, rule))
            continue;
        applyRuleToSpec(rule, spec);
        ++applied;
        if (appliedDescriptions) {
            appliedDescriptions->append(QStringLiteral("[%1] %2").arg(rule.category, rule.description));
        }
    }

    return applied;
}

} // namespace KisAi