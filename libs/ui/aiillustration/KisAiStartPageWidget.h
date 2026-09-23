/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_START_PAGE_WIDGET_H
#define KIS_AI_START_PAGE_WIDGET_H

#include "kritaui_export.h"

#include <QWidget>
#include <QPointer>
#include <QModelIndex>
#include <QList>

class QBoxLayout;
class QGridLayout;
class QVBoxLayout;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QListView;
class QLabel;
class QStackedWidget;
class QScrollArea;
class KisMainWindow;
class KisAiIllustrationDocker;

/**
 * Modern AI creative studio start page for AI Stroke Painter.
 * Provides quick prompt-to-canvas generation, 1-click style presets,
 * primary workflow cards, recent artworks, and creative tips.
 */
class KRITAUI_EXPORT KisAiStartPageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit KisAiStartPageWidget(KisMainWindow *mainWindow, QWidget *parent = nullptr);
    ~KisAiStartPageWidget() override;

public Q_SLOTS:
    void slotQuickPromptGenerate();
    void slotQuickCanvas1024();
    void slotNewFile();
    void slotOpenFile();
    void slotPasteFromClipboard();
    void slotApplyPreset(int presetId);
    void slotFocusAiDocker();
    void slotRecentDocumentClicked(const QModelIndex &index);
    void slotClearRecentFiles();
    void slotUpdateRecentFiles();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QWidget *createHeroSection();
    QWidget *createPromptBarSection();
    QWidget *createPrimaryActionsSection();
    QWidget *createPresetsSection();
    QWidget *createRecentAndGuideSection();

    KisAiIllustrationDocker *aiDocker() const;
    void showCanvasNotification(const QString &message);
    void updateResponsiveLayout();

    QPointer<KisMainWindow> m_mainWindow;
    QScrollArea *m_scrollArea {nullptr};
    QVBoxLayout *m_contentLayout {nullptr};
    bool m_isNarrowLayout {false};
    QVBoxLayout *m_heroLayout {nullptr};
    QBoxLayout *m_heroHeaderLayout {nullptr};
    QBoxLayout *m_promptBarLayout {nullptr};
    QBoxLayout *m_primaryCardsLayout {nullptr};
    QBoxLayout *m_recentAndGuideLayout {nullptr};
    QGridLayout *m_presetsGrid {nullptr};
    QList<QBoxLayout *> m_presetButtonLayouts;
    QLineEdit *m_promptInput {nullptr};
    QPushButton *m_promptSubmitBtn {nullptr};
    QListView *m_recentListView {nullptr};
    QLabel *m_emptyRecentLabel {nullptr};
    QStackedWidget *m_recentStack {nullptr};
    QPushButton *m_clearRecentBtn {nullptr};
};

#endif // KIS_AI_START_PAGE_WIDGET_H
