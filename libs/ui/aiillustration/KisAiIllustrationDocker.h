/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ILLUSTRATION_DOCKER_H
#define KIS_AI_ILLUSTRATION_DOCKER_H

#include <QDockWidget>
#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QToolButton;
class QWidget;
class QImage;
class QString;

class KisMainWindow;

/**
 * The compiled-in AI illustration workspace. It owns the prompt-to-canvas
 * flow and deliberately does not use the PyKrita plugin manager.
 */
class KisAiIllustrationDocker final : public QDockWidget
{
public:
    explicit KisAiIllustrationDocker(KisMainWindow *mainWindow);
    ~KisAiIllustrationDocker() override;

    void focusPrompt();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class GenerationMode {
        LlmStrokes,    // Text-based LLM coordinate strokes (Chat Completions)
        LocalStrokes,  // Offline procedural coordinate strokes
        RemoteImage,   // Image generation model API (DALL-E)
        LocalConcept,  // Offline deterministic single concept image
    };

    void createCanvas();
    void generateIllustration();
    void generateLlmStrokes(const QString &prompt);
    void finishLlmStrokesRequest();
    void generateLocalStrokes(const QString &prompt);
    void generateLocalConcept(const QString &prompt);
    void generateRemoteImage(const QString &prompt);
    void finishRemoteImageRequest();
    void cancelRemoteRequest();
    void updateModeUi();
    void setBusy(bool busy);
    bool ensureCanvas();
    bool addImageAsLayer(const QImage &image, const QString &layerName);
    QString promptForLayerName(const QString &prompt) const;
    void setStatus(const QString &message, bool isError = false);

    QPointer<KisMainWindow> m_mainWindow;
    QNetworkAccessManager *m_networkManager {nullptr};
    QPointer<QNetworkReply> m_reply;
    bool m_requestWasCancelled {false};
    bool m_requestTimedOut {false};
    bool m_responseTooLarge {false};
    GenerationMode m_currentMode {GenerationMode::LlmStrokes};

    QComboBox *m_presetCombo {nullptr};
    QPlainTextEdit *m_promptEditor {nullptr};
    QComboBox *m_modeCombo {nullptr};
    class QFormLayout *m_remoteForm {nullptr};
    QLineEdit *m_endpointEditor {nullptr};
    QLineEdit *m_modelEditor {nullptr};
    QLineEdit *m_apiKeyEditor {nullptr};
    QSpinBox *m_widthSpin {nullptr};
    QSpinBox *m_heightSpin {nullptr};
    QSpinBox *m_strokeBudgetSpin {nullptr};
    QLabel *m_strokeBudgetLabel {nullptr};
    QLabel *m_remoteOptionsLabel {nullptr};
    QToolButton *m_detailsToggleBtn {nullptr};
    QWidget *m_detailsContainer {nullptr};
    QLabel *m_statusLabel {nullptr};
    QLabel *m_previewLabel {nullptr};
    QProgressBar *m_progressBar {nullptr};
    QPushButton *m_newCanvasButton {nullptr};
    QPushButton *m_generateButton {nullptr};
    QPushButton *m_cancelButton {nullptr};
};

#endif // KIS_AI_ILLUSTRATION_DOCKER_H
