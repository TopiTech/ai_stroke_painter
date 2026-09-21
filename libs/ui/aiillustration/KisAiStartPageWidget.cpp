/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStartPageWidget.h"
#include "KisAiIllustrationDocker.h"
#include "KisAiPromptAnalyzer.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisDocument.h"
#include "kis_config.h"
#include "kis_clipboard.h"
#include "dialogs/KisDlgCreateNewDocument.h"
#include "utils/KisRecentDocumentsModelWrapper.h"

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <klocalizedstring.h>

#include <QBoxLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

KisAiStartPageWidget::KisAiStartPageWidget(KisMainWindow *mainWindow, QWidget *parent)
    : QWidget(parent)
    , m_mainWindow(mainWindow)
{
    setObjectName(QStringLiteral("KisAiStartPageWidget"));
    setAttribute(Qt::WA_StyledBackground, true);

    setStyleSheet(QStringLiteral(
        "QWidget#KisAiStartPageWidget { background: qlineargradient(x1:0, y1:0, x2:0.8, y2:1, stop:0 #0d121f, stop:0.5 #111827, stop:1 #0b0f19); color: #edf3ff; }"
        "QScrollArea#aiStartScrollArea { background: transparent; border: none; }"
        "QWidget#aiStartContent { background: transparent; }"

        /* Hero Banner */
        "QLabel#aiHeroTitle { color: #ffffff; font-size: 30px; font-weight: 800; letter-spacing: 0.5px; }"
        "QLabel#aiHeroSubtitle { color: #94a8c9; font-size: 14px; font-weight: 400; }"
        "QLabel#aiStatusBadge { background: rgba(30, 41, 59, 0.85); color: #7dd3fc; border: 1px solid rgba(56, 189, 248, 0.35); border-radius: 12px; font-size: 11px; font-weight: 600; padding: 4px 10px; }"
        "QLabel#aiStatusBadgeGreen { background: rgba(16, 40, 32, 0.85); color: #6ee7b7; border: 1px solid rgba(52, 211, 153, 0.4); border-radius: 12px; font-size: 11px; font-weight: 600; padding: 4px 10px; }"

        /* Omnibar Prompt Container */
        "QFrame#aiPromptOmnibarFrame { background: rgba(22, 30, 49, 0.9); border: 1px solid rgba(75, 110, 175, 0.4); border-radius: 12px; padding: 6px 10px; }"
        "QFrame#aiPromptOmnibarFrame:hover { border: 1px solid rgba(96, 165, 250, 0.7); background: rgba(26, 36, 58, 0.95); }"
        "QLineEdit#aiPromptOmnibarInput { background: transparent; border: none; color: #f8fafc; font-size: 14px; padding: 6px 8px; }"
        "QLineEdit#aiPromptOmnibarInput:focus { outline: none; }"
        "QPushButton#aiPromptGenerateBtn { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:1 #6366f1); color: #ffffff; border: none; border-radius: 8px; font-size: 13px; font-weight: 700; padding: 8px 18px; }"
        "QPushButton#aiPromptGenerateBtn:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #60a5fa, stop:1 #818cf8); }"
        "QPushButton#aiPromptGenerateBtn:pressed { background: #2563eb; }"

        /* Cards Base */
        "QFrame.aiActionCard { background: rgba(20, 27, 43, 0.75); border: 1px solid rgba(51, 65, 85, 0.6); border-radius: 12px; }"
        "QFrame.aiActionCard:hover { background: rgba(28, 38, 60, 0.9); border: 1px solid rgba(99, 102, 241, 0.55); }"
        "QLabel#aiCardTitle { color: #f1f5f9; font-size: 14px; font-weight: 700; }"
        "QLabel#aiCardDesc { color: #94a3b8; font-size: 12px; line-height: 1.3; }"
        "QLabel#aiCardShortcut { background: rgba(15, 23, 42, 0.8); color: #64748b; border: 1px solid rgba(51, 65, 85, 0.5); border-radius: 4px; font-size: 10px; font-weight: 600; padding: 2px 6px; }"

        /* Clickable Card Buttons */
        "QPushButton.aiCardButton { text-align: left; background: rgba(20, 27, 43, 0.75); border: 1px solid rgba(51, 65, 85, 0.6); border-radius: 10px; padding: 12px 14px; color: #edf3ff; }"
        "QPushButton.aiCardButton:hover { background: rgba(30, 42, 66, 0.92); border: 1px solid rgba(99, 130, 240, 0.65); }"
        "QPushButton.aiCardButton:pressed { background: rgba(15, 20, 32, 0.95); border-color: #3b82f6; }"

        /* Presets */
        "QPushButton.aiPresetBtn { text-align: left; background: rgba(22, 30, 48, 0.7); border: 1px solid rgba(45, 60, 85, 0.5); border-radius: 10px; padding: 10px 14px; }"
        "QPushButton.aiPresetBtn:hover { background: rgba(33, 46, 74, 0.85); border: 1px solid rgba(129, 140, 248, 0.6); }"
        "QPushButton.aiPresetBtn:pressed { background: rgba(18, 24, 38, 0.95); }"

        /* Section Titles */
        "QLabel#aiSectionTitle { color: #cbd5e1; font-size: 13px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.8px; }"

        /* Panels (Recent / Guide) */
        "QFrame#aiPanelFrame { background: rgba(17, 24, 39, 0.65); border: 1px solid rgba(40, 53, 75, 0.6); border-radius: 12px; padding: 16px; }"
        "QListView#aiRecentListView { background: transparent; border: none; color: #e2e8f0; }"
        "QListView#aiRecentListView::item { padding: 6px 8px; border-radius: 6px; }"
        "QListView#aiRecentListView::item:hover { background: rgba(45, 62, 95, 0.5); }"
        "QListView#aiRecentListView::item:selected { background: rgba(59, 130, 246, 0.4); color: #ffffff; }"

        "QLabel#aiEmptyStateLabel { color: #64748b; font-size: 12px; }"
        "QPushButton#aiClearRecentBtn { background: transparent; color: #64748b; border: none; font-size: 11px; text-decoration: underline; }"
        "QPushButton#aiClearRecentBtn:hover { color: #94a3b8; }"

        /* Guide text */
        "QLabel#aiGuideHeading { color: #93c5fd; font-size: 12px; font-weight: 600; }"
        "QLabel#aiGuideBody { color: #889bb8; font-size: 11px; line-height: 1.4; }"
        "QLabel#aiShortcutKey { background: rgba(30, 41, 59, 0.9); color: #e2e8f0; border: 1px solid #334155; border-radius: 4px; font-family: monospace; font-size: 10px; padding: 2px 6px; }"
        "QLabel#aiShortcutDesc { color: #94a3b8; font-size: 11px; }"
    ));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("aiStartScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto *contentWidget = new QWidget(scrollArea);
    contentWidget->setObjectName(QStringLiteral("aiStartContent"));

    auto *contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(40, 36, 40, 40);
    contentLayout->setSpacing(24);

    // 1. Hero Brand Section
    contentLayout->addWidget(createHeroSection());

    // 2. Omnibar Prompt Section
    contentLayout->addWidget(createPromptBarSection());

    // 3. Primary Actions Cards
    contentLayout->addWidget(createPrimaryActionsSection());

    // 4. Creative Style Presets
    contentLayout->addWidget(createPresetsSection());

    // 5. Recent Artworks & Guide (2 Columns)
    contentLayout->addWidget(createRecentAndGuideSection());

    contentLayout->addStretch(1);

    scrollArea->setWidget(contentWidget);
    mainLayout->addWidget(scrollArea);

    // Initialize recent files model
    slotUpdateRecentFiles();
}

KisAiStartPageWidget::~KisAiStartPageWidget() = default;

KisAiIllustrationDocker *KisAiStartPageWidget::aiDocker() const
{
    if (!m_mainWindow) return nullptr;
    if (QDockWidget *docker = m_mainWindow->findChild<QDockWidget *>(QStringLiteral("AiIllustrationDocker"))) {
        return dynamic_cast<KisAiIllustrationDocker *>(docker);
    }
    return nullptr;
}

QWidget *KisAiStartPageWidget::createHeroSection()
{
    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(14);

    auto *title = new QLabel(i18n("AI Stroke Painter"), container);
    title->setObjectName(QStringLiteral("aiHeroTitle"));
    topRow->addWidget(title);

    // Status Badges
    auto *badgeAi = new QLabel(i18n("🟢 AI Engine Active"), container);
    badgeAi->setObjectName(QStringLiteral("aiStatusBadgeGreen"));
    topRow->addWidget(badgeAi);

    auto *badgePipeline = new QLabel(i18n("⚡ V10 Atomic Ink"), container);
    badgePipeline->setObjectName(QStringLiteral("aiStatusBadge"));
    topRow->addWidget(badgePipeline);

    auto *badgeHdr = new QLabel(i18n("🎨 Linear HDR 32-bit"), container);
    badgeHdr->setObjectName(QStringLiteral("aiStatusBadge"));
    topRow->addWidget(badgeHdr);

    topRow->addStretch(1);

    // Quick focus button
    auto *focusDockerBtn = new QPushButton(i18n("AI ワークスペースを表示"), container);
    focusDockerBtn->setCursor(Qt::PointingHandCursor);
    focusDockerBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: rgba(30, 41, 59, 0.8); color: #cbd5e1; border: 1px solid rgba(71, 85, 105, 0.5); border-radius: 6px; font-size: 12px; font-weight: 500; padding: 6px 14px; }"
        "QPushButton:hover { background: rgba(51, 65, 85, 0.9); color: #f8fafc; border-color: #64748b; }"));
    connect(focusDockerBtn, &QPushButton::clicked, this, &KisAiStartPageWidget::slotFocusAiDocker);
    topRow->addWidget(focusDockerBtn);

    layout->addLayout(topRow);

    auto *subtitle = new QLabel(i18n("自律ベクターストローク & 物理インクシミュレーションによる次世代AIクリエイティブ環境"), container);
    subtitle->setObjectName(QStringLiteral("aiHeroSubtitle"));
    layout->addWidget(subtitle);

    return container;
}

QWidget *KisAiStartPageWidget::createPromptBarSection()
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("aiPromptOmnibarFrame"));

    auto *layout = new QHBoxLayout(frame);
    layout->setContentsMargins(10, 6, 8, 6);
    layout->setSpacing(10);

    auto *iconLabel = new QLabel(QStringLiteral("✨"), frame);
    iconLabel->setStyleSheet(QStringLiteral("font-size: 16px;"));
    iconLabel->setAccessibleName(QString());
    layout->addWidget(iconLabel);

    m_promptInput = new QLineEdit(frame);
    m_promptInput->setObjectName(QStringLiteral("aiPromptOmnibarInput"));
    m_promptInput->setPlaceholderText(i18n("描きたいイラストの指示（プロンプト）を入力... (例: 月夜に佇む銀髪の魔法使いのアニメ調イラスト)"));
    m_promptInput->setAccessibleName(i18n("イラストのプロンプト入力"));
    m_promptInput->setAccessibleDescription(i18n("生成したいイラストの指示を入力します。Enter で生成を開始します。"));
    m_promptInput->installEventFilter(this);
    connect(m_promptInput, &QLineEdit::returnPressed, this, &KisAiStartPageWidget::slotQuickPromptGenerate);
    layout->addWidget(m_promptInput, 1);

    m_promptSubmitBtn = new QPushButton(i18n("⚡ 生成して開く"), frame);
    m_promptSubmitBtn->setObjectName(QStringLiteral("aiPromptGenerateBtn"));
    m_promptSubmitBtn->setCursor(Qt::PointingHandCursor);
    m_promptSubmitBtn->setToolTip(i18n("キャンバスを作成し、プロンプトを流し込んでAI生成を開始します (Enter)"));
    connect(m_promptSubmitBtn, &QPushButton::clicked, this, &KisAiStartPageWidget::slotQuickPromptGenerate);
    layout->addWidget(m_promptSubmitBtn);

    return frame;
}

QWidget *KisAiStartPageWidget::createPrimaryActionsSection()
{
    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *label = new QLabel(i18n("クイックスタート"), container);
    label->setObjectName(QStringLiteral("aiSectionTitle"));
    layout->addWidget(label);

    auto *cardsLayout = new QHBoxLayout();
    cardsLayout->setSpacing(14);

    // 1. New Canvas Button
    auto *newBtn = new QPushButton(container);
    newBtn->setProperty("class", "aiCardButton");
    newBtn->setCursor(Qt::PointingHandCursor);
    auto *newLayout = new QVBoxLayout(newBtn);
    newLayout->setSpacing(4);
    auto *newTop = new QHBoxLayout();
    auto *newIcon = new QLabel(QStringLiteral("🎨"), newBtn);
    newIcon->setStyleSheet(QStringLiteral("font-size: 20px;"));
    auto *newTitle = new QLabel(i18n("新しいキャンバス"), newBtn);
    newTitle->setObjectName(QStringLiteral("aiCardTitle"));
    newTop->addWidget(newIcon);
    newTop->addWidget(newTitle);
    newTop->addStretch(1);
    auto *newShortcut = new QLabel(QStringLiteral("Ctrl+N"), newBtn);
    newShortcut->setObjectName(QStringLiteral("aiCardShortcut"));
    newTop->addWidget(newShortcut);
    newLayout->addLayout(newTop);
    auto *newDesc = new QLabel(i18n("サイズや色空間を指定して作成"), newBtn);
    newDesc->setObjectName(QStringLiteral("aiCardDesc"));
    newLayout->addWidget(newDesc);
    connect(newBtn, &QPushButton::clicked, this, &KisAiStartPageWidget::slotNewFile);
    cardsLayout->addWidget(newBtn, 1);

    // 2. Quick 1024x1024 AI Canvas
    auto *quickBtn = new QPushButton(container);
    quickBtn->setProperty("class", "aiCardButton");
    quickBtn->setCursor(Qt::PointingHandCursor);
    auto *quickLayout = new QVBoxLayout(quickBtn);
    quickLayout->setSpacing(4);
    auto *quickTop = new QHBoxLayout();
    auto *quickIcon = new QLabel(QStringLiteral("⚡"), quickBtn);
    quickIcon->setStyleSheet(QStringLiteral("font-size: 20px;"));
    auto *quickTitle = new QLabel(i18n("AI クイックキャンバス"), quickBtn);
    quickTitle->setObjectName(QStringLiteral("aiCardTitle"));
    quickTop->addWidget(quickIcon);
    quickTop->addWidget(quickTitle);
    quickTop->addStretch(1);
    auto *quickBadge = new QLabel(QStringLiteral("1024x1024"), quickBtn);
    quickBadge->setObjectName(QStringLiteral("aiCardShortcut"));
    quickTop->addWidget(quickBadge);
    quickLayout->addLayout(quickTop);
    auto *quickDesc = new QLabel(i18n("最適解像度で即座にキャンバスを展開"), quickBtn);
    quickDesc->setObjectName(QStringLiteral("aiCardDesc"));
    quickLayout->addWidget(quickDesc);
    connect(quickBtn, &QPushButton::clicked, this, &KisAiStartPageWidget::slotQuickCanvas1024);
    cardsLayout->addWidget(quickBtn, 1);

    // 3. Open Image / Project
    auto *openBtn = new QPushButton(container);
    openBtn->setProperty("class", "aiCardButton");
    openBtn->setCursor(Qt::PointingHandCursor);
    auto *openLayout = new QVBoxLayout(openBtn);
    openLayout->setSpacing(4);
    auto *openTop = new QHBoxLayout();
    auto *openIcon = new QLabel(QStringLiteral("📂"), openBtn);
    openIcon->setStyleSheet(QStringLiteral("font-size: 20px;"));
    auto *openTitle = new QLabel(i18n("画像・作品を開く"), openBtn);
    openTitle->setObjectName(QStringLiteral("aiCardTitle"));
    openTop->addWidget(openIcon);
    openTop->addWidget(openTitle);
    openTop->addStretch(1);
    auto *openShortcut = new QLabel(QStringLiteral("Ctrl+O"), openBtn);
    openShortcut->setObjectName(QStringLiteral("aiCardShortcut"));
    openTop->addWidget(openShortcut);
    openLayout->addLayout(openTop);
    auto *openDesc = new QLabel(i18n("既存のKRA、PNG、PSDなどを開く"), openBtn);
    openDesc->setObjectName(QStringLiteral("aiCardDesc"));
    openLayout->addWidget(openDesc);
    connect(openBtn, &QPushButton::clicked, this, &KisAiStartPageWidget::slotOpenFile);
    cardsLayout->addWidget(openBtn, 1);

    // 4. Paste from Clipboard
    auto *pasteBtn = new QPushButton(container);
    pasteBtn->setProperty("class", "aiCardButton");
    pasteBtn->setCursor(Qt::PointingHandCursor);
    auto *pasteLayout = new QVBoxLayout(pasteBtn);
    pasteLayout->setSpacing(4);
    auto *pasteTop = new QHBoxLayout();
    auto *pasteIcon = new QLabel(QStringLiteral("📋"), pasteBtn);
    pasteIcon->setStyleSheet(QStringLiteral("font-size: 20px;"));
    auto *pasteTitle = new QLabel(i18n("クリップボードから"), pasteBtn);
    pasteTitle->setObjectName(QStringLiteral("aiCardTitle"));
    pasteTop->addWidget(pasteIcon);
    pasteTop->addWidget(pasteTitle);
    pasteTop->addStretch(1);
    auto *pasteShortcut = new QLabel(QStringLiteral("Ctrl+V"), pasteBtn);
    pasteShortcut->setObjectName(QStringLiteral("aiCardShortcut"));
    pasteTop->addWidget(pasteShortcut);
    pasteLayout->addLayout(pasteTop);
    auto *pasteDesc = new QLabel(i18n("コピーした画像を新規キャンバス化"), pasteBtn);
    pasteDesc->setObjectName(QStringLiteral("aiCardDesc"));
    pasteLayout->addWidget(pasteDesc);
    connect(pasteBtn, &QPushButton::clicked, this, &KisAiStartPageWidget::slotPasteFromClipboard);
    cardsLayout->addWidget(pasteBtn, 1);

    layout->addLayout(cardsLayout);
    return container;
}

QWidget *KisAiStartPageWidget::createPresetsSection()
{
    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *label = new QLabel(i18n("クリエイティブスタイル プリセット (ワンクリック開始)"), container);
    label->setObjectName(QStringLiteral("aiSectionTitle"));
    layout->addWidget(label);

    auto *grid = new QGridLayout();
    grid->setSpacing(12);

    struct PresetInfo {
        int id;
        QString icon;
        QString title;
        QString desc;
    };

    const QList<PresetInfo> presets = {
        {0, QStringLiteral("🖋️"), i18n("Anime & Manga Line Art"), i18n("彩色なし・高精細ペンタッチの純粋なインク線画と塗り絵")},
        {1, QStringLiteral("🌸"), i18n("Anime Cel-Shading"), i18n("繊細なベクター主線と鮮やかなセルアニメ調着彩")},
        {2, QStringLiteral("🌆"), i18n("Cyberpunk & Scifi"), i18n("ネオンライトと大気感のあるコンセプトアート")},
        {3, QStringLiteral("🖌️"), i18n("Watercolor & Atomic Ink"), i18n("物理シミュレーションによる滲み・水彩タッチ")},
        {4, QStringLiteral("📐"), i18n("Vector Geometric Art"), i18n("精密な幾何学パスとストロークグラフ描写")},
        {5, QStringLiteral("🎲"), i18n("Surprise Me! (お題生成)"), i18n("ランダムなインスピレーションプロンプトで即開始")},
    };

    for (int i = 0; i < presets.size(); ++i) {
        const auto &p = presets[i];
        auto *btn = new QPushButton(container);
        btn->setProperty("class", "aiPresetBtn");
        btn->setCursor(Qt::PointingHandCursor);

        auto *bLayout = new QHBoxLayout(btn);
        bLayout->setContentsMargins(12, 10, 12, 10);
        bLayout->setSpacing(12);

        auto *icon = new QLabel(p.icon, btn);
        icon->setStyleSheet(QStringLiteral("font-size: 22px;"));
        bLayout->addWidget(icon);

        auto *textLayout = new QVBoxLayout();
        textLayout->setSpacing(2);
        auto *title = new QLabel(p.title, btn);
        title->setStyleSheet(QStringLiteral("color: #f1f5f9; font-size: 13px; font-weight: 700;"));
        auto *desc = new QLabel(p.desc, btn);
        desc->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
        textLayout->addWidget(title);
        textLayout->addWidget(desc);
        bLayout->addLayout(textLayout, 1);

        auto *arrow = new QLabel(QStringLiteral("➔"), btn);
        arrow->setStyleSheet(QStringLiteral("color: #6366f1; font-weight: bold; font-size: 14px;"));
        bLayout->addWidget(arrow);

        const int presetId = p.id;
        connect(btn, &QPushButton::clicked, this, [this, presetId] {
            slotApplyPreset(presetId);
        });

        // 2 items per row
        grid->addWidget(btn, i / 2, i % 2);
    }

    layout->addLayout(grid);
    return container;
}

QWidget *KisAiStartPageWidget::createRecentAndGuideSection()
{
    auto *container = new QWidget(this);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    // Left Column: Recent Artworks
    auto *recentPanel = new QFrame(container);
    recentPanel->setObjectName(QStringLiteral("aiPanelFrame"));
    auto *recentLayout = new QVBoxLayout(recentPanel);
    recentLayout->setContentsMargins(16, 14, 16, 14);
    recentLayout->setSpacing(10);

    auto *recentHeader = new QHBoxLayout();
    auto *recentTitle = new QLabel(i18n("最近の作品"), recentPanel);
    recentTitle->setObjectName(QStringLiteral("aiSectionTitle"));
    recentHeader->addWidget(recentTitle);
    recentHeader->addStretch(1);

    m_clearRecentBtn = new QPushButton(i18n("履歴をクリア"), recentPanel);
    m_clearRecentBtn->setObjectName(QStringLiteral("aiClearRecentBtn"));
    m_clearRecentBtn->setCursor(Qt::PointingHandCursor);
    connect(m_clearRecentBtn, &QPushButton::clicked, this, &KisAiStartPageWidget::slotClearRecentFiles);
    recentHeader->addWidget(m_clearRecentBtn);
    recentLayout->addLayout(recentHeader);

    m_recentStack = new QStackedWidget(recentPanel);

    // Empty state
    m_emptyRecentLabel = new QLabel(i18n("最近開いた作品はありません。\n新規キャンバスまたはプロンプト入力からアートを作成しましょう。"), m_recentStack);
    m_emptyRecentLabel->setObjectName(QStringLiteral("aiEmptyStateLabel"));
    m_emptyRecentLabel->setAlignment(Qt::AlignCenter);
    m_recentStack->addWidget(m_emptyRecentLabel);

    // List view
    m_recentListView = new QListView(m_recentStack);
    m_recentListView->setObjectName(QStringLiteral("aiRecentListView"));
    m_recentListView->setUniformItemSizes(true);
    m_recentListView->setIconSize(QSize(36, 36));
    m_recentListView->setSpacing(4);
    m_recentListView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentListView->setAccessibleName(i18n("最近開いた作品の一覧"));
    m_recentListView->setAccessibleDescription(i18n("Enter キーでも作品を開けます。"));
    connect(m_recentListView, &QListView::clicked, this, &KisAiStartPageWidget::slotRecentDocumentClicked);
    connect(m_recentListView, &QListView::activated, this, &KisAiStartPageWidget::slotRecentDocumentClicked);
    m_recentStack->addWidget(m_recentListView);

    recentLayout->addWidget(m_recentStack, 1);
    layout->addWidget(recentPanel, 1);

    // Right Column: Tips & Keyboard Shortcuts
    auto *guidePanel = new QFrame(container);
    guidePanel->setObjectName(QStringLiteral("aiPanelFrame"));
    auto *guideLayout = new QVBoxLayout(guidePanel);
    guideLayout->setContentsMargins(16, 14, 16, 14);
    guideLayout->setSpacing(12);

    auto *guideTitle = new QLabel(i18n("AI クリエイティブガイド & ショートカット"), guidePanel);
    guideTitle->setObjectName(QStringLiteral("aiSectionTitle"));
    guideLayout->addWidget(guideTitle);

    // Tip 1
    auto *tip1 = new QVBoxLayout();
    tip1->setSpacing(2);
    auto *tip1H = new QLabel(i18n("💡 ストローク予算（Stroke Budget）の活用"), guidePanel);
    tip1H->setObjectName(QStringLiteral("aiGuideHeading"));
    auto *tip1B = new QLabel(i18n("AIワークスペースのストローク数を増やすと、物理インクや陰影のディテールが精細になります。"), guidePanel);
    tip1B->setObjectName(QStringLiteral("aiGuideBody"));
    tip1B->setWordWrap(true);
    tip1->addWidget(tip1H);
    tip1->addWidget(tip1B);
    guideLayout->addLayout(tip1);

    // Tip 2
    auto *tip2 = new QVBoxLayout();
    tip2->setSpacing(2);
    auto *tip2H = new QLabel(i18n("🔄 自律 Goal Mode"), guidePanel);
    tip2H->setObjectName(QStringLiteral("aiGuideHeading"));
    auto *tip2B = new QLabel(i18n("Goal ModeをONにすると、構図→線画→ベース着彩→光沢陰影をAIが自律的かつ段階的に描き上げます。"), guidePanel);
    tip2B->setObjectName(QStringLiteral("aiGuideBody"));
    tip2B->setWordWrap(true);
    tip2->addWidget(tip2H);
    tip2->addWidget(tip2B);
    guideLayout->addLayout(tip2);

    // Shortcuts Grid
    auto *shortcutGrid = new QGridLayout();
    shortcutGrid->setSpacing(6);

    const struct {
        QString key;
        QString desc;
    } shortcuts[] = {
        {QStringLiteral("Ctrl + Alt + A"), i18n("AIプロンプト入力にフォーカス")},
        {QStringLiteral("Ctrl + Enter"), i18n("プロンプトから作画を開始")},
        {QStringLiteral("Ctrl + N"), i18n("新規キャンバスダイアログ")},
        {QStringLiteral("Ctrl + O"), i18n("ファイルを開く")},
        {QStringLiteral("Space + ドラッグ"), i18n("キャンバスパン移動")},
    };

    for (int i = 0; i < 5; ++i) {
        auto *k = new QLabel(shortcuts[i].key, guidePanel);
        k->setObjectName(QStringLiteral("aiShortcutKey"));
        auto *d = new QLabel(shortcuts[i].desc, guidePanel);
        d->setObjectName(QStringLiteral("aiShortcutDesc"));
        shortcutGrid->addWidget(k, i, 0);
        shortcutGrid->addWidget(d, i, 1);
    }
    guideLayout->addLayout(shortcutGrid);

    guideLayout->addStretch(1);
    layout->addWidget(guidePanel, 1);

    return container;
}

bool KisAiStartPageWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_promptInput && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            slotQuickPromptGenerate();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void KisAiStartPageWidget::slotQuickPromptGenerate()
{
    const QString prompt = m_promptInput ? m_promptInput->text().trimmed() : QString();
    if (prompt.isEmpty()) {
        if (m_promptInput) {
            m_promptInput->setFocus();
        }
        return;
    }

    // Ensure canvas exists
    if (m_mainWindow && !m_mainWindow->activeView()) {
        KisDocument *document = KisPart::instance()->createDocument();
        if (document) {
            const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
            const KoColor background(QColor(QStringLiteral("#f4f7ff")), colorSpace);
            const QString name = i18n("AI Illustration");
            if (document->newImage(name, 1024, 1024, colorSpace, background, KisConfig::RASTER_LAYER, 1, QString(), 1.0)) {
                document->setObjectName(name);
                KisPart::instance()->addDocument(document);
                m_mainWindow->showDocument(document);
            } else {
                delete document;
            }
        }
    }

    if (auto *docker = aiDocker()) {
        docker->show();
        docker->raise();
        docker->triggerGeneration(prompt);
    }
}

void KisAiStartPageWidget::slotQuickCanvas1024()
{
    if (!m_mainWindow) return;

    KisDocument *document = KisPart::instance()->createDocument();
    if (document) {
        const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
        const KoColor background(QColor(QStringLiteral("#f4f7ff")), colorSpace);
        const QString name = i18n("AI Illustration");
        if (document->newImage(name, 1024, 1024, colorSpace, background, KisConfig::RASTER_LAYER, 1, QString(), 1.0)) {
            document->setObjectName(name);
            KisPart::instance()->addDocument(document);
            m_mainWindow->showDocument(document);
        } else {
            delete document;
        }
    }

    if (auto *docker = aiDocker()) {
        docker->show();
        docker->raise();
        docker->focusPrompt();
    }
}

void KisAiStartPageWidget::slotNewFile()
{
    if (m_mainWindow) {
        m_mainWindow->slotFileNew();
    }
}

void KisAiStartPageWidget::slotOpenFile()
{
    if (m_mainWindow) {
        m_mainWindow->slotFileOpen();
    }
}

void KisAiStartPageWidget::slotPasteFromClipboard()
{
    KisClipboard *clipboard = KisClipboard::instance();
    if (!clipboard || !clipboard->hasImage()) {
        showCanvasNotification(i18n("クリップボードに画像がありません。"));
        return;
    }
    auto *dlg = new KisDlgCreateNewDocument(this);
    dlg->SelectPage(KisDlgCreateNewDocument::Page::CreateFromClipboard);
    dlg->exec();
    dlg->deleteLater();
}

void KisAiStartPageWidget::slotApplyPreset(int presetId)
{
    QString presetPrompt;
    int styleIndex = 0;

    switch (presetId) {
    case 0: // Anime & Manga Line Art
        presetPrompt = i18n("美しいアニメ風美少女の繊細な線画、高精細な髪の毛と瞳のペン画、服のシワ、交点だまり、塗り絵");
        styleIndex = static_cast<int>(KisAiPromptAnalyzer::ArtStyle::PureLineart);
        break;
    case 1: // Anime Cel-Shading
        presetPrompt = i18n("アニメ調の繊細なキャラクターイラスト、高精細なペンタッチとセル着彩、ドラマチックなハイライト");
        styleIndex = static_cast<int>(KisAiPromptAnalyzer::ArtStyle::AnimeCel);
        break;
    case 2: // Cyberpunk
        presetPrompt = i18n("雨に濡れた近未来のサイバーパンク都市、ネオンサインの反射、大気感のある光芒とシネマティックライティング");
        styleIndex = static_cast<int>(KisAiPromptAnalyzer::ArtStyle::CyberNeon);
        break;
    case 3: // Watercolor & Atomic Ink
        presetPrompt = i18n("伝統的な透明水彩と物理滲みインク、柔らかいエッジと美しいグラデーションの自然風景イラスト");
        styleIndex = static_cast<int>(KisAiPromptAnalyzer::ArtStyle::Watercolor);
        break;
    case 4: // Vector Geometric Art
        presetPrompt = i18n("精密な幾何学的ストロークグラフ、美しい対称性を持つベクターエンブレム、ミニマルでモダンなデザイン");
        styleIndex = static_cast<int>(KisAiPromptAnalyzer::ArtStyle::FineLineart);
        break;
    case 5: { // Surprise Me
        const QStringList randomThemes = {
            i18n("夜空を泳ぐ光る巨大なクジラと星屑の海、幻想的なファンタジーアート"),
            i18n("アンティークな懐中時計と機械仕掛けの蝶、緻密なスチームパンク構造図"),
            i18n("雲海の上に浮かぶ天空の古代遺跡、夕暮れの黄金の光とベクター風の陰影"),
            i18n("サイバーパンクなストリートに佇むネコ耳サイボーグ少女、ネオングロー"),
            i18n("朝霧に包まれた神秘的な深林と光の精霊、水彩インクブレンディング"),
        };
        const int pick = QRandomGenerator::global()->bounded(randomThemes.size());
        presetPrompt = randomThemes[pick];
        const int surpriseStyles[] = {static_cast<int>(KisAiPromptAnalyzer::ArtStyle::Watercolor),
                                      static_cast<int>(KisAiPromptAnalyzer::ArtStyle::PureLineart),
                                      static_cast<int>(KisAiPromptAnalyzer::ArtStyle::AnimeCel),
                                      static_cast<int>(KisAiPromptAnalyzer::ArtStyle::CyberNeon)};
        styleIndex = surpriseStyles[pick % 4];
        break;
    }
    default:
        presetPrompt = i18n("クリエイティブなイラストレーション");
        styleIndex = 0;
        break;
    }

    if (m_promptInput) {
        m_promptInput->setText(presetPrompt);
    }

    // Create canvas
    slotQuickCanvas1024();

    if (auto *docker = aiDocker()) {
        docker->show();
        docker->raise();
        docker->setPromptText(presetPrompt);
        docker->applyStylePreset(styleIndex);
        docker->focusPrompt();
    }
}

void KisAiStartPageWidget::slotFocusAiDocker()
{
    if (auto *docker = aiDocker()) {
        docker->show();
        docker->raise();
        docker->focusPrompt();
    } else {
        showCanvasNotification(i18n("AI ドッカーが見つかりません。"));
    }
}

void KisAiStartPageWidget::slotRecentDocumentClicked(const QModelIndex &index)
{
    if (!m_mainWindow || !index.isValid())
        return;
    const QString fileUrl = index.data(Qt::ToolTipRole).toString();
    if (fileUrl.isEmpty())
        return;
    m_mainWindow->openDocument(fileUrl, KisMainWindow::None);
}

void KisAiStartPageWidget::showCanvasNotification(const QString &message)
{
    Q_UNUSED(message);
}

void KisAiStartPageWidget::slotClearRecentFiles()
{
    if (m_mainWindow) {
        m_mainWindow->clearRecentFiles();
        slotUpdateRecentFiles();
    }
}

void KisAiStartPageWidget::slotUpdateRecentFiles()
{
    KisRecentDocumentsModelWrapper *recentFilesModel = KisRecentDocumentsModelWrapper::instance();
    if (!recentFilesModel) return;

    if (m_recentListView) {
        m_recentListView->setModel(&recentFilesModel->model());
    }

    const bool modelIsEmpty = (recentFilesModel->model().rowCount() == 0);
    if (m_recentStack) {
        m_recentStack->setCurrentWidget(modelIsEmpty ? static_cast<QWidget *>(m_emptyRecentLabel) : static_cast<QWidget *>(m_recentListView));
    }
    if (m_clearRecentBtn) {
        m_clearRecentBtn->setVisible(!modelIsEmpty);
    }
}
