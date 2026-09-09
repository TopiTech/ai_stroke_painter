/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_TYPE_CHECKER_H
#define KIS_AI_STROKE_TYPE_CHECKER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

struct KRITAUI_EXPORT KisAiStrokeTypeCheckReport
{
    bool isValid {true};
    int totalCheckedOperations {0};
    int coercedValues {0};
    int typeErrors {0};
    QStringList errorMessages;
    QStringList warnings;

    QString summary() const;
};

/**
 * Runtime schema and type validator for LLM-generated StrokeProgram JSON.
 * Performs rigorous type-checking, validates field domains, and provides
 * safe coercion for forgiving types (e.g. numeric strings, varied point representations).
 */
class KRITAUI_EXPORT KisAiStrokeTypeChecker
{
public:
    /**
     * Check and normalize the entire StrokeProgram root JSON object.
     * Modifies the input object in-place if coercions are applied.
     * Returns true if the program conforms to the schema or was successfully coerced.
     */
    static bool checkAndCoerceProgram(
        QJsonObject *programObject,
        KisAiStrokeTypeCheckReport *report = nullptr
    );

    /**
     * Check and normalize a single operation JSON object.
     */
    static bool checkAndCoerceOperation(
        QJsonObject *opObj,
        int opIndex = 0,
        KisAiStrokeTypeCheckReport *report = nullptr
    );

    /**
     * Check and normalize points array (accepts [[x, y], [x, y, p]] or [{"x": x, "y": y, ...}]).
     */
    static bool checkPointsArray(
        QJsonArray *pointsArray,
        QString *outError = nullptr,
        int *coercedCount = nullptr
    );

    /**
     * Check and normalize polygon array (accepts [[x, y], ...] or [{"x": x, "y": y}]).
     */
    static bool checkPolygonArray(
        QJsonArray *polygonArray,
        QString *outError = nullptr,
        int *coercedCount = nullptr
    );

    /**
     * Check and normalize brush object (validates profile, color hex, size, opacity, is_eraser).
     */
    static bool checkBrushObject(
        QJsonObject *brushObj,
        QString *outError = nullptr,
        int *coercedCount = nullptr
    );

    /** Safe coercion helpers */
    static bool coerceToNumber(const QJsonValue &val, qreal *outVal);
    static bool coerceToBool(const QJsonValue &val, bool *outVal);
    static bool coerceToString(const QJsonValue &val, QString *outVal);
    static bool isValidColorString(const QString &str);
};

#endif // KIS_AI_STROKE_TYPE_CHECKER_H
