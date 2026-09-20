/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_INK_STROKE_H
#define KIS_AI_INK_STROKE_H

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

/**
 * V9 Atomic Ink IR annotations.
 *
 * KisAiStrokeOperation remains the drawable contract. These fields
 * (copied onto the operation) tell the committer how to plan, join,
 * and review one stroke at a time.
 */
struct KRITAUI_EXPORT KisAiInkStroke {
    KisAiStrokeOperation op;
    QString groupId;
    QString inkRole; // mass, contour, internal, accent, construction
    QString parentId;
    int retryBudget{2};

    static KisAiInkStroke fromOperation(const KisAiStrokeOperation &op);
    KisAiStrokeOperation toOperation() const;
};

struct KRITAUI_EXPORT KisAiStrokeCommitLog {
    QStringList lines;
    int committed{0};
    int skipped{0};
    int repaired{0};
    int retried{0};

    QString summary() const;
    bool isEmpty() const
    {
        return lines.isEmpty() && committed == 0 && skipped == 0;
    }
};

namespace KisAiInkRole
{
KRITAUI_EXPORT QString mass();
KRITAUI_EXPORT QString contour();
KRITAUI_EXPORT QString internalFlow();
KRITAUI_EXPORT QString accent();
KRITAUI_EXPORT QString construction();
KRITAUI_EXPORT int rank(const QString &role);
} // namespace KisAiInkRole

#endif // KIS_AI_INK_STROKE_H
