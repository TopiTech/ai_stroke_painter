/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStartPageTest.h"
#include "aiillustration/KisAiStartPageWidget.h"

#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QLabel>
#include <QListView>

#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

using namespace QTest;

void KisAiStartPageTest::testWidgetInstantiation()
{
    KisAiStartPageWidget widget(nullptr);
    widget.resize(1024, 768);

    auto *scrollArea = widget.findChild<QScrollArea *>(QStringLiteral("aiStartScrollArea"));
    QVERIFY(scrollArea != nullptr);

    auto *heroTitle = widget.findChild<QLabel *>(QStringLiteral("aiHeroTitle"));
    QVERIFY(heroTitle != nullptr);
    QCOMPARE(heroTitle->text(), QStringLiteral("AI Stroke Painter"));

    auto *promptInput = widget.findChild<QLineEdit *>(QStringLiteral("aiPromptOmnibarInput"));
    QVERIFY(promptInput != nullptr);
    QVERIFY(!promptInput->placeholderText().isEmpty());

    auto *generateBtn = widget.findChild<QPushButton *>(QStringLiteral("aiPromptGenerateBtn"));
    QVERIFY(generateBtn != nullptr);
}

void KisAiStartPageTest::testPresetPromptApplication()
{
    KisAiStartPageWidget widget(nullptr);

    auto *promptInput = widget.findChild<QLineEdit *>(QStringLiteral("aiPromptOmnibarInput"));
    QVERIFY(promptInput != nullptr);

    // Apply preset 0 (Anime)
    widget.slotApplyPreset(0);
    QVERIFY(!promptInput->text().isEmpty());
    QVERIFY(promptInput->text().contains(QStringLiteral("アニメ")));

    // Apply preset 1 (Cyberpunk)
    widget.slotApplyPreset(1);
    QVERIFY(promptInput->text().contains(QStringLiteral("サイバーパンク")));

    // Apply preset 2 (Watercolor)
    widget.slotApplyPreset(2);
    QVERIFY(promptInput->text().contains(QStringLiteral("水彩")));

    // Apply preset 3 (Vector Geometry)
    widget.slotApplyPreset(3);
    QVERIFY(promptInput->text().contains(QStringLiteral("幾何学")));

    // Apply preset 4 (Surprise Me)
    widget.slotApplyPreset(4);
    QVERIFY(!promptInput->text().isEmpty());
}

void KisAiStartPageTest::testActionCardLayout()
{
    KisAiStartPageWidget widget(nullptr);

    // Verify action cards are present
    const QList<QPushButton *> cardButtons = widget.findChildren<QPushButton *>();
    QVERIFY(cardButtons.size() >= 4);

    bool foundNewCanvas = false;
    bool foundQuickCanvas = false;
    bool foundOpenImage = false;
    bool foundClipboard = false;

    for (QPushButton *btn : cardButtons) {
        const QString text = btn->text();
        const auto labels = btn->findChildren<QLabel *>();
        for (QLabel *lbl : labels) {
            if (lbl->text().contains(QStringLiteral("新しいキャンバス"))) foundNewCanvas = true;
            if (lbl->text().contains(QStringLiteral("AI クイックキャンバス"))) foundQuickCanvas = true;
            if (lbl->text().contains(QStringLiteral("画像・作品を開く"))) foundOpenImage = true;
            if (lbl->text().contains(QStringLiteral("クリップボードから"))) foundClipboard = true;
        }
    }

    QVERIFY(foundNewCanvas);
    QVERIFY(foundQuickCanvas);
    QVERIFY(foundOpenImage);
    QVERIFY(foundClipboard);
}

void KisAiStartPageTest::testRecentStackEmptyState()
{
    KisAiStartPageWidget widget(nullptr);

    auto *recentListView = widget.findChild<QListView *>(QStringLiteral("aiRecentListView"));
    QVERIFY(recentListView != nullptr);

    auto *emptyLabel = widget.findChild<QLabel *>(QStringLiteral("aiEmptyStateLabel"));
    QVERIFY(emptyLabel != nullptr);
    QVERIFY(emptyLabel->text().contains(QStringLiteral("最近開いた作品はありません")));
}

KISTEST_MAIN(KisAiStartPageTest)
