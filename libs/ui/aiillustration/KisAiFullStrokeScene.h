/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_FULL_STROKE_SCENE_H
#define KIS_AI_FULL_STROKE_SCENE_H

#include <QByteArray>
#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiStrokeProgram.h"

struct KRITAUI_EXPORT KisAiFullStrokeObject {
    QString id;
    QString type;
    QString parentId;
    QString behindId;
    QRectF bounds;
    qreal focalWeight{0.5};
    bool required{true};
};

/** Structured LLM plan. All geometry stays in normalized canvas coordinates. */
struct KRITAUI_EXPORT KisAiFullStrokeScene {
    QVector<KisAiFullStrokeObject> objects;

    static bool parse(const QJsonObject &json, KisAiFullStrokeScene *out, QString *error = nullptr);
    static bool parseResponse(const QByteArray &response, KisAiFullStrokeScene *out, QString *error = nullptr);
    QStringList missingRequiredObjects(const KisAiStrokeProgram &program) const;
    QString drawingInstructions() const;
    KisAiStrokeProgram prioritize(const KisAiStrokeProgram &program, int operationBudget) const;
};

#endif // KIS_AI_FULL_STROKE_SCENE_H
