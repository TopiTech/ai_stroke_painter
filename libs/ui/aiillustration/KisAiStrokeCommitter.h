/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_COMMITTER_H
#define KIS_AI_STROKE_COMMITTER_H

#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSize>
#include <QString>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiDeliberateStroke.h"
#include "KisAiInkStroke.h"
#include "KisAiStrokeProgram.h"

/**
 * V9 Atomic Ink committer.
 *
 * One stroke at a time: stabilize → lint (on stabilized geometry) →
 * repair → dry-run pixel review → commit or skip. Composite kinds are
 * expanded before the loop so leftover AnimeEye/Mouth never reach ink.
 */
class KRITAUI_EXPORT KisAiStrokeCommitter
{
public:
    struct Options {
        int supersampleScale{1};
        int maxRetries{2};
        bool pixelReview{true};
        bool skipFaceParticles{true};
    };

    static QVector<KisAiStrokeOperation> prepareAtomicOps(const QVector<KisAiStrokeOperation> &operations,
                                                          const QSize &canvasSize);

    static KisAiStrokeOperation stabilizeOperation(const KisAiStrokeOperation &op, const QSize &canvasSize);

    static KisAiStrokeOperation
    repairOperation(const KisAiStrokeOperation &op, const KisAiStrokeLintReport &lint, const QSize &canvasSize);

    static KisAiStrokeCommitReview reviewPixels(const QImage &before, const QImage &after, const QRect &dirtyPx);

    /**
     * Paint prepared atomic operations onto @p painter (working-size
     * coordinates). Returns the surviving ops and a commit log.
     */
    static QVector<KisAiStrokeOperation> commitToPainter(QPainter &painter,
                                                         const QVector<KisAiStrokeOperation> &operations,
                                                         const QSize &workingSize,
                                                         const QSize &logicalSize,
                                                         int supersampleScale,
                                                         const QPainterPath &faceExclusionPath,
                                                         const QMap<QString, QPolygonF> &globalSilhouettes,
                                                         KisAiStrokeCommitLog *log = nullptr);

    static const KisAiStrokeCommitLog &lastLog();

    static qreal atomicStrokeRatio(const QVector<KisAiStrokeOperation> &ops);

private:
    static KisAiStrokeCommitLog s_lastLog;
};

#endif // KIS_AI_STROKE_COMMITTER_H
