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
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:1 #06b6d4); border-radius: 2px; }"));

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

    // Quick chip buttons
    auto *chipRow = new QHBoxLayout();
    chipRow->setSpacing(4);
    const QList<QPair<QString, QString>> chips = {
        {i18n("美少女"), QStringLiteral("アニメ美少女のクローズアップポートレート、大きな輝く青い瞳、二重まぶた、繊細なまつ毛、さらさらの銀髪、柔らかい頬の赤み、天使の輪")},
        {i18n("桜風景"), QStringLiteral("壮大な富士山と満開の桜の木、夕暮れのグラデーション空、舞い散る花びら、伝統的な日本風景")},
        {i18n("サイバー"), QStringLiteral("ネオン輝くサイバーパンク高層ビル群、夜の摩天楼、雨に反射する光、ホログラム広告、近未来都市")},
        {i18n("浮世絵"), QStringLiteral("葛飾北斎風のダイナミックな大波、力強い水しぶき、伝統的な青と白のコントラスト、富士山")},
        {i18n("黒猫"), QStringLiteral("月夜に佇む美しい黒猫、金色に輝く瞳、繊細なヒゲ、神秘的な夜空と星の光")},
    };
    for (const auto &chip : chips) {
        auto *chipBtn = new QPushButton(chip.first, promptCard.frame);
        chipBtn->setProperty("class", "aiChipButton");
        chipBtn->setCursor(Qt::PointingHandCursor);
        connect(chipBtn, &QPushButton::clicked, this, [this, prompt = chip.second] {
            if (m_promptEditor) {
                m_promptEditor->setPlainText(prompt);
            }
            focusPrompt();
        });
        chipRow->addWidget(chipBtn);
    }
    promptCard.layout->addLayout(chipRow);

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
    m_detailsToggleBtn->setText(i18n("▶ 詳細設定（エンドポイント / API キー）"));
    m_detailsToggleBtn->setCursor(Qt::PointingHandCursor);
    engineCard.layout->addWidget(m_detailsToggleBtn);

    m_detailsContainer = new QWidget(engineCard.frame);
    auto *detailsLayout = new QVBoxLayout(m_detailsContainer);
    detailsLayout->setContentsMargins(0, 4, 0, 0);
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
    engineCard.layout->addWidget(m_detailsContainer);

    layout->addWidget(engineCard.frame);

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

    // Card 3.5: Goal Mode Settings
    CardWidget goalCard = createCard();
    auto *goalHeader = new QLabel(i18n("🎯 自律多段階作画 (Goal Mode)"), goalCard.frame);
    goalHeader->setProperty("class", "aiCardTitle");
    goalCard.layout->addWidget(goalHeader);

    m_goalModeCheck = new QCheckBox(i18n("Goalモード（自律多段階作画）を有効にする"), goalCard.frame);
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

    m_critiqueLabel = new QLabel(i18n("AIの視覚批評・自己分析がここに表示されます。"), m_goalInspectorCard);
    m_critiqueLabel->setWordWrap(true);
    m_critiqueLabel->setStyleSheet(QStringLiteral(
        "background: #0d1117; color: #94a3b8; border: 1px solid #273142; border-left: 3px solid #38bdf8; border-radius: 4px; padding: 6px 8px; font-size: 11px;"));
    inspectorLayout->addWidget(m_critiqueLabel);

    auto *stepBtnRow = new QHBoxLayout();
    stepBtnRow->setSpacing(6);
    m_nextStepButton = new QPushButton(i18n("▶ 次のステップへ進む"), m_goalInspectorCard);
    m_nextStepButton->setObjectName(QStringLiteral("aiGenerateButton"));
    m_nextStepButton->setCursor(Qt::PointingHandCursor);
    m_nextStepButton->setVisible(false);

    m_finishGoalButton = new QPushButton(i18n("🏁 ここで完成"), m_goalInspectorCard);
    m_finishGoalButton->setObjectName(QStringLiteral("aiSecondaryButton"));
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
    connect(m_finishGoalButton, &QPushButton::clicked, this, &KisAiIllustrationDocker::finishGoalMode);

    // Card 4: Action & Preview
    CardWidget actionCard = createCard();

    auto *buttonRow = new QHBoxLayout();
    m_generateButton = new QPushButton(i18n("🎨 生成してレイヤーに追加"), actionCard.frame);
    m_generateButton->setObjectName(QStringLiteral("aiGenerateButton"));
    m_generateButton->setAccessibleDescription(i18n("プロンプトからイラストを生成し、現在のキャンバスに新しいレイヤーを追加します。"));
    m_cancelButton = new QPushButton(i18n("中止"), actionCard.frame);
    m_cancelButton->setObjectName(QStringLiteral("aiSecondaryButton"));
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
    m_goalApiKey.clear();
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
            generateIllustration();
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
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

    const QSize canvasSize(m_widthSpin->value(), m_heightSpin->value());
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
    m_responseBuffer.clear();
    m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);
    m_apiKeyEditor->clear();
    setBusy(true);
    setStatus(i18n("%1 に LLM 座標ストローク生成を依頼しています…", KisAiIllustrationRenderer::displayEndpoint(endpoint)));

    connect(m_reply.data(), &QNetworkReply::readyRead, this, [this] { appendReplyData(m_reply.data()); });
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
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    setBusy(false);

    if (!reply) {
        m_responseBuffer.clear();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray response = takeReplyData(reply.data());
    const bool responseTooLarge = m_responseTooLarge;
    m_responseTooLarge = false;
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
}

void KisAiIllustrationDocker::generateLocalConcept(const QString &prompt)
{
    setBusy(true);
    setStatus(i18n("ローカルのコンセプトスケッチを構成しています…"));

    const QSize canvasSize(m_widthSpin->value(), m_heightSpin->value());
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
    m_responseBuffer.clear();
    m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);
    m_apiKeyEditor->clear();
    setBusy(true);
    setStatus(i18n("%1 に画像生成を依頼しています…", KisAiIllustrationRenderer::displayEndpoint(endpoint)));

    connect(m_reply.data(), &QNetworkReply::readyRead, this, [this] { appendReplyData(m_reply.data()); });
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
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    setBusy(false);

    if (!reply) {
        m_responseBuffer.clear();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray response = takeReplyData(reply.data());
    const bool responseTooLarge = m_responseTooLarge;
    m_responseTooLarge = false;
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

    const QSize previewTargetSize = m_previewLabel->size().isEmpty() ? QSize(256, 256) : m_previewLabel->size();
    m_previewLabel->setPixmap(QPixmap::fromImage(image).scaled(previewTargetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (addImageAsLayer(image, promptForLayerName(m_promptEditor->toPlainText()))) {
        setStatus(i18n("画像モデルの結果を新しいレイヤーに追加しました。"));
    }
}

void KisAiIllustrationDocker::cancelRemoteRequest()
{
    if (!m_reply) {
        if (m_goalModeActive) {
            finishGoalMode();
            setStatus(i18n("Goalモード作画を中止しました。"));
        }
        return;
    }

    m_requestWasCancelled = true;
    m_reply->abort();
    if (m_goalModeActive) {
        finishGoalMode();
    }
    setStatus(i18n("リクエストを中止しています…"));
}

bool KisAiIllustrationDocker::appendReplyData(QNetworkReply *reply)
{
    if (!reply || m_responseTooLarge) {
        return false;
    }

    const QByteArray chunk = reply->readAll();
    if (chunk.size() > MAX_REMOTE_RESPONSE_BYTES - m_responseBuffer.size()) {
        m_responseBuffer.clear();
        m_responseTooLarge = true;
        reply->abort();
        return false;
    }

    m_responseBuffer.append(chunk);
    return true;
}

QByteArray KisAiIllustrationDocker::takeReplyData(QNetworkReply *reply)
{
    appendReplyData(reply);
    QByteArray response = m_responseBuffer;
    m_responseBuffer.clear();
    return response;
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
    m_newCanvasButton->setEnabled(!busy);
    m_generateButton->setEnabled(!busy);
    if (m_goalModeActive) {
        m_generateButton->setText(busy ? i18n("⏳ Goal作画中…") : i18n("🎯 Goal作画進行中"));
    } else if (m_goalModeCheck && m_goalModeCheck->isChecked()) {
        m_generateButton->setText(busy ? i18n("⏳ 作画中…") : i18n("🎯 Goal作画を開始"));
    } else {
        m_generateButton->setText(busy ? i18n("⏳ 生成中…") : i18n("🎨 生成してレイヤーに追加"));
    }
    m_modeCombo->setEnabled(!busy);
    if (m_goalModeCheck) {
        m_goalModeCheck->setEnabled(!busy);
    }
    if (m_goalStepsSpin) {
        m_goalStepsSpin->setEnabled(!busy);
    }
    if (m_artStyleCombo) {
        m_artStyleCombo->setEnabled(!busy);
    }
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

    const auto mode = static_cast<GenerationMode>(m_modeCombo->currentData().toInt());
    if (mode == GenerationMode::LlmStrokes) {
        const QString endpoint = m_endpointEditor->text().trimmed();
        const QString model = m_modelEditor->text().trimmed();
        const QString apiKey = m_apiKeyEditor->text();

        QString errorMessage;
        if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint, &errorMessage)) {
            setStatus(errorMessage, true);
            m_goalModeActive = false;
            return;
        }
        if (model.isEmpty()) {
            setStatus(i18n("LLM モデル名を入力してください。"), true);
            m_goalModeActive = false;
            return;
        }
        if (apiKey.isEmpty()) {
            setStatus(i18n("このリクエストに使う API キーを入力してください。"), true);
            m_goalModeActive = false;
            return;
        }

        m_goalApiKey = apiKey;
        m_apiKeyEditor->clear();

        QSettings settings;
        settings.setValue(QStringLiteral("AIIllustration/llmEndpoint"), endpoint);
        settings.setValue(QStringLiteral("AIIllustration/llmModel"), model);
        settings.setValue(QStringLiteral("AIIllustration/endpoint"), endpoint);
        settings.setValue(QStringLiteral("AIIllustration/model"), model);
    } else {
        m_goalApiKey.clear();
    }

    if (m_goalInspectorCard) {
        m_goalInspectorCard->setVisible(true);
        if (m_goalPhaseLabel) {
            m_goalPhaseLabel->setText(i18n("🎯 ステップ 1/%1 開始準備中…", m_goalTotalSteps));
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
    const QSize canvasSize(m_widthSpin->value(), m_heightSpin->value());
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
                setStatus(i18n("%1 (%2 / 構造品質 %3%)", statusMsg, summary, qualityPercent));
            } else {
                setStatus(statusMsg, true);
            }
        } else {
            setStatus(i18n("キャンバスが利用できないため、ストロークを描画できませんでした。"), true);
        }

        if (m_goalPhaseLabel) {
            m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2 (%3) 完了", m_goalCurrentStep, m_goalTotalSteps, program.stepPhase));
        }
        if (m_critiqueLabel) {
            m_critiqueLabel->setText(program.visualCritique.isEmpty()
                ? i18n("ローカルプロシージャル作画ステップを完了しました。")
                : program.visualCritique);
        }

        setBusy(false);

        if (m_goalCurrentStep >= m_goalTotalSteps || program.goalReached) {
            finishGoalMode();
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

        if (apiKey.isEmpty()) {
            setStatus(i18n("API キーが見つかりません。Goalモードを終了します。"), true);
            finishGoalMode();
            return;
        }

        QString imageBase64;
        if (m_goalCurrentStep > 1) {
            KisView *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
            if (view && view->image()) {
#ifndef AI_STROKE_STANDALONE
                imageBase64 = KisAiStrokeRenderer::captureCanvasBase64(view->image(), 768);
#endif
            }
        }

        KisAiPromptAnalyzer::SemanticSpec spec = KisAiPromptAnalyzer::analyze(m_goalPrompt, canvasSize);
        if (artStyle != KisAiPromptAnalyzer::ArtStyle::General) {
            spec.style = artStyle;
        }
        const QString guidance = KisAiPromptAnalyzer::generateGoalPhaseGuidance(
            m_goalCurrentStep, spec, canvasSize);

        const QJsonObject payload = KisAiStrokeProgramCodec::buildGoalStepPayload(
            model,
            m_goalPrompt,
            canvasSize,
            m_goalCurrentStep,
            m_goalTotalSteps,
            imageBase64,
            guidance,
            m_strokeBudgetSpin ? m_strokeBudgetSpin->value() : 400
        );

        QNetworkRequest request{QUrl(endpoint)};
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

        m_requestWasCancelled = false;
        m_requestTimedOut = false;
        m_responseTooLarge = false;
        m_responseBuffer.clear();
        m_reply = m_networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        m_reply->setReadBufferSize(MAX_REMOTE_RESPONSE_BYTES);
        setBusy(true);

        const QString visionTag = !imageBase64.isEmpty() ? i18n(" (Vision画像付)") : QString();
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

        const QPointer<QNetworkReply> pendingReply = m_reply;
        QTimer::singleShot(REMOTE_REQUEST_TIMEOUT_MS, this, [this, pendingReply] {
            if (m_reply && m_reply.data() == pendingReply.data()) {
                m_requestTimedOut = true;
                m_reply->abort();
            }
        });
    }
}

void KisAiIllustrationDocker::finishGoalStepRequest()
{
    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    const bool requestWasCancelled = m_requestWasCancelled;
    const bool requestTimedOut = m_requestTimedOut;
    m_requestWasCancelled = false;
    m_requestTimedOut = false;
    setBusy(false);

    if (!reply) {
        m_responseBuffer.clear();
        finishGoalMode();
        return;
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool requestSucceeded = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    const QByteArray response = takeReplyData(reply.data());
    const bool responseTooLarge = m_responseTooLarge;
    m_responseTooLarge = false;
    reply->deleteLater();

    if (requestWasCancelled) {
        setStatus(i18n("Goal モードを中止しました。"));
        finishGoalMode();
        return;
    }
    if (requestTimedOut) {
        setStatus(i18n("LLM の応答が 2 分以内に届かなかったため中止しました。"), true);
        finishGoalMode();
        return;
    }
    if (responseTooLarge) {
        setStatus(i18n("LLM の応答が上限を超えています。"), true);
        finishGoalMode();
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
        finishGoalMode();
        return;
    }

    KisAiStrokeProgram program;
    QString parseError;
    if (!KisAiStrokeProgramCodec::parseResponse(response, &program, &parseError)) {
        setStatus(parseError, true);
        finishGoalMode();
        return;
    }

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

    if (m_goalPhaseLabel) {
        m_goalPhaseLabel->setText(i18n("🎯 ステップ %1/%2 (%3) 完了", m_goalCurrentStep, m_goalTotalSteps, program.stepPhase));
    }
    if (m_critiqueLabel) {
        if (!program.visualCritique.isEmpty()) {
            m_critiqueLabel->setText(i18n("👀 AI視覚批評: %1", program.visualCritique));
        } else {
            m_critiqueLabel->setText(i18n("ステップ %1 の作画が完了しました。", m_goalCurrentStep));
        }
    }

    if (m_goalCurrentStep >= m_goalTotalSteps || program.goalReached) {
        finishGoalMode();
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
    m_goalCurrentStep++;
    if (m_goalCurrentStep > m_goalTotalSteps) {
        finishGoalMode();
        return;
    }
    executeGoalStep();
}

void KisAiIllustrationDocker::finishGoalMode()
{
    m_goalModeActive = false;
    m_waitingForUserStepAdvance = false;
    m_goalApiKey.clear();

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
            m_goalPhaseLabel->setText(i18n("🎯 Goal作画 完了 (全 %1 段階)", m_goalCurrentStep));
        }
    }
    setStatus(i18n("🎯 Goal作画が完了しました。Kritaのレイヤードックで各層を確認・調整できます。"));
    setBusy(false);
}
