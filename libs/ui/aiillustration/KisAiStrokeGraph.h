/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_GRAPH_H
#define KIS_AI_STROKE_GRAPH_H

#include <QSize>
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
 * V9 stroke graph: paint order honours layer, role, mass, and
 * left/right symmetry interleaving so paired features (eyes) advance
 * role-by-role instead of finishing one side first.
 */
class KRITAUI_EXPORT KisAiStrokeGraph
{
public:
    static QVector<KisAiStrokeOperation> orderForCommit(const QVector<KisAiStrokeOperation> &ops,
                                                        const QSize &canvasSize);

    static QString inferGroupId(const KisAiStrokeOperation &op);
    static QString inferParentId(const KisAiStrokeOperation &op);
    static QString inferRole(const KisAiStrokeOperation &op);

    static QStringList groupIdsInOrder(const QVector<KisAiStrokeOperation> &ops);
    static QVector<KisAiStrokeOperation> opsInGroup(const QVector<KisAiStrokeOperation> &ops, const QString &groupId);

    /**
     * Snap child Path endpoints onto the nearest point of their parent
     * contour (T-stop), including mid-segment projections. Returns the
     * number of endpoints moved.
     */
    static int snapTStops(QVector<KisAiStrokeOperation> &ops, const QSize &canvasSize, qreal snapPx = 1.2);

    static QStringList critiqueGroup(const QString &groupId, const QVector<KisAiStrokeOperation> &ops);
};

#endif // KIS_AI_STROKE_GRAPH_H
