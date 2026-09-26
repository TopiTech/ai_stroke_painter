/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStartPageTest.h"
#include "aiillustration/KisAiPromptAnalyzer.h"
#include "aiillustration/KisAiStartPageWidget.h"
#include "utils/KisRecentDocumentsModelWrapper.h"

#include <QBoxLayout>
#include <QFrame>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QLabel>
#include <QListView>

#include "KisAiTestCrashGuard.h"
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) AI_STROKE_TEST_MAIN(TestClass)
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

void KisAiStartPageTest::testKeyboardFocusAndShortcuts()
{
    KisAiStartPageWidget widget(nullptr);
    widget.show();

    auto *promptInput = widget.findChild<QLineEdit *>(QStringLiteral("aiPromptOmnibarInput"));
    QVERIFY(promptInput != nullptr);

    // Test Ctrl+Alt+A focuses and selects the prompt input
    promptInput->setText(QStringLiteral("テストプロンプト"));
    promptInput->clearFocus();
    QVERIFY(widget.focusWidget() != promptInput);

    QKeyEvent ctrlAltA(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::sendEvent(&widget, &ctrlAltA);

    QCOMPARE(widget.focusWidget(), promptInput);
    QCOMPARE(promptInput->selectedText(), QStringLiteral("テストプロンプト"));

    // Test Ctrl+Return and Ctrl+Enter trigger slotQuickPromptGenerate without crash
    QKeyEvent ctrlReturn(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QCoreApplication::sendEvent(&widget, &ctrlReturn);

    QKeyEvent ctrlEnter(QEvent::KeyPress, Qt::Key_Enter, Qt::ControlModifier);
    QCoreApplication::sendEvent(&widget, &ctrlEnter);

    // Test Ctrl+N, Ctrl+O, and Ctrl+V shortcuts handle cleanly without main window
    QKeyEvent ctrlN(QEvent::KeyPress, Qt::Key_N, Qt::ControlModifier);
    QCoreApplication::sendEvent(&widget, &ctrlN);

    QKeyEvent ctrlO(QEvent::KeyPress, Qt::Key_O, Qt::ControlModifier);
    QCoreApplication::sendEvent(&widget, &ctrlO);

    QKeyEvent ctrlV(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    QCoreApplication::sendEvent(&widget, &ctrlV);

    // Verify all primary cards and presets have TabFocus policy for keyboard navigation
    const QList<QPushButton *> buttons = widget.findChildren<QPushButton *>();
    bool foundPasteCardWithShortcut = false;
    for (QPushButton *btn : buttons) {
        if (btn->isVisible()) {
            QVERIFY2(btn->focusPolicy() & Qt::TabFocus,
                     qPrintable(QStringLiteral("Button missing TabFocus: %1").arg(btn->accessibleName())));
            QVERIFY2(!btn->accessibleName().trimmed().isEmpty(),
                     qPrintable(QStringLiteral("Button missing accessibleName: %1").arg(btn->objectName())));
            if (btn->accessibleName().contains(QStringLiteral("Ctrl+V"))) {
                foundPasteCardWithShortcut = true;
            }
        }
    }
    QVERIFY(foundPasteCardWithShortcut);
}

void KisAiStartPageTest::testStartPageAutoFocusAndTabOrder()
{
    KisAiStartPageWidget widget(nullptr);
    widget.show();
    QCoreApplication::processEvents();

    auto *promptInput = widget.findChild<QLineEdit *>(QStringLiteral("aiPromptOmnibarInput"));
    auto *generateBtn = widget.findChild<QPushButton *>(QStringLiteral("aiPromptGenerateBtn"));
    QVERIFY(promptInput != nullptr);
    QVERIFY(generateBtn != nullptr);

    // Verify auto-focus on showEvent
    QCOMPARE(widget.focusWidget(), promptInput);

    // Verify tab order chain from promptInput to generateBtn
    QWidget *nextAfterPrompt = promptInput->nextInFocusChain();
    while (nextAfterPrompt && !(nextAfterPrompt->focusPolicy() & Qt::TabFocus)) {
        nextAfterPrompt = nextAfterPrompt->nextInFocusChain();
    }
    QCOMPARE(nextAfterPrompt, generateBtn);

    // Test responsive prompt bar direction: LeftToRight at >= 540px, TopToBottom at < 540px
    widget.resize(750, 700);
    QCoreApplication::processEvents();
    auto *promptBar = widget.findChild<QFrame *>(QStringLiteral("aiPromptOmnibarFrame"));
    QVERIFY(promptBar != nullptr);
    auto *promptLayout = qobject_cast<QBoxLayout *>(promptBar->layout());
    QVERIFY(promptLayout != nullptr);
    QCOMPARE(promptLayout->direction(), QBoxLayout::LeftToRight);

    widget.resize(320, 700);
    QCoreApplication::processEvents();
    QCOMPARE(promptLayout->direction(), QBoxLayout::TopToBottom);
}

void KisAiStartPageTest::testCardAndPresetHeightAndNonOverlapping()
{
    KisAiStartPageWidget widget(nullptr);
    widget.resize(1280, 800);
    widget.show();
    QCoreApplication::processEvents();

    int cardCount = 0;
    int presetCount = 0;

    for (QPushButton *btn : widget.findChildren<QPushButton *>()) {
        const QString className = btn->property("class").toString();
        if (className == QStringLiteral("aiCardButton")) {
            ++cardCount;
            QVERIFY2(btn->height() >= 60, qPrintable(QStringLiteral("aiCardButton height too small: %1").arg(btn->height())));
            auto *title = btn->findChild<QLabel *>(QStringLiteral("aiCardTitle"));
            auto *desc = btn->findChild<QLabel *>(QStringLiteral("aiCardDesc"));
            QVERIFY(title != nullptr);
            QVERIFY(desc != nullptr);
            QVERIFY(title->height() > 0);
            QVERIFY(desc->height() > 0);
            const QPoint titleBottom = title->mapTo(btn, QPoint(0, title->height()));
            const QPoint descTop = desc->mapTo(btn, QPoint(0, 0));
            QVERIFY2(titleBottom.y() <= descTop.y() + 1, "Card title and description are overlapping");
        } else if (className == QStringLiteral("aiPresetBtn")) {
            ++presetCount;
            QVERIFY2(btn->height() >= 55, qPrintable(QStringLiteral("aiPresetBtn height too small: %1").arg(btn->height())));
            auto *title = btn->findChild<QLabel *>(QStringLiteral("aiPresetTitle"));
            auto *desc = btn->findChild<QLabel *>(QStringLiteral("aiPresetDesc"));
            QVERIFY(title != nullptr);
            QVERIFY(desc != nullptr);
            QVERIFY(title->height() > 0);
            QVERIFY(desc->height() > 0);
            const QPoint titleBottom = title->mapTo(btn, QPoint(0, title->height()));
            const QPoint descTop = desc->mapTo(btn, QPoint(0, 0));
            QVERIFY2(titleBottom.y() <= descTop.y() + 1, "Preset title and description are overlapping");
        }
    }

    QCOMPARE(cardCount, 4);
    QCOMPARE(presetCount, 6);
}

KISTEST_MAIN(KisAiStartPageTest)
