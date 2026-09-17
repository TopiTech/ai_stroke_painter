/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiAbstractOntologyTest.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiAbstractOntology.h"
#include "aiillustration/KisAiSceneSpec.h"

using namespace KisAi;

void KisAiAbstractOntologyTest::testDefaultRulesCount()
{
    const OntologyRuleset ruleset = OntologyRuleset::defaultRules();
    // 計画書要件: 30+ ルールを内蔵
    QVERIFY2(ruleset.rules.size() >= 30,
             qPrintable(QString("Expected >= 30 rules, got %1").arg(ruleset.rules.size())));
}

void KisAiAbstractOntologyTest::testRuleCategoriesCovered()
{
    const OntologyRuleset ruleset = OntologyRuleset::defaultRules();
    QSet<QString> categories;
    for (const OntologyRule &r : ruleset.rules) {
        categories.insert(r.category);
    }

    QVERIFY(categories.contains(QStringLiteral("mood")));
    QVERIFY(categories.contains(QStringLiteral("time")));
    QVERIFY(categories.contains(QStringLiteral("weather")));
    QVERIFY(categories.contains(QStringLiteral("texture")));
    QVERIFY(categories.contains(QStringLiteral("palette")));
}

void KisAiAbstractOntologyTest::testApplyJazzyVibrant()
{
    KisAiSceneSpec spec;
    spec.colorScript.accentWeight = 0.10;

    QStringList descriptions;
    const int count = OntologyApplier::apply(QStringLiteral("a jazzy vibrant street at evening"),
                                            &spec,
                                            OntologyRuleset::defaultRules(),
                                            &descriptions);

    QVERIFY(count > 0);
    // accentWeight should have increased
    QVERIFY(spec.colorScript.accentWeight > 0.10);
    QVERIFY(!descriptions.isEmpty());
}

void KisAiAbstractOntologyTest::testApplyMelancholy()
{
    KisAiSceneSpec spec;
    const int count = OntologyApplier::apply(QStringLiteral("melancholy portrait of a lone girl"),
                                            &spec,
                                            OntologyRuleset::defaultRules());

    QVERIFY(count > 0);
    // light.timeOfDay should be "night"
    QCOMPARE(spec.light.timeOfDay, QStringLiteral("night"));
}

void KisAiAbstractOntologyTest::testApplyJapaneseKeywords()
{
    KisAiSceneSpec spec;
    const int count = OntologyApplier::apply(QStringLiteral("鮮やかでエネルギッシュな街並み"),
                                            &spec,
                                            OntologyRuleset::defaultRules());

    QVERIFY(count > 0);
    QVERIFY(spec.colorScript.accentWeight > 0.0);
}

void KisAiAbstractOntologyTest::testMultipleRulesOnSameField()
{
    KisAiSceneSpec spec;
    spec.colorScript.accentWeight = 0.0;

    // "jazzy" と "vibrant" は同じ accentWeight を加算
    const int count = OntologyApplier::apply(QStringLiteral("jazzy and vibrant city"),
                                            &spec,
                                            OntologyRuleset::defaultRules());

    QVERIFY(count >= 1);
    QVERIFY(spec.colorScript.accentWeight > 0.0);
}

void KisAiAbstractOntologyTest::testNoMatchLeavesSpecUnchanged()
{
    KisAiSceneSpec spec;
    spec.light.timeOfDay = QStringLiteral("noon");
    const qreal origAccent = 0.25;
    spec.colorScript.accentWeight = origAccent;

    QStringList descriptions;
    const int count = OntologyApplier::apply(QStringLiteral("a plain standard cat sitting"),
                                            &spec,
                                            OntologyRuleset::defaultRules(),
                                            &descriptions);

    QCOMPARE(count, 0);
    QCOMPARE(spec.light.timeOfDay, QStringLiteral("noon"));
    QCOMPARE(spec.colorScript.accentWeight, origAccent);
    QVERIFY(descriptions.isEmpty());
}

void KisAiAbstractOntologyTest::testCaseInsensitiveMatching()
{
    KisAiSceneSpec spec1, spec2;
    OntologyApplier::apply(QStringLiteral("JAZZY VIBRANT"), &spec1);
    OntologyApplier::apply(QStringLiteral("jazzy vibrant"), &spec2);

    QCOMPARE(spec1.colorScript.accentWeight, spec2.colorScript.accentWeight);
}

void KisAiAbstractOntologyTest::testJsonRoundtrip()
{
    const OntologyRuleset original = OntologyRuleset::defaultRules();
    const QJsonArray arr = original.toJson();
    QVERIFY(!arr.isEmpty());

    const OntologyRuleset parsed = OntologyRuleset::fromJson(arr);
    QCOMPARE(parsed.rules.size(), original.rules.size());
    QCOMPARE(parsed.rules.first().specPath, original.rules.first().specPath);
}

void KisAiAbstractOntologyTest::testLoadCustomFromJsonArray()
{
    QJsonArray arr;
    QJsonObject customRule;
    customRule[QStringLiteral("triggerWords")] = QJsonArray{QStringLiteral("cyberpunk_neon")};
    customRule[QStringLiteral("specPath")] = QStringLiteral("light.timeOfDay");
    customRule[QStringLiteral("kind")] = QStringLiteral("SetString");
    customRule[QStringLiteral("stringValue")] = QStringLiteral("midnight");
    customRule[QStringLiteral("weight")] = 1.0;
    arr.append(customRule);

    const OntologyRuleset customRuleset = OntologyRuleset::fromJson(arr);
    QCOMPARE(customRuleset.rules.size(), 1);

    KisAiSceneSpec spec;
    const int count = OntologyApplier::apply(QStringLiteral("a cyberpunk_neon alley"), &spec, customRuleset);
    QCOMPARE(count, 1);
    QCOMPARE(spec.light.timeOfDay, QStringLiteral("midnight"));
}

void KisAiAbstractOntologyTest::testAppliedDescriptionsOutParam()
{
    KisAiSceneSpec spec;
    QStringList descs;
    OntologyApplier::apply(QStringLiteral("ethereal dreamy forest"), &spec, OntologyRuleset::defaultRules(), &descs);

    QVERIFY(!descs.isEmpty());
}

void KisAiAbstractOntologyTest::testWeightScaling()
{
    OntologyRuleset rSet;
    OntologyRule r1;
    r1.triggerWords = QStringList{QStringLiteral("boost")};
    r1.specPath = QStringLiteral("colorScript.accentWeight");
    r1.kind = OntologyRule::AddNumber;
    r1.numberValue = 0.50;
    r1.weight = 2.0;
    rSet.rules.append(r1);

    KisAiSceneSpec spec;
    spec.colorScript.accentWeight = 0.0;
    OntologyApplier::apply(QStringLiteral("boost colors"), &spec, rSet);
    QVERIFY(spec.colorScript.accentWeight > 0.0);
}

KISTEST_MAIN(KisAiAbstractOntologyTest)
