/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ABSTRACT_ONTOLOGY_H
#define KIS_AI_ABSTRACT_ONTOLOGY_H

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiSceneSpec.h"

/**
 * V8 Phase 4: 抽象語オントロジー.
 *
 * プロンプト内の抽象語 ("jazzy", "melancholy", "ethereal", "noir", "painterly" など) を
 * SceneSpec の具体フィールドに自動変換する軽量ルールエンジン。
 *
 * - デフォルト 30+ ルールを内蔵 (mood / time / weather / texture / palette カテゴリ)
 * - ユーザー辞書 JSON をマージして優先適用
 * - 同じ specPath への複数ヒットは加重平均で集約
 */
namespace KisAi
{

struct KRITAUI_EXPORT OntologyRule {
    QStringList triggerWords; // 英語 (大文字小文字無視)
    QStringList triggerWordsJa; // 日本語
    QString specPath; // "colorScript.accentWeight", "light.timeOfDay" など
    enum DeltaKind {
        SetString,
        SetColor,
        AddNumber,
        SetNumber
    };
    DeltaKind kind{SetString};
    QString stringValue;
    QColor colorValue;
    qreal numberValue{0.0};
    QString artStyleHint; // "anime_cel" など (空なら変更なし)
    qreal weight{1.0}; // ルール適用重み (1.0 が標準)
    QString category; // "mood", "time", "weather", "texture", "palette"
    QString description;
};

struct KRITAUI_EXPORT OntologyRuleset {
    QList<OntologyRule> rules;

    static OntologyRuleset defaultRules(); // 30+ ルール
    static OntologyRuleset loadCustom(const QString &path); // ユーザー辞書 JSON
    QJsonArray toJson() const;
    static OntologyRuleset fromJson(const QJsonArray &array);
};

/// ルール適用器
class KRITAUI_EXPORT OntologyApplier
{
public:
    /// プロンプト文字列をトークン化して、ルールを SceneSpec に適用する。
    /// 同じ specPath への複数ヒットは加重平均 (重み = OntologyRule::weight) で集約。
    /// 戻り値: 適用されたルール数と、新 SceneSpec。
    static int apply(const QString &prompt,
                     KisAiSceneSpec *spec,
                     const OntologyRuleset &ruleset = OntologyRuleset::defaultRules(),
                     QStringList *appliedDescriptions = nullptr);
};

} // namespace KisAi

#endif // KIS_AI_ABSTRACT_ONTOLOGY_H