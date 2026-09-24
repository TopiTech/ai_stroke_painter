/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStartPageTest.h"
#include "aiillustration/KisAiPromptAnalyzer.h"
#include "aiillustration/KisAiStartPageWidget.h"
#include "utils/KisRecentDocumentsModelWrapper.h"

#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
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

    // Preset text only (no canvas/docker side effects in unit tests).
    // Preset 0: Anime & Manga Line Art
    widget.slotApplyPreset(0);
    QVERIFY(!promptInput->text().isEmpty());
    QVERIFY(promptInput->text().contains(QStringLiteral("線画")));

    // Preset 1: Anime Cel-Shading
    widget.slotApplyPreset(1);
    QVERIFY(promptInput->text().contains(QStringLiteral("アニメ")));

    // Preset 2: Cyberpunk & Scifi
    widget.slotApplyPreset(2);
    QVERIFY(promptInput->text().contains(QStringLiteral("サイバーパンク")));

    // Preset 3: Watercolor & Atomic Ink
    widget.slotApplyPreset(3);
    QVERIFY(promptInput->text().contains(QStringLiteral("水彩")));

    // Preset 4: Vector Geometric Art
    widget.slotApplyPreset(4);
    QVERIFY(promptInput->text().contains(QStringLiteral("幾何学")));

    // Preset 5: Surprise Me!
    widget.slotApplyPreset(5);
    QVERIFY(!promptInput->text().isEmpty());
}

void KisAiStartPageTest::testPresetStyleMapping()
{
    QCOMPARE(static_cast<int>(KisAiPromptAnalyzer::ArtStyle::General), 0);
    QVERIFY(static_cast<int>(KisAiPromptAnalyzer::ArtStyle::AnimeCel) >= 1);
    QVERIFY(static_cast<int>(KisAiPromptAnalyzer::ArtStyle::Watercolor) >= 1);
    QVERIFY(static_cast<int>(KisAiPromptAnalyzer::ArtStyle::CyberNeon) >= 1);
    QVERIFY(static_cast<int>(KisAiPromptAnalyzer::ArtStyle::FineLineart) >= 1);
}

void KisAiStartPageTest::testQuickPromptEmptyDoesNotCrash()
{
    KisAiStartPageWidget widget(nullptr);
    auto *promptInput = widget.findChild<QLineEdit *>(QStringLiteral("aiPromptOmnibarInput"));
    QVERIFY(promptInput != nullptr);
    promptInput->clear();
    widget.slotQuickPromptGenerate();
    QVERIFY(promptInput->text().isEmpty());
}

void KisAiStartPageTest::testPasteEmptyClipboardDoesNotCrash()
{
    KisAiStartPageWidget widget(nullptr);
    widget.slotPasteFromClipboard();
    QVERIFY(true);
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
        const auto labels = btn->findChildren<QLabel *>();
        for (QLabel *lbl : labels) {
            if (lbl->text().contains(QStringLiteral("新しいキャンバス"))) {
                foundNewCanvas = true;
                QVERIFY(!btn->accessibleName().isEmpty());
            }
            if (lbl->text().contains(QStringLiteral("AI クイックキャンバス"))) {
                foundQuickCanvas = true;
                QVERIFY(!btn->accessibleName().isEmpty());
            }
            if (lbl->text().contains(QStringLiteral("画像・作品を開く"))) {
                foundOpenImage = true;
                QVERIFY(!btn->accessibleName().isEmpty());
            }
            if (lbl->text().contains(QStringLiteral("クリップボードから"))) {
                foundClipboard = true;
                QVERIFY(!btn->accessibleName().isEmpty());
            }
        }
    }

    QVERIFY(foundNewCanvas);
    QVERIFY(foundQuickCanvas);
    QVERIFY(foundOpenImage);
    QVERIFY(foundClipboard);
}

void KisAiStartPageTest::testResponsiveLayoutKeepsControlsWithinViewport()
{
    KisAiStartPageWidget widget(nullptr);
    widget.show();

    auto *scrollArea = widget.findChild<QScrollArea *>(QStringLiteral("aiStartScrollArea"));
    auto *promptInput = widget.findChild<QLineEdit *>(QStringLiteral("aiPromptOmnibarInput"));
    auto *generateButton = widget.findChild<QPushButton *>(QStringLiteral("aiPromptGenerateBtn"));
    QVERIFY(scrollArea);
    QVERIFY(promptInput);
    QVERIFY(generateButton);

    for (const int width : {320, 750, 1200, 750, 320}) {
        widget.resize(width, 700);
        QCoreApplication::processEvents();

        QCOMPARE(scrollArea->horizontalScrollBar()->maximum(), 0);
        const QRect viewportRect(scrollArea->viewport()->mapTo(&widget, QPoint()), scrollArea->viewport()->size());
        for (QPushButton *button : widget.findChildren<QPushButton *>()) {
            if (button->isHidden()) {
                continue;
            }
            const QRect buttonRect(button->mapTo(&widget, QPoint()), button->size());
            QVERIFY2(viewportRect.left() <= buttonRect.left(), qPrintable(button->accessibleName()));
            QVERIFY2(buttonRect.right() <= viewportRect.right(), qPrintable(button->accessibleName()));
        }
        const QRect promptRect(promptInput->mapTo(&widget, QPoint()), promptInput->size());
        const QRect generateRect(generateButton->mapTo(&widget, QPoint()), generateButton->size());
        QVERIFY(viewportRect.left() <= promptRect.left());
        QVERIFY(promptRect.right() <= viewportRect.right());
        QVERIFY(viewportRect.left() <= generateRect.left());
        QVERIFY(generateRect.right() <= viewportRect.right());
    }
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

void KisAiStartPageTest::testRecentDocumentsModelSignalConnection()
{
    KisAiStartPageWidget widget(nullptr);
    KisRecentDocumentsModelWrapper *modelWrapper = KisRecentDocumentsModelWrapper::instance();
    QVERIFY(modelWrapper != nullptr);

    // Emitting the signal must refresh without error
    Q_EMIT modelWrapper->sigModelIsUpToDate();
    QCoreApplication::processEvents();

    auto *recentListView = widget.findChild<QListView *>(QStringLiteral("aiRecentListView"));
    QVERIFY(recentListView != nullptr);
    QVERIFY(recentListView->model() != nullptr);
}

KISTEST_MAIN(KisAiStartPageTest)
