/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ABSTRACT_ONTOLOGY_TEST_H
#define KIS_AI_ABSTRACT_ONTOLOGY_TEST_H

#include <QObject>

class KisAiAbstractOntologyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testDefaultRulesCount();
    void testRuleCategoriesCovered();
    void testApplyJazzyVibrant();
    void testApplyMelancholy();
    void testApplyJapaneseKeywords();
    void testMultipleRulesOnSameField();
    void testNoMatchLeavesSpecUnchanged();
    void testCaseInsensitiveMatching();
    void testJsonRoundtrip();
    void testLoadCustomFromJsonArray();
    void testLoadCustomRejectsUnsafePath();
    void testAppliedDescriptionsOutParam();
    void testWeightScaling();
    void testWordBoundaryMatching();
    void testDefaultSpecForPromptAppliesOntology();
};

#endif // KIS_AI_ABSTRACT_ONTOLOGY_TEST_H
