/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_PRIMITIVE_EXPANDER_H
#define KIS_AI_PRIMITIVE_EXPANDER_H

#include <QSize>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiInkStroke.h"
#include "KisAiStrokeProgram.h"

/**
 * V9: explode composite primitives (AnimeEye / AnimeMouth / Hatch /
 * MangaLines / Particles) into atomic Path/Fill operations so each
 * line can be linted, reviewed, and committed independently.
 *
 * Path / Fill / Ribbon / GradientFill pass through 1:1.
 */
class KRITAUI_EXPORT KisAiPrimitiveExpander
{
public:
    static QVector<KisAiStrokeOperation> expand(const KisAiStrokeOperation &op, const QSize &canvasSize);
    static QVector<KisAiStrokeOperation> expandAll(const QVector<KisAiStrokeOperation> &ops, const QSize &canvasSize);

    static bool isCompositeKind(KisAiStrokeOperation::Kind kind);

    static int atomicPathCount(const QVector<KisAiStrokeOperation> &ops);
    static int leftoverCompositeCount(const QVector<KisAiStrokeOperation> &ops);

private:
    static QVector<KisAiStrokeOperation> expandEye(const KisAiStrokeOperation &op, const QSize &canvasSize);
    static QVector<KisAiStrokeOperation> expandMouth(const KisAiStrokeOperation &op, const QSize &canvasSize);
    static QVector<KisAiStrokeOperation> expandHatch(const KisAiStrokeOperation &op, const QSize &canvasSize);
    static QVector<KisAiStrokeOperation> expandMangaLines(const KisAiStrokeOperation &op, const QSize &canvasSize);
    static QVector<KisAiStrokeOperation> expandParticles(const KisAiStrokeOperation &op, const QSize &canvasSize);

    static KisAiStrokeOperation makePath(const QString &id,
                                         const QString &groupId,
                                         const QString &layer,
                                         const QVector<KisAiStrokePoint> &pts,
                                         const QColor &color,
                                         const QString &profile,
                                         qreal size,
                                         qreal opacity,
                                         const QString &role,
                                         const QString &parentId = QString());
    static KisAiStrokeOperation makeFill(const QString &id,
                                         const QString &groupId,
                                         const QString &layer,
                                         const QPolygonF &poly,
                                         const QColor &color,
                                         qreal opacity,
                                         const QString &role,
                                         const QString &clipToId = QString());
};

#endif // KIS_AI_PRIMITIVE_EXPANDER_H
