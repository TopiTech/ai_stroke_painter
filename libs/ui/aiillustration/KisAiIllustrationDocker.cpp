/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiIllustrationDocker.h"

#include "KisAiIllustrationRenderer.h"
#include "KisAiPromptAnalyzer.h"
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
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
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
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <algorithm>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <wincrypt.h>
#endif

namespace
{
constexpr qint64 MAX_REMOTE_RESPONSE_BYTES = 32LL * 1024 * 1024;
constexpr qint64 MAX_REMOTE_IMAGE_BYTES = 24LL * 1024 * 1024;
constexpr qint64 MAX_REMOTE_IMAGE_PIXELS = 24LL * 1024 * 1024;
constexpr int INITIAL_REQUEST_TIMEOUT_MS = 120'000;
constexpr int ACTIVITY_TIMEOUT_MS = 60'000;
constexpr int MAX_REQUEST_TIMEOUT_MS = 600'000;
constexpr int REMOTE_IMAGE_TIMEOUT_MS = 180'000;

const QString kDpapiApiKeyPrefix = QStringLiteral("dpapi:");

bool protectApiKeyForCurrentUser(const QString &apiKey, QString *protectedValue)
{
#if defined(Q_OS_WIN)
    if (!protectedValue || apiKey.isEmpty()) {
        return false;
    }

    QByteArray plainText = apiKey.toUtf8();
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plainText.size());
    input.pbData = reinterpret_cast<BYTE *>(plainText.data());

    DATA_BLOB encrypted{};
    const BOOL ok = CryptProtectData(&input,
                                     L"AI Stroke Painter API key",
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN,
                                     &encrypted);
    SecureZeroMemory(plainText.data(), plainText.size());
    if (!ok) {
        return false;
    }

    const QByteArray protectedBytes(reinterpret_cast<const char *>(encrypted.pbData),
                                    static_cast<int>(encrypted.cbData));
    SecureZeroMemory(encrypted.pbData, encrypted.cbData);
    LocalFree(encrypted.pbData);
    *protectedValue = kDpapiApiKeyPrefix + QString::fromLatin1(protectedBytes.toBase64());
    return true;
#else
    Q_UNUSED(apiKey);
    Q_UNUSED(protectedValue);
    return false;
#endif
}

bool unprotectApiKeyForCurrentUser(const QString &protectedValue, QString *apiKey)
{
#if defined(Q_OS_WIN)
    if (!apiKey || !protectedValue.startsWith(kDpapiApiKeyPrefix)) {
        return false;
    }

    QByteArray protectedBytes = QByteArray::fromBase64(protectedValue.mid(kDpapiApiKeyPrefix.size()).toLatin1());
    if (protectedBytes.isEmpty()) {
        return false;
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(protectedBytes.size());
    input.pbData = reinterpret_cast<BYTE *>(protectedBytes.data());
    DATA_BLOB plainText{};
    LPWSTR description = nullptr;
    const bool decrypted =
        CryptUnprotectData(&input, &description, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plainText);
    if (description) {
        LocalFree(description);
    }
    if (!decrypted) {
        return false;
    }

    const QByteArray plainBytes(reinterpret_cast<const char *>(plainText.pbData), static_cast<int>(plainText.cbData));
    SecureZeroMemory(plainText.pbData, plainText.cbData);
    LocalFree(plainText.pbData);
    *apiKey = QString::fromUtf8(plainBytes);
    return !apiKey->isEmpty();
#else
    Q_UNUSED(protectedValue);
    Q_UNUSED(apiKey);
    return false;
#endif
}

QString imageSizeText(const QSpinBox *widthSpin, const QSpinBox *heightSpin)
{
    return QString::number(widthSpin->value()) + QLatin1Char('x') + QString::number(heightSpin->value());
}

QString readSafeStoredEndpoint(QSettings &settings, const QString &primaryKey, const QString &legacyKey = QString())
{
    const QVariant fallbackValue = legacyKey.isEmpty() ? QVariant() : settings.value(legacyKey);
    const QString endpoint = settings.value(primaryKey, fallbackValue).toString().trimmed();
    if (endpoint.isEmpty()) {
        return endpoint;
    }

    QString validationError;
    if (KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &validationError)) {
        return endpoint;
    }

    settings.remove(primaryKey);
    if (!legacyKey.isEmpty()) {
        settings.remove(legacyKey);
    }
    return QString();
}

// QSettings values are user- or tool-editable; a corrupt or out-of-range persisted
// number must not widen a spin box range (Qt silently clamps to the widget range,
// so an explicit clamp keeps logic and UI in sync and predictable).
int readBoundedSetting(QSettings &settings, const QString &key, int defaultValue, int minValue, int maxValue)
{
    const int value = settings.value(key, defaultValue).toInt();
    return qBound(minValue, value, maxValue);
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
        "QWidget#aiIllustrationPanel { background: #11151e; color: #e2e8f0; }"
        "QWidget#aiIllustrationPanel QLabel { color: #cbd5e1; font-size: 12px; }"
        "QLabel#aiTitle { color: #f8fafc; font-size: 17px; font-weight: 700; }"
        "QLabel#aiSubtitle { color: #94a3b8; font-size: 11px; line-height: 1.4; }"
        "QLabel[class=\"aiCardTitle\"] { color: #7c8ba1; font-size: 11px; font-weight: 700; }"
        "QFrame[class=\"aiCard\"] { background: #171d29; border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 8px; }"
        "QPlainTextEdit, QLineEdit, QSpinBox, QComboBox {"
        " background: #0d1117; border: 1px solid #273142; border-radius: 6px; padding: 5px 8px; color: #f1f5f9; font-size: 12px; }"
        "QPlainTextEdit:focus, QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid #3b82f6; background: #0f141d; }"
        "QCheckBox { color: #cbd5e1; font-size: 12px; spacing: 6px; }"
        "QCheckBox:hover { color: #f1f5f9; }"
        "QPushButton#aiGenerateButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #4f46e5);"
        " color: #ffffff; border: none; border-radius: 6px; font-weight: 700; font-size: 13px; min-height: 36px; padding: 6px 14px; }"
        "QPushButton#aiGenerateButton:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:1 #6366f1); }"
        "QPushButton#aiGenerateButton:pressed { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1d4ed8, stop:1 #4338ca); }"
        "QPushButton#aiGenerateButton:disabled { background: #1f2736; color: #55657d; }"
        "QPushButton#aiSecondaryButton { background: #1f2737; color: #cbd5e1; border: 1px solid #344258;"
        " border-radius: 6px; min-height: 28px; padding: 4px 10px; font-size: 12px; }"
        "QPushButton#aiSecondaryButton:hover { background: #283348; color: #ffffff; border-color: #4b6282; }"
        "QPushButton#aiSecondaryButton:pressed { background: #151b27; }"
        "QPushButton[class=\"aiChipButton\"] { background: #1b2230; color: #94a3b8; border: 1px solid #2e3b50;"
        " border-radius: 11px; padding: 2px 8px; font-size: 11px; }"
        "QPushButton[class=\"aiChipButton\"]:hover { background: #253044; color: #38bdf8; border-color: #38bdf8; }"
        "QPushButton[class=\"aiChipButton\"]:pressed { background: #101520; }"
        "QPushButton[class=\"aiRatioButton\"] { background: #1b2230; color: #94a3b8; border: 1px solid #2e3b50;"
        " border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: 600; min-width: 32px; }"
        "QPushButton[class=\"aiRatioButton\"]:hover { background: #253044; color: #f1f5f9; border-color: #3b82f6; }"
        "QLabel#aiStatus { color: #94a3b8; padding: 2px 0px; font-size: 11px; }"
        "QLabel#aiStatus[error=\"true\"] { color: #f87171; font-weight: 600; }"
        "QFrame#aiRule { background: #273142; max-height: 1px; }"
        "QToolButton#aiToggleDetails { color: #7c8ba1; background: transparent; border: none; font-size: 11px; text-align: left; padding: 2px 0px; font-weight: 500; }"
        "QToolButton#aiToggleDetails:hover { color: #cbd5e1; }"
        "QProgressBar { background: #0d1117; border: 1px solid #273142; border-radius: 3px; max-height: 6px; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:1 #06b6d4); border-radius: 2px; }"
        "QPlainTextEdit#aiDebugLog { background: #080b10; border: 1px solid #1e293b; border-radius: 4px; font-family: Consolas, 'Courier New', monospace; font-size: 11px; color: #94a3b8; }"
        "QLabel#aiTestStatus { font-size: 11px; padding: 2px 0px; }"));

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(i18n("🎨 AI Stroke Painter"), panel);
    title->setObjectName(QStringLiteral("aiTitle"));
    title->setAccessibleName(i18n("AI Stroke Painter workspace"));
    layout->addWidget(title);

    auto *subtitle = new QLabel(i18n("描きたい情景を言葉で指定すると、AIが絵筆のストロークを生成してキャンバスを描画します。"), panel);
    subtitle->setObjectName(QStringLiteral("aiSubtitle"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto *rule = new QFrame(panel);
    rule->setObjectName(QStringLiteral("aiRule"));
    rule->setFrameShape(QFrame::HLine);
    layout->addWidget(rule);

    struct CardWidget {
        QFrame *frame;
        QVBoxLayout *layout;
    };
    auto createCard = [panel]() -> CardWidget {
        auto *card = new QFrame(panel);
        card->setProperty("class", "aiCard");
        card->setFrameShape(QFrame::StyledPanel);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 10, 10, 10);
        cardLayout->setSpacing(6);
        return {card, cardLayout};
    };

    // Card 1: Illustration Prompt
    CardWidget promptCard = createCard();
    auto *promptHeader = new QLabel(i18n("イラストの指示"), promptCard.frame);
    promptHeader->setProperty("class", "aiCardTitle");
    promptCard.layout->addWidget(promptHeader);

    // Quick chip buttons (grid layout prevents clipping when dock is narrow)
    auto *chipGrid = new QGridLayout();
    chipGrid->setSpacing(4);
    chipGrid->setContentsMargins(0, 0, 0, 0);
    const QList<QPair<QString, QString>> chips = {
        {i18n("美少女"), QStringLiteral("アニメ美少女のクローズアップポートレート、大きな輝く青い瞳、二重まぶた、繊細なまつ毛、さらさらの銀髪、柔らかい頬の赤み、天使の輪")},
        {i18n("桜風景"), QStringLiteral("壮大な富士山と満開の桜の木、夕暮れのグラデーション空、舞い散る花びら、伝統的な日本風景")},
        {i18n("サイバー"), QStringLiteral("ネオン輝くサイバーパンク高層ビル群、夜の摩天楼、雨に反射する光、ホログラム広告、近未来都市")},
        {i18n("浮世絵"), QStringLiteral("葛飾北斎風のダイナミックな大波、力強い水しぶき、伝統的な青と白のコントラスト、富士山")},
        {i18n("黒猫"), QStringLiteral("月夜に佇む美しい黒猫、金色に輝く瞳、繊細なヒゲ、神秘的な夜空と星の光")},
    };
    int col = 0;
    int row = 0;
    for (const auto &chip : chips) {
        auto *chipBtn = new QPushButton(chip.first, promptCard.frame);
        chipBtn->setProperty("class", "aiChipButton");
        chipBtn->setFocusPolicy(Qt::StrongFocus);
        chipBtn->setAccessibleName(i18n("Prompt preset %1", chip.first));
        chipBtn->setCursor(Qt::PointingHandCursor);
        connect(chipBtn, &QPushButton::clicked, this, [this, prompt = chip.second] {
            if (m_promptEditor) {
                m_promptEditor->setPlainText(prompt);
            }
            focusPrompt();
        });
        chipGrid->addWidget(chipBtn, row, col);
        col++;
        if (col >= 3) {
            col = 0;
            row++;
        }
    }
    promptCard.layout->addLayout(chipGrid);

    m_presetCombo = new QComboBox(promptCard.frame);
    m_presetCombo->addItem(i18n("プリセット一覧から選択…"), QString());
    m_presetCombo->addItem(i18n("👤 美少女アニメ顔 (Anime Girl)"), QStringLiteral("アニメ美少女のクローズアップポートレート、大きな輝く青い瞳、二重まぶた、繊細なまつ毛、さらさらの銀髪、柔らかい頬の赤み、天使の輪"));
    m_presetCombo->addItem(i18n("🌸 山と桜の風景 (Mountain & Sakura)"), QStringLiteral("壮大な富士山と満開の桜の木、夕暮れのグラデーション空、舞い散る花びら、伝統的な日本風景"));
    m_presetCombo->addItem(i18n("🏙️ サイバーパンク都市 (Cyberpunk City)"), QStringLiteral("ネオン輝くサイバーパンク高層ビル群、夜の摩天楼、雨に反射する光、ホログラム広告、近未来都市"));
    m_presetCombo->addItem(i18n("🌊 浮世絵風の大波 (Ukiyo-e Great Wave)"), QStringLiteral("葛飾北斎風のダイナミックな大波、力強い水しぶき、伝統的な青と白のコントラスト、富士山"));
    m_presetCombo->addItem(i18n("✨ 魔法陣とルーン (Magic Circle)"), QStringLiteral("神秘的な幾何学魔法陣、古代ルーン文字、輝くエネルギー粒子、神聖な光のエフェクト"));
    m_presetCombo->addItem(i18n("🐱 幻想的な黒猫 (Mystical Black Cat)"), QStringLiteral("月夜に佇む美しい黒猫、金色に輝く瞳、繊細なヒゲ、神秘的な夜空と星の光"));
    m_presetCombo->addItem(i18n("🌹 バラの花束 (Botanical Rose)"), QStringLiteral("咲き誇る深紅のバラの花束、重なり合う繊細な花びら、朝露のハイライト、瑞々しい緑の葉"));
    m_presetCombo->setAccessibleName(i18n("Quick illustration presets"));
    promptCard.layout->addWidget(m_presetCombo);

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        const QString text = m_presetCombo->itemData(idx).toString();
        if (!text.isEmpty()) {
            if (m_promptEditor) {
                m_promptEditor->setPlainText(text);
            }
            focusPrompt();
        }
    });

    m_promptEditor = new QPlainTextEdit(promptCard.frame);
    m_promptEditor->setTabChangesFocus(true);
    m_promptEditor->setPlaceholderText(i18n("例: 雨上がりの夜、青い光に包まれた猫と花のある静かな路地 (Ctrl+Enter で生成)"));
    m_promptEditor->setMinimumHeight(75);
    m_promptEditor->setMaximumHeight(130);
    m_promptEditor->setAccessibleName(i18n("Illustration prompt"));
    m_promptEditor->installEventFilter(this);
    promptCard.layout->addWidget(m_promptEditor);

    layout->addWidget(promptCard.frame);

    // Card 2: Canvas Settings
    CardWidget canvasCard = createCard();
    auto *canvasHeaderRow = new QHBoxLayout();
    auto *canvasHeader = new QLabel(i18n("キャンバスサイズ"), canvasCard.frame);
    canvasHeader->setProperty("class", "aiCardTitle");
    canvasHeaderRow->addWidget(canvasHeader);
    canvasHeaderRow->addStretch(1);

    // Aspect ratio buttons: 1:1, 16:9, 9:16, 4:3
    const QList<QPair<QString, QPair<int, int>>> ratios = {
        {QStringLiteral("1:1"), {1024, 1024}},
        {QStringLiteral("16:9"), {1280, 720}},
        {QStringLiteral("9:16"), {720, 1280}},
        {QStringLiteral("4:3"), {1024, 768}},
    };
    for (const auto &ratio : ratios) {
        auto *ratioBtn = new QPushButton(ratio.first, canvasCard.frame);
        ratioBtn->setProperty("class", "aiRatioButton");
        ratioBtn->setFocusPolicy(Qt::StrongFocus);
        ratioBtn->setAccessibleName(i18n("Aspect ratio %1", ratio.first));
        ratioBtn->setCursor(Qt::PointingHandCursor);
        const int w = ratio.second.first;
        const int h = ratio.second.second;
        connect(ratioBtn, &QPushButton::clicked, this, [this, w, h] {
            if (m_widthSpin && m_heightSpin) {
                m_widthSpin->setValue(w);
                m_heightSpin->setValue(h);
            }
        });
        canvasHeaderRow->addWidget(ratioBtn);
    }
    canvasCard.layout->addLayout(canvasHeaderRow);

    auto *canvasRow = new QHBoxLayout();
    m_widthSpin = new QSpinBox(canvasCard.frame);
    m_widthSpin->setRange(256, 4096);
    m_widthSpin->setSingleStep(64);
    m_widthSpin->setValue(1024);
    m_widthSpin->setPrefix(i18n("幅: "));
    m_widthSpin->setSuffix(QStringLiteral(" px"));
    m_widthSpin->setAccessibleName(i18n("Canvas width"));
    m_heightSpin = new QSpinBox(canvasCard.frame);
    m_heightSpin->setRange(256, 4096);
    m_heightSpin->setSingleStep(64);
    m_heightSpin->setValue(1024);
    m_heightSpin->setPrefix(i18n("高さ: "));
    m_heightSpin->setSuffix(QStringLiteral(" px"));
    m_heightSpin->setAccessibleName(i18n("Canvas height"));
    canvasRow->addWidget(m_widthSpin);
    canvasRow->addWidget(m_heightSpin);
    canvasCard.layout->addLayout(canvasRow);

    m_newCanvasButton = new QPushButton(i18n("＋ 新しいキャンバスを作成"), canvasCard.frame);
    m_newCanvasButton->setObjectName(QStringLiteral("aiSecondaryButton"));
    m_newCanvasButton->setFocusPolicy(Qt::StrongFocus);
    m_newCanvasButton->setAccessibleName(i18n("Create new canvas"));
    m_newCanvasButton->setAccessibleDescription(i18n("指定した大きさの AI イラスト用キャンバスを作成します。"));
    canvasCard.layout->addWidget(m_newCanvasButton);

    layout->addWidget(canvasCard.frame);

    // Card 3: Generation Engine
    CardWidget engineCard = createCard();
    auto *engineHeader = new QLabel(i18n("生成エンジン"), engineCard.frame);
    engineHeader->setProperty("class", "aiCardTitle");
    engineCard.layout->addWidget(engineHeader);

    m_modeCombo = new QComboBox(engineCard.frame);
    m_modeCombo->addItem(i18n("LLM 座標ストローク描画 (Chat Completions)"), static_cast<int>(GenerationMode::LlmStrokes));
    m_modeCombo->addItem(i18n("ローカル座標ストローク描画"), static_cast<int>(GenerationMode::LocalStrokes));
    m_modeCombo->addItem(i18n("画像モデル API (DALL-E)"), static_cast<int>(GenerationMode::RemoteImage));
    m_modeCombo->addItem(i18n("ローカル・コンセプトスケッチ"), static_cast<int>(GenerationMode::LocalConcept));
    m_modeCombo->setAccessibleName(i18n("Generation source"));
    engineCard.layout->addWidget(m_modeCombo);

    m_detailsToggleBtn = new QToolButton(engineCard.frame);
    m_detailsToggleBtn->setObjectName(QStringLiteral("aiToggleDetails"));
    m_detailsToggleBtn->setCheckable(true);
    m_detailsToggleBtn->setFocusPolicy(Qt::StrongFocus);
    m_detailsToggleBtn->setAccessibleName(i18n("Toggle detailed settings"));
    m_detailsToggleBtn->setText(i18n("▶ 詳細設定（エンドポイント / API キー）"));
    m_detailsToggleBtn->setCursor(Qt::PointingHandCursor);
    engineCard.layout->addWidget(m_detailsToggleBtn);

    m_detailsContainer = new QWidget(engineCard.frame);
    auto *detailsLayout = new QVBoxLayout(m_detailsContainer);
    detailsLayout->setContentsMargins(0, 4, 0, 0);
    detailsLayout->setSpacing(6);

    m_remoteOptionsLabel = new QLabel(i18n("OpenAI 互換の Chat Completions エンドポイントを指定します。Vision対応モデルを推奨します（画像非対応時はテキストにフォールバック）。"), m_detailsContainer);
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
    m_apiKeyEditor->setPlaceholderText(i18n("API キー (sk-...)"));
    m_apiKeyEditor->setAccessibleName(i18n("API key"));

    m_saveApiKeyCheck = new QCheckBox(i18n("🔑 API キーをこの端末に保存する"), m_detailsContainer);
    m_saveApiKeyCheck->setChecked(false);
#if defined(Q_OS_WIN)
    m_saveApiKeyCheck->setToolTip(
        i18n("チェックを入れると、API キーを Windows の現在のユーザー向け保護で保存し、次回起動時に自動入力します。"));
#else
    m_saveApiKeyCheck->setEnabled(false);
    m_saveApiKeyCheck->setToolTip(
        i18n("このプラットフォームでは API キーの保存は利用できません。リクエストごとに入力してください。"));
#endif

    m_strokeBudgetLabel = new QLabel(i18n("ストローク予算"), m_detailsContainer);
    m_strokeBudgetSpin = new QSpinBox(m_detailsContainer);
    m_strokeBudgetSpin->setRange(20, 2000);
    m_strokeBudgetSpin->setValue(500);
    m_strokeBudgetSpin->setSingleStep(50);
    m_strokeBudgetSpin->setSuffix(i18n(" 本"));
    m_strokeBudgetSpin->setAccessibleName(i18n("Stroke budget"));

    m_temperatureSpin = new QDoubleSpinBox(m_detailsContainer);
    m_temperatureSpin->setRange(0.0, 2.0);
    m_temperatureSpin->setSingleStep(0.05);
    m_temperatureSpin->setValue(0.50); // A6: Stable 0.50 default for geometry precision
    m_temperatureSpin->setDecimals(2);
    m_temperatureSpin->setAccessibleName(i18n("Sampling temperature"));
    m_temperatureSpin->setToolTip(i18n("サンプリング温度 (0.0=確定的/構造維持, 0.5=推奨安定, 1.0=創造的)"));

    m_topPSpin = new QDoubleSpinBox(m_detailsContainer);
    m_topPSpin->setRange(0.05, 1.0);
    m_topPSpin->setSingleStep(0.05);
    m_topPSpin->setValue(1.0);
    m_topPSpin->setDecimals(2);
    m_topPSpin->setAccessibleName(i18n("Top-P sampling"));
    m_topPSpin->setToolTip(i18n("Top-P (核サンプリングの累積確率閾値)"));

    // A2: Flats trapping spinbox
    m_trappingPxSpin = new QDoubleSpinBox(m_detailsContainer);
    m_trappingPxSpin->setRange(0.0, 10.0);
    m_trappingPxSpin->setSingleStep(0.5);
    m_trappingPxSpin->setValue(1.5);
    m_trappingPxSpin->setDecimals(1);
    m_trappingPxSpin->setSuffix(i18n(" px"));
    m_trappingPxSpin->setAccessibleName(i18n("Flats trapping width"));
    m_trappingPxSpin->setToolTip(i18n("線画と塗りの境界の白抜けを防ぐトラッピング（塗り拡張）幅 (px)"));

    m_maxTokensSpin = new QSpinBox(m_detailsContainer);
    m_maxTokensSpin->setRange(0, 65536);
    m_maxTokensSpin->setSingleStep(1024);
    m_maxTokensSpin->setValue(0);
    m_maxTokensSpin->setSpecialValueText(i18n("自動計算"));
    m_maxTokensSpin->setSuffix(i18n(" トークン"));
    m_maxTokensSpin->setAccessibleName(i18n("Maximum tokens"));
    m_maxTokensSpin->setToolTip(i18n("最大生成トークン数 (0でストローク予算から自動計算)"));

    m_timeoutSecSpin = new QSpinBox(m_detailsContainer);
    m_timeoutSecSpin->setRange(10, 600);
    m_timeoutSecSpin->setSingleStep(10);
    m_timeoutSecSpin->setValue(90);
    m_timeoutSecSpin->setSuffix(i18n(" 秒"));
    m_timeoutSecSpin->setAccessibleName(i18n("Request timeout seconds"));
    m_timeoutSecSpin->setToolTip(i18n("LLM リクエストの初期応答タイムアウト時間"));

    m_maxRetriesSpin = new QSpinBox(m_detailsContainer);
    m_maxRetriesSpin->setRange(0, 5);
    m_maxRetriesSpin->setValue(2);
    m_maxRetriesSpin->setSuffix(i18n(" 回"));
    m_maxRetriesSpin->setAccessibleName(i18n("Maximum retry count"));
    m_maxRetriesSpin->setToolTip(i18n("通信一時エラー時や品質自己修復時の自動リトライ最大回数"));

    m_jsonModeCombo = new QComboBox(m_detailsContainer);
    m_jsonModeCombo->addItem(i18n("自動判定 (エンドポイント依存)"), 0);
    m_jsonModeCombo->addItem(i18n("構造化出力 (json_schema)"), 1);
    m_jsonModeCombo->addItem(i18n("強制 (json_object)"), 2);
    m_jsonModeCombo->addItem(i18n("無効 (プロンプトのみで指示)"), 3);
    m_jsonModeCombo->setAccessibleName(i18n("JSON response mode"));
    m_jsonModeCombo->setToolTip(i18n("APIの response_format (json_schema / json_object) を利用するかどうか"));

    m_compositionPlanCheck = new QCheckBox(i18n("2段階構図生成 (Composition Plan)"), m_detailsContainer);
    m_compositionPlanCheck->setChecked(false);
    m_compositionPlanCheck->setToolTip(i18n("複雑な構図向けに、事前に構図計画を策定してから実ストロークを生成します。"));

    m_reasoningEffortCombo = new QComboBox(m_detailsContainer);
    m_reasoningEffortCombo->addItem(i18n("指定なし (デフォルト)"), QStringLiteral(""));
    m_reasoningEffortCombo->addItem(i18n("Low (高速・低思考)"), QStringLiteral("low"));
    m_reasoningEffortCombo->addItem(i18n("Medium (標準思考)"), QStringLiteral("medium"));
    m_reasoningEffortCombo->addItem(i18n("High (深層思考・高品質)"), QStringLiteral("high"));
    m_reasoningEffortCombo->setAccessibleName(i18n("Reasoning effort"));
    m_reasoningEffortCombo->setToolTip(i18n("推論モデル（o1, o3, etc.）の reasoning_effort レベル"));

    m_customInstructionsEdit = new QPlainTextEdit(m_detailsContainer);
    m_customInstructionsEdit->setTabChangesFocus(true);
    m_customInstructionsEdit->setAccessibleName(i18n("Custom instructions"));
    m_customInstructionsEdit->setPlaceholderText(i18n("システムプロンプトに追加する独自の作画指示・画風・禁止事項"));
    m_customInstructionsEdit->setMaximumHeight(70);

    remoteForm->addRow(i18n("エンドポイント"), m_endpointEditor);
    remoteForm->addRow(i18n("モデル"), m_modelEditor);
    remoteForm->addRow(i18n("API キー"), m_apiKeyEditor);
    remoteForm->addRow(QString(), m_saveApiKeyCheck);
    remoteForm->addRow(m_strokeBudgetLabel, m_strokeBudgetSpin);
    remoteForm->addRow(i18n("Temperature"), m_temperatureSpin);
    remoteForm->addRow(i18n("Top-P"), m_topPSpin);
    remoteForm->addRow(i18n("トラッピング幅"), m_trappingPxSpin);
    remoteForm->addRow(i18n("最大トークン"), m_maxTokensSpin);
    remoteForm->addRow(i18n("タイムアウト"), m_timeoutSecSpin);
    remoteForm->addRow(i18n("自動リトライ"), m_maxRetriesSpin);
    remoteForm->addRow(i18n("JSONモード"), m_jsonModeCombo);
    remoteForm->addRow(QString(), m_compositionPlanCheck);
    remoteForm->addRow(i18n("推論エフォート"), m_reasoningEffortCombo);
    remoteForm->addRow(i18n("追加指示"), m_customInstructionsEdit);
    detailsLayout->addLayout(remoteForm);

    auto *settingsBtnRow = new QHBoxLayout();
    settingsBtnRow->setSpacing(6);
    m_testConnectionButton = new QPushButton(i18n("🔌 接続テスト"), m_detailsContainer);
    m_testConnectionButton->setObjectName(QStringLiteral("aiSecondaryButton"));
    m_testConnectionButton->setFocusPolicy(Qt::StrongFocus);
    m_testConnectionButton->setAccessibleName(i18n("Test connection"));
    m_testConnectionButton->setCursor(Qt::PointingHandCursor);
    m_testConnectionButton->setToolTip(i18n("入力されたエンドポイント・モデル・APIキーで導通テストを行います。"));

    m_saveSettingsButton = new QPushButton(i18n("💾 設定を保存"), m_detailsContainer);
    m_saveSettingsButton->setObjectName(QStringLiteral("aiSecondaryButton"));
    m_saveSettingsButton->setFocusPolicy(Qt::StrongFocus);
    m_saveSettingsButton->setAccessibleName(i18n("Save settings"));
    m_saveSettingsButton->setCursor(Qt::PointingHandCursor);
    m_saveSettingsButton->setToolTip(i18n("現在のエンドポイント、モデル名、APIキー、各生成設定を保存します。"));

    settingsBtnRow->addWidget(m_testConnectionButton);
    settingsBtnRow->addWidget(m_saveSettingsButton);
    detailsLayout->addLayout(settingsBtnRow);

    m_testConnectionStatusLabel = new QLabel(m_detailsContainer);
    m_testConnectionStatusLabel->setObjectName(QStringLiteral("aiTestStatus"));
    m_testConnectionStatusLabel->setWordWrap(true);
    m_testConnectionStatusLabel->setVisible(false);
    detailsLayout->addWidget(m_testConnectionStatusLabel);

    m_detailsContainer->setVisible(false);
    engineCard.layout->addWidget(m_detailsContainer);

    m_debugModeCheck = new QCheckBox(i18n("🐛 デバッグモード（LLM送受信ログを表示）"), engineCard.frame);
    m_debugModeCheck->setFocusPolicy(Qt::StrongFocus);
    m_debugModeCheck->setAccessibleName(i18n("Debug mode"));
    m_debugModeCheck->setToolTip(i18n("LLMへのリクエスト・レスポンス・エラー・パース等の詳細ログを表示します。"));
    m_debugModeCheck->setCursor(Qt::PointingHandCursor);
    engineCard.layout->addWidget(m_debugModeCheck);

    layout->addWidget(engineCard.frame);

    connect(m_detailsToggleBtn, &QToolButton::toggled, this, [this](bool checked) {
        m_detailsContainer->setVisible(checked);
        m_detailsToggleBtn->setText(checked ? i18n("▼ 詳細設定（エンドポイント / API キー）")
                                            : i18n("▶ 詳細設定（エンドポイント / API キー）"));
    });

    // Card 3.5: Goal Mode Settings
    CardWidget goalCard = createCard();
    auto *goalHeader = new QLabel(i18n("🎯 自律多段階作画 (Goal Mode)"), goalCard.frame);
    goalHeader->setProperty("class", "aiCardTitle");
    goalCard.layout->addWidget(goalHeader);

    m_goalModeCheck = new QCheckBox(i18n("Goalモード（自律多段階作画）を有効にする"), goalCard.frame);
    m_goalModeCheck->setFocusPolicy(Qt::StrongFocus);
    m_goalModeCheck->setAccessibleName(i18n("Autonomous goal mode"));
    m_goalModeCheck->setToolTip(i18n("下地・陰影・線画・ハイライト等を段階的に自律作画し、完成度を高めます。"));
    m_goalModeCheck->setCursor(Qt::PointingHandCursor);
    goalCard.layout->addWidget(m_goalModeCheck);

    auto *goalOptionsWidget = new QWidget(goalCard.frame);
    auto *goalOptionsLayout = new QFormLayout(goalOptionsWidget);
    goalOptionsLayout->setContentsMargins(0, 4, 0, 0);
    goalOptionsLayout->setSpacing(6);

    m_goalStepsSpin = new QSpinBox(goalOptionsWidget);
    m_goalStepsSpin->setRange(2, 6);
    m_goalStepsSpin->setValue(4);
    m_goalStepsSpin->setSuffix(i18n(" 段階"));
    m_goalStepsSpin->setAccessibleName(i18n("Goal mode step count"));
    goalOptionsLayout->addRow(i18n("作画ステップ数"), m_goalStepsSpin);

    m_artStyleCombo = new QComboBox(goalOptionsWidget);
    m_artStyleCombo->addItem(i18n("🎨 おまかせ (Auto)"), static_cast<int>(KisAiPromptAnalyzer::ArtStyle::General));
    m_artStyleCombo->addItem(i18n("✨ アニメセル画 (Anime Cel)"), static_cast<int>(KisAiPromptAnalyzer::ArtStyle::AnimeCel));
    m_artStyleCombo->addItem(i18n("💧 透明水彩 (Watercolor)"), static_cast<int>(KisAiPromptAnalyzer::ArtStyle::Watercolor));
    m_artStyleCombo->addItem(i18n("🖌️ 厚塗り・油彩 (Impasto)"), static_cast<int>(KisAiPromptAnalyzer::ArtStyle::Impasto));
    m_artStyleCombo->addItem(i18n("✒️ マンガ・インク (Ink Sketch)"), static_cast<int>(KisAiPromptAnalyzer::ArtStyle::InkSketch));
    m_artStyleCombo->addItem(i18n("⚡ サイバーネオン (Cyber Neon)"), static_cast<int>(KisAiPromptAnalyzer::ArtStyle::CyberNeon));
    m_artStyleCombo->setAccessibleName(i18n("Art style"));
    goalOptionsLayout->addRow(i18n("画風スタイル"), m_artStyleCombo);

    m_pausePerStepCheck = new QCheckBox(i18n("段階ごとに一時停止（手動加筆・確認を待つ）"), goalOptionsWidget);
    m_pausePerStepCheck->setFocusPolicy(Qt::StrongFocus);
    m_pausePerStepCheck->setAccessibleName(i18n("Pause after each step"));
    m_pausePerStepCheck->setToolTip(i18n("各ステップ完了時に一時停止し、Kritaのブラシで自由に加筆してから次のステップへ進めます。"));
    m_pausePerStepCheck->setCursor(Qt::PointingHandCursor);
    goalOptionsLayout->addRow(QString(), m_pausePerStepCheck);

    goalOptionsWidget->setVisible(false);
    goalCard.layout->addWidget(goalOptionsWidget);

    layout->addWidget(goalCard.frame);

    // Goal Inspector Card
    m_goalInspectorCard = new QFrame(panel);
    m_goalInspectorCard->setProperty("class", "aiCard");
    m_goalInspectorCard->setFrameShape(QFrame::StyledPanel);
    auto *inspectorLayout = new QVBoxLayout(m_goalInspectorCard);
    inspectorLayout->setContentsMargins(10, 10, 10, 10);
    inspectorLayout->setSpacing(6);

    auto *inspectorTitle = new QLabel(i18n("🎯 Goal作画インスペクター"), m_goalInspectorCard);
    inspectorTitle->setProperty("class", "aiCardTitle");
    inspectorLayout->addWidget(inspectorTitle);

    m_goalPhaseLabel = new QLabel(i18n("待機中"), m_goalInspectorCard);
    m_goalPhaseLabel->setStyleSheet(QStringLiteral("font-weight: 700; color: #38bdf8; font-size: 13px;"));
    m_goalPhaseLabel->setWordWrap(true);
    inspectorLayout->addWidget(m_goalPhaseLabel);

    m_agentFocusLabel = new QLabel(i18n("🎯 着目領域: 待機中"), m_goalInspectorCard);
    m_agentFocusLabel->setStyleSheet(QStringLiteral("font-weight: 600; color: #a5b4fc; font-size: 11px;"));
    m_agentFocusLabel->setWordWrap(true);
    inspectorLayout->addWidget(m_agentFocusLabel);

    m_readinessBar = new QProgressBar(m_goalInspectorCard);
    m_readinessBar->setRange(0, 100);
    m_readinessBar->setValue(0);
    m_readinessBar->setFormat(i18n("自律完成度: %p%"));
    m_readinessBar->setAlignment(Qt::AlignCenter);
    m_readinessBar->setStyleSheet(QStringLiteral(
        "QProgressBar { background: #0d1117; border: 1px solid #273142; border-radius: 4px; height: 16px; text-align: center; color: #f8fafc; font-size: 10px; font-weight: 600; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #38bdf8, stop:1 #818cf8); border-radius: 3px; }"));
    inspectorLayout->addWidget(m_readinessBar);

    m_critiqueLabel = new QLabel(i18n("AIの視覚批評・自己分析がここに表示されます。"), m_goalInspectorCard);
    m_critiqueLabel->setWordWrap(true);
    m_critiqueLabel->setStyleSheet(QStringLiteral(
        "background: #0d1117; color: #94a3b8; border: 1px solid #273142; border-left: 3px solid #38bdf8; border-radius: 4px; padding: 6px 8px; font-size: 11px;"));
    inspectorLayout->addWidget(m_critiqueLabel);

    auto *stepBtnRow = new QHBoxLayout();
    stepBtnRow->setSpacing(6);
    m_nextStepButton = new QPushButton(i18n("▶ 次のステップへ進む"), m_goalInspectorCard);
    m_nextStepButton->setObjectName(QStringLiteral("aiGenerateButton"));
    m_nextStepButton->setFocusPolicy(Qt::StrongFocus);
    m_nextStepButton->setAccessibleName(i18n("Advance to next step"));
    m_nextStepButton->setToolTip(i18n("現在のステップを完了し、次のステップへ進みます (Ctrl+Enter)"));
    m_nextStepButton->setCursor(Qt::PointingHandCursor);
    m_nextStepButton->setVisible(false);

    m_finishGoalButton = new QPushButton(i18n("🏁 ここで完成"), m_goalInspectorCard);
    m_finishGoalButton->setObjectName(QStringLiteral("aiSecondaryButton"));
    m_finishGoalButton->setFocusPolicy(Qt::StrongFocus);
    m_finishGoalButton->setAccessibleName(i18n("Finish goal mode"));
    m_finishGoalButton->setCursor(Qt::PointingHandCursor);
    m_finishGoalButton->setVisible(false);

    stepBtnRow->addWidget(m_nextStepButton, 1);
    stepBtnRow->addWidget(m_finishGoalButton);
    inspectorLayout->addLayout(stepBtnRow);

    m_goalInspectorCard->setVisible(false);
    layout->addWidget(m_goalInspectorCard);

    connect(m_goalModeCheck, &QCheckBox::toggled, this, [this, goalOptionsWidget](bool checked) {
        goalOptionsWidget->setVisible(checked);
        if (m_generateButton) {
            m_generateButton->setText(checked ? i18n("🎯 Goal作画を開始") : i18n("🎨 生成してレイヤーに追加"));
        }
    });
    connect(m_nextStepButton, &QPushButton::clicked, this, &KisAiIllustrationDocker::advanceGoalStep);
    connect(m_finishGoalButton, &QPushButton::clicked, this, [this] { finishGoalMode(true); });

    // Card 4: Action & Preview
    CardWidget actionCard = createCard();

    auto *buttonRow = new QHBoxLayout();
    m_generateButton = new QPushButton(i18n("🎨 生成してレイヤーに追加"), actionCard.frame);
    m_generateButton->setObjectName(QStringLiteral("aiGenerateButton"));
    m_generateButton->setFocusPolicy(Qt::StrongFocus);
    m_generateButton->setAccessibleName(i18n("Generate illustration"));
    m_generateButton->setAccessibleDescription(i18n("プロンプトからイラストを生成し、現在のキャンバスに新しいレイヤーを追加します。"));
    m_cancelButton = new QPushButton(i18n("中止"), actionCard.frame);
    m_cancelButton->setObjectName(QStringLiteral("aiSecondaryButton"));
    m_cancelButton->setFocusPolicy(Qt::StrongFocus);
    m_cancelButton->setAccessibleName(i18n("Cancel generation"));
    m_cancelButton->setVisible(false);
    buttonRow->addWidget(m_generateButton, 1);
    buttonRow->addWidget(m_cancelButton);
    actionCard.layout->addLayout(buttonRow);

    m_progressBar = new QProgressBar(actionCard.frame);
    m_progressBar->setRange(0, 0);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(6);
    m_progressBar->setVisible(false);
    actionCard.layout->addWidget(m_progressBar);

    m_statusLabel = new QLabel(i18n("キャンバスがない場合は、生成時に新しい AI キャンバスを作成します。"), actionCard.frame);
    m_statusLabel->setObjectName(QStringLiteral("aiStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAccessibleName(i18n("Generation status"));
    actionCard.layout->addWidget(m_statusLabel);

    m_previewLabel = new QLabel(i18n("生成結果のプレビューはここに表示されます。"), actionCard.frame);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setWordWrap(true);
    m_previewLabel->setMinimumHeight(60);
    m_previewLabel->setStyleSheet(QStringLiteral("background: #0d1117; border: 1px solid #273142; border-radius: 6px; color: #7f95b5; padding: 6px; font-size: 11px;"));
    m_previewLabel->setAccessibleName(i18n("Generation preview"));
    actionCard.layout->addWidget(m_previewLabel);

    layout->addWidget(actionCard.frame);

    // Card 5: Debug Log Card
    CardWidget debugCardWidget = createCard();
    m_debugCard = debugCardWidget.frame;
    m_debugCard->setObjectName(QStringLiteral("aiDebugCard"));

    auto *debugHeaderRow = new QHBoxLayout();
    auto *debugTitle = new QLabel(i18n("🐛 デバッグログ"), m_debugCard);
    debugTitle->setProperty("class", "aiCardTitle");
    debugHeaderRow->addWidget(debugTitle);
    debugHeaderRow->addStretch(1);

    m_copyLogButton = new QPushButton(i18n("📋 コピー"), m_debugCard);
    m_copyLogButton->setProperty("class", "aiChipButton");
    m_copyLogButton->setFocusPolicy(Qt::StrongFocus);
    m_copyLogButton->setAccessibleName(i18n("Copy debug log"));
    m_copyLogButton->setCursor(Qt::PointingHandCursor);

    m_clearLogButton = new QPushButton(i18n("🗑️ クリア"), m_debugCard);
    m_clearLogButton->setProperty("class", "aiChipButton");
    m_clearLogButton->setFocusPolicy(Qt::StrongFocus);
    m_clearLogButton->setAccessibleName(i18n("Clear debug log"));
    m_clearLogButton->setCursor(Qt::PointingHandCursor);

    debugHeaderRow->addWidget(m_copyLogButton);
    debugHeaderRow->addWidget(m_clearLogButton);
    debugCardWidget.layout->addLayout(debugHeaderRow);

    m_debugLogText = new QPlainTextEdit(m_debugCard);
    m_debugLogText->setObjectName(QStringLiteral("aiDebugLog"));
    m_debugLogText->setReadOnly(true);
    m_debugLogText->setMinimumHeight(140);
    m_debugLogText->setMaximumHeight(260);
    m_debugLogText->setLineWrapMode(QPlainTextEdit::NoWrap);
    debugCardWidget.layout->addWidget(m_debugLogText);

    m_debugCard->setVisible(false);
    layout->addWidget(m_debugCard);

    layout->addStretch(1);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidget(panel);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWidget(scrollArea);

    // Restore persisted settings before wiring the auto-save handlers below.
    // loadSettings() assigns widget values, and if the handlers were already
    // connected those assignments would immediately write still-default values
    // back over the user's stored settings.
    loadSettings();

    connect(m_newCanvasButton, &QPushButton::clicked, this, [this] { createCanvas(); });
    connect(m_generateButton, &QPushButton::clicked, this, [this] { generateIllustration(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] { cancelRemoteRequest(); });
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        updateModeUi();
        saveSettings();
    });

    connect(m_testConnectionButton, &QPushButton::clicked, this, &KisAiIllustrationDocker::testLlmConnection);
    connect(m_saveSettingsButton, &QPushButton::clicked, this, [this] {
        saveSettings();
        setStatus(i18n("設定を保存しました。"));
    });
    connect(m_debugModeCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_debugCard) {
            m_debugCard->setVisible(checked);
        }
        saveSettings();
    });
    connect(m_copyLogButton, &QPushButton::clicked, this, &KisAiIllustrationDocker::copyDebugLog);
    connect(m_clearLogButton, &QPushButton::clicked, this, &KisAiIllustrationDocker::clearDebugLog);

    connect(m_endpointEditor, &QLineEdit::editingFinished, this, [this] { saveSettings(); });
    connect(m_modelEditor, &QLineEdit::editingFinished, this, [this] { saveSettings(); });
    connect(m_saveApiKeyCheck, &QCheckBox::toggled, this, [this] { saveSettings(); });
    connect(m_widthSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_heightSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_strokeBudgetSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_goalModeCheck, &QCheckBox::toggled, this, [this] { saveSettings(); });
    connect(m_goalStepsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_artStyleCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { saveSettings(); });
    connect(m_pausePerStepCheck, &QCheckBox::toggled, this, [this] { saveSettings(); });
    connect(m_temperatureSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_topPSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_maxTokensSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_maxRetriesSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_timeoutSecSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
    connect(m_jsonModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { saveSettings(); });
    connect(m_reasoningEffortCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { saveSettings(); });
    connect(m_customInstructionsEdit, &QPlainTextEdit::textChanged, this, [this] { saveSettings(); });

    updateModeUi();
}

KisAiIllustrationDocker::~KisAiIllustrationDocker()
{
    cancelRetry();
    clearInFlightApiKey();
    saveSettings();
    if (!m_goalApiKey.isEmpty()) {
        m_goalApiKey.fill(QLatin1Char('\0'));
        m_goalApiKey.clear();
    }
    if (m_testReply) {
        m_testReply->disconnect(this);
        m_testReply->abort();
        m_testReply->deleteLater();
        m_testReply = nullptr;
    }
    m_testResponseBuffer.clear();
    m_testResponseTooLarge = false;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_responseBuffer.clear();
}

bool KisAiIllustrationDocker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_promptEditor && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->modifiers() & Qt::ControlModifier) &&
            (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)) {
            if (m_goalModeActive && m_waitingForUserStepAdvance) {
                advanceGoalStep();
            } else {
                generateIllustration();
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape && (m_reply || m_goalModeActive || m_testReply || (m_retryTimer && m_retryTimer->isActive()))) {
            cancelRemoteRequest();
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void KisAiIllustrationDocker::keyPressEvent(QKeyEvent *event)
{
    if ((event->modifiers() & Qt::ControlModifier) &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        if (m_goalModeActive && m_waitingForUserStepAdvance) {
            advanceGoalStep();
        } else {
            generateIllustration();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && (m_reply || m_goalModeActive || m_testReply || (m_retryTimer && m_retryTimer->isActive()))) {
        cancelRemoteRequest();
        event->accept();
        return;
    }
    QDockWidget::keyPressEvent(event);
}

void KisAiIllustrationDocker::focusPrompt()
{
    if (m_promptEditor) {
        m_promptEditor->setFocus();
    }
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
    if (m_reply || m_goalModeActive) {
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
    if (m_goalModeCheck && m_goalModeCheck->isChecked() &&
        (mode == GenerationMode::LlmStrokes || mode == GenerationMode::LocalStrokes)) {
        startGoalMode(prompt);
        return;
    }

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

    const QSize canvasSize = effectiveCanvasSize();
    const KisAiStrokeProgram program = KisAiStrokeProgramCodec::createDeterministicProgram(prompt, canvasSize);

    const QSize previewTargetSize = m_previewLabel->size().isEmpty() ? QSize(256, 256) : m_previewLabel->size();
    const QImage preview = KisAiStrokeRenderer::renderProgramToImage(program, previewTargetSize);
    if (!preview.isNull()) {
        m_previewLabel->setPixmap(QPixmap::fromImage(preview).scaled(previewTargetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
    if (view && view->image()) {
        QString statusMsg;
        if (KisAiStrokeRenderer::renderProgramToLayers(view->image(), m_mainWindow->viewManager(), program, &statusMsg)) {
            const QString summary = KisAiStrokeProgramCodec::formatLayerSummary(program);
            const int qualityPercent = qRound(qBound<qreal>(0.0, program.completionScore, 1.0) * 100.0);
            const QString detailMsg = i18n("%1 (%2 / 構造品質 %3%)", statusMsg, summary, qualityPercent);
            setStatus(detailMsg);
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
    const QString apiKey = !m_inFlightApiKey.isEmpty() ? m_inFlightApiKey : m_apiKeyEditor->text();

    if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &errorMessage)) {
        if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
            m_detailsToggleBtn->setChecked(true);
        }
        if (m_endpointEditor) {
            m_endpointEditor->setFocus();
        }
        setStatus(errorMessage, true);
        return;
    }
    if (model.isEmpty()) {
        if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
            m_detailsToggleBtn->setChecked(true);
        }
        if (m_modelEditor) {
            m_modelEditor->setFocus();
        }
        setStatus(i18n("LLM モデル名を入力してください。"), true);
        return;
    }
    if (apiKey.isEmpty()) {
        if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
            m_detailsToggleBtn->setChecked(true);
        }
        if (m_apiKeyEditor) {
            m_apiKeyEditor->setFocus();
        }
        setStatus(i18n("このリクエストに使う API キーを入力してください。"), true);
        return;
    }

    // A fresh user request starts a new retry budget; a re-entry from
    // executeRetry() must keep counting towards the current budget, otherwise the
    // retry limit is never reached and the app re-POSTs forever.
    if (!m_retryInFlight) {
        m_currentRetryCount = 0;
        m_lastFailedPrompt = prompt;
    }
    m_retryInFlight = false;
    if (m_maxRetriesSpin) {
        m_maxRetryCount = m_maxRetriesSpin->value();
    }

    saveSettings();

    QNetworkRequest request{QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    if (endpoint.contains(QLatin1String("openrouter.ai"), Qt::CaseInsensitive)) {
        request.setRawHeader("HTTP-Referer", "https://github.com/TopiTech/ai_stroke_painter");
        request.setRawHeader("X-Title", "AI Stroke Painter");
    }

    bool enforceJson = false;
    const int jsonModeIdx = m_jsonModeCombo ? m_jsonModeCombo->currentIndex() : 0;
    if (jsonModeIdx == 0) {
        enforceJson = KisAiStrokeProgramCodec::supportsJsonFormat(endpoint);
    } else if (jsonModeIdx == 1) {
        enforceJson = true;
    } else {
        enforceJson = false;
    }
    if (m_isSelfCorrectionRetry) {
        enforceJson = true;
    }

    const qreal configuredTemp = m_temperatureSpin ? m_temperatureSpin->value() : 0.70;
    const qreal temperature = m_isSelfCorrectionRetry ? qMin<qreal>(0.20, configuredTemp) : configuredTemp;
    const qreal topP = m_topPSpin ? m_topPSpin->value() : 1.0;
    const int maxTokens = m_maxTokensSpin ? m_maxTokensSpin->value() : 0;
    const QString reasoningEffort = m_reasoningEffortCombo ? m_reasoningEffortCombo->currentData().toString() : QString();
    const QString customInstructions = m_customInstructionsEdit ? m_customInstructionsEdit->toPlainText() : QString();
    const int artStyle = m_artStyleCombo ? m_artStyleCombo->currentData().toInt() : 0;

    const int strokeBudget = m_strokeBudgetSpin ? m_strokeBudgetSpin->value() : 500;
    const QSize canvasSize = effectiveCanvasSize();
    const QJsonObject payload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(
        model,
        prompt,
        canvasSize,
        strokeBudget,
        reasoningEffort,
        customInstructions,
        true, // enableStreaming
        enforceJson,
        temperature,
        topP,
        maxTokens,
        artStyle
    );

    logDebug(QStringLiteral("LLM_REQ"),
             QStringLiteral("POST %1 (model=%2, stream=true, budget=%3, temp=%4, top_p=%5, prompt=\"%6\")")
                 .arg(KisAiIllustrationRenderer::displayEndpoint(endpoint),
                      model,
                      QString::number(strokeBudget),
                      QString::number(temperature, 'f', 2),
                      QString::number(topP, 'f', 2),
                      prompt.left(60)));

    m_activeRequestEndpoint = endpoint;
    m_isStreamingRequest = true;
    m_streamedContent.clear();
    m_sseBuffer.clear();
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    m_responseTooLarge = false;
    m_responseBuffer.clear();
    m_requestElapsedTimer.start();

    m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);

    m_inFlightApiKey = apiKey;
    if (m_saveApiKeyCheck && !m_saveApiKeyCheck->isChecked()) {
        m_apiKeyEditor->clear();
    }

    setBusy(true);
    setStatus(i18n("%1 に LLM 座標ストローク生成を依頼しています…", KisAiIllustrationRenderer::displayEndpoint(endpoint)));

    connect(m_reply.data(), &QNetworkReply::readyRead, this, [this] { appendReplyData(m_reply.data()); });
    connect(m_reply.data(), &QNetworkReply::finished, this, [this] { finishLlmStrokesRequest(); });

    if (!m_activityTimer) {
        m_activityTimer = new QTimer(this);
        m_activityTimer->setSingleShot(true);
        connect(m_activityTimer, &QTimer::timeout, this, [this] {
            if (m_reply) {
                m_requestTimedOut = true;
                m_reply->abort();
            }
        });
    }
    const int timeoutSec = m_timeoutSecSpin ? m_timeoutSecSpin->value() : 90;
    const int initialTimeoutMs = qBound(10'000, timeoutSec * 1000, MAX_REQUEST_TIMEOUT_MS);
    m_activityTimer->start(initialTimeoutMs);

    if (!m_progressTimer) {
        m_progressTimer = new QTimer(this);
        connect(m_progressTimer, &QTimer::timeout, this, &KisAiIllustrationDocker::updateProgressStatus);
    }
    m_progressTimer->start(1000);
}

void KisAiIllustrationDocker::finishLlmStrokesRequest()
{
    stopAllRequestTimers();

    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    const bool requestWasCancelled = m_requestWasCancelled;
    const bool requestTimedOut = m_requestTimedOut;
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    setBusy(false);

    if (!reply) {
        m_responseBuffer.clear();
        m_streamedContent.clear();
        m_sseBuffer.clear();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray responseContentType = reply->rawHeader("Content-Type");
    const QByteArray rawResponse = takeReplyData(reply.data());
    const bool responseTooLarge = m_responseTooLarge;
    m_responseTooLarge = false;
    reply->deleteLater();

    if (requestWasCancelled) {
        cancelRetry();
        clearInFlightApiKey();
        logDebug(QStringLiteral("LLM_CANCEL"), QStringLiteral("ユーザーにより生成が中止されました。"));
        setStatus(i18n("LLM ストローク生成を中止しました。"));
        m_streamedContent.clear();
        m_sseBuffer.clear();
        return;
    }
    if (requestTimedOut) {
        const qint64 elapsedSec = m_requestElapsedTimer.isValid() ? m_requestElapsedTimer.elapsed() / 1000 : 0;
        logDebug(QStringLiteral("LLM_TIMEOUT"), QStringLiteral("リクエストがタイムアウトしました (%1秒経過)。").arg(elapsedSec));
        // Capture this before clearing the streamed content, otherwise the
        // "partial data then stall" case can never be reported.
        const bool receivedPartialData = !m_streamedContent.isEmpty();
        m_streamedContent.clear();
        m_sseBuffer.clear();
        const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
        if (m_currentRetryCount < maxRetries) {
            scheduleRetry(i18n("リクエストタイムアウト (%1秒)", elapsedSec), false);
            return;
        }
        if (receivedPartialData) {
            setStatus(i18n("LLM からのデータ受信が %1 秒間途絶えたため中止しました。", ACTIVITY_TIMEOUT_MS / 1000), true);
        } else {
            setStatus(i18n("LLM の初期応答が %1 秒以内に届かなかったため中止しました。", elapsedSec), true);
        }
        m_currentRetryCount = 0;
        m_isSelfCorrectionRetry = false;
        clearInFlightApiKey();
        return;
    }
    if (responseTooLarge) {
        logDebug(QStringLiteral("LLM_OVERFLOW"), QStringLiteral("レスポンスが上限サイズを超えました。"));
        setStatus(i18n("LLM の応答が上限を超えています。"), true);
        m_streamedContent.clear();
        m_sseBuffer.clear();
        m_currentRetryCount = 0;
        m_isSelfCorrectionRetry = false;
        clearInFlightApiKey();
        return;
    }
    // Content-type validation: reject clearly non-JSON responses before parsing.
    if (!KisAiStrokeProgramCodec::isAcceptedResponseContentType(responseContentType, true)) {
        logDebug(QStringLiteral("LLM_CONTENT_TYPE"),
                 QStringLiteral("予期しないContent-Type: %1").arg(QString::fromUtf8(responseContentType)));
        setStatus(i18n("LLM の応答が JSON または SSE 形式ではありません (Content-Type: %1)。",
                       QString::fromUtf8(responseContentType.isEmpty() ? QByteArray("unknown") : responseContentType)),
                  true);
        m_streamedContent.clear();
        m_sseBuffer.clear();
        m_currentRetryCount = 0;
        m_isSelfCorrectionRetry = false;
        clearInFlightApiKey();
        return;
    }
    if (!requestSucceeded) {
        QString detail;
        QJsonParseError parseErr;
        const QJsonDocument errDoc = QJsonDocument::fromJson(rawResponse, &parseErr);
        if (!errDoc.isNull() && errDoc.isObject()) {
            const QJsonValue errVal = errDoc.object().value(QStringLiteral("error"));
            if (errVal.isObject()) {
                detail = errVal.toObject().value(QStringLiteral("message")).toString().trimmed();
            } else if (errVal.isString()) {
                detail = errVal.toString().trimmed();
            }
        }
        if (detail.isEmpty()) {
            detail = reply->errorString();
        }
        logDebug(QStringLiteral("LLM_ERROR"), QStringLiteral("HTTP %1: %2\nRaw: %3")
            .arg(httpStatus).arg(detail, QString::fromUtf8(rawResponse.left(500))));

        const bool isRetryableHttp = (httpStatus == 429 || (httpStatus >= 500 && httpStatus <= 504));
        const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
        if (isRetryableHttp && m_currentRetryCount < maxRetries) {
            int retryAfterSec = 0;
            const QByteArray retryAfterHeader = reply->rawHeader("Retry-After");
            if (!retryAfterHeader.isEmpty()) {
                bool ok = false;
                const int val = retryAfterHeader.trimmed().toInt(&ok);
                if (ok && val > 0) {
                    retryAfterSec = qMin(val, 60);
                }
            }
            scheduleRetry(i18n("HTTP %1 一時エラー", httpStatus), false, retryAfterSec);
            return;
        }

        if (!detail.isEmpty()) {
            setStatus(i18n("LLM への接続または応答に失敗しました (HTTP %1): %2", httpStatus, detail), true);
        } else {
            setStatus(i18n("LLM への接続または応答に失敗しました (HTTP %1)。", httpStatus), true);
        }
        m_streamedContent.clear();
        m_sseBuffer.clear();
        m_currentRetryCount = 0;
        m_isSelfCorrectionRetry = false;
        clearInFlightApiKey();
        return;
    }

    if (m_isStreamingRequest && !m_sseBuffer.isEmpty()) {
        bool isDone = false;
        KisAiStrokeProgramCodec::parseSseStreamChunk(QByteArrayLiteral("\n\n"), &m_sseBuffer, &m_streamedContent, &isDone);
    }

    const QByteArray response = !m_streamedContent.trimmed().isEmpty()
        ? m_streamedContent.toUtf8()
        : rawResponse;
    m_streamedContent.clear();
    m_sseBuffer.clear();

    logDebug(QStringLiteral("LLM_RESP"), QStringLiteral("HTTP %1 (%2 bytes) 受信完了\nRaw: %3")
        .arg(httpStatus).arg(response.size()).arg(QString::fromUtf8(response.left(1000))));

    KisAiStrokeProgram program;
    QString parseError;
    KisAiJsonDiagnostic diagnostic;
    KisAiStrokeQualityReport qualityReport;
    if (!KisAiStrokeProgramCodec::parseResponse(response, &program, &parseError, &diagnostic, &qualityReport)) {
        m_lastJsonDiagnostic = diagnostic;
        m_isQualityCorrectionRetry = false;
        logDebug(QStringLiteral("LLM_PARSE_ERR"), QStringLiteral("%1\n%2").arg(parseError, diagnostic.formatForLog()));
        const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
        if (m_currentRetryCount < maxRetries) {
            scheduleRetry(parseError, true);
            return;
        }
        setStatus(parseError, true);
        m_currentRetryCount = 0;
        m_isSelfCorrectionRetry = false;
        m_isQualityCorrectionRetry = false;
        clearInFlightApiKey();
        return;
    }

    m_lastQualityReport = qualityReport;

    // A1: Quality Feedback Self-Correction Loop
    // Trigger correction retry if Flats layer is missing, structural score is below 0.55,
    // or over half of the generated operations were dropped as invalid.
    const int flatsCount = KisAiStrokeProgramCodec::countLayerOperations(program).value(QStringLiteral("Flats"), 0);
    const bool severelyDegradedQuality = (qualityReport.score < 0.55) ||
                                         (flatsCount == 0 && !program.operations.isEmpty()) ||
                                         (qualityReport.inputOperations > 4 && qualityReport.droppedOperations > qualityReport.inputOperations * 0.5);

    const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
    if (severelyDegradedQuality && m_currentRetryCount < maxRetries) {
        QString qualityReason;
        if (flatsCount == 0) {
            qualityReason = i18n("Flatsレイヤー（シルエット下地）不在");
        } else if (qualityReport.score < 0.55) {
            qualityReason = i18n("構造品質スコア不足 (%1/1.0)", QString::number(qualityReport.score, 'f', 2));
        } else {
            qualityReason = i18n("無効操作多数 (%1/%2 除外)", qualityReport.droppedOperations, qualityReport.inputOperations);
        }
        logDebug(QStringLiteral("LLM_QUALITY_RETRY"), QStringLiteral("品質自己修復を発火: %1").arg(qualityReason));
        m_isQualityCorrectionRetry = true;
        scheduleRetry(qualityReason, true);
        return;
    }

    m_currentRetryCount = 0;
    m_isSelfCorrectionRetry = false;
    m_isQualityCorrectionRetry = false;
    clearInFlightApiKey();

    logDebug(QStringLiteral("LLM_PARSE_OK"), QStringLiteral("解析成功: %1 operations, completionScore=%2")
        .arg(program.operations.size()).arg(program.completionScore));

    const qreal trappingPx = m_trappingPxSpin ? m_trappingPxSpin->value() : 1.5;
    const QSize previewTargetSize = m_previewLabel->size().isEmpty() ? QSize(256, 256) : m_previewLabel->size();
    const QImage preview = KisAiStrokeRenderer::renderProgramToImage(program, previewTargetSize, true, nullptr, trappingPx);
    if (!preview.isNull()) {
        m_previewLabel->setPixmap(QPixmap::fromImage(preview).scaled(previewTargetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
    if (view && view->image()) {
        QString statusMsg;
        if (KisAiStrokeRenderer::renderProgramToLayers(view->image(), m_mainWindow->viewManager(), program, &statusMsg, true, nullptr, trappingPx)) {
            const QString summary = KisAiStrokeProgramCodec::formatLayerSummary(program);
            const int qualityPercent = qRound(qBound<qreal>(0.0, program.completionScore, 1.0) * 100.0);
            const QString detailMsg = i18n("%1 (%2 / 構造品質 %3%)", statusMsg, summary, qualityPercent);
            setStatus(detailMsg);
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

    const QSize canvasSize = effectiveCanvasSize();
    const QImage result = KisAiIllustrationRenderer::createConceptImage(prompt, canvasSize);
    const QSize previewTargetSize = m_previewLabel->size().isEmpty() ? QSize(256, 256) : m_previewLabel->size();
    m_previewLabel->setPixmap(QPixmap::fromImage(result).scaled(previewTargetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

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
        if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
            m_detailsToggleBtn->setChecked(true);
        }
        if (m_endpointEditor) {
            m_endpointEditor->setFocus();
        }
        setStatus(errorMessage, true);
        return;
    }
    if (model.isEmpty()) {
        if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
            m_detailsToggleBtn->setChecked(true);
        }
        if (m_modelEditor) {
            m_modelEditor->setFocus();
        }
        setStatus(i18n("画像モデル名を入力してください。"), true);
        return;
    }
    if (apiKey.isEmpty()) {
        if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
            m_detailsToggleBtn->setChecked(true);
        }
        if (m_apiKeyEditor) {
            m_apiKeyEditor->setFocus();
        }
        setStatus(i18n("このリクエストに使う API キーを入力してください。"), true);
        return;
    }

    saveSettings();

    QNetworkRequest request {QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    const QString sizeStr = imageSizeText(m_widthSpin, m_heightSpin);
    const QJsonObject payload {
        {QStringLiteral("model"), model},
        {QStringLiteral("prompt"), prompt},
        {QStringLiteral("size"), sizeStr},
        {QStringLiteral("response_format"), QStringLiteral("b64_json")},
    };

    logDebug(QStringLiteral("IMG_REQ"),
             QStringLiteral("POST %1 (model=%2, size=%3, prompt=\"%4\")")
                 .arg(KisAiIllustrationRenderer::displayEndpoint(endpoint), model, sizeStr, prompt.left(60)));

    m_activeRequestEndpoint = endpoint;
    m_isStreamingRequest = false;
    m_streamedContent.clear();
    m_sseBuffer.clear();
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    m_responseTooLarge = false;
    m_responseBuffer.clear();
    m_requestElapsedTimer.start();
    m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);

    if (m_saveApiKeyCheck && !m_saveApiKeyCheck->isChecked()) {
        m_apiKeyEditor->clear();
    }

    setBusy(true);
    setStatus(i18n("%1 に画像生成を依頼しています…", KisAiIllustrationRenderer::displayEndpoint(endpoint)));

    connect(m_reply.data(), &QNetworkReply::readyRead, this, [this] { appendReplyData(m_reply.data()); });
    connect(m_reply.data(), &QNetworkReply::finished, this, [this] { finishRemoteImageRequest(); });

    if (!m_activityTimer) {
        m_activityTimer = new QTimer(this);
        m_activityTimer->setSingleShot(true);
        connect(m_activityTimer, &QTimer::timeout, this, [this] {
            if (m_reply) {
                m_requestTimedOut = true;
                m_reply->abort();
            }
        });
    }
    m_activityTimer->start(REMOTE_IMAGE_TIMEOUT_MS);

    if (!m_progressTimer) {
        m_progressTimer = new QTimer(this);
        connect(m_progressTimer, &QTimer::timeout, this, &KisAiIllustrationDocker::updateProgressStatus);
    }
    m_progressTimer->start(1000);
}

void KisAiIllustrationDocker::finishRemoteImageRequest()
{
    stopAllRequestTimers();

    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    const bool requestWasCancelled = m_requestWasCancelled;
    const bool requestTimedOut = m_requestTimedOut;
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    setBusy(false);

    if (!reply) {
        m_responseBuffer.clear();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray responseContentType = reply->rawHeader("Content-Type");
    const QByteArray response = takeReplyData(reply.data());
    const bool responseTooLarge = m_responseTooLarge;
    m_responseTooLarge = false;
    reply->deleteLater();

    if (requestWasCancelled) {
        logDebug(QStringLiteral("IMG_CANCEL"), QStringLiteral("ユーザーにより画像生成が中止されました。"));
        setStatus(i18n("画像生成を中止しました。"));
        return;
    }
    if (requestTimedOut) {
        logDebug(QStringLiteral("IMG_TIMEOUT"), QStringLiteral("画像モデルの応答がタイムアウトしました (%1 秒)。").arg(REMOTE_IMAGE_TIMEOUT_MS / 1000));
        setStatus(i18n("画像モデルの応答が %1 秒以内に届かなかったため中止しました。", REMOTE_IMAGE_TIMEOUT_MS / 1000), true);
        return;
    }
    if (responseTooLarge) {
        logDebug(QStringLiteral("IMG_OVERFLOW"), QStringLiteral("画像モデルの応答が上限を超えています。"));
        setStatus(i18n("画像モデルの応答が上限を超えています。"), true);
        return;
    }
    // Content-type validation: reject clearly non-JSON responses before parsing.
    if (!KisAiStrokeProgramCodec::isAcceptedResponseContentType(responseContentType, false)) {
        logDebug(QStringLiteral("IMG_CONTENT_TYPE"), QStringLiteral("予期しないContent-Type: %1").arg(QString::fromUtf8(responseContentType)));
        setStatus(i18n("画像モデルの応答が JSON 形式ではありません (Content-Type: %1)。",
            QString::fromUtf8(responseContentType.isEmpty() ? QByteArray("unknown") : responseContentType)), true);
        return;
    }
    if (!requestSucceeded) {
        QString detail;
        QJsonParseError parseErr;
        const QJsonDocument errDoc = QJsonDocument::fromJson(response, &parseErr);
        if (!errDoc.isNull() && errDoc.isObject()) {
            const QJsonValue errVal = errDoc.object().value(QStringLiteral("error"));
            if (errVal.isObject()) {
                detail = errVal.toObject().value(QStringLiteral("message")).toString().trimmed();
            } else if (errVal.isString()) {
                detail = errVal.toString().trimmed();
            }
        }
        if (detail.isEmpty()) {
            detail = reply->errorString();
        }
        logDebug(QStringLiteral("IMG_ERROR"), QStringLiteral("HTTP %1: %2\nRaw: %3")
            .arg(httpStatus).arg(detail, QString::fromUtf8(response.left(500))));
        if (!detail.isEmpty()) {
            setStatus(i18n("画像モデルへの接続または応答に失敗しました (HTTP %1): %2", httpStatus, detail), true);
        } else {
            setStatus(i18n("画像モデルへの接続または応答に失敗しました (HTTP %1)。", httpStatus), true);
        }
        return;
    }
    if (response.size() > MAX_REMOTE_RESPONSE_BYTES) {
        logDebug(QStringLiteral("IMG_OVERFLOW"), QStringLiteral("画像モデルの応答サイズが上限を超えています。"));
        setStatus(i18n("画像モデルの応答が上限を超えています。"), true);
        return;
    }

    logDebug(QStringLiteral("IMG_RESP"), QStringLiteral("HTTP %1 (%2 bytes) 受信").arg(httpStatus).arg(response.size()));

    QString errorMessage;
    const QImage image = decodeModelImage(response, &errorMessage);
    if (image.isNull()) {
        logDebug(QStringLiteral("IMG_DECODE_ERR"), errorMessage);
        setStatus(errorMessage, true);
        return;
    }

    logDebug(QStringLiteral("IMG_DECODE_OK"), QStringLiteral("画像デコード成功: %1x%2").arg(image.width()).arg(image.height()));

    const QSize previewTargetSize = m_previewLabel->size().isEmpty() ? QSize(256, 256) : m_previewLabel->size();
    m_previewLabel->setPixmap(QPixmap::fromImage(image).scaled(previewTargetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (addImageAsLayer(image, promptForLayerName(m_promptEditor->toPlainText()))) {
        setStatus(i18n("画像モデルの結果を新しいレイヤーに追加しました。"));
    }
}

void KisAiIllustrationDocker::cancelRemoteRequest()
{
    cancelRetry();
    stopAllRequestTimers();
    m_streamedContent.clear();
    m_sseBuffer.clear();

    if (m_testReply) {
        m_testReply->abort();
    }
    if (!m_reply) {
        if (m_goalModeActive) {
            finishGoalMode(false);
            setStatus(i18n("Goalモード作画を中止しました。"));
        } else {
            setStatus(i18n("生成リクエストを中止しました。"));
            setBusy(false);
        }
        return;
    }

    m_requestWasCancelled = true;
    m_reply->abort();
    setStatus(i18n("リクエストを中止しています…"));
}

bool KisAiIllustrationDocker::appendReplyData(QNetworkReply *reply)
{
    if (!reply || m_responseTooLarge) {
        return false;
    }

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) {
        return false;
    }

    resetActivityTimeout();

    if (chunk.size() > MAX_REMOTE_RESPONSE_BYTES - m_responseBuffer.size()) {
        m_responseBuffer.clear();
        m_responseTooLarge = true;
        reply->abort();
        return false;
    }

    m_responseBuffer.append(chunk);

    if (m_isStreamingRequest) {
        bool isDone = false;
        KisAiStrokeProgramCodec::parseSseStreamChunk(chunk, &m_sseBuffer, &m_streamedContent, &isDone);
    }

    return true;
}

QByteArray KisAiIllustrationDocker::takeReplyData(QNetworkReply *reply)
{
    appendReplyData(reply);
    QByteArray response = m_responseBuffer;
    m_responseBuffer.clear();
    return response;
}

bool KisAiIllustrationDocker::appendTestReplyData(QNetworkReply *reply)
{
    if (!reply || m_testResponseTooLarge) {
        return false;
    }

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) {
        return false;
    }

    if (chunk.size() > MAX_REMOTE_RESPONSE_BYTES - m_testResponseBuffer.size()) {
        m_testResponseBuffer.clear();
        m_testResponseTooLarge = true;
        reply->abort();
        return false;
    }

    m_testResponseBuffer.append(chunk);
    return true;
}

QByteArray KisAiIllustrationDocker::takeTestReplyData(QNetworkReply *reply)
{
    appendTestReplyData(reply);
    QByteArray response = m_testResponseBuffer;
    m_testResponseBuffer.clear();
    return response;
}

void KisAiIllustrationDocker::updateModeUi()
{
    const auto newMode = static_cast<GenerationMode>(m_modeCombo->currentData().toInt());

    // Save previous mode's settings before switching
    if (m_currentMode != newMode) {
        saveSettingsForMode(m_currentMode);
        m_currentMode = newMode;
    }

    const bool isLlm = (newMode == GenerationMode::LlmStrokes);
    const bool isRemoteImage = (newMode == GenerationMode::RemoteImage);
    const bool needsRemote = isLlm || isRemoteImage;

    m_remoteOptionsLabel->setVisible(needsRemote);
    QSettings settings;
    if (isLlm) {
        m_remoteOptionsLabel->setText(i18n("OpenAI 互換の Chat Completions エンドポイント (/v1/chat/completions) を指定します。Vision対応モデルを推奨します（画像非対応時はテキストに自動フォールバック）。"));
        m_endpointEditor->setPlaceholderText(QStringLiteral("https://api.openai.com/v1/chat/completions"));
        m_modelEditor->setPlaceholderText(i18n("モデル名 (例: gpt-4o, o3-mini, deepseek-chat)"));

        const QString savedEndpoint = readSafeStoredEndpoint(settings,
                                                             QStringLiteral("AIIllustration/llmEndpoint"),
                                                             QStringLiteral("AIIllustration/endpoint"));
        const QString savedModel = settings.value(QStringLiteral("AIIllustration/llmModel"),
            settings.value(QStringLiteral("AIIllustration/model"))).toString();
        m_endpointEditor->setText(savedEndpoint.isEmpty() ? QStringLiteral("https://api.openai.com/v1/chat/completions") : savedEndpoint);
        m_modelEditor->setText(savedModel.isEmpty() ? QStringLiteral("gpt-4o") : savedModel);
    } else if (isRemoteImage) {
        m_remoteOptionsLabel->setText(i18n("OpenAI 互換の画像生成エンドポイント (/v1/images/generations) を指定します。"));
        m_endpointEditor->setPlaceholderText(QStringLiteral("https://provider.example/v1/images/generations"));
        m_modelEditor->setPlaceholderText(i18n("画像モデル名 (例: dall-e-3)"));

        const QString savedEndpoint = readSafeStoredEndpoint(settings, QStringLiteral("AIIllustration/imageEndpoint"));
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

    const bool isStrokeMode = (isLlm || newMode == GenerationMode::LocalStrokes);
    if (m_remoteForm) {
        m_remoteForm->setRowVisible(0, needsRemote);
        m_remoteForm->setRowVisible(1, needsRemote);
        m_remoteForm->setRowVisible(2, needsRemote);
        m_remoteForm->setRowVisible(3, needsRemote); // Save API Key checkbox
        m_remoteForm->setRowVisible(4, isLlm);       // Stroke Budget
        m_remoteForm->setRowVisible(5, isLlm);       // Temperature
        m_remoteForm->setRowVisible(6, isLlm);       // Top-P
        m_remoteForm->setRowVisible(7, isStrokeMode);// Trapping px
        m_remoteForm->setRowVisible(8, isLlm);       // Max Tokens
        m_remoteForm->setRowVisible(9, needsRemote); // Timeout
        m_remoteForm->setRowVisible(10, isLlm);      // Auto-retries
        m_remoteForm->setRowVisible(11, isLlm);      // JSON Mode
        m_remoteForm->setRowVisible(12, isLlm);      // Composition Plan
        m_remoteForm->setRowVisible(13, isLlm);      // Reasoning Effort
        m_remoteForm->setRowVisible(14, isLlm);      // Custom Instructions
    }

    if (m_testConnectionButton) {
        m_testConnectionButton->setEnabled(needsRemote);
    }
    if (m_testConnectionStatusLabel) {
        m_testConnectionStatusLabel->setVisible(false);
    }

    const bool isGoalCompatible = (newMode == GenerationMode::LlmStrokes || newMode == GenerationMode::LocalStrokes);
    if (m_goalModeCheck) {
        m_goalModeCheck->setEnabled(isGoalCompatible);
        if (!isGoalCompatible && m_goalModeCheck->isChecked()) {
            m_goalModeCheck->setChecked(false);
        }
    }
}


void KisAiIllustrationDocker::setBusy(bool busy)
{
    const bool allowGeneralInput = !busy && !m_goalModeActive;
    m_newCanvasButton->setEnabled(allowGeneralInput);
    m_generateButton->setEnabled(allowGeneralInput);
    if (m_widthSpin) {
        m_widthSpin->setEnabled(allowGeneralInput);
    }
    if (m_heightSpin) {
        m_heightSpin->setEnabled(allowGeneralInput);
    }
    if (m_strokeBudgetSpin) {
        m_strokeBudgetSpin->setEnabled(allowGeneralInput);
    }
    if (m_testConnectionButton) {
        m_testConnectionButton->setEnabled(allowGeneralInput && (m_currentMode == GenerationMode::LlmStrokes || m_currentMode == GenerationMode::RemoteImage));
    }
    if (m_saveSettingsButton) {
        m_saveSettingsButton->setEnabled(allowGeneralInput);
    }
    if (m_goalModeActive) {
        m_generateButton->setText(busy ? i18n("⏳ Goal作画中…") : i18n("🎯 Goal作画進行中"));
    } else if (m_goalModeCheck && m_goalModeCheck->isChecked()) {
        m_generateButton->setText(busy ? i18n("⏳ 作画中…") : i18n("🎯 Goal作画を開始"));
    } else {
        m_generateButton->setText(busy ? i18n("⏳ 生成中…") : i18n("🎨 生成してレイヤーに追加"));
    }
    m_modeCombo->setEnabled(allowGeneralInput);
    if (m_goalModeCheck) {
        m_goalModeCheck->setEnabled(allowGeneralInput);
    }
    if (m_goalStepsSpin) {
        m_goalStepsSpin->setEnabled(allowGeneralInput);
    }
    if (m_artStyleCombo) {
        m_artStyleCombo->setEnabled(allowGeneralInput);
    }
    if (m_temperatureSpin) {
        m_temperatureSpin->setEnabled(allowGeneralInput);
    }
    if (m_topPSpin) {
        m_topPSpin->setEnabled(allowGeneralInput);
    }
    if (m_maxTokensSpin) {
        m_maxTokensSpin->setEnabled(allowGeneralInput);
    }
    if (m_maxRetriesSpin) {
        m_maxRetriesSpin->setEnabled(allowGeneralInput);
    }
    if (m_timeoutSecSpin) {
        m_timeoutSecSpin->setEnabled(allowGeneralInput);
    }
    if (m_jsonModeCombo) {
        m_jsonModeCombo->setEnabled(allowGeneralInput);
    }
    if (m_reasoningEffortCombo) {
        m_reasoningEffortCombo->setEnabled(allowGeneralInput);
    }
    if (m_customInstructionsEdit) {
        m_customInstructionsEdit->setEnabled(allowGeneralInput);
    }
    const bool retryPending = (m_retryTimer && m_retryTimer->isActive());
    m_cancelButton->setVisible((busy && (!m_reply.isNull() || retryPending)) || m_goalModeActive);
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

QSize KisAiIllustrationDocker::effectiveCanvasSize() const
{
    if (m_mainWindow) {
        KisView *view = m_mainWindow->activeView();
        if (view && view->image()) {
            const QSize size = view->image()->bounds().size();
            if (size.isValid() && !size.isEmpty()) {
                return size;
            }
        }
    }

    return QSize(m_widthSpin ? m_widthSpin->value() : 1024, m_heightSpin ? m_heightSpin->value() : 1024);
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

void KisAiIllustrationDocker::resetActivityTimeout()
{
    if (m_requestElapsedTimer.isValid() && m_requestElapsedTimer.elapsed() > MAX_REQUEST_TIMEOUT_MS) {
        logDebug(QStringLiteral("TIMEOUT"), QStringLiteral("最大リクエスト時間 (%1 秒) を超過したためリクエストを打ち切ります。").arg(MAX_REQUEST_TIMEOUT_MS / 1000));
        m_requestTimedOut = true;
        if (m_reply) {
            m_reply->abort();
        }
        return;
    }

    if (m_activityTimer) {
        m_activityTimer->start(ACTIVITY_TIMEOUT_MS);
    }
}

void KisAiIllustrationDocker::updateProgressStatus()
{
    if (!m_reply) {
        return;
    }

    const qint64 elapsedSec = m_requestElapsedTimer.isValid() ? m_requestElapsedTimer.elapsed() / 1000 : 0;
    const QString endpointName = KisAiIllustrationRenderer::displayEndpoint(m_activeRequestEndpoint);

    if (m_isStreamingRequest) {
        const int receivedChars = m_streamedContent.length();
        if (receivedChars > 0) {
            const int estimatedTokens = qMax(1, receivedChars / 3);
            const int detectedOps = m_streamedContent.count(QStringLiteral("\"kind\""));
            const QString opsStr = detectedOps > 0
                ? i18n(" / %1 本検出", detectedOps)
                : QString();
            if (m_goalModeActive) {
                const QString visionTag = m_lastGoalRequestHadImage
                    ? i18n(" (Vision画像付)")
                    : (m_goalVisionFallbackActive ? i18n(" (テキストフォールバック)") : QString());
                setStatus(i18n("Goal ステップ %1/%2: ストリーム受信中… (%3 秒経過 / 約 %4 トークン%5)",
                    m_goalCurrentStep, m_goalTotalSteps, elapsedSec, estimatedTokens, opsStr));
                if (m_goalPhaseLabel) {
                    m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2: 受信中 (%3 秒 / 約 %4 トークン%5)%6…",
                        m_goalCurrentStep, m_goalTotalSteps, elapsedSec, estimatedTokens, opsStr, visionTag));
                }
            } else {
                setStatus(i18n("%1 からストロークを受信中… (%2 秒経過 / 約 %3 トークン%4)",
                    endpointName, elapsedSec, estimatedTokens, opsStr));
            }
        } else {
            if (m_goalModeActive) {
                setStatus(i18n("Goal ステップ %1/%2: LLM 思考・待機中… (%3 秒経過)",
                    m_goalCurrentStep, m_goalTotalSteps, elapsedSec));
            } else {
                setStatus(i18n("%1 に接続中・思考待機中… (%2 秒経過)",
                    endpointName, elapsedSec));
            }
        }
    } else {
        setStatus(i18n("%1 にリクエスト送信中… (%2 秒経過)",
            endpointName, elapsedSec));
    }
}

void KisAiIllustrationDocker::stopAllRequestTimers()
{
    if (m_activityTimer) {
        m_activityTimer->stop();
    }
    if (m_progressTimer) {
        m_progressTimer->stop();
    }
}

void KisAiIllustrationDocker::startGoalMode(const QString &prompt)
{
    if (m_reply || m_goalModeActive) {
        return;
    }

    if (!ensureCanvas()) {
        return;
    }

    m_goalModeActive = true;
    m_goalPrompt = prompt;
    m_goalCurrentStep = 1;
    m_goalTotalSteps = m_goalStepsSpin ? m_goalStepsSpin->value() : 4;
    m_waitingForUserStepAdvance = false;
    m_goalVisionFallbackActive = false;
    m_lastGoalRequestHadImage = false;
    m_goalAccumulatedProgram = KisAiStrokeProgram();
    m_lastGoalCritique.clear();

    saveSettings();

    const auto mode = static_cast<GenerationMode>(m_modeCombo->currentData().toInt());
    if (mode == GenerationMode::LlmStrokes) {
        const QString endpoint = m_endpointEditor->text().trimmed();
        const QString model = m_modelEditor->text().trimmed();
        const QString apiKey = m_apiKeyEditor->text();

        QString errorMessage;
        if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &errorMessage)) {
            if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
                m_detailsToggleBtn->setChecked(true);
            }
            if (m_endpointEditor) {
                m_endpointEditor->setFocus();
            }
            setStatus(errorMessage, true);
            m_goalModeActive = false;
            return;
        }
        if (model.isEmpty()) {
            if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
                m_detailsToggleBtn->setChecked(true);
            }
            if (m_modelEditor) {
                m_modelEditor->setFocus();
            }
            setStatus(i18n("LLM モデル名を入力してください。"), true);
            m_goalModeActive = false;
            return;
        }
        if (apiKey.isEmpty()) {
            if (m_detailsToggleBtn && !m_detailsToggleBtn->isChecked()) {
                m_detailsToggleBtn->setChecked(true);
            }
            if (m_apiKeyEditor) {
                m_apiKeyEditor->setFocus();
            }
            setStatus(i18n("このリクエストに使う API キーを入力してください。"), true);
            m_goalModeActive = false;
            return;
        }

        m_goalApiKey = apiKey;
        if (m_saveApiKeyCheck && !m_saveApiKeyCheck->isChecked()) {
            m_apiKeyEditor->clear();
        }
    } else {
        m_goalApiKey.clear();
    }

    logDebug(QStringLiteral("GOAL_START"), QStringLiteral("Goalモード開始 (全 %1 段階, prompt=\"%2\")")
        .arg(m_goalTotalSteps).arg(prompt.left(60)));

    if (m_goalInspectorCard) {
        m_goalInspectorCard->setVisible(true);
        if (m_goalPhaseLabel) {
            m_goalPhaseLabel->setText(i18n("🎯 ステップ 1/%1 開始準備中…", m_goalTotalSteps));
        }
        if (m_agentFocusLabel) {
            m_agentFocusLabel->setText(i18n("🎯 着目領域: 構図立案・ベース構築"));
        }
        if (m_readinessBar) {
            m_readinessBar->setValue(0);
        }
        if (m_critiqueLabel) {
            m_critiqueLabel->setText(i18n("作画計画を策定中…"));
        }
        if (m_nextStepButton) {
            m_nextStepButton->setVisible(false);
            m_nextStepButton->setEnabled(false);
        }
        if (m_finishGoalButton) {
            m_finishGoalButton->setVisible(false);
            m_finishGoalButton->setEnabled(false);
        }
    }

    executeGoalStep();
}

void KisAiIllustrationDocker::executeGoalStep()
{
    if (!m_goalModeActive) {
        return;
    }

    const auto mode = static_cast<GenerationMode>(m_modeCombo->currentData().toInt());
    const QSize canvasSize = effectiveCanvasSize();
    const auto artStyle = static_cast<KisAiPromptAnalyzer::ArtStyle>(
        m_artStyleCombo ? m_artStyleCombo->currentData().toInt() : 0);

    if (m_nextStepButton) {
        m_nextStepButton->setVisible(false);
        m_nextStepButton->setEnabled(false);
    }
    if (m_finishGoalButton) {
        m_finishGoalButton->setVisible(false);
        m_finishGoalButton->setEnabled(false);
    }

    if (mode == GenerationMode::LocalStrokes) {
        setBusy(true);
        if (m_goalPhaseLabel) {
            m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2: ローカル作画実行中…", m_goalCurrentStep, m_goalTotalSteps));
        }
        setStatus(i18n("Goal ステップ %1/%2 のストロークを生成しています…", m_goalCurrentStep, m_goalTotalSteps));

        const KisAiStrokeProgram program = KisAiStrokeProgramCodec::createDeterministicProgramStep(
            m_goalPrompt, canvasSize, m_goalCurrentStep, m_goalTotalSteps);

        const QSize previewTargetSize = m_previewLabel->size().isEmpty() ? QSize(256, 256) : m_previewLabel->size();
        m_goalAccumulatedProgram = KisAiStrokeProgramCodec::mergePrograms(m_goalAccumulatedProgram, program);
        const QImage preview = KisAiStrokeRenderer::renderProgramToImage(m_goalAccumulatedProgram, previewTargetSize);
        if (!preview.isNull()) {
            m_previewLabel->setPixmap(QPixmap::fromImage(preview).scaled(previewTargetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }

        KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
        if (view && view->image()) {
            QString statusMsg;
            if (KisAiStrokeRenderer::renderProgramToLayers(view->image(),
                                                           m_mainWindow->viewManager(),
                                                           program,
                                                           &statusMsg,
                                                           true,
                                                           &m_goalAccumulatedProgram)) {
                const QString summary = KisAiStrokeProgramCodec::formatLayerSummary(program);
                const int qualityPercent = qRound(qBound<qreal>(0.0, program.completionScore, 1.0) * 100.0);
                setStatus(i18n("%1 (%2 / 構造品質 %3%)", statusMsg, summary, qualityPercent));
            } else {
                setStatus(statusMsg, true);
            }
        } else {
            setStatus(i18n("キャンバスが利用できないため、ストロークを描画できませんでした。"), true);
            finishGoalMode(false);
            return;
        }

        if (m_goalPhaseLabel) {
            m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2 (%3) 完了", m_goalCurrentStep, m_goalTotalSteps, program.stepPhase));
        }
        if (m_agentFocusLabel) {
            m_agentFocusLabel->setText(i18n("🎯 着目領域: %1", program.stepPhase));
        }
        if (m_readinessBar) {
            m_readinessBar->setValue(qRound(qreal(m_goalCurrentStep) / m_goalTotalSteps * 100.0));
        }
        if (m_critiqueLabel) {
            m_critiqueLabel->setText(program.visualCritique.isEmpty()
                ? i18n("ローカルプロシージャル作画ステップを完了しました。")
                : program.visualCritique);
        }

        setBusy(false);

        if (m_goalCurrentStep >= m_goalTotalSteps) {
            finishGoalMode(true);
        } else if (m_pausePerStepCheck && m_pausePerStepCheck->isChecked()) {
            m_waitingForUserStepAdvance = true;
            if (m_nextStepButton) {
                m_nextStepButton->setVisible(true);
                m_nextStepButton->setEnabled(true);
            }
            if (m_finishGoalButton) {
                m_finishGoalButton->setVisible(true);
                m_finishGoalButton->setEnabled(true);
            }
            setStatus(i18n("ステップ %1 完了。加筆や確認後、「次のステップへ進む」を押してください。", m_goalCurrentStep));
        } else {
            QTimer::singleShot(300, this, [this] {
                advanceGoalStep();
            });
        }
    } else if (mode == GenerationMode::LlmStrokes) {
        const QString endpoint = m_endpointEditor->text().trimmed();
        const QString model = m_modelEditor->text().trimmed();
        const QString apiKey = m_goalApiKey;

        // The endpoint field stays editable during Goal mode, so every step must
        // re-validate it before attaching the Authorization header.
        QString endpointError;
        if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &endpointError)) {
            setStatus(endpointError, true);
            finishGoalMode(false);
            return;
        }

        if (apiKey.isEmpty()) {
            setStatus(i18n("API キーが見つかりません。Goalモードを終了します。"), true);
            finishGoalMode(false);
            return;
        }

        QString imageBase64;
        if (m_goalCurrentStep > 1 && !m_goalVisionFallbackActive) {
            KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
            if (view && view->image()) {
#ifndef AI_STROKE_STANDALONE
                imageBase64 = KisAiStrokeRenderer::captureCanvasBase64(view->image(), 768);
#endif
            }
        }

        m_lastGoalRequestHadImage = (!imageBase64.isEmpty() && !m_goalVisionFallbackActive);

        bool enforceJson = false;
        const int jsonModeIdx = m_jsonModeCombo ? m_jsonModeCombo->currentIndex() : 0;
        if (jsonModeIdx == 0) {
            enforceJson = KisAiStrokeProgramCodec::supportsJsonFormat(endpoint);
        } else if (jsonModeIdx == 1) {
            enforceJson = true;
        } else {
            enforceJson = false;
        }
        if (!m_goalSelfCorrectionFeedback.isEmpty()) {
            enforceJson = true;
        }

        const int strokeBudget = m_strokeBudgetSpin ? m_strokeBudgetSpin->value() : 400;
        const QString reasoningEffort = m_reasoningEffortCombo ? m_reasoningEffortCombo->currentData().toString() : QString();
        const qreal configuredTemp = m_temperatureSpin ? m_temperatureSpin->value() : 0.70;
        const qreal temperature = !m_goalSelfCorrectionFeedback.isEmpty() ? qMin<qreal>(0.20, configuredTemp) : configuredTemp;
        const qreal topP = m_topPSpin ? m_topPSpin->value() : 1.0;
        const int maxTokens = m_maxTokensSpin ? m_maxTokensSpin->value() : 0;

        const QString additionalInstruction = m_goalSelfCorrectionFeedback;

        const KisAiStrokeProgram *accumProg = (m_goalCurrentStep > 1 && !m_goalAccumulatedProgram.operations.isEmpty())
            ? &m_goalAccumulatedProgram : nullptr;
        const QJsonObject payload = KisAiStrokeProgramCodec::buildGoalStepPayload(
            model,
            m_goalPrompt,
            canvasSize,
            m_goalCurrentStep,
            m_goalTotalSteps,
            imageBase64,
            additionalInstruction,
            strokeBudget,
            reasoningEffort,
            !m_goalVisionFallbackActive,
            true, // enableStreaming
            enforceJson, // enforceJsonFormat
            temperature,
            topP,
            maxTokens,
            static_cast<int>(artStyle),
            accumProg,
            m_lastGoalCritique,
            QStringLiteral("auto")
        );

        logDebug(QStringLiteral("GOAL_REQ"), QStringLiteral(
            "Step %1/%2 POST (model=%3, withImage=%4, fallbackActive=%5, temp=%6, top_p=%7, stream=true)")
            .arg(m_goalCurrentStep).arg(m_goalTotalSteps).arg(model)
            .arg(m_lastGoalRequestHadImage ? QStringLiteral("Yes") : QStringLiteral("No"))
            .arg(m_goalVisionFallbackActive ? QStringLiteral("Yes") : QStringLiteral("No"))
            .arg(QString::number(temperature, 'f', 2))
            .arg(QString::number(topP, 'f', 2)));

        QNetworkRequest request{QUrl(endpoint)};
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

        if (endpoint.contains(QLatin1String("openrouter.ai"), Qt::CaseInsensitive)) {
            request.setRawHeader("HTTP-Referer", "https://github.com/TopiTech/ai_stroke_painter");
            request.setRawHeader("X-Title", "AI Stroke Painter");
        }

        m_activeRequestEndpoint = endpoint;
        m_isStreamingRequest = true;
        m_streamedContent.clear();
        m_sseBuffer.clear();
        m_requestWasCancelled = false;
        m_requestTimedOut = false;
        m_responseTooLarge = false;
        m_responseBuffer.clear();
        m_requestElapsedTimer.start();
        m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);
        setBusy(true);

        const QString visionTag = m_lastGoalRequestHadImage
            ? i18n(" (Vision画像付)")
            : (m_goalVisionFallbackActive ? i18n(" (テキストフォールバック)") : QString());
        setStatus(i18n("%1 に Goal ステップ %2/%3 を送信中%4…",
            KisAiIllustrationRenderer::displayEndpoint(endpoint),
            m_goalCurrentStep,
            m_goalTotalSteps,
            visionTag));
        if (m_goalPhaseLabel) {
            m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2: LLM 生成中%3…", m_goalCurrentStep, m_goalTotalSteps, visionTag));
        }

        connect(m_reply.data(), &QNetworkReply::readyRead, this, [this] { appendReplyData(m_reply.data()); });
        connect(m_reply.data(), &QNetworkReply::finished, this, [this] { finishGoalStepRequest(); });

        if (!m_activityTimer) {
            m_activityTimer = new QTimer(this);
            m_activityTimer->setSingleShot(true);
            connect(m_activityTimer, &QTimer::timeout, this, [this] {
                if (m_reply) {
                    m_requestTimedOut = true;
                    m_reply->abort();
                }
            });
        }
        const int timeoutSec = m_timeoutSecSpin ? m_timeoutSecSpin->value() : 90;
        const int initialTimeoutMs = qBound(10'000, timeoutSec * 1000, MAX_REQUEST_TIMEOUT_MS);
        m_activityTimer->start(initialTimeoutMs);

        if (!m_progressTimer) {
            m_progressTimer = new QTimer(this);
            connect(m_progressTimer, &QTimer::timeout, this, &KisAiIllustrationDocker::updateProgressStatus);
        }
        m_progressTimer->start(1000);
    }
}

void KisAiIllustrationDocker::finishGoalStepRequest()
{
    stopAllRequestTimers();

    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    const bool requestWasCancelled = m_requestWasCancelled;
    const bool requestTimedOut = m_requestTimedOut;
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    setBusy(false);

    if (!reply) {
        m_responseBuffer.clear();
        m_streamedContent.clear();
        m_sseBuffer.clear();
        finishGoalMode(false);
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray responseContentType = reply->rawHeader("Content-Type");
    const QByteArray rawResponse = takeReplyData(reply.data());
    const bool responseTooLarge = m_responseTooLarge;
    m_responseTooLarge = false;
    reply->deleteLater();

    if (requestWasCancelled) {
        logDebug(QStringLiteral("GOAL_CANCEL"), QStringLiteral("Goalモードが中止されました。"));
        setStatus(i18n("Goal モードを中止しました。"));
        m_streamedContent.clear();
        m_sseBuffer.clear();
        finishGoalMode(false);
        return;
    }
    if (requestTimedOut) {
        const qint64 elapsedSec = m_requestElapsedTimer.isValid() ? m_requestElapsedTimer.elapsed() / 1000 : 0;
        logDebug(QStringLiteral("GOAL_TIMEOUT"), QStringLiteral("ステップ %1 のリクエストがタイムアウトしました (%2秒経過)。").arg(m_goalCurrentStep).arg(elapsedSec));
        const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
        if (m_goalCurrentRetryCount < maxRetries) {
            m_streamedContent.clear();
            m_sseBuffer.clear();
            scheduleGoalStepRetry(i18n("リクエストタイムアウト"), false);
            return;
        }
        if (!m_streamedContent.isEmpty()) {
            setStatus(i18n("LLM からのデータ受信が %1 秒間途絶えたため中止しました。", ACTIVITY_TIMEOUT_MS / 1000), true);
        } else {
            setStatus(i18n("LLM の初期応答が %1 秒以内に届かなかったため中止しました。", INITIAL_REQUEST_TIMEOUT_MS / 1000), true);
        }
        m_streamedContent.clear();
        m_sseBuffer.clear();
        finishGoalMode(false);
        return;
    }
    if (responseTooLarge) {
        logDebug(QStringLiteral("GOAL_OVERFLOW"), QStringLiteral("ステップ %1 の応答が上限を超えました。").arg(m_goalCurrentStep));
        setStatus(i18n("LLM の応答が上限を超えています。"), true);
        m_streamedContent.clear();
        m_sseBuffer.clear();
        finishGoalMode(false);
        return;
    }
    // Content-type validation: reject clearly non-JSON responses before parsing.
    if (!KisAiStrokeProgramCodec::isAcceptedResponseContentType(responseContentType, true)) {
        logDebug(QStringLiteral("GOAL_CONTENT_TYPE"),
                 QStringLiteral("予期しないContent-Type: %1").arg(QString::fromUtf8(responseContentType)));
        setStatus(i18n("LLM の応答が JSON または SSE 形式ではありません (Content-Type: %1)。",
                       QString::fromUtf8(responseContentType.isEmpty() ? QByteArray("unknown") : responseContentType)),
                  true);
        m_streamedContent.clear();
        m_sseBuffer.clear();
        finishGoalMode(false);
        return;
    }
    if (!requestSucceeded) {
        QString detail;
        QJsonParseError parseErr;
        const QJsonDocument errDoc = QJsonDocument::fromJson(rawResponse, &parseErr);
        if (!errDoc.isNull() && errDoc.isObject()) {
            const QJsonValue errVal = errDoc.object().value(QStringLiteral("error"));
            if (errVal.isObject()) {
                detail = errVal.toObject().value(QStringLiteral("message")).toString().trimmed();
            } else if (errVal.isString()) {
                detail = errVal.toString().trimmed();
            }
        }
        if (detail.isEmpty()) {
            detail = reply->errorString();
        }

        logDebug(QStringLiteral("GOAL_ERROR"), QStringLiteral("Step %1 HTTP %2: %3\nRaw: %4")
            .arg(m_goalCurrentStep).arg(httpStatus).arg(detail, QString::fromUtf8(rawResponse.left(500))));

        m_streamedContent.clear();
        m_sseBuffer.clear();

        // Vision フォールバック（保険機構）:
        // 画像付きリクエストが失敗した場合、テキストのみの指示にフォールバックして再試行
        if (m_lastGoalRequestHadImage && !m_goalVisionFallbackActive) {
            m_goalVisionFallbackActive = true;
            m_lastGoalRequestHadImage = false;
            logDebug(QStringLiteral("GOAL_FALLBACK"), QStringLiteral(
                "⚠️ 画像付きリクエストが失敗したため (HTTP %1: %2)、テキストのみの指示にフォールバックしてステップ %3 を再試行します…")
                .arg(httpStatus).arg(detail).arg(m_goalCurrentStep));
            setStatus(i18n("⚠️ Vision画像エラーのため、テキストのみの指示にフォールバックして再試行します (HTTP %1)…", httpStatus), true);
            if (m_goalPhaseLabel) {
                m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2: テキストフォールバック再試行中…", m_goalCurrentStep, m_goalTotalSteps));
            }
            QTimer::singleShot(600, this, [this] {
                executeGoalStep();
            });
            return;
        }

        const bool isRetryableHttp = (httpStatus == 429 || (httpStatus >= 500 && httpStatus <= 504));
        const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
        if (isRetryableHttp && m_goalCurrentRetryCount < maxRetries) {
            int retryAfterSec = 0;
            const QByteArray retryAfterHeader = reply->rawHeader("Retry-After");
            if (!retryAfterHeader.isEmpty()) {
                bool ok = false;
                const int val = retryAfterHeader.trimmed().toInt(&ok);
                if (ok && val > 0) {
                    retryAfterSec = qMin(val, 60);
                }
            }
            scheduleGoalStepRetry(i18n("HTTP %1 一時エラー", httpStatus), false, retryAfterSec);
            return;
        }

        if (!detail.isEmpty()) {
            setStatus(i18n("LLM への接続または応答に失敗しました (HTTP %1): %2", httpStatus, detail), true);
        } else {
            setStatus(i18n("LLM への接続または応答に失敗しました (HTTP %1)。", httpStatus), true);
        }
        finishGoalMode(false);
        return;
    }

    if (m_isStreamingRequest && !m_sseBuffer.isEmpty()) {
        bool isDone = false;
        KisAiStrokeProgramCodec::parseSseStreamChunk(QByteArrayLiteral("\n\n"), &m_sseBuffer, &m_streamedContent, &isDone);
    }

    const QByteArray response = !m_streamedContent.trimmed().isEmpty()
        ? m_streamedContent.toUtf8()
        : rawResponse;
    m_streamedContent.clear();
    m_sseBuffer.clear();

    logDebug(QStringLiteral("GOAL_RESP"), QStringLiteral("Step %1 HTTP %2 (%3 bytes) 受信完了\nRaw: %4")
        .arg(m_goalCurrentStep).arg(httpStatus).arg(response.size()).arg(QString::fromUtf8(response.left(1000))));

    KisAiStrokeProgram program;
    QString parseError;
    KisAiJsonDiagnostic diagnostic;
    if (!KisAiStrokeProgramCodec::parseResponse(response, &program, &parseError, &diagnostic)) {
        m_lastJsonDiagnostic = diagnostic;
        logDebug(QStringLiteral("GOAL_PARSE_ERR"), QStringLiteral("%1\n%2").arg(parseError, diagnostic.formatForLog()));
        const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
        if (m_goalCurrentRetryCount < maxRetries) {
            scheduleGoalStepRetry(parseError, true);
            return;
        }
        setStatus(parseError, true);
        finishGoalMode(false);
        return;
    }
    m_goalCurrentRetryCount = 0;
    m_goalSelfCorrectionFeedback.clear();

    // Step state is controlled by the local Goal Mode controller, not by an
    // untrusted model response. It also keeps the generated undo-group title
    // and the cumulative preview in sync.
    program.canvasSize = effectiveCanvasSize();
    program.currentStep = m_goalCurrentStep;
    program.totalSteps = m_goalTotalSteps;
    program.goalReached = m_goalCurrentStep >= m_goalTotalSteps;

    const QSize previewTargetSize = m_previewLabel->size().isEmpty() ? QSize(256, 256) : m_previewLabel->size();
    m_goalAccumulatedProgram = KisAiStrokeProgramCodec::mergePrograms(m_goalAccumulatedProgram, program);
    const QImage preview = KisAiStrokeRenderer::renderProgramToImage(m_goalAccumulatedProgram, previewTargetSize);
    if (!preview.isNull()) {
        m_previewLabel->setPixmap(QPixmap::fromImage(preview).scaled(previewTargetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
    if (view && view->image()) {
        QString statusMsg;
        if (KisAiStrokeRenderer::renderProgramToLayers(view->image(),
                                                       m_mainWindow->viewManager(),
                                                       program,
                                                       &statusMsg,
                                                       true,
                                                       &m_goalAccumulatedProgram)) {
            const QString summary = KisAiStrokeProgramCodec::formatLayerSummary(program);
            const int qualityPercent = qRound(qBound<qreal>(0.0, program.completionScore, 1.0) * 100.0);
            const QString detailMsg = i18n("%1 (%2 / 構造品質 %3%)", statusMsg, summary, qualityPercent);
            setStatus(detailMsg);
        } else {
            setStatus(statusMsg, true);
        }
    } else {
        setStatus(i18n("キャンバスが利用できないため、ストロークを描画できませんでした。"), true);
        finishGoalMode(false);
        return;
    }

    if (m_goalPhaseLabel) {
        m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2 (%3) 完了", m_goalCurrentStep, m_goalTotalSteps, program.stepPhase));
    }
    if (m_agentFocusLabel) {
        if (!program.targetFocusArea.isEmpty()) {
            m_agentFocusLabel->setText(i18n("🎯 着目領域: %1", program.targetFocusArea));
        } else {
            m_agentFocusLabel->setText(i18n("🎯 着目領域: 全体構成"));
        }
    }
    if (m_readinessBar) {
        m_readinessBar->setValue(qRound(program.readinessScore * 100.0));
    }
    if (m_critiqueLabel) {
        const QString critique = !program.agentCritique.isEmpty() ? program.agentCritique : program.visualCritique;
        m_lastGoalCritique = critique;
        if (!critique.isEmpty()) {
            m_critiqueLabel->setText(i18n("👀 AI視覚批評: %1", critique));
        } else {
            m_critiqueLabel->setText(i18n("ステップ %1 の作画が完了しました。", m_goalCurrentStep));
        }
    }

    const bool agentEarlyFinish = (program.readinessScore >= 0.85 && program.goalReached && m_goalCurrentStep >= 2);
    if (agentEarlyFinish) {
        logDebug(QStringLiteral("GOAL_AGENT"), QStringLiteral("Autonomous Agent achieved target readiness (%1 >= 0.85). Early completion triggered.")
            .arg(program.readinessScore));
    }

    if (m_goalCurrentStep >= m_goalTotalSteps || agentEarlyFinish) {
        finishGoalMode(true);
    } else if (m_pausePerStepCheck && m_pausePerStepCheck->isChecked()) {
        m_waitingForUserStepAdvance = true;
        if (m_nextStepButton) {
            m_nextStepButton->setVisible(true);
            m_nextStepButton->setEnabled(true);
        }
        if (m_finishGoalButton) {
            m_finishGoalButton->setVisible(true);
            m_finishGoalButton->setEnabled(true);
        }
        setStatus(i18n("ステップ %1 完了。キャンバスへの手動加筆・確認後、「次のステップへ進む」を押してください。", m_goalCurrentStep));
    } else {
        QTimer::singleShot(300, this, [this] {
            advanceGoalStep();
        });
    }
}

void KisAiIllustrationDocker::advanceGoalStep()
{
    if (!m_goalModeActive) {
        return;
    }
    m_waitingForUserStepAdvance = false;
    m_goalCurrentRetryCount = 0;
    m_goalSelfCorrectionFeedback.clear();
    m_goalCurrentStep++;
    if (m_goalCurrentStep > m_goalTotalSteps) {
        finishGoalMode(true);
        return;
    }
    executeGoalStep();
}

void KisAiIllustrationDocker::finishGoalMode(bool success)
{
    m_goalModeActive = false;
    m_waitingForUserStepAdvance = false;
    m_goalCurrentRetryCount = 0;
    m_goalSelfCorrectionFeedback.clear();
    if (!m_goalApiKey.isEmpty()) {
        m_goalApiKey.fill(QLatin1Char('\0'));
        m_goalApiKey.clear();
    }
    m_goalVisionFallbackActive = false;
    m_lastGoalRequestHadImage = false;
    m_goalAccumulatedProgram = KisAiStrokeProgram();
    m_lastGoalCritique.clear();

    logDebug(QStringLiteral("GOAL_FINISH"), QStringLiteral("Goalモード終了 (success=%1, step=%2/%3)")
        .arg(success ? QStringLiteral("true") : QStringLiteral("false")).arg(m_goalCurrentStep).arg(m_goalTotalSteps));

    if (m_goalInspectorCard) {
        if (m_nextStepButton) {
            m_nextStepButton->setVisible(false);
            m_nextStepButton->setEnabled(false);
        }
        if (m_finishGoalButton) {
            m_finishGoalButton->setVisible(false);
            m_finishGoalButton->setEnabled(false);
        }
        if (m_goalPhaseLabel) {
            if (success) {
                m_goalPhaseLabel->setText(i18n("🎯 Goal自律作画 完了 (全 %1 段階, 完成度 %2%)",
                    m_goalCurrentStep, m_readinessBar ? m_readinessBar->value() : 100));
            } else {
                m_goalPhaseLabel->setText(i18n("🎯 Goal作画 中断 (ステップ %1/%2)", m_goalCurrentStep, m_goalTotalSteps));
            }
        }
    }
    if (success) {
        setStatus(i18n("🎯 Goal作画が完了しました。Kritaのレイヤードックで各層を確認・調整できます。"));
    }
    setBusy(false);
}

void KisAiIllustrationDocker::scheduleGoalStepRetry(const QString &reasonMessage, bool isSelfCorrection, int retryAfterSec)
{
    const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
    if (m_goalCurrentRetryCount >= maxRetries) {
        logDebug(QStringLiteral("GOAL_RETRY_ABORT"),
                 QStringLiteral("最大リトライ回数 (%1回) に達したためGoalモード中断: %2")
                     .arg(maxRetries).arg(reasonMessage));
        setStatus(reasonMessage, true);
        finishGoalMode(false);
        return;
    }

    m_goalCurrentRetryCount++;

    int delayMs = 1500 * (1 << (m_goalCurrentRetryCount - 1));
    delayMs += (QRandomGenerator::global()->bounded(500));
    if (retryAfterSec > 0) {
        delayMs = qMax(delayMs, retryAfterSec * 1000);
    } else if (isSelfCorrection) {
        delayMs = 800 * m_goalCurrentRetryCount;
    }

    if (isSelfCorrection) {
        QString feedback = QStringLiteral("[CRITICAL RETRY / JSON ERROR: Your step %1 output could not be parsed as valid JSON.\n")
            .arg(m_goalCurrentStep);
        if (!m_lastJsonDiagnostic.errorMessage.isEmpty()) {
            feedback += QStringLiteral("Error: %1\n").arg(m_lastJsonDiagnostic.errorMessage);
        }
        if (m_lastJsonDiagnostic.errorLine > 0) {
            feedback += QStringLiteral("Near line %1, column %2.\n")
                .arg(m_lastJsonDiagnostic.errorLine)
                .arg(m_lastJsonDiagnostic.errorColumn);
        }
        if (!m_lastJsonDiagnostic.errorSnippet.isEmpty()) {
            feedback += QStringLiteral("Snippet: %1\n").arg(m_lastJsonDiagnostic.errorSnippet);
        }
        if (m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("end of file"), Qt::CaseInsensitive) ||
            m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("unterminated"), Qt::CaseInsensitive) ||
            m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("truncate"), Qt::CaseInsensitive) ||
            m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("unclosed"), Qt::CaseInsensitive)) {
            feedback += QStringLiteral("Notice: Output was cut off/truncated before completion. Keep operations count lower and close all JSON brackets properly.\n");
        }
        feedback += QStringLiteral("CRITICAL: Output ONLY valid RFC 8259 JSON without any markdown formatting or commentary outside JSON.]");
        m_goalSelfCorrectionFeedback = feedback;
    } else {
        m_goalSelfCorrectionFeedback.clear();
    }

    const QString retryTypeStr = isSelfCorrection
        ? i18n("JSON自己修復")
        : i18n("一時エラー再送");

    const QString statusMsg = i18n("⚠️ Goal ステップ %1 %2 (%3/%4回目): %5 秒後に自動再試行します… [%6]",
        m_goalCurrentStep,
        retryTypeStr,
        m_goalCurrentRetryCount,
        maxRetries,
        QString::number(delayMs / 1000.0, 'f', 1),
        reasonMessage);

    setStatus(statusMsg);
    if (m_goalPhaseLabel) {
        m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2: %3 再試行待機中…",
            m_goalCurrentStep, m_goalTotalSteps, retryTypeStr));
    }
    logDebug(QStringLiteral("GOAL_RETRY_SCHEDULE"),
             QStringLiteral("Goal Step %1 %2 (回数: %3/%4, 待機: %5ms, 理由: %6)")
                 .arg(m_goalCurrentStep)
                 .arg(retryTypeStr)
                 .arg(m_goalCurrentRetryCount)
                 .arg(maxRetries)
                 .arg(delayMs)
                 .arg(reasonMessage));

    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &KisAiIllustrationDocker::executeRetry);
    }
    // Start the timer before setBusy(): setBusy() decides the Cancel button's
    // visibility from m_retryTimer->isActive(), so starting it afterwards hid
    // Cancel for the whole back-off wait.
    m_retryTimer->start(delayMs);
    setBusy(true);
}

void KisAiIllustrationDocker::executeGoalStepRetry()
{
    if (!m_goalModeActive) {
        return;
    }
    logDebug(QStringLiteral("GOAL_RETRY_EXEC"),
             QStringLiteral("Goal ステップ %1 再試行実行中 (回数: %2)")
                 .arg(m_goalCurrentStep).arg(m_goalCurrentRetryCount));

    executeGoalStep();
}

void KisAiIllustrationDocker::scheduleRetry(const QString &reasonMessage, bool isSelfCorrection, int retryAfterSec)
{
    const int maxRetries = m_maxRetriesSpin ? m_maxRetriesSpin->value() : 2;
    if (m_currentRetryCount >= maxRetries) {
        logDebug(QStringLiteral("RETRY_ABORT"),
                 QStringLiteral("最大リトライ回数 (%1回) に達したため中断: %2")
                     .arg(maxRetries).arg(reasonMessage));
        setStatus(reasonMessage, true);
        m_currentRetryCount = 0;
        m_isSelfCorrectionRetry = false;
        clearInFlightApiKey();
        return;
    }

    m_currentRetryCount++;
    m_isSelfCorrectionRetry = isSelfCorrection;

    // Exponential backoff: base 1500ms * 2^(retry-1) + jitter (0..500ms)
    int delayMs = 1500 * (1 << (m_currentRetryCount - 1));
    delayMs += (QRandomGenerator::global()->bounded(500));
    if (retryAfterSec > 0) {
        delayMs = qMax(delayMs, retryAfterSec * 1000);
    } else if (isSelfCorrection) {
        delayMs = 800 * m_currentRetryCount;
    }

    const QString retryTypeStr = isSelfCorrection
        ? i18n("JSON自己修復")
        : i18n("一時エラー再送");

    const QString statusMsg = i18n("⚠️ %1 (%2/%3回目): %4 秒後に自動再試行します… [%5]",
        retryTypeStr,
        m_currentRetryCount,
        maxRetries,
        QString::number(delayMs / 1000.0, 'f', 1),
        reasonMessage);

    setStatus(statusMsg);
    logDebug(QStringLiteral("RETRY_SCHEDULE"),
             QStringLiteral("%1 (回数: %2/%3, 待機: %4ms, 理由: %5)")
                 .arg(retryTypeStr)
                 .arg(m_currentRetryCount)
                 .arg(maxRetries)
                 .arg(delayMs)
                 .arg(reasonMessage));

    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &KisAiIllustrationDocker::executeRetry);
    }
    // See scheduleRetry(): the timer must be active before setBusy() evaluates it.
    m_retryTimer->start(delayMs);
    setBusy(true);
}

void KisAiIllustrationDocker::executeRetry()
{
    if (m_goalModeActive) {
        executeGoalStepRetry();
        return;
    }

    // Mark the re-entry so generateLlmStrokes() keeps the current retry budget.
    m_retryInFlight = true;

    if (m_isSelfCorrectionRetry) {
        QString correctionPrompt = m_lastFailedPrompt;
        if (m_isQualityCorrectionRetry) {
            correctionPrompt += QStringLiteral("\n\n[QUALITY CORRECTION REQUEST]\n"
                "Your previous output parsed as valid JSON, but had structural quality issues:\n");
            if (m_lastQualityReport.score < 0.55) {
                correctionPrompt += QStringLiteral("- Low structural score: %1/1.0. Ensure rich silhouettes, clean contours, and registered layers.\n")
                    .arg(QString::number(m_lastQualityReport.score, 'f', 2));
            }
            if (!m_lastQualityReport.warnings.isEmpty()) {
                correctionPrompt += QStringLiteral("- Specific issues detected:\n");
                for (const QString &w : m_lastQualityReport.warnings) {
                    correctionPrompt += QStringLiteral("  * %1\n").arg(w);
                }
            }
            correctionPrompt += QStringLiteral("- Please generate solid silhouette base color fills on 'Flats' layer first before adding Shading and Lineart.\n"
                "- Ensure major subjects are filled with solid color masses to prevent empty transparent holes.\n"
                "Please regenerate the StrokeProgram correcting these issues while strictly preserving the original user prompt.]");

            logDebug(QStringLiteral("RETRY_EXEC"),
                     QStringLiteral("品質自己修復リクエストを実行中 (回数: %1/%2)")
                         .arg(m_currentRetryCount).arg(m_maxRetryCount));
        } else {
            correctionPrompt += QStringLiteral("\n\n[SYSTEM FEEDBACK / RETRY: Your previous output could not be parsed as valid JSON.\n");
            if (!m_lastJsonDiagnostic.errorMessage.isEmpty()) {
                correctionPrompt += QStringLiteral("Error: %1\n").arg(m_lastJsonDiagnostic.errorMessage);
            }
            if (m_lastJsonDiagnostic.errorLine > 0) {
                correctionPrompt += QStringLiteral("Near line %1, column %2.\n")
                    .arg(m_lastJsonDiagnostic.errorLine)
                    .arg(m_lastJsonDiagnostic.errorColumn);
            }
            if (!m_lastJsonDiagnostic.errorSnippet.isEmpty()) {
                correctionPrompt += QStringLiteral("Snippet: %1\n").arg(m_lastJsonDiagnostic.errorSnippet);
            }
            if (m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("end of file"), Qt::CaseInsensitive) ||
                m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("unterminated"), Qt::CaseInsensitive) ||
                m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("truncate"), Qt::CaseInsensitive) ||
                m_lastJsonDiagnostic.errorMessage.contains(QStringLiteral("unclosed"), Qt::CaseInsensitive)) {
                correctionPrompt += QStringLiteral("Notice: The JSON was truncated before completion. Please generate a more concise response with fewer operations to fit within token limits, or close all opened objects and arrays properly.\n");
            }
            correctionPrompt += QStringLiteral("CRITICAL: Output ONLY valid JSON conforming strictly to the KisAiStrokeProgram schema without markdown explanation or stray text.]");

            logDebug(QStringLiteral("RETRY_EXEC"),
                     QStringLiteral("JSON自己修復リクエストを実行中 (回数: %1/%2)")
                         .arg(m_currentRetryCount).arg(m_maxRetryCount));
        }

        generateLlmStrokes(correctionPrompt);
    } else {
        logDebug(QStringLiteral("RETRY_EXEC"),
                 QStringLiteral("通信リトライリクエストを実行中 (回数: %1/%2)")
                     .arg(m_currentRetryCount).arg(m_maxRetryCount));

        generateLlmStrokes(m_lastFailedPrompt);
    }
}

void KisAiIllustrationDocker::cancelRetry()
{
    if (m_retryTimer && m_retryTimer->isActive()) {
        m_retryTimer->stop();
    }
    m_currentRetryCount = 0;
    m_isSelfCorrectionRetry = false;
    m_retryInFlight = false;
    m_goalCurrentRetryCount = 0;
    m_goalSelfCorrectionFeedback.clear();
    m_lastFailedPrompt.clear();
    m_lastJsonDiagnostic = KisAiJsonDiagnostic();
    clearInFlightApiKey();
}

void KisAiIllustrationDocker::clearInFlightApiKey()
{
    if (!m_inFlightApiKey.isEmpty()) {
        m_inFlightApiKey.fill(QLatin1Char('\0'));
        m_inFlightApiKey.clear();
    }
}

void KisAiIllustrationDocker::loadSettings()
{
    QSettings settings;
    // エンドポイント & モデル
    const QString savedLlmEp = readSafeStoredEndpoint(settings,
                                                      QStringLiteral("AIIllustration/llmEndpoint"),
                                                      QStringLiteral("AIIllustration/endpoint"));
    const QString savedLlmModel = settings.value(QStringLiteral("AIIllustration/llmModel"),
        settings.value(QStringLiteral("AIIllustration/model"))).toString();
    if (m_endpointEditor) {
        m_endpointEditor->setText(savedLlmEp.isEmpty() ? QStringLiteral("https://api.openai.com/v1/chat/completions") : savedLlmEp);
    }
    if (m_modelEditor) {
        m_modelEditor->setText(savedLlmModel.isEmpty() ? QStringLiteral("gpt-4o") : savedLlmModel);
    }

    // API keys are opt-in and use the operating system's current-user data
    // protection. Previous releases wrote plaintext/base64-equivalent data to
    // QSettings, so migrate it once on Windows or remove it elsewhere.
    bool saveKey = settings.value(QStringLiteral("AIIllustration/saveApiKey"), false).toBool();
    QString restoredKey;
    if (saveKey) {
        const QString storedKey = settings.value(QStringLiteral("AIIllustration/apiKey")).toString();
        if (!storedKey.isEmpty() && !unprotectApiKeyForCurrentUser(storedKey, &restoredKey)) {
#if defined(Q_OS_WIN)
            // Legacy value: either a plaintext key or its Base64 form. A blob that
            // decrypts to garbage (for example a corrupt dpapi: payload or a key
            // from another user/device) must be deleted rather than re-encrypted.
            const QByteArray legacyBytes = storedKey.startsWith(QStringLiteral("sk-"))
                ? storedKey.toUtf8()
                : QByteArray::fromBase64(storedKey.toLatin1());
            const QString legacyKey = QString::fromUtf8(legacyBytes);
            const bool plausibleKey = !legacyKey.isEmpty()
                && std::all_of(legacyKey.cbegin(), legacyKey.cend(), [](QChar c) {
                       return c.isPrint() && !c.isSpace();
                   });
            QString protectedKey;
            if (plausibleKey && protectApiKeyForCurrentUser(legacyKey, &protectedKey)) {
                settings.setValue(QStringLiteral("AIIllustration/apiKey"), protectedKey);
                restoredKey = legacyKey;
            } else {
                saveKey = false;
            }
#else
            saveKey = false;
#endif
        }
    }
    if (!saveKey) {
        settings.setValue(QStringLiteral("AIIllustration/saveApiKey"), false);
        settings.remove(QStringLiteral("AIIllustration/apiKey"));
    }
    if (m_saveApiKeyCheck) {
        const QSignalBlocker blocker(m_saveApiKeyCheck);
        m_saveApiKeyCheck->setChecked(saveKey);
    }
    if (saveKey && m_apiKeyEditor && !restoredKey.isEmpty()) {
        m_apiKeyEditor->setText(restoredKey);
    }

    // キャンバスサイズ
    if (m_widthSpin) m_widthSpin->setValue(readBoundedSetting(settings, QStringLiteral("AIIllustration/canvasWidth"), 1024, m_widthSpin->minimum(), m_widthSpin->maximum()));
    if (m_heightSpin) m_heightSpin->setValue(readBoundedSetting(settings, QStringLiteral("AIIllustration/canvasHeight"), 1024, m_heightSpin->minimum(), m_heightSpin->maximum()));

    // ストローク予算
    if (m_strokeBudgetSpin) m_strokeBudgetSpin->setValue(readBoundedSetting(settings, QStringLiteral("AIIllustration/strokeBudget"), 500, m_strokeBudgetSpin->minimum(), m_strokeBudgetSpin->maximum()));

    // Goal モード
    const bool goalEnabled = settings.value(QStringLiteral("AIIllustration/goalModeEnabled"), false).toBool();
    if (m_goalModeCheck) m_goalModeCheck->setChecked(goalEnabled);
    if (m_goalStepsSpin) m_goalStepsSpin->setValue(readBoundedSetting(settings, QStringLiteral("AIIllustration/goalSteps"), 4, m_goalStepsSpin->minimum(), m_goalStepsSpin->maximum()));
    const int artStyle = settings.value(QStringLiteral("AIIllustration/artStyle"), 0).toInt();
    if (m_artStyleCombo) {
        int idx = m_artStyleCombo->findData(artStyle);
        if (idx >= 0) m_artStyleCombo->setCurrentIndex(idx);
    }
    const bool pauseStep = settings.value(QStringLiteral("AIIllustration/pausePerStep"), false).toBool();
    if (m_pausePerStepCheck) m_pausePerStepCheck->setChecked(pauseStep);

    // デバッグモード
    const bool debugEnabled = settings.value(QStringLiteral("AIIllustration/debugModeEnabled"), false).toBool();
    if (m_debugModeCheck) {
        m_debugModeCheck->setChecked(debugEnabled);
        if (m_debugCard) m_debugCard->setVisible(debugEnabled);
    }

    // 生成モード
    const int genMode = readBoundedSetting(settings, QStringLiteral("AIIllustration/generationMode"), 0, 0, 3);
    if (m_modeCombo) {
        int idx = m_modeCombo->findData(genMode);
        if (idx >= 0) {
            const QSignalBlocker blocker(m_modeCombo);
            m_modeCombo->setCurrentIndex(idx);
            m_currentMode = static_cast<GenerationMode>(genMode);
        }
    }

    // AI 詳細設定
    if (m_temperatureSpin) m_temperatureSpin->setValue(qBound(m_temperatureSpin->minimum(), settings.value(QStringLiteral("AIIllustration/temperature"), 0.70).toDouble(), m_temperatureSpin->maximum()));
    if (m_topPSpin) m_topPSpin->setValue(qBound(m_topPSpin->minimum(), settings.value(QStringLiteral("AIIllustration/topP"), 1.0).toDouble(), m_topPSpin->maximum()));
    if (m_maxTokensSpin) m_maxTokensSpin->setValue(readBoundedSetting(settings, QStringLiteral("AIIllustration/maxTokens"), 0, m_maxTokensSpin->minimum(), m_maxTokensSpin->maximum()));
    if (m_maxRetriesSpin) {
        m_maxRetryCount = readBoundedSetting(settings, QStringLiteral("AIIllustration/maxRetries"), 2, m_maxRetriesSpin->minimum(), m_maxRetriesSpin->maximum());
        m_maxRetriesSpin->setValue(m_maxRetryCount);
    }
    if (m_timeoutSecSpin) m_timeoutSecSpin->setValue(readBoundedSetting(settings, QStringLiteral("AIIllustration/timeoutSec"), 90, m_timeoutSecSpin->minimum(), m_timeoutSecSpin->maximum()));
    if (m_jsonModeCombo) {
        const int jMode = settings.value(QStringLiteral("AIIllustration/jsonMode"), 0).toInt();
        if (jMode >= 0 && jMode < m_jsonModeCombo->count()) {
            m_jsonModeCombo->setCurrentIndex(jMode);
        }
    }
    if (m_reasoningEffortCombo) {
        const QString rEffort = settings.value(QStringLiteral("AIIllustration/reasoningEffort"), QString()).toString();
        const int idx = m_reasoningEffortCombo->findData(rEffort);
        if (idx >= 0) {
            m_reasoningEffortCombo->setCurrentIndex(idx);
        }
    }
    if (m_customInstructionsEdit) {
        m_customInstructionsEdit->setPlainText(settings.value(QStringLiteral("AIIllustration/customInstructions"), QString()).toString());
    }
}

void KisAiIllustrationDocker::saveSettingsForMode(GenerationMode mode)
{
    QSettings settings;
    if (m_endpointEditor && m_modelEditor) {
        const QString ep = m_endpointEditor->text().trimmed();
        const QString mdl = m_modelEditor->text().trimmed();
        QString validationError;
        const bool safeEndpoint =
            ep.isEmpty() || KisAiIllustrationRenderer::validateImageEndpoint(ep, &validationError);
        if (mode == GenerationMode::RemoteImage) {
            if (!safeEndpoint) {
                settings.remove(QStringLiteral("AIIllustration/imageEndpoint"));
                return;
            }
            settings.setValue(QStringLiteral("AIIllustration/imageEndpoint"), ep);
            settings.setValue(QStringLiteral("AIIllustration/imageModel"), mdl);
        } else if (mode == GenerationMode::LlmStrokes) {
            if (!safeEndpoint) {
                settings.remove(QStringLiteral("AIIllustration/llmEndpoint"));
                settings.remove(QStringLiteral("AIIllustration/endpoint"));
                return;
            }
            settings.setValue(QStringLiteral("AIIllustration/llmEndpoint"), ep);
            settings.setValue(QStringLiteral("AIIllustration/llmModel"), mdl);
            settings.setValue(QStringLiteral("AIIllustration/endpoint"), ep);
            settings.setValue(QStringLiteral("AIIllustration/model"), mdl);
        }
    }
}

void KisAiIllustrationDocker::saveSettings()
{
    QSettings settings;
    const auto mode = static_cast<GenerationMode>(m_modeCombo ? m_modeCombo->currentData().toInt() : 0);
    settings.setValue(QStringLiteral("AIIllustration/generationMode"), static_cast<int>(mode));

    saveSettingsForMode(mode);

    if (m_saveApiKeyCheck && m_apiKeyEditor) {
        const bool saveKey = m_saveApiKeyCheck->isChecked();
        settings.setValue(QStringLiteral("AIIllustration/saveApiKey"), saveKey);
        if (saveKey) {
            const QString rawKey = m_apiKeyEditor->text();
            if (rawKey.isEmpty()) {
                settings.remove(QStringLiteral("AIIllustration/apiKey"));
            } else {
                QString protectedKey;
                if (protectApiKeyForCurrentUser(rawKey, &protectedKey)) {
                    settings.setValue(QStringLiteral("AIIllustration/apiKey"), protectedKey);
                } else {
                    settings.setValue(QStringLiteral("AIIllustration/saveApiKey"), false);
                    settings.remove(QStringLiteral("AIIllustration/apiKey"));
                    const QSignalBlocker blocker(m_saveApiKeyCheck);
                    m_saveApiKeyCheck->setChecked(false);
                }
            }
        } else {
            settings.remove(QStringLiteral("AIIllustration/apiKey"));
        }
    }

    if (m_widthSpin) settings.setValue(QStringLiteral("AIIllustration/canvasWidth"), m_widthSpin->value());
    if (m_heightSpin) settings.setValue(QStringLiteral("AIIllustration/canvasHeight"), m_heightSpin->value());
    if (m_strokeBudgetSpin) settings.setValue(QStringLiteral("AIIllustration/strokeBudget"), m_strokeBudgetSpin->value());

    if (m_goalModeCheck) settings.setValue(QStringLiteral("AIIllustration/goalModeEnabled"), m_goalModeCheck->isChecked());
    if (m_goalStepsSpin) settings.setValue(QStringLiteral("AIIllustration/goalSteps"), m_goalStepsSpin->value());
    if (m_artStyleCombo) settings.setValue(QStringLiteral("AIIllustration/artStyle"), m_artStyleCombo->currentData().toInt());
    if (m_pausePerStepCheck) settings.setValue(QStringLiteral("AIIllustration/pausePerStep"), m_pausePerStepCheck->isChecked());
    if (m_debugModeCheck) settings.setValue(QStringLiteral("AIIllustration/debugModeEnabled"), m_debugModeCheck->isChecked());
    if (m_temperatureSpin) settings.setValue(QStringLiteral("AIIllustration/temperature"), m_temperatureSpin->value());
    if (m_topPSpin) settings.setValue(QStringLiteral("AIIllustration/topP"), m_topPSpin->value());
    if (m_maxTokensSpin) settings.setValue(QStringLiteral("AIIllustration/maxTokens"), m_maxTokensSpin->value());
    if (m_maxRetriesSpin) settings.setValue(QStringLiteral("AIIllustration/maxRetries"), m_maxRetriesSpin->value());
    if (m_timeoutSecSpin) settings.setValue(QStringLiteral("AIIllustration/timeoutSec"), m_timeoutSecSpin->value());
    if (m_jsonModeCombo) settings.setValue(QStringLiteral("AIIllustration/jsonMode"), m_jsonModeCombo->currentIndex());
    if (m_reasoningEffortCombo) settings.setValue(QStringLiteral("AIIllustration/reasoningEffort"), m_reasoningEffortCombo->currentData().toString());
    if (m_customInstructionsEdit) settings.setValue(QStringLiteral("AIIllustration/customInstructions"), m_customInstructionsEdit->toPlainText());
}

void KisAiIllustrationDocker::testLlmConnection()
{
    if (m_testReply) {
        return;
    }

    const QString endpoint = m_endpointEditor ? m_endpointEditor->text().trimmed() : QString();
    const QString model = m_modelEditor ? m_modelEditor->text().trimmed() : QString();
    const QString apiKey = m_apiKeyEditor ? m_apiKeyEditor->text() : QString();

    QString errorMessage;
    if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &errorMessage)) {
        if (m_testConnectionStatusLabel) {
            m_testConnectionStatusLabel->setStyleSheet(QStringLiteral("color: #f87171;"));
            m_testConnectionStatusLabel->setText(i18n("❌ エンドポイントエラー: %1", errorMessage));
            m_testConnectionStatusLabel->setVisible(true);
        }
        return;
    }
    if (model.isEmpty()) {
        if (m_testConnectionStatusLabel) {
            m_testConnectionStatusLabel->setStyleSheet(QStringLiteral("color: #f87171;"));
            m_testConnectionStatusLabel->setText(i18n("❌ モデル名が入力されていません。"));
            m_testConnectionStatusLabel->setVisible(true);
        }
        return;
    }
    if (apiKey.isEmpty()) {
        if (m_testConnectionStatusLabel) {
            m_testConnectionStatusLabel->setStyleSheet(QStringLiteral("color: #f87171;"));
            m_testConnectionStatusLabel->setText(i18n("❌ API キーが入力されていません。"));
            m_testConnectionStatusLabel->setVisible(true);
        }
        return;
    }

    saveSettings();

    if (m_testConnectionButton) {
        m_testConnectionButton->setEnabled(false);
        m_testConnectionButton->setText(i18n("テスト中…"));
    }
    if (m_testConnectionStatusLabel) {
        m_testConnectionStatusLabel->setStyleSheet(QStringLiteral("color: #38bdf8;"));
        m_testConnectionStatusLabel->setText(i18n("⏳ %1 に接続テスト中…", KisAiIllustrationRenderer::displayEndpoint(endpoint)));
        m_testConnectionStatusLabel->setVisible(true);
    }

    logDebug(QStringLiteral("TEST_REQ"),
             QStringLiteral("接続テスト開始: Endpoint=%1, Model=%2")
                 .arg(KisAiIllustrationRenderer::displayEndpoint(endpoint), model));

    QNetworkRequest request{QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    const auto mode = static_cast<GenerationMode>(m_modeCombo ? m_modeCombo->currentData().toInt() : 0);
    QJsonObject payload;
    if (mode == GenerationMode::RemoteImage) {
        payload[QStringLiteral("model")] = model;
        payload[QStringLiteral("prompt")] = QStringLiteral("ping test");
        payload[QStringLiteral("size")] = QStringLiteral("256x256");
    } else {
        payload[QStringLiteral("model")] = model;
        QJsonArray messages;
        messages.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QStringLiteral("Hi")}
        });
        payload[QStringLiteral("messages")] = messages;
        if (KisAiStrokeProgramCodec::isReasoningModel(model)) {
            payload[QStringLiteral("max_completion_tokens")] = 32;
        } else {
            payload[QStringLiteral("max_tokens")] = 16;
        }
    }

    m_testStartTimeMs = QDateTime::currentMSecsSinceEpoch();
    m_testResponseBuffer.clear();
    m_testResponseTooLarge = false;
    m_testReply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_testReply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);

    connect(m_testReply.data(), &QNetworkReply::readyRead, this, [this] {
        appendTestReplyData(m_testReply.data());
    });
    connect(m_testReply.data(), &QNetworkReply::finished, this, &KisAiIllustrationDocker::finishTestConnectionRequest);

    const QPointer<QNetworkReply> pendingReply = m_testReply;
    QTimer::singleShot(20000, this, [this, pendingReply] {
        if (m_testReply && m_testReply.data() == pendingReply.data()) {
            m_testReply->abort();
        }
    });
}

void KisAiIllustrationDocker::finishTestConnectionRequest()
{
    QPointer<QNetworkReply> reply = m_testReply;
    m_testReply = nullptr;

    if (m_testConnectionButton) {
        m_testConnectionButton->setEnabled(true);
        m_testConnectionButton->setText(i18n("🔌 接続テスト"));
    }

    if (!reply) {
        return;
    }

    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - m_testStartTimeMs;
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool success = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray response = takeTestReplyData(reply.data());
    const bool responseTooLarge = m_testResponseTooLarge;
    m_testResponseTooLarge = false;
    reply->deleteLater();

    if (responseTooLarge) {
        const QString message = i18n("❌ 接続テストの応答が安全上限を超えました。");
        if (m_testConnectionStatusLabel) {
            m_testConnectionStatusLabel->setStyleSheet(QStringLiteral("color: #f87171; font-weight: 600;"));
            m_testConnectionStatusLabel->setText(message);
            m_testConnectionStatusLabel->setVisible(true);
        }
        logDebug(QStringLiteral("TEST_OVERFLOW"), QStringLiteral("接続テストの応答が上限サイズを超えました。"));
        return;
    }

    if (success) {
        QString modelResponseText;
        const QJsonDocument doc = QJsonDocument::fromJson(response);
        if (doc.isObject()) {
            const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
            if (!choices.isEmpty()) {
                const QJsonObject msg = choices.at(0).toObject().value(QStringLiteral("message")).toObject();
                modelResponseText = msg.value(QStringLiteral("content")).toString().trimmed();
            }
        }
        if (modelResponseText.length() > 50) {
            modelResponseText.truncate(47);
            modelResponseText += QStringLiteral("…");
        }

        const QString successMsg = modelResponseText.isEmpty()
            ? i18n("✅ 接続成功 (%1ms): HTTP %2 応答を受信しました。", elapsedMs, httpStatus)
            : i18n("✅ 接続成功 (%1ms): \"%2\"", elapsedMs, modelResponseText);

        if (m_testConnectionStatusLabel) {
            m_testConnectionStatusLabel->setStyleSheet(QStringLiteral("color: #4ade80; font-weight: 600;"));
            m_testConnectionStatusLabel->setText(successMsg);
            m_testConnectionStatusLabel->setVisible(true);
        }
        logDebug(QStringLiteral("TEST_SUCCESS"), QStringLiteral("HTTP %1 (%2ms): %3").arg(httpStatus).arg(elapsedMs).arg(QString::fromUtf8(response.left(500))));
    } else {
        QString errorDetail;
        const QJsonDocument doc = QJsonDocument::fromJson(response);
        if (doc.isObject()) {
            const QJsonObject err = doc.object().value(QStringLiteral("error")).toObject();
            errorDetail = err.value(QStringLiteral("message")).toString().trimmed();
        }
        if (errorDetail.isEmpty()) {
            errorDetail = reply->errorString();
        }

        const QString failMsg = i18n("❌ 接続失敗 (HTTP %1, %2ms): %3", httpStatus, elapsedMs, errorDetail);
        if (m_testConnectionStatusLabel) {
            m_testConnectionStatusLabel->setStyleSheet(QStringLiteral("color: #f87171; font-weight: 600;"));
            m_testConnectionStatusLabel->setText(failMsg);
            m_testConnectionStatusLabel->setVisible(true);
        }
        logDebug(QStringLiteral("TEST_FAILED"), QStringLiteral("HTTP %1 (%2ms): %3\nRaw: %4")
            .arg(httpStatus).arg(elapsedMs).arg(errorDetail, QString::fromUtf8(response.left(500))));
    }
}

void KisAiIllustrationDocker::logDebug(const QString &category, const QString &message)
{
    if (!m_debugModeCheck || !m_debugModeCheck->isChecked() || !m_debugLogText) {
        return;
    }
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString formatted = QStringLiteral("[%1] [%2] %3").arg(timestamp, category, message);
    m_debugLogText->appendPlainText(formatted);
    m_debugLogText->ensureCursorVisible();
}

void KisAiIllustrationDocker::clearDebugLog()
{
    if (m_debugLogText) {
        m_debugLogText->clear();
    }
}

void KisAiIllustrationDocker::copyDebugLog()
{
    if (m_debugLogText) {
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (clipboard) {
            clipboard->setText(m_debugLogText->toPlainText());
            setStatus(i18n("デバッグログをクリップボードにコピーしました。"));
        }
    }
}
