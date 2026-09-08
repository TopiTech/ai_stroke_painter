/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiIllustrationDocker.h"

#include "KisAiIllustrationRenderer.h"
#include "KisAiStrokeProgram.h"
#include "KisAiStrokeRenderer.h"
#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "kis_config.h"
#include "kis_image.h"
#include "kis_node.h"
#include "kis_node_commands_adapter.h"
#include "kis_paint_layer.h"

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>

#include <QBuffer>
#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <klocalizedstring.h>

namespace
{
constexpr qint64 MAX_REMOTE_RESPONSE_BYTES = 32LL * 1024 * 1024;
constexpr qint64 MAX_REMOTE_IMAGE_BYTES = 24LL * 1024 * 1024;
constexpr qint64 MAX_REMOTE_IMAGE_PIXELS = 24LL * 1024 * 1024;
constexpr int REMOTE_REQUEST_TIMEOUT_MS = 120'000;

QString imageSizeText(const QSpinBox *widthSpin, const QSpinBox *heightSpin)
{
    return QString::number(widthSpin->value()) + QLatin1Char('x') + QString::number(heightSpin->value());
}

QImage decodeModelImage(const QByteArray &response, QString *errorMessage)
{
    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (!document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("画像モデルの応答は JSON オブジェクトではありません。");
        }
        return {};
    }

    const QJsonArray data = document.object().value(QStringLiteral("data")).toArray();
    if (data.isEmpty() || !data.at(0).isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("画像モデルの応答に画像データがありません。");
        }
        return {};
    }

    const QByteArray encoded = data.at(0).toObject().value(QStringLiteral("b64_json")).toString().toLatin1();
    if (encoded.isEmpty() || encoded.size() > MAX_REMOTE_RESPONSE_BYTES) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("画像モデルの応答が大きすぎるか、base64 画像を含んでいません。");
        }
        return {};
    }

    const QByteArray imageBytes = QByteArray::fromBase64(encoded);
    if (imageBytes.isEmpty() || imageBytes.size() > MAX_REMOTE_IMAGE_BYTES) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("画像モデルが返した画像データを安全に読み込めませんでした。");
        }
        return {};
    }

    QBuffer buffer;
    buffer.setData(imageBytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize decodedSize = reader.size();
    if (!decodedSize.isValid() || qint64(decodedSize.width()) * decodedSize.height() > MAX_REMOTE_IMAGE_PIXELS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("画像モデルが返した画像の寸法が上限を超えています。");
        }
        return {};
    }

    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("画像モデルが返した画像をデコードできませんでした。");
        }
    }
    return image;
}
}

KisAiIllustrationDocker::KisAiIllustrationDocker(KisMainWindow *mainWindow)
    : QDockWidget(i18n("AI Illustration"), mainWindow)
    , m_mainWindow(mainWindow)
    , m_networkManager(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("AiIllustrationDocker"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    setMinimumWidth(360);

    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("aiIllustrationPanel"));
    panel->setAccessibleName(i18n("AI illustration workspace"));
    panel->setStyleSheet(QStringLiteral(
        "QWidget#aiIllustrationPanel { background: #151b28; color: #edf3ff; }"
        "QWidget#aiIllustrationPanel QLabel { color: #dbe5f5; font-size: 13px; }"
        "QLabel#aiTitle { color: #f3f7ff; font-size: 20px; font-weight: 700; }"
        "QLabel#aiSubtitle { color: #95a8c5; font-size: 12px; }"
        "QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {"
        " background: #0f1521; border: 1px solid #314460; border-radius: 5px; padding: 6px 8px; color: #edf3ff; }"
        "QPlainTextEdit:focus, QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid #6d9df2; }"
        "QPushButton#aiGenerateButton { background: #4a88f7; color: #ffffff; border: none; border-radius: 5px;"
        " font-weight: 700; font-size: 14px; min-height: 36px; padding: 6px 14px; }"
        "QPushButton#aiGenerateButton:hover { background: #629aff; }"
        "QPushButton#aiGenerateButton:pressed { background: #3572df; }"
        "QPushButton#aiGenerateButton:disabled { background: #28374d; color: #6d809c; }"
        "QPushButton#aiSecondaryButton { background: #1a2232; color: #c4d8f5; border: 1px solid #3b4f6e;"
        " border-radius: 5px; min-height: 32px; padding: 5px 12px; }"
        "QPushButton#aiSecondaryButton:hover { background: #243046; color: #ffffff; border-color: #526c95; }"
        "QPushButton#aiSecondaryButton:pressed { background: #141b27; }"
        "QLabel#aiStatus { color: #a4b8d6; padding: 4px 1px; font-size: 12px; }"
        "QLabel#aiStatus[error=\"true\"] { color: #ff8e97; font-weight: 600; }"
        "QFrame#aiRule { background: #314460; max-height: 1px; }"
        "QToolButton#aiToggleDetails { color: #8ea5c8; background: transparent; border: none; font-size: 12px; text-align: left; padding: 4px 0px; font-weight: 500; }"
        "QToolButton#aiToggleDetails:hover { color: #c8daf2; }"));

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    auto *title = new QLabel(i18n("AI Stroke Painter"), panel);
    title->setObjectName(QStringLiteral("aiTitle"));
    title->setAccessibleName(i18n("AI Stroke Painter workspace"));
    layout->addWidget(title);

    auto *rule = new QFrame(panel);
    rule->setObjectName(QStringLiteral("aiRule"));
    rule->setFrameShape(QFrame::HLine);
    layout->addWidget(rule);

    auto *subtitle = new QLabel(i18n("描きたい情景を言葉で指定すると、結果を新しいレイヤーとして追加します。"), panel);
    subtitle->setObjectName(QStringLiteral("aiSubtitle"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto *promptLabel = new QLabel(i18n("イラストの指示"), panel);
    layout->addWidget(promptLabel);

    m_promptEditor = new QPlainTextEdit(panel);
    m_promptEditor->setPlaceholderText(i18n("例: 雨上がりの夜、青い光に包まれた猫と花のある静かな路地 (Ctrl+Enter で生成)"));
    m_promptEditor->setMinimumHeight(80);
    m_promptEditor->setMaximumHeight(140);
    m_promptEditor->setAccessibleName(i18n("Illustration prompt"));
    m_promptEditor->installEventFilter(this);
    layout->addWidget(m_promptEditor);

    auto *canvasLabel = new QLabel(i18n("新しいキャンバスの大きさ"), panel);
    layout->addWidget(canvasLabel);

    auto *canvasRow = new QHBoxLayout();
    m_widthSpin = new QSpinBox(panel);
    m_widthSpin->setRange(256, 4096);
    m_widthSpin->setSingleStep(64);
    m_widthSpin->setValue(1024);
    m_widthSpin->setPrefix(i18n("幅: "));
    m_widthSpin->setSuffix(QStringLiteral(" px"));
    m_widthSpin->setAccessibleName(i18n("Canvas width"));
    m_heightSpin = new QSpinBox(panel);
    m_heightSpin->setRange(256, 4096);
    m_heightSpin->setSingleStep(64);
    m_heightSpin->setValue(1024);
    m_heightSpin->setPrefix(i18n("高さ: "));
    m_heightSpin->setSuffix(QStringLiteral(" px"));
    m_heightSpin->setAccessibleName(i18n("Canvas height"));
    canvasRow->addWidget(m_widthSpin);
    canvasRow->addWidget(m_heightSpin);
    layout->addLayout(canvasRow);

    m_modeCombo = new QComboBox(panel);
    m_modeCombo->addItem(i18n("LLM 座標ストローク描画 (Chat Completions)"), static_cast<int>(GenerationMode::LlmStrokes));
    m_modeCombo->addItem(i18n("ローカル座標ストローク描画"), static_cast<int>(GenerationMode::LocalStrokes));
    m_modeCombo->addItem(i18n("画像モデル API (DALL-E)"), static_cast<int>(GenerationMode::RemoteImage));
    m_modeCombo->addItem(i18n("ローカル・コンセプトスケッチ"), static_cast<int>(GenerationMode::LocalConcept));
    m_modeCombo->setAccessibleName(i18n("Generation source"));
    layout->addWidget(m_modeCombo);

    m_detailsToggleBtn = new QToolButton(panel);
    m_detailsToggleBtn->setObjectName(QStringLiteral("aiToggleDetails"));
    m_detailsToggleBtn->setCheckable(true);
    m_detailsToggleBtn->setText(i18n("▶ 詳細設定（エンドポイント / API キー）"));
    m_detailsToggleBtn->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_detailsToggleBtn);

    m_detailsContainer = new QWidget(panel);
    auto *detailsLayout = new QVBoxLayout(m_detailsContainer);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(6);

    m_remoteOptionsLabel = new QLabel(i18n("OpenAI 互換の Chat Completions エンドポイントを指定します。API キーは保存しません。"), m_detailsContainer);
    m_remoteOptionsLabel->setObjectName(QStringLiteral("aiSubtitle"));
    m_remoteOptionsLabel->setWordWrap(true);
    detailsLayout->addWidget(m_remoteOptionsLabel);

    auto *remoteForm = new QFormLayout();
    remoteForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_remoteForm = remoteForm;
    m_endpointEditor = new QLineEdit(m_detailsContainer);
    m_endpointEditor->setPlaceholderText(QStringLiteral("https://api.openai.com/v1/chat/completions"));
    m_endpointEditor->setAccessibleName(i18n("LLM endpoint"));
    m_modelEditor = new QLineEdit(m_detailsContainer);
    m_modelEditor->setPlaceholderText(i18n("モデル名 (例: gpt-4o, o3-mini, deepseek-chat)"));
    m_modelEditor->setAccessibleName(i18n("LLM model name"));
    m_apiKeyEditor = new QLineEdit(m_detailsContainer);
    m_apiKeyEditor->setEchoMode(QLineEdit::Password);
    m_apiKeyEditor->setPlaceholderText(i18n("このリクエストだけに使用"));
    m_apiKeyEditor->setAccessibleName(i18n("API key"));

    m_strokeBudgetLabel = new QLabel(i18n("ストローク予算"), m_detailsContainer);
    m_strokeBudgetSpin = new QSpinBox(m_detailsContainer);
    m_strokeBudgetSpin->setRange(20, 2000);
    m_strokeBudgetSpin->setValue(500);
    m_strokeBudgetSpin->setSingleStep(50);
    m_strokeBudgetSpin->setSuffix(i18n(" 本"));
    m_strokeBudgetSpin->setAccessibleName(i18n("Stroke budget"));

    remoteForm->addRow(i18n("エンドポイント"), m_endpointEditor);
    remoteForm->addRow(i18n("モデル"), m_modelEditor);
    remoteForm->addRow(i18n("API キー"), m_apiKeyEditor);
    remoteForm->addRow(m_strokeBudgetLabel, m_strokeBudgetSpin);
    detailsLayout->addLayout(remoteForm);

    m_detailsContainer->setVisible(false);
    layout->addWidget(m_detailsContainer);

    connect(m_detailsToggleBtn, &QToolButton::toggled, this, [this](bool checked) {
        m_detailsContainer->setVisible(checked);
        m_detailsToggleBtn->setText(checked ? i18n("▼ 詳細設定（エンドポイント / API キー）")
                                            : i18n("▶ 詳細設定（エンドポイント / API キー）"));
    });

    QSettings settings;
    const QString savedEndpoint = settings.value(QStringLiteral("AIIllustration/llmEndpoint"),
        settings.value(QStringLiteral("AIIllustration/endpoint"))).toString();
    const QString savedModel = settings.value(QStringLiteral("AIIllustration/llmModel"),
        settings.value(QStringLiteral("AIIllustration/model"))).toString();
    m_endpointEditor->setText(savedEndpoint.isEmpty() ? QStringLiteral("https://api.openai.com/v1/chat/completions") : savedEndpoint);
    m_modelEditor->setText(savedModel.isEmpty() ? QStringLiteral("gpt-4o") : savedModel);

    m_previewLabel = new QLabel(i18n("生成結果のプレビューはここに表示されます。"), panel);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setWordWrap(true);
    m_previewLabel->setMinimumHeight(64);
    m_previewLabel->setStyleSheet(QStringLiteral("background: #0f1521; border: 1px solid #24344d; border-radius: 4px; color: #7f95b5; padding: 4px;"));
    m_previewLabel->setAccessibleName(i18n("Generation preview"));
    layout->addWidget(m_previewLabel);

    m_progressBar = new QProgressBar(panel);
    m_progressBar->setRange(0, 0);
    m_progressBar->setTextVisible(false);
    m_progressBar->setVisible(false);
    layout->addWidget(m_progressBar);

    m_statusLabel = new QLabel(i18n("キャンバスがない場合は、生成時に新しい AI キャンバスを作成します。"), panel);
    m_statusLabel->setObjectName(QStringLiteral("aiStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAccessibleName(i18n("Generation status"));
    layout->addWidget(m_statusLabel);

    auto *buttonRow = new QHBoxLayout();
    m_newCanvasButton = new QPushButton(i18n("新しいキャンバス"), panel);
    m_newCanvasButton->setObjectName(QStringLiteral("aiSecondaryButton"));
    m_newCanvasButton->setAccessibleDescription(i18n("指定した大きさの AI イラスト用キャンバスを作成します。"));
    m_generateButton = new QPushButton(i18n("生成してレイヤーに追加"), panel);
    m_generateButton->setObjectName(QStringLiteral("aiGenerateButton"));
    m_generateButton->setAccessibleDescription(i18n("プロンプトからイラストを生成し、現在のキャンバスに新しいレイヤーを追加します。"));
    m_cancelButton = new QPushButton(i18n("中止"), panel);
    m_cancelButton->setObjectName(QStringLiteral("aiSecondaryButton"));
    m_cancelButton->setVisible(false);
    buttonRow->addWidget(m_newCanvasButton);
    buttonRow->addWidget(m_generateButton, 1);
    buttonRow->addWidget(m_cancelButton);
    layout->addLayout(buttonRow);
    layout->addStretch(1);


    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidget(panel);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWidget(scrollArea);

    connect(m_newCanvasButton, &QPushButton::clicked, this, [this] { createCanvas(); });
    connect(m_generateButton, &QPushButton::clicked, this, [this] { generateIllustration(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] { cancelRemoteRequest(); });
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateModeUi(); });

    updateModeUi();
}

KisAiIllustrationDocker::~KisAiIllustrationDocker()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

bool KisAiIllustrationDocker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_promptEditor && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->modifiers() & Qt::ControlModifier) &&
            (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)) {
            generateIllustration();
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void KisAiIllustrationDocker::focusPrompt()
{
    m_promptEditor->setFocus();
}

void KisAiIllustrationDocker::createCanvas()
{
    if (!m_mainWindow) {
        setStatus(i18n("メインウィンドウが利用できないため、キャンバスを作成できません。"), true);
        return;
    }

    KisDocument *document = KisPart::instance()->createDocument();
    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    const KoColor background(QColor(QStringLiteral("#f4f7ff")), colorSpace);
    const QString name = i18n("AI Illustration");

    if (!document->newImage(name,
                            m_widthSpin->value(),
                            m_heightSpin->value(),
                            colorSpace,
                            background,
                            KisConfig::RASTER_LAYER,
                            1,
                            QString(),
                            1.0)) {
        delete document;
        setStatus(i18n("新しい AI キャンバスを作成できませんでした。"), true);
        return;
    }

    document->setObjectName(name);
    KisPart::instance()->addDocument(document);
    m_mainWindow->showDocument(document);
    setStatus(i18n("%1 の AI キャンバスを作成しました。", imageSizeText(m_widthSpin, m_heightSpin)));
    focusPrompt();
}

void KisAiIllustrationDocker::generateIllustration()
{
    if (m_reply) {
        return;
    }

    const QString prompt = KisAiIllustrationRenderer::normalizedPrompt(m_promptEditor->toPlainText());
    if (prompt.isEmpty()) {
        setStatus(i18n("まず、描きたいイラストの指示を入力してください。"), true);
        focusPrompt();
        return;
    }

    if (!ensureCanvas()) {
        return;
    }

    const auto mode = static_cast<GenerationMode>(m_modeCombo->currentData().toInt());
    switch (mode) {
    case GenerationMode::LlmStrokes:
        generateLlmStrokes(prompt);
        break;
    case GenerationMode::LocalStrokes:
        generateLocalStrokes(prompt);
        break;
    case GenerationMode::RemoteImage:
        generateRemoteImage(prompt);
        break;
    case GenerationMode::LocalConcept:
        generateLocalConcept(prompt);
        break;
    }
}

void KisAiIllustrationDocker::generateLocalStrokes(const QString &prompt)
{
    setBusy(true);
    setStatus(i18n("ローカルの座標ストロークを生成しています…"));

    const QSize canvasSize(m_widthSpin->value(), m_heightSpin->value());
    const KisAiStrokeProgram program = KisAiStrokeProgramCodec::createDeterministicProgram(prompt, canvasSize);

    const QImage preview = KisAiStrokeRenderer::renderProgramToImage(program, m_previewLabel->size());
    m_previewLabel->setPixmap(QPixmap::fromImage(preview).scaled(m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
    if (view && view->image()) {
        QString statusMsg;
        if (KisAiStrokeRenderer::renderProgramToLayers(view->image(), m_mainWindow->viewManager(), program, &statusMsg)) {
            setStatus(statusMsg);
        } else {
            setStatus(statusMsg, true);
        }
    } else {
        setStatus(i18n("キャンバスが利用できないため、ストロークを描画できませんでした。"), true);
    }
    setBusy(false);
}

void KisAiIllustrationDocker::generateLlmStrokes(const QString &prompt)
{
    QString errorMessage;
    const QString endpoint = m_endpointEditor->text().trimmed();
    const QString model = m_modelEditor->text().trimmed();
    const QString apiKey = m_apiKeyEditor->text();

    if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &errorMessage)) {
        setStatus(errorMessage, true);
        return;
    }
    if (model.isEmpty()) {
        setStatus(i18n("LLM モデル名を入力してください。"), true);
        return;
    }
    if (apiKey.isEmpty()) {
        setStatus(i18n("このリクエストに使う API キーを入力してください。"), true);
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("AIIllustration/llmEndpoint"), endpoint);
    settings.setValue(QStringLiteral("AIIllustration/llmModel"), model);
    settings.setValue(QStringLiteral("AIIllustration/endpoint"), endpoint);
    settings.setValue(QStringLiteral("AIIllustration/model"), model);

    QNetworkRequest request{QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    const QSize canvasSize(m_widthSpin->value(), m_heightSpin->value());
    const QJsonObject payload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(
        model,
        prompt,
        canvasSize,
        m_strokeBudgetSpin ? m_strokeBudgetSpin->value() : 500
    );

    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    m_responseTooLarge = false;
    m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);
    m_apiKeyEditor->clear();
    setBusy(true);
    setStatus(i18n("%1 に LLM 座標ストローク生成を依頼しています…", KisAiIllustrationRenderer::displayEndpoint(endpoint)));

    connect(m_reply.data(), &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        if (m_reply && received > MAX_REMOTE_RESPONSE_BYTES) {
            m_responseTooLarge = true;
            setStatus(i18n("LLM の応答が上限を超えたため中止しました。"), true);
            m_reply->abort();
        }
    });
    connect(m_reply.data(), &QNetworkReply::finished, this, [this] { finishLlmStrokesRequest(); });

    const QPointer<QNetworkReply> pendingReply = m_reply;
    QTimer::singleShot(REMOTE_REQUEST_TIMEOUT_MS, this, [this, pendingReply] {
        if (m_reply && m_reply.data() == pendingReply.data()) {
            m_requestTimedOut = true;
            m_reply->abort();
        }
    });
}

void KisAiIllustrationDocker::finishLlmStrokesRequest()
{
    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    const bool requestWasCancelled = m_requestWasCancelled;
    const bool requestTimedOut = m_requestTimedOut;
    const bool responseTooLarge = m_responseTooLarge;
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    m_responseTooLarge = false;
    setBusy(false);

    if (!reply) {
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray response = reply->readAll();
    reply->deleteLater();

    if (requestWasCancelled) {
        setStatus(i18n("LLM ストローク生成を中止しました。"));
        return;
    }
    if (requestTimedOut) {
        setStatus(i18n("LLM の応答が 2 分以内に届かなかったため中止しました。"), true);
        return;
    }
    if (responseTooLarge) {
        setStatus(i18n("LLM の応答が上限を超えています。"), true);
        return;
    }
    if (!requestSucceeded) {
        QString detail;
        QJsonParseError parseErr;
        const QJsonDocument errDoc = QJsonDocument::fromJson(response, &parseErr);
        if (!errDoc.isNull() && errDoc.isObject()) {
            const QJsonObject errObj = errDoc.object().value(QStringLiteral("error")).toObject();
            detail = errObj.value(QStringLiteral("message")).toString().trimmed();
        }
        if (!detail.isEmpty()) {
            setStatus(i18n("LLM への接続または応答に失敗しました (HTTP %1): %2", httpStatus, detail), true);
        } else {
            setStatus(i18n("LLM への接続または応答に失敗しました (HTTP %1)。", httpStatus), true);
        }
        return;
    }

    KisAiStrokeProgram program;
    QString parseError;
    if (!KisAiStrokeProgramCodec::parseResponse(response, &program, &parseError)) {
        setStatus(parseError, true);
        return;
    }

    const QImage preview = KisAiStrokeRenderer::renderProgramToImage(program, m_previewLabel->size());
    m_previewLabel->setPixmap(QPixmap::fromImage(preview).scaled(m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
    if (view && view->image()) {
        QString statusMsg;
        if (KisAiStrokeRenderer::renderProgramToLayers(view->image(), m_mainWindow->viewManager(), program, &statusMsg)) {
            setStatus(statusMsg);
        } else {
            setStatus(statusMsg, true);
        }
    } else {
        setStatus(i18n("キャンバスが利用できないため、ストロークを描画できませんでした。"), true);
    }
}

void KisAiIllustrationDocker::generateLocalConcept(const QString &prompt)
{
    setBusy(true);
    setStatus(i18n("ローカルのコンセプトスケッチを構成しています…"));

    const QSize canvasSize(m_widthSpin->value(), m_heightSpin->value());
    const QImage result = KisAiIllustrationRenderer::createConceptImage(prompt, canvasSize);
    m_previewLabel->setPixmap(QPixmap::fromImage(result).scaled(m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    if (addImageAsLayer(result, promptForLayerName(prompt))) {
        setStatus(i18n("コンセプトスケッチを新しいレイヤーに追加しました。"));
    }
    setBusy(false);
}

void KisAiIllustrationDocker::generateRemoteImage(const QString &prompt)
{
    QString errorMessage;
    const QString endpoint = m_endpointEditor->text().trimmed();
    const QString model = m_modelEditor->text().trimmed();
    const QString apiKey = m_apiKeyEditor->text();

    if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &errorMessage)) {
        setStatus(errorMessage, true);
        return;
    }
    if (model.isEmpty()) {
        setStatus(i18n("画像モデル名を入力してください。"), true);
        return;
    }
    if (apiKey.isEmpty()) {
        setStatus(i18n("このリクエストに使う API キーを入力してください。"), true);
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("AIIllustration/imageEndpoint"), endpoint);
    settings.setValue(QStringLiteral("AIIllustration/imageModel"), model);

    QNetworkRequest request {QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    const QJsonObject payload {
        {QStringLiteral("model"), model},
        {QStringLiteral("prompt"), prompt},
        {QStringLiteral("size"), imageSizeText(m_widthSpin, m_heightSpin)},
        {QStringLiteral("response_format"), QStringLiteral("b64_json")},
    };

    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    m_responseTooLarge = false;
    m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);
    m_apiKeyEditor->clear();
    setBusy(true);
    setStatus(i18n("%1 に画像生成を依頼しています…", KisAiIllustrationRenderer::displayEndpoint(endpoint)));

    connect(m_reply.data(), &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        if (m_reply && received > MAX_REMOTE_RESPONSE_BYTES) {
            m_responseTooLarge = true;
            setStatus(i18n("画像モデルの応答が上限を超えたため中止しました。"), true);
            m_reply->abort();
        }
    });
    connect(m_reply.data(), &QNetworkReply::finished, this, [this] { finishRemoteImageRequest(); });

    const QPointer<QNetworkReply> pendingReply = m_reply;
    QTimer::singleShot(REMOTE_REQUEST_TIMEOUT_MS, this, [this, pendingReply] {
        if (m_reply && m_reply.data() == pendingReply.data()) {
            m_requestTimedOut = true;
            m_reply->abort();
        }
    });
}

void KisAiIllustrationDocker::finishRemoteImageRequest()
{
    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    const bool requestWasCancelled = m_requestWasCancelled;
    const bool requestTimedOut = m_requestTimedOut;
    const bool responseTooLarge = m_responseTooLarge;
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    m_responseTooLarge = false;
    setBusy(false);

    if (!reply) {
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray response = reply->readAll();
    reply->deleteLater();

    if (requestWasCancelled) {
        setStatus(i18n("画像生成を中止しました。"));
        return;
    }
    if (requestTimedOut) {
        setStatus(i18n("画像モデルの応答が 2 分以内に届かなかったため中止しました。"), true);
        return;
    }
    if (responseTooLarge) {
        setStatus(i18n("画像モデルの応答が上限を超えています。"), true);
        return;
    }
    if (!requestSucceeded) {
        QString detail;
        QJsonParseError parseErr;
        const QJsonDocument errDoc = QJsonDocument::fromJson(response, &parseErr);
        if (!errDoc.isNull() && errDoc.isObject()) {
            const QJsonObject errObj = errDoc.object().value(QStringLiteral("error")).toObject();
            detail = errObj.value(QStringLiteral("message")).toString().trimmed();
        }
        if (!detail.isEmpty()) {
            setStatus(i18n("画像モデルへの接続または応答に失敗しました (HTTP %1): %2", httpStatus, detail), true);
        } else {
            setStatus(i18n("画像モデルへの接続または応答に失敗しました (HTTP %1)。", httpStatus), true);
        }
        return;
    }
    if (response.size() > MAX_REMOTE_RESPONSE_BYTES) {
        setStatus(i18n("画像モデルの応答が上限を超えています。"), true);
        return;
    }

    QString errorMessage;
    const QImage image = decodeModelImage(response, &errorMessage);
    if (image.isNull()) {
        setStatus(errorMessage, true);
        return;
    }

    m_previewLabel->setPixmap(QPixmap::fromImage(image).scaled(m_previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (addImageAsLayer(image, promptForLayerName(m_promptEditor->toPlainText()))) {
        setStatus(i18n("画像モデルの結果を新しいレイヤーに追加しました。"));
    }
}

void KisAiIllustrationDocker::cancelRemoteRequest()
{
    if (!m_reply) {
        return;
    }

    m_requestWasCancelled = true;
    m_reply->abort();
    setStatus(i18n("リクエストを中止しています…"));
}

void KisAiIllustrationDocker::updateModeUi()
{
    const auto newMode = static_cast<GenerationMode>(m_modeCombo->currentData().toInt());

    // Save current values before switching modes
    QSettings settings;
    if (m_currentMode == GenerationMode::LlmStrokes) {
        const QString ep = m_endpointEditor->text().trimmed();
        const QString mdl = m_modelEditor->text().trimmed();
        if (!ep.isEmpty()) {
            settings.setValue(QStringLiteral("AIIllustration/llmEndpoint"), ep);
        }
        if (!mdl.isEmpty()) {
            settings.setValue(QStringLiteral("AIIllustration/llmModel"), mdl);
        }
    } else if (m_currentMode == GenerationMode::RemoteImage) {
        const QString ep = m_endpointEditor->text().trimmed();
        const QString mdl = m_modelEditor->text().trimmed();
        if (!ep.isEmpty()) {
            settings.setValue(QStringLiteral("AIIllustration/imageEndpoint"), ep);
        }
        if (!mdl.isEmpty()) {
            settings.setValue(QStringLiteral("AIIllustration/imageModel"), mdl);
        }
    }

    m_currentMode = newMode;
    const bool isLlm = (newMode == GenerationMode::LlmStrokes);
    const bool isRemoteImage = (newMode == GenerationMode::RemoteImage);
    const bool needsRemote = isLlm || isRemoteImage;

    m_remoteOptionsLabel->setVisible(needsRemote);
    if (isLlm) {
        m_remoteOptionsLabel->setText(i18n("OpenAI 互換の Chat Completions エンドポイント (/v1/chat/completions) を指定します。API キーは保存しません。"));
        m_endpointEditor->setPlaceholderText(QStringLiteral("https://api.openai.com/v1/chat/completions"));
        m_modelEditor->setPlaceholderText(i18n("モデル名 (例: gpt-4o, o3-mini, deepseek-chat)"));

        const QString savedEndpoint = settings.value(QStringLiteral("AIIllustration/llmEndpoint"),
            settings.value(QStringLiteral("AIIllustration/endpoint"))).toString();
        const QString savedModel = settings.value(QStringLiteral("AIIllustration/llmModel"),
            settings.value(QStringLiteral("AIIllustration/model"))).toString();
        m_endpointEditor->setText(savedEndpoint.isEmpty() ? QStringLiteral("https://api.openai.com/v1/chat/completions") : savedEndpoint);
        m_modelEditor->setText(savedModel.isEmpty() ? QStringLiteral("gpt-4o") : savedModel);
    } else if (isRemoteImage) {
        m_remoteOptionsLabel->setText(i18n("OpenAI 互換の画像生成エンドポイント (/v1/images/generations) を指定します。API キーは保存しません。"));
        m_endpointEditor->setPlaceholderText(QStringLiteral("https://provider.example/v1/images/generations"));
        m_modelEditor->setPlaceholderText(i18n("画像モデル名 (例: dall-e-3)"));

        const QString savedEndpoint = settings.value(QStringLiteral("AIIllustration/imageEndpoint")).toString();
        const QString savedModel = settings.value(QStringLiteral("AIIllustration/imageModel")).toString();
        m_endpointEditor->setText(savedEndpoint.isEmpty() ? QStringLiteral("https://api.openai.com/v1/images/generations") : savedEndpoint);
        m_modelEditor->setText(savedModel.isEmpty() ? QStringLiteral("dall-e-3") : savedModel);
    }

    if (m_detailsToggleBtn) {
        m_detailsToggleBtn->setVisible(needsRemote);
    }
    if (m_detailsContainer && !needsRemote) {
        m_detailsContainer->setVisible(false);
        if (m_detailsToggleBtn) {
            m_detailsToggleBtn->setChecked(false);
            m_detailsToggleBtn->setText(i18n("▶ 詳細設定（エンドポイント / API キー）"));
        }
    }

    if (m_remoteForm) {
        m_remoteForm->setRowVisible(0, needsRemote);
        m_remoteForm->setRowVisible(1, needsRemote);
        m_remoteForm->setRowVisible(2, needsRemote);
        m_remoteForm->setRowVisible(3, isLlm);
    }
}


void KisAiIllustrationDocker::setBusy(bool busy)
{
    m_newCanvasButton->setEnabled(!busy);
    m_generateButton->setEnabled(!busy);
    m_modeCombo->setEnabled(!busy);
    m_cancelButton->setVisible(busy && !m_reply.isNull());
    m_progressBar->setVisible(busy);
}

bool KisAiIllustrationDocker::ensureCanvas()
{
    if (!m_mainWindow) {
        setStatus(i18n("メインウィンドウが利用できないため、生成できません。"), true);
        return false;
    }
    if (m_mainWindow->activeView()) {
        return true;
    }

    createCanvas();
    return m_mainWindow->activeView() != nullptr;
}

bool KisAiIllustrationDocker::addImageAsLayer(const QImage &sourceImage, const QString &layerName)
{
    if (!m_mainWindow || sourceImage.isNull()) {
        setStatus(i18n("生成結果をキャンバスに追加できません。"), true);
        return false;
    }

    KisView *view = m_mainWindow->activeView();
    if (!view || !view->image()) {
        setStatus(i18n("キャンバスが利用できないため、生成結果を追加できません。"), true);
        return false;
    }

    KisImageWSP image = view->image();
    const QRect bounds = image->bounds();
    if (bounds.isEmpty()) {
        setStatus(i18n("キャンバスの大きさが無効です。"), true);
        return false;
    }

    QImage canvasImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    canvasImage.fill(Qt::transparent);
    {
        QPainter painter(&canvasImage);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QImage scaled = sourceImage.scaled(bounds.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QPoint destination((bounds.width() - scaled.width()) / 2, (bounds.height() - scaled.height()) / 2);
        painter.drawImage(destination, scaled);
    }

    KisPaintLayerSP layer = new KisPaintLayer(image, layerName, OPACITY_OPAQUE_U8);
    layer->paintDevice()->convertFromQImage(canvasImage, nullptr);

    KisNodeSP aboveNode = view->currentNode();
    KisNodeSP parentNode = aboveNode ? aboveNode->parent() : image->root();
    if (!parentNode) {
        parentNode = image->root();
        aboveNode = KisNodeSP();
    }

    KisNodeCommandsAdapter adapter(m_mainWindow->viewManager());
    adapter.addNode(layer, parentNode, aboveNode);
    view->setCurrentNode(layer);
    return true;
}

QString KisAiIllustrationDocker::promptForLayerName(const QString &prompt) const
{
    QString title = KisAiIllustrationRenderer::normalizedPrompt(prompt);
    constexpr int maxLayerTitleLength = 56;
    if (title.size() > maxLayerTitleLength) {
        title.truncate(maxLayerTitleLength - 1);
        title += QChar(0x2026);
    }
    return title.isEmpty() ? i18n("AI Illustration") : i18n("AI: %1", title);
}

void KisAiIllustrationDocker::setStatus(const QString &message, bool isError)
{
    m_statusLabel->setText(message);
    m_statusLabel->setProperty("error", isError);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
}
