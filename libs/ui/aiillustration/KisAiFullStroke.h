/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_FULL_STROKE_H
#define KIS_AI_FULL_STROKE_H

#include <QJsonObject>
#include <QSize>
#include <QString>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiStrokeProgram.h"

/** The LLM-only drawing contract: no generated/reference images may enter. */
class KRITAUI_EXPORT KisAiFullStroke
{
public:
    static QJsonObject buildScenePayload(const QString &model, const QString &prompt, const QSize &canvasSize);
    static QJsonObject buildPayload(const QString &model,
                                    const QString &prompt,
                                    const QSize &canvasSize,
                                    int strokeBudget,
                                    const QString &customInstructions = QString());
    static bool acceptsEndpoint(const QString &endpoint);
    static bool acceptsProgram(const KisAiStrokeProgram &program);
};

#endif // KIS_AI_FULL_STROKE_H
