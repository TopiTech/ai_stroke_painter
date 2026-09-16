/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiIllustrationRenderer.h"
#include "KisAiStrokeProgram.h"

#include <QColor>
#include <QFont>
#include <QHostAddress>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace
{
constexpr int MAX_LOCAL_RENDER_EDGE = 1536;

QSize boundedSize(const QSize &requestedSize)
{
    QSize result = requestedSize.isValid() ? requestedSize : QSize(1024, 1024);
    result = result.boundedTo(QSize(MAX_LOCAL_RENDER_EDGE, MAX_LOCAL_RENDER_EDGE));

    if (result.width() < 64 || result.height() < 64) {
        result = QSize(1024, 1024);
    }

    return result;
}

bool isLoopbackHost(const QString &host)
{
    QString normalized = host.trimmed().toLower();
    normalized = QUrl::fromPercentEncoding(normalized.toUtf8());
    if (normalized.startsWith(QLatin1Char('[')) && normalized.endsWith(QLatin1Char(']'))) {
        normalized = normalized.mid(1, normalized.length() - 2);
    }
    const int percentIndex = normalized.indexOf(QLatin1Char('%'));
    if (percentIndex >= 0) {
        normalized = normalized.left(percentIndex);
    }
    while (normalized.endsWith(QLatin1Char('.'))) {
        normalized.chop(1);
    }
    if (normalized == QLatin1String("localhost") || normalized.endsWith(QLatin1String(".localhost"))) {
        return true;
    }

    QHostAddress address;
    if (!address.setAddress(normalized)) {
        return false;
    }
    if (address.isLoopback()) {
        return true;
    }

    bool ok = false;
    const quint32 ipv4 = address.toIPv4Address(&ok);
    if (ok) {
        return QHostAddress(ipv4).isLoopback();
    }

    return false;
}

bool hasSensitiveUrlComponent(const QUrl &url)
{
    if (!url.fragment(QUrl::FullyDecoded).trimmed().isEmpty()) {
        return true;
    }

    static const QStringList sensitiveKeys = {
        QStringLiteral("apikey"),
        QStringLiteral("key"),
        QStringLiteral("token"),
        QStringLiteral("accesstoken"),
        QStringLiteral("auth"),
        QStringLiteral("authorization"),
        QStringLiteral("password"),
        QStringLiteral("passwd"),
        QStringLiteral("secret"),
        QStringLiteral("clientsecret"),
        QStringLiteral("signature"),
        QStringLiteral("sig"),
        QStringLiteral("subscriptionkey"),
        QStringLiteral("credential"),
        QStringLiteral("credentials"),
    };

    const QUrlQuery query(url);
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        QString normalizedKey;
        for (const QChar character : item.first.trimmed().toLower()) {
            if (character.isLetterOrNumber()) {
                normalizedKey.append(character);
            }
        }
        if (sensitiveKeys.contains(normalizedKey)) {
            return true;
        }

        const QString value = item.second.trimmed();
        if (value.startsWith(QLatin1String("sk-"), Qt::CaseInsensitive) ||
            value.startsWith(QLatin1String("Bearer "), Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

QColor hueColor(int hue, int saturation, int lightness, int alpha = 255)
{
    // QColor::setHsl() is only specified for hue in [0, 359]. Callers pass
    // offsets like (hue - 48) that can go negative, and C++ % keeps the sign
    // of the dividend, so wrap explicitly (same convention as
    // KisAiStrokeProgramCodec::calculateHueShiftedShadow()).
    const int wrappedHue = ((hue % 360) + 360) % 360;
    QColor color;
    color.setHsl(wrappedHue, saturation, lightness, alpha);
    return color;
}

void drawBrushRibbon(QPainter &painter, const QRectF &bounds, QRandomGenerator &random, int hue)
{
    const QPointF start(bounds.left() - bounds.width() * 0.08,
                        bounds.center().y() + bounds.height() * (random.generateDouble() - 0.5) * 0.3);
    const QPointF end(bounds.right() + bounds.width() * 0.08,
                      bounds.center().y() + bounds.height() * (random.generateDouble() - 0.5) * 0.3);

    QPainterPath ribbon;
    ribbon.moveTo(start);
    ribbon.cubicTo(QPointF(bounds.left() + bounds.width() * 0.30,
                           bounds.top() + bounds.height() * random.generateDouble()),
                   QPointF(bounds.left() + bounds.width() * 0.68,
                           bounds.bottom() - bounds.height() * random.generateDouble()),
                   end);

    QPen pen(hueColor(hue, 82, 72, 180));
    pen.setWidthF(qMax<qreal>(3.0, bounds.width() * 0.013));
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(ribbon);

    pen.setColor(hueColor(hue + 24, 88, 82, 210));
    pen.setWidthF(qMax<qreal>(1.0, pen.widthF() * 0.28));
    painter.setPen(pen);
    painter.drawPath(ribbon);
}

void drawPromptMotif(QPainter &painter, const QRectF &bounds, const QString &prompt, int hue)
{
    const QString lowerPrompt = prompt.toLower();
    const QPointF center = bounds.center();
    const qreal radius = qMin(bounds.width(), bounds.height()) * 0.16;

    painter.setPen(Qt::NoPen);
    painter.setBrush(hueColor(hue + 30, 70, 72, 230));

    if (lowerPrompt.contains(QLatin1String("cat")) || prompt.contains(QStringLiteral("猫"))) {
        QPainterPath cat;
        cat.addEllipse(center, radius, radius * 0.86);
        QPolygonF ears;
        ears << QPointF(center.x() - radius * 0.72, center.y() - radius * 0.38)
             << QPointF(center.x() - radius * 0.62, center.y() - radius * 1.35)
             << QPointF(center.x() - radius * 0.10, center.y() - radius * 0.70)
             << QPointF(center.x() + radius * 0.10, center.y() - radius * 0.70)
             << QPointF(center.x() + radius * 0.62, center.y() - radius * 1.35)
             << QPointF(center.x() + radius * 0.72, center.y() - radius * 0.38);
        cat.addPolygon(ears);
        painter.drawPath(cat);
        painter.setBrush(QColor(20, 27, 43, 220));
        painter.drawEllipse(QPointF(center.x() - (radius * 0.34), center.y()), radius * 0.10, radius * 0.16);
        painter.drawEllipse(QPointF(center.x() + (radius * 0.34), center.y()), radius * 0.10, radius * 0.16);
        return;
    }

    if (lowerPrompt.contains(QLatin1String("flower")) || prompt.contains(QStringLiteral("花"))) {
        for (int i = 0; i < 8; ++i) {
            painter.save();
            painter.translate(center);
            painter.rotate(i * 45.0);
            painter.drawEllipse(QRectF(radius * 0.35, -radius * 0.29, radius * 1.00, radius * 0.58));
            painter.restore();
        }
        painter.setBrush(hueColor(hue - 48, 82, 60, 235));
        painter.drawEllipse(center, radius * 0.36, radius * 0.36);
        return;
    }

    if (lowerPrompt.contains(QLatin1String("city")) || prompt.contains(QStringLiteral("街")) || prompt.contains(QStringLiteral("都市"))) {
        const qreal baseline = center.y() + (radius * 0.75);
        const qreal left = center.x() - (radius * 1.4);
        painter.setBrush(hueColor(hue + 15, 50, 30, 235));
        for (int i = 0; i < 7; ++i) {
            const qreal width = radius * (0.24 + 0.08 * (i % 3));
            const qreal height = radius * (0.45 + 0.20 * ((i + 1) % 4));
            painter.drawRoundedRect(QRectF(left + (i * radius * 0.40), baseline - height, width, height), 2.0, 2.0);
        }
        return;
    }

    QPainterPath figure;
    figure.addEllipse(QPointF(center.x(), center.y() - (radius * 0.50)), radius * 0.44, radius * 0.44);
    figure.moveTo(center.x() - (radius * 0.86), center.y() + (radius * 1.25));
    figure.quadTo(center.x(), center.y() - (radius * 0.05), center.x() + (radius * 0.86), center.y() + (radius * 1.25));
    figure.closeSubpath();
    painter.drawPath(figure);
}
} // namespace

QString KisAiIllustrationRenderer::normalizedPrompt(const QString &prompt)
{
    QString result = prompt.simplified();
    constexpr int maxPromptLength = 12'000;
    if (result.size() > maxPromptLength) {
        result.truncate(maxPromptLength);
    }
    return result;
}

bool KisAiIllustrationRenderer::validateImageEndpoint(const QString &endpoint, QString *errorMessage)
{
    const QUrl url = QUrl::fromUserInput(endpoint.trimmed());
    const QString scheme = url.scheme().toLower();

    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (!url.isValid() || (scheme != QLatin1String("https") && scheme != QLatin1String("http"))) {
        return fail(QStringLiteral("エンドポイントの URL は http:// または https:// で指定してください。"));
    }
    if (url.host().isEmpty()) {
        return fail(QStringLiteral("エンドポイントの URL にホスト名がありません。"));
    }
    if (!url.userName().isEmpty() || !url.password().isEmpty()) {
        return fail(QStringLiteral("エンドポイントの URL に認証情報を含めることはできません。"));
    }
    if (hasSensitiveUrlComponent(url)) {
        return fail(QStringLiteral(
            "エンドポイントの URL に API キーなどの認証情報を含めることはできません。API キー欄を使用してください。"));
    }
    if (scheme == QLatin1String("http") && !isLoopbackHost(url.host())) {
        return fail(QStringLiteral("外部のエンドポイントには HTTPS を使用してください。"));
    }

    return true;
}

QString KisAiIllustrationRenderer::displayEndpoint(const QString &endpoint)
{
    const QUrl url = QUrl::fromUserInput(endpoint.trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        return QStringLiteral("API エンドポイント");
    }

    return url.scheme().toLower() + QStringLiteral("://") + url.host();
}

QImage KisAiIllustrationRenderer::createConceptImage(const QString &prompt, const QSize &requestedSize)
{
    const QSize size = boundedSize(requestedSize);
    const QString normalized = normalizedPrompt(prompt);
    QRandomGenerator random(KisAiStrokeProgramCodec::stableSeed(normalized));
    const int hue = random.bounded(360);

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(16, 22, 35));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF canvasRect(QPointF(0, 0), QSizeF(size));
    QLinearGradient background(canvasRect.topLeft(), canvasRect.bottomRight());
    background.setColorAt(0.0, hueColor(hue - 28, 48, 16));
    background.setColorAt(0.48, hueColor(hue + 8, 56, 25));
    background.setColorAt(1.0, hueColor(hue + 54, 44, 13));
    painter.fillRect(canvasRect, background);

    for (int i = 0; i < 7; ++i) {
        const qreal diameter = size.width() * (0.15 + random.generateDouble() * 0.28);
        const QPointF center(random.generateDouble() * size.width(), random.generateDouble() * size.height());
        QRadialGradient wash(center, diameter * 0.50);
        wash.setColorAt(0.0, hueColor(hue + random.bounded(-45, 70), 85, 70, 42));
        wash.setColorAt(1.0, Qt::transparent);
        painter.setPen(Qt::NoPen);
        painter.setBrush(wash);
        painter.drawEllipse(center, diameter * 0.50, diameter * 0.50);
    }

    const QRectF illustrationBounds = canvasRect.adjusted(size.width() * 0.12,
                                                            size.height() * 0.12,
                                                            -size.width() * 0.12,
                                                            -size.height() * 0.12);
    for (int i = 0; i < 3; ++i) {
        drawBrushRibbon(painter, illustrationBounds, random, hue + (i * 38));
    }
    drawPromptMotif(painter, illustrationBounds, normalized, hue);

    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 42; ++i) {
        const qreal diameter = qMax<qreal>(1.5, size.width() * (0.0018 + random.generateDouble() * 0.0035));
        painter.setBrush(hueColor(hue + random.bounded(-80, 80), 70, 82, random.bounded(70, 185)));
        painter.drawEllipse(QPointF(random.generateDouble() * size.width(), random.generateDouble() * size.height()),
                            diameter,
                            diameter);
    }

    QLinearGradient vignette(canvasRect.topLeft(), canvasRect.bottomRight());
    vignette.setColorAt(0.0, QColor(0, 0, 0, 68));
    vignette.setColorAt(0.45, Qt::transparent);
    vignette.setColorAt(1.0, QColor(0, 0, 0, 92));
    painter.fillRect(canvasRect, vignette);

    return image;
}
