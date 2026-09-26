/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_START_PAGE_TEST_H
#define KIS_AI_START_PAGE_TEST_H

#include <QObject>

class KisAiStartPageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testWidgetInstantiation();
    void testPresetPromptApplication();
    void testPresetStyleMapping();
    void testActionCardLayout();
    void testResponsiveLayoutKeepsControlsWithinViewport();
    void testRecentStackEmptyState();
    void testQuickPromptEmptyDoesNotCrash();
    void testPasteEmptyClipboardDoesNotCrash();
    void testRecentDocumentsModelSignalConnection();
    void testKeyboardFocusAndShortcuts();
    void testStartPageAutoFocusAndTabOrder();
    void testCardAndPresetHeightAndNonOverlapping();
};

#endif // KIS_AI_START_PAGE_TEST_H
