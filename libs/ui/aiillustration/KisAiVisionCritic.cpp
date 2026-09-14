/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiVisionCritic.h"

#include "KisAiLightRig.h"

#include <QBuffer>
#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace
{
constexpr int MAX_CRITIQUE_JSON_BYTES = 4 * 1024 * 1024;
constexpr int GRID = 4; // edge-density grid resolution

QStringList allowedCritiqueAreas()
{
    return {
        QStringLiteral("left_eye"), QStringLiteral("right_eye"),
        QStringLiteral("hair"), QStringLiteral("face_skin"),
        QStringLiteral("mouth"), QStringLiteral("nose"),
        QStringLiteral("shading"), QStringLiteral("highlights"),
        QStringLiteral("background"), QStringLiteral("fx"),
        QStringLiteral("clothing"), QStringLiteral("composition")
    };
}

QStringList allowedActions()
{
    return {
        QStringLiteral("repaint"), QStringLiteral("soften"),
        QStringLiteral("remove"), QStringLiteral("keep")
    };
}

qreal edgeDensityOfTile(const QImage &image, const QRect &tile)
{
    // Luminance gradient magnitude averaged over the tile. Cheap Sobel-lite:
    // horizontal + vertical finite differences on a 2px stride.
    const QRect clipped = tile.intersected(image.rect());
    if (clipped.width() < 4 || clipped.height() < 4)
        return 0.0;

    qreal sum = 0.0;
    int count = 0;
    for (int y = clipped.top(); y < clipped.bottom() - 2; y += 2) {
        const QRgb *row0 = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        const QRgb *row2 = reinterpret_cast<const QRgb *>(image.constScanLine(y + 2));
        for (int x = clipped.left(); x < clipped.right() - 2; x += 2) {
            const QColor c00(row0[x]);
            const QColor c10(row0[x + 2]);
            const QColor c01(row2[x]);
            const qreal gx = qGray(c10.red(), c10.green(), c10.blue())
                - qGray(c00.red(), c00.green(), c00.blue());
            const qreal gy = qGray(c01.red(), c01.green(), c01.blue())
                - qGray(c00.red(), c00.green(), c00.blue());
            sum += std::sqrt(gx * gx + gy * gy);
            count++;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

QImage cropAndUpscale(const QImage &canvas, const QRectF &region)
{
    if (canvas.isNull() || canvas.width() <= 0 || canvas.height() <= 0)
        return {};

    const QRect src(
        qBound(0, qRound(region.left() * canvas.width()), canvas.width() - 1),
        qBound(0, qRound(region.top() * canvas.height()), canvas.height() - 1),
        qMax(2, qRound(region.width() * canvas.width())),
        qMax(2, qRound(region.height() * canvas.height())));
    const QImage cropped = canvas.copy(src.intersected(canvas.rect()));
    if (cropped.isNull() || cropped.width() <= 0 || cropped.height() <= 0)
        return {};
    const int minSide = qMin(cropped.width(), cropped.height());
    if (minSide >= 384)
        return cropped;
    const qreal factor = qreal(384) / qMax(1, minSide);
    return cropped.scaled(qRound(cropped.width() * factor),
                          qRound(cropped.height() * factor),
                          Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QString imageToDataUrl(const QImage &image)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 82);
    return QStringLiteral("data:image/jpeg;base64,%1")
        .arg(QString::fromLatin1(buffer.data().toBase64()));
}

QJsonObject imageContentPart(const QString &dataUrl, const QString &text)
{
    QJsonArray content;
    if (!text.isEmpty())
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), text}});
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                               {QStringLiteral("image_url"),
                                QJsonObject{{QStringLiteral("url"), dataUrl}}}});
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), content);
    return message;
}
} // namespace

QRectF KisAiVisionCritic::faceBoxFromAnchor(const QPointF &headCenter, qreal headWidth, qreal headHeight)
{
    return QRectF(headCenter.x() - headWidth * 0.80,
                  headCenter.y() - headHeight * 0.65,
                  headWidth * 1.60,
                  headHeight * 1.70);
}

QVector<KisAiCriticCrop> KisAiVisionCritic::selectCrops(
    const QImage &canvas,
    const QRectF &faceBox,
    const QVector<KisAiCritiqueRegion> &previousRegions,
    int maxCrops)
{
    QVector<KisAiCriticCrop> crops;
    if (canvas.isNull() || maxCrops <= 0)
        return crops;

    // Ensure 32-bit scanline access safety across arbitrary input formats.
    const QImage safeCanvas = (canvas.format() == QImage::Format_ARGB32 || canvas.format() == QImage::Format_RGB32)
        ? canvas
        : canvas.convertToFormat(QImage::Format_ARGB32);

    // 1. Face crop (highest priority when a face exists).
    if (faceBox.isValid() && faceBox.width() > 0.01) {
        KisAiCriticCrop face;
        face.region = faceBox.intersected(QRectF(0, 0, 1, 1));
        face.reason = QStringLiteral("face");
        face.image = cropAndUpscale(safeCanvas, face.region);
        crops.append(face);
    }

    // 2. Edge-density winner among the 4x4 grid, excluding the face box.
    if (safeCanvas.width() >= 64 && safeCanvas.height() >= 64) {
        const int tw = safeCanvas.width() / GRID;
        const int th = safeCanvas.height() / GRID;
        qreal bestScore = -1.0;
        QRectF bestTile;
        for (int gy = 0; gy < GRID; ++gy) {
            for (int gx = 0; gx < GRID; ++gx) {
                const QRect tile(gx * tw, gy * th, tw, th);
                const QRectF norm(qreal(tile.left()) / safeCanvas.width(),
                                  qreal(tile.top()) / safeCanvas.height(),
                                  qreal(tile.width()) / safeCanvas.width(),
                                  qreal(tile.height()) / safeCanvas.height());
                if (faceBox.isValid() && faceBox.intersects(norm))
                    continue;
                const qreal score = edgeDensityOfTile(safeCanvas, tile);
                if (score > bestScore) {
                    bestScore = score;
                    bestTile = norm;
                }
            }
        }
        if (bestScore > 0.0 && bestTile.isValid()) {
            KisAiCriticCrop detail;
            detail.region = bestTile;
            detail.reason = QStringLiteral("edge_density");
            detail.image = cropAndUpscale(safeCanvas, bestTile);
            crops.append(detail);
        }
    }

    // 3. Previously flagged high-priority regions.
    for (const KisAiCritiqueRegion &r : previousRegions) {
        if (crops.size() >= maxCrops)
            break;
        if (r.priority < 4)
            continue;
        // Region has no rect; approximate with a centered band per area name.
        QRectF region(0.2, 0.2, 0.6, 0.6);
        if (r.area.contains(QLatin1String("eye")))
            region = QRectF(0.18, 0.30, 0.64, 0.30);
        else if (r.area == QLatin1String("hair"))
            region = QRectF(0.05, 0.02, 0.90, 0.35);
        else if (r.area == QLatin1String("mouth") || r.area == QLatin1String("nose"))
            region = QRectF(0.30, 0.55, 0.40, 0.25);
        else if (r.area == QLatin1String("background"))
            region = QRectF(0.0, 0.0, 0.30, 0.30);
        KisAiCriticCrop crop;
        crop.region = region;
        crop.reason = QStringLiteral("prior_critique:%1").arg(r.area);
        crop.image = cropAndUpscale(canvas, region);
        crops.append(crop);
    }

    return crops.mid(0, maxCrops);
}

QString KisAiVisionCritic::buildCritiqueSystemPrompt(const KisAiLightSettings &rig)
{
    const QString dir = QStringLiteral("(%1, %2)")
        .arg(QString::number(rig.direction.x(), 'f', 2),
             QString::number(rig.direction.y(), 'f', 2));
    return QStringLiteral(
        "You are a meticulous painting inspector. Examine the attached images "
        "(image 1 is the full canvas; later images are zoomed crops with their reason labelled). "
        "Answer STRICTLY as JSON: {\"readiness_score\": <0..1>, \"regions\": [...]}\n"
        "Each region: {\"area\": one of [left_eye, right_eye, hair, face_skin, mouth, nose, shading, "
        "highlights, background, fx, clothing, composition], \"issue\": <one short sentence>, "
        "\"action\": one of [repaint, soften, remove, keep], \"priority\": <1..5>, "
        "\"suggestion_patches\": [optional minimal patch objects]}\n"
        "Checklist (verify each item; do not invent defects that are not visible):\n"
        "1. Eye pair symmetry (size, height, spacing).\n"
        "2. Nothing stray drawn on the face (particles, text, smudges).\n"
        "3. Light direction consistency — the canonical key light points toward %1; "
        "shadows must fall opposite, highlights on the lit side.\n"
        "4. Line quality: jitter, spikes, broken contours, tapered tips.\n"
        "5. Hair: no bubble/afro artifacts; highlight bands follow the dome.\n"
        "6. Missing or extra parts (brows, mouth, nose point).\n"
        "7. Background: no elements colliding with the character silhouette.\n"
        "Only include regions for defects you can actually see. An empty regions list is valid.")
        .arg(dir);
}

QJsonObject KisAiVisionCritic::buildCritiquePayload(
    const QString &model,
    const QString &prompt,
    const QImage &canvas,
    const QVector<KisAiCriticCrop> &crops,
    const KisAiLightSettings &rig,
    const QJsonArray &previousRegions)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("model"), model.trimmed());
    payload.insert(QStringLiteral("temperature"), 0.2);
    payload.insert(QStringLiteral("max_tokens"), 2048);

    if (KisAiStrokeProgramCodec::supportsJsonSchema(model)) {
        QJsonObject schemaObj;
        schemaObj.insert(QStringLiteral("name"), QStringLiteral("critique"));
        schemaObj.insert(QStringLiteral("strict"), true);
        QJsonObject schema;
        schema.insert(QStringLiteral("type"), QStringLiteral("object"));
        QJsonObject props;
        QJsonObject score;
        score.insert(QStringLiteral("type"), QStringLiteral("number"));
        score.insert(QStringLiteral("minimum"), 0.0);
        score.insert(QStringLiteral("maximum"), 1.0);
        props.insert(QStringLiteral("readiness_score"), score);
        QJsonObject regions;
        regions.insert(QStringLiteral("type"), QStringLiteral("array"));
        QJsonObject regionItem;
        regionItem.insert(QStringLiteral("type"), QStringLiteral("object"));
        QJsonObject regionProps;
        regionProps.insert(QStringLiteral("area"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
        regionProps.insert(QStringLiteral("issue"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
        regionProps.insert(QStringLiteral("action"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
        QJsonObject prio;
        prio.insert(QStringLiteral("type"), QStringLiteral("number"));
        regionProps.insert(QStringLiteral("priority"), prio);
        regionItem.insert(QStringLiteral("properties"), regionProps);
        regionItem.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("area"), QStringLiteral("issue"), QStringLiteral("action")});
        QJsonArray items;
        items.append(regionItem);
        regions.insert(QStringLiteral("items"), regionItem);
        props.insert(QStringLiteral("regions"), regions);
        schema.insert(QStringLiteral("properties"), props);
        schema.insert(QStringLiteral("additionalProperties"), false);
        schemaObj.insert(QStringLiteral("schema"), schema);
        QJsonObject responseFormat;
        responseFormat.insert(QStringLiteral("type"), QStringLiteral("json_schema"));
        responseFormat.insert(QStringLiteral("json_schema"), schemaObj);
        payload.insert(QStringLiteral("response_format"), responseFormat);
    } else {
        QJsonObject responseFormat;
        responseFormat.insert(QStringLiteral("type"), QStringLiteral("json_object"));
        payload.insert(QStringLiteral("response_format"), responseFormat);
    }

    QJsonArray messages;
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("content"), buildCritiqueSystemPrompt(rig)}});

    QJsonArray userContent;
    userContent.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"),
         QStringLiteral("Art direction request: \"%1\". Inspect the current state.").arg(prompt.trimmed())}});
    userContent.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("image_url")},
        {QStringLiteral("image_url"),
         QJsonObject{{QStringLiteral("url"), imageToDataUrl(canvas)}}}});
    for (int i = 0; i < crops.size(); ++i) {
        userContent.append(imageContentPart(
            imageToDataUrl(crops.at(i).image),
            QStringLiteral("Crop %1 (%2)").arg(i + 2).arg(crops.at(i).reason)));
    }

    if (!previousRegions.isEmpty()) {
        userContent.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"),
             QStringLiteral("Previous critique regions (verify whether each still applies): %1")
                 .arg(QString::fromUtf8(QJsonDocument(previousRegions).toJson(QJsonDocument::Compact)))}});
    }

    QJsonObject userMessage;
    userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMessage.insert(QStringLiteral("content"), userContent);
    messages.append(userMessage);
    payload.insert(QStringLiteral("messages"), messages);
    return payload;
}

bool KisAiVisionCritic::parseCritiqueResponse(
    const QByteArray &responseBytes,
    QVector<KisAiCritiqueRegion> *outRegions,
    qreal *readinessScore,
    QString *errorMessage)
{
    if (!outRegions) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Null output region list.");
        return false;
    }
    outRegions->clear();
    if (readinessScore)
        *readinessScore = 1.0;

    if (responseBytes.size() > MAX_CRITIQUE_JSON_BYTES) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Critique response exceeded the size limit.");
        return false;
    }

    const QString jsonText = KisAiStrokeProgramCodec::sanitizeAndExtractJson(
        QString::fromUtf8(responseBytes));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Critique response is not valid JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    if (readinessScore) {
        const qreal s = root.value(QStringLiteral("readiness_score")).toDouble(1.0);
        *readinessScore = qBound<qreal>(0.0, s, 1.0);
    }

    const QStringList areas = allowedCritiqueAreas();
    const QStringList actions = allowedActions();
    const QJsonArray regions = root.value(QStringLiteral("regions")).toArray();
    for (const QJsonValue &v : regions) {
        const QJsonObject obj = v.toObject();
        KisAiCritiqueRegion region;
        QString area = obj.value(QStringLiteral("area")).toString().trimmed().toLower();
        // Alias repair: keep the machine-readable vocabulary stable.
        if (area == QLatin1String("eye") || area == QLatin1String("eyes"))
            area = QStringLiteral("left_eye");
        if (area == QLatin1String("skin"))
            area = QStringLiteral("face_skin");
        if (area == QLatin1String("bg"))
            area = QStringLiteral("background");
        if (!areas.contains(area))
            continue;
        region.area = area;
        region.issue = obj.value(QStringLiteral("issue")).toString().trimmed();
        if (region.issue.isEmpty())
            continue;
        QString action = obj.value(QStringLiteral("action")).toString(QStringLiteral("repaint")).toLower();
        if (!actions.contains(action))
            action = QStringLiteral("repaint");
        region.action = action;
        region.priority = qBound(1, obj.value(QStringLiteral("priority")).toInt(2), 5);
        outRegions->append(region);
    }

    if (errorMessage)
        errorMessage->clear();
    return true;
}

QVector<KisAiProgramPatch> KisAiVisionCritic::extractSuggestedPatches(
    const QByteArray &responseBytes,
    QStringList *rejected)
{
    QVector<KisAiProgramPatch> patches;
    const QString jsonText = KisAiStrokeProgramCodec::sanitizeAndExtractJson(
        QString::fromUtf8(responseBytes));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return patches;

    const QJsonArray regions = doc.object().value(QStringLiteral("regions")).toArray();
    for (const QJsonValue &v : regions) {
        const QJsonArray suggested = v.toObject().value(QStringLiteral("suggestion_patches")).toArray();
        for (const QJsonValue &p : suggested) {
            QJsonObject obj = p.toObject();
            // Regions may omit the op wrapper; default to replace.
            if (!obj.contains(QStringLiteral("op")))
                obj.insert(QStringLiteral("op"), QStringLiteral("replace"));
            const QJsonArray wrapper = QJsonArray{obj};
            QJsonDocument wrapDoc;
            QJsonObject wrapObj;
            wrapObj.insert(QStringLiteral("patches"), wrapper);
            wrapDoc.setObject(wrapObj);

            QVector<KisAiProgramPatch> parsed;
            QStringList localRejected;
            if (KisAiProgramPatchCodec::parsePatches(wrapDoc.toJson(QJsonDocument::Compact),
                                                     &parsed, &localRejected)) {
                for (const KisAiProgramPatch &patch : parsed)
                    patches.append(patch);
            }
            if (rejected)
                rejected->append(localRejected);
        }
    }
    return patches;
}

bool KisAiVisionCritic::hasConverged(qreal psnrBefore, qreal psnrAfter, qreal minImprovementDb)
{
    return (psnrAfter - psnrBefore) < minImprovementDb;
}

qreal KisAiVisionCritic::psnr(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return 0.0;

    const QImage ia = a.convertToFormat(QImage::Format_ARGB32);
    const QImage ib = b.convertToFormat(QImage::Format_ARGB32);

    qreal sumSq = 0.0;
    qint64 count = 0;
    for (int y = 0; y < ia.height(); ++y) {
        const QRgb *rowA = reinterpret_cast<const QRgb *>(ia.constScanLine(y));
        const QRgb *rowB = reinterpret_cast<const QRgb *>(ib.constScanLine(y));
        for (int x = 0; x < ia.width(); ++x) {
            const QRgb pxA = rowA[x];
            const QRgb pxB = rowB[x];
            const int dr = qRed(pxA) - qRed(pxB);
            const int dg = qGreen(pxA) - qGreen(pxB);
            const int db = qBlue(pxA) - qBlue(pxB);
            const int da = qAlpha(pxA) - qAlpha(pxB);
            sumSq += qreal(dr * dr + dg * dg + db * db + da * da);
            count += 4;
        }
    }
    if (count == 0 || sumSq <= 0.0)
        return 60.0; // identical
    const qreal mse = sumSq / count;
    return qMin<qreal>(60.0, 10.0 * std::log10(255.0 * 255.0 / mse));
}

QVector<KisAiCritiqueRegion> KisAiVisionCritic::mergeRegions(
    const QVector<KisAiCritiqueRegion> &accumulated,
    const QVector<KisAiCritiqueRegion> &incoming)
{
    QVector<KisAiCritiqueRegion> merged = accumulated;
    for (const KisAiCritiqueRegion &r : incoming) {
        bool found = false;
        for (KisAiCritiqueRegion &existing : merged) {
            if (existing.area == r.area && existing.issue == r.issue) {
                existing.priority = qMax(existing.priority, r.priority);
                existing.action = r.action; // latest verdict wins
                found = true;
                break;
            }
        }
        if (!found)
            merged.append(r);
    }
    return merged;
}
