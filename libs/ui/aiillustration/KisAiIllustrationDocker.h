/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ILLUSTRATION_DOCKER_H
#define KIS_AI_ILLUSTRATION_DOCKER_H

#include "KisAiStrokeProgram.h"
#include <QByteArray>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
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
    void keyPressEvent(QKeyEvent *event) override;

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

    // Goal Mode (autonomous multi-stage drawing with vision-in-the-loop)
    void startGoalMode(const QString &prompt);
    void executeGoalStep();
    void finishGoalStepRequest();
    void advanceGoalStep();
    void finishGoalMode(bool success = true);
    void scheduleGoalStepRetry(const QString &reasonMessage, bool isSelfCorrection, int retryAfterSec = 0);
    void executeGoalStepRetry();

    // Two-Tier Retry & Self-Correction
    void scheduleRetry(const QString &reasonMessage, bool isSelfCorrection, int retryAfterSec = 0);
    void executeRetry();
    void cancelRetry();
    void clearInFlightApiKey();

    // Settings persistence
    void loadSettings();
    void saveSettings();
    void saveSettingsForMode(GenerationMode mode);

    // LLM Connection Test
    void testLlmConnection();
    void finishTestConnectionRequest();

    // Debug Mode & Logging
    void logDebug(const QString &category, const QString &message);
    void clearDebugLog();
    void copyDebugLog();

    bool appendReplyData(QNetworkReply *reply);
    QByteArray takeReplyData(QNetworkReply *reply);
    bool appendTestReplyData(QNetworkReply *reply);
    QByteArray takeTestReplyData(QNetworkReply *reply);
    void updateModeUi();
    void setBusy(bool busy);
    bool ensureCanvas();
    QSize effectiveCanvasSize() const;
    bool addImageAsLayer(const QImage &image, const QString &layerName);
    QString promptForLayerName(const QString &prompt) const;
    void setStatus(const QString &message, bool isError = false);
    void resetActivityTimeout();
    void updateProgressStatus();
    void stopAllRequestTimers();

    QPointer<KisMainWindow> m_mainWindow;
    QNetworkAccessManager *m_networkManager {nullptr};
    QPointer<QNetworkReply> m_reply;
    bool m_requestWasCancelled {false};
    bool m_requestTimedOut {false};
    bool m_responseTooLarge {false};
    QByteArray m_responseBuffer;
    GenerationMode m_currentMode {GenerationMode::LlmStrokes};

    // Dynamic timeout & streaming state
    QTimer *m_activityTimer {nullptr};
    QTimer *m_progressTimer {nullptr};
    QElapsedTimer m_requestElapsedTimer;
    QString m_streamedContent;
    QByteArray m_sseBuffer;
    bool m_isStreamingRequest {false};
    QString m_activeRequestEndpoint;

    // Connection test state
    QPointer<QNetworkReply> m_testReply;
    QByteArray m_testResponseBuffer;
    bool m_testResponseTooLarge{false};
    qint64 m_testStartTimeMs {0};

    // Retry & Self-Correction State
    QTimer *m_retryTimer {nullptr};
    int m_currentRetryCount {0};
    int m_maxRetryCount {2};
    bool m_isSelfCorrectionRetry {false};
    // True while generateLlmStrokes() is being re-entered from executeRetry().
    // A single-shot QTimer is already inactive by then, so the retry budget must
    // not be reset based on the timer state.
    bool m_retryInFlight {false};
    QString m_lastFailedPrompt;
    KisAiJsonDiagnostic m_lastJsonDiagnostic;
    QString m_inFlightApiKey;

    // Goal Mode State
    bool m_goalModeActive {false};
    int m_goalCurrentStep {1};
    int m_goalTotalSteps {4};
    int m_goalCurrentRetryCount {0};
    QString m_goalSelfCorrectionFeedback;
    QString m_goalPrompt;
    QString m_goalApiKey;
    bool m_waitingForUserStepAdvance {false};
    bool m_goalVisionFallbackActive {false};
    bool m_lastGoalRequestHadImage {false};
    KisAiStrokeProgram m_goalAccumulatedProgram;

    QComboBox *m_presetCombo {nullptr};
    QPlainTextEdit *m_promptEditor {nullptr};
    QComboBox *m_modeCombo {nullptr};
    class QFormLayout *m_remoteForm {nullptr};
    QLineEdit *m_endpointEditor {nullptr};
    QLineEdit *m_modelEditor {nullptr};
    QLineEdit *m_apiKeyEditor {nullptr};
    QCheckBox *m_saveApiKeyCheck {nullptr};
    QPushButton *m_testConnectionButton {nullptr};
    QLabel *m_testConnectionStatusLabel {nullptr};
    QPushButton *m_saveSettingsButton {nullptr};
    QSpinBox *m_widthSpin {nullptr};
    QSpinBox *m_heightSpin {nullptr};
    QSpinBox *m_strokeBudgetSpin {nullptr};
    QLabel *m_strokeBudgetLabel {nullptr};
    QLabel *m_remoteOptionsLabel {nullptr};
    QToolButton *m_detailsToggleBtn {nullptr};
    QWidget *m_detailsContainer {nullptr};

    // Fine-grained AI Settings UI Controls
    QDoubleSpinBox *m_temperatureSpin {nullptr};
    QDoubleSpinBox *m_topPSpin {nullptr};
    QSpinBox *m_maxTokensSpin {nullptr};
    QSpinBox *m_maxRetriesSpin {nullptr};
    QSpinBox *m_timeoutSecSpin {nullptr};
    QComboBox *m_jsonModeCombo {nullptr};
    QComboBox *m_reasoningEffortCombo {nullptr};
    QPlainTextEdit *m_customInstructionsEdit {nullptr};

    // Goal Mode UI Controls
    QCheckBox *m_goalModeCheck {nullptr};
    QCheckBox *m_pausePerStepCheck {nullptr};
    QSpinBox *m_goalStepsSpin {nullptr};
    QComboBox *m_artStyleCombo {nullptr};
    QFrame *m_goalInspectorCard {nullptr};
    QLabel *m_goalPhaseLabel {nullptr};
    QLabel *m_agentFocusLabel {nullptr};
    QLabel *m_critiqueLabel {nullptr};
    QProgressBar *m_readinessBar {nullptr};
    QPushButton *m_nextStepButton {nullptr};
    QPushButton *m_finishGoalButton {nullptr};

    // Debug Mode UI Controls
    QCheckBox *m_debugModeCheck {nullptr};
    QFrame *m_debugCard {nullptr};
    QPlainTextEdit *m_debugLogText {nullptr};
    QPushButton *m_copyLogButton {nullptr};
    QPushButton *m_clearLogButton {nullptr};

    QLabel *m_statusLabel {nullptr};
    QLabel *m_previewLabel {nullptr};
    QProgressBar *m_progressBar {nullptr};
    QPushButton *m_newCanvasButton {nullptr};
    QPushButton *m_generateButton {nullptr};
    QPushButton *m_cancelButton {nullptr};
};

#endif // KIS_AI_ILLUSTRATION_DOCKER_H
