/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiFullStroke.h"

#include "KisAiIllustrationRenderer.h"

#include <QJsonArray>
#include <QUrl>
#include <QtGlobal>

QJsonObject KisAiFullStroke::buildScenePayload(const QString &model, const QString &prompt, const QSize &canvasSize)
{
    const QString instruction = QStringLiteral(
        "Return only a JSON object with an 'objects' array (1..64 items). Each object must have a unique ASCII id, "
        "type, bounds [x,y,w,h] within 0..1, required (boolean), focal_weight (0..1), and optional "
        "parent_id/behind_id. "
        "Plan all essential subjects, foreground/background depth and negative space. Do not return drawing operations "
        "or images at this stage.");
    QJsonObject payload = KisAiStrokeProgramCodec::buildChatCompletionsPayload(model,
                                                                               prompt,
                                                                               canvasSize,
                                                                               100,
                                                                               QString(),
                                                                               instruction,
                                                                               false,
                                                                               true,
                                                                               0.35,
                                                                               1.0,
                                                                               2048,
                                                                               0,
                                                                               true,
                                                                               QString());
    // This stage returns a scene description, not the stroke-program schema.
    payload[QStringLiteral("response_format")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}};
    return payload;
}

QJsonObject KisAiFullStroke::buildPayload(const QString &model,
                                          const QString &prompt,
                                          const QSize &canvasSize,
                                          int strokeBudget,
                                          const QString &customInstructions)
{
    const QString fullInstructions =
        customInstructions
        + QStringLiteral(
            "\n[FULL STROKE CONTRACT]\n"
            "Construct all background, flats, shading, lineart and highlights from supported draw operations."
            " Assign every primary object an ASCII group_id. Draw required Flats silhouettes before details."
            " Never request generated images or embed image pixels; return only a stroke program.");
    return KisAiStrokeProgramCodec::buildChatCompletionsPayload(model,
                                                                prompt,
                                                                canvasSize,
                                                                qBound(20, strokeBudget, 4000),
                                                                QString(),
                                                                fullInstructions,
                                                                true,
                                                                true,
                                                                0.5,
                                                                1.0,
                                                                0,
                                                                0,
                                                                false,
                                                                QString());
}

bool KisAiFullStroke::acceptsEndpoint(const QString &endpoint)
{
    if (!KisAiIllustrationRenderer::validateImageEndpoint(endpoint))
        return false;
    const QUrl url = QUrl::fromUserInput(endpoint.trimmed());
    return url.path().endsWith(QLatin1String("/chat/completions"), Qt::CaseInsensitive);
}

bool KisAiFullStroke::acceptsProgram(const KisAiStrokeProgram &program)
{
    if (program.operations.isEmpty())
        return false;
    for (const KisAiStrokeOperation &op : program.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::Unknown || op.id.isEmpty())
            return false;
    }
    return true;
}
