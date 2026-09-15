/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_RENDERER_H
#define KIS_AI_STROKE_RENDERER_H

#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

#include "KisAiStrokeProgram.h"
#ifndef AI_STROKE_STANDALONE
#include "kis_types.h"
#include "kritaui_export.h"

class KisViewManager;
#else
#define KRITAUI_EXPORT
#endif

/**
 * Native coordinate stroke rasterizer that materializes a KisAiStrokeProgram
 * into Krita paint layers (Flats, Shading, Lineart, Highlights, FX) using Krita's
 * command architecture for full Undo/Redo fidelity.
 * Features Centripetal Catmull-Rom spline interpolation, pressure tapering,
 * brush profile dynamics, and automatic silhouette clipping.
 */
class KRITAUI_EXPORT KisAiStrokeRenderer
{
public:
    /**
     * Compute a smooth Centripetal Catmull-Rom spline curve from discrete control points.
     */
    static QVector<QPointF>
    generateCatmullRomSpline(const QVector<QPointF> &points, int subdivisions = 8, bool closed = false);

#ifndef AI_STROKE_STANDALONE
    /**
     * Render the stroke program into discrete paint layers on the specified Krita image.
     * Each layer group (Flats, Shading, Lineart, Highlights, FX) is created as an independent
     * KisPaintLayer and added via KisNodeCommandsAdapter so the entire generation is undoable.
     * When clipShadingToFlats is true, Shading and Highlights layers are automatically clipped
     * to the Flats silhouette to prevent color spills.
     */
    static bool renderProgramToLayers(KisImageWSP image,
                                      KisViewManager *viewManager,
                                      const KisAiStrokeProgram &program,
                                      QString *statusMessage = nullptr,
                                      bool clipShadingToFlats = true,
                                      qreal trappingPx = -1.0);

    /**
     * Render one program step while using Flats operations from an earlier step
     * as a clipping mask for Shading and Highlights.
     */
    static bool renderProgramToLayers(KisImageWSP image,
                                      KisViewManager *viewManager,
                                      const KisAiStrokeProgram &program,
                                      QString *statusMessage,
                                      bool clipShadingToFlats,
                                      const KisAiStrokeProgram *inheritedFlatsProgram,
                                      qreal trappingPx = -1.0);
#endif

    /**
     * Render the complete stroke program into a composite QImage (useful for previewing).
     * Respects layer blending modes (Multiply for Shading, Addition for Highlights) and clipping.
     */
    static QImage renderProgramToImage(const KisAiStrokeProgram &program,
                                       const QSize &targetSize,
                                       bool clipShadingToFlats = true,
                                       qreal trappingPx = -1.0);

    /** Render a program while using Flats operations from an earlier program as a clipping mask. */
    static QImage renderProgramToImage(const KisAiStrokeProgram &program,
                                       const QSize &targetSize,
                                       bool clipShadingToFlats,
                                       const KisAiStrokeProgram *inheritedFlatsProgram,
                                       qreal trappingPx = -1.0);

    /**
     * Encode a QImage into a JPEG Base64 Data URL (scaled down if exceeding maxDimension).
     */
    static QString captureImageBase64(const QImage &image, int maxDimension = 768, int quality = 80);

#ifndef AI_STROKE_STANDALONE
    /**
     * Capture the current state of a Krita image to a Base64 Data URL for Vision LLM input.
     */
    static QString captureCanvasBase64(KisImageWSP image, int maxDimension = 768, int quality = 80);
#endif

    /**
     * Expand high-level procedural operations into rich organic stroke primitives (Phase 3).
     * Converts single hair ribbons into flowing strand clumps + flyaways + angel halo,
     * and canopy polygons into layered petal/foliage clusters.
     */
    static QVector<KisAiStrokeOperation> expandProceduralOperations(const QVector<KisAiStrokeOperation> &operations,
                                                                    const QSize &canvasSize);

    /**
     * Generate an isolated luminous bloom halo map on a transparent canvas (Phase 4).
     */
    static QImage generateBloomMap(const QImage &image, qreal intensity = 0.40, int radius = 6);

    /**
     * Apply luminous bloom diffusion to highlights and FX (Phase 4).
     */
    static void applyBloomEffect(QImage &image, qreal intensity = 0.40, int radius = 6);

    /**
     * Apply subtle optical chromatic aberration lens fringing (Phase 4).
     */
    static void applyChromaticAberration(QImage &image, int shiftPx = 1);

    /**
     * Apply smooth cinematic vignette to guide viewer focus toward center (Phase 4).
     */
    static void applyVignette(QImage &image, qreal strength = 0.12);

    /**
     * Apply full professional post-processing finishing suite (Phase 4).
     */
    static void applyFinishingPostProcess(QImage &image);

    /**
     * Fast 2-pass separable box blur for form shading diffusion and organic transitions.
     */
    static void applySoftEdgeDiffusion(QImage &image, int radius);

    /**
     * V6 W6: render performance guard. Measures the preview render and
     * reports whether the caller should degrade (fewer crops, smaller blur,
     * single critique round). Pure: never changes the output image.
     */
    struct RenderBudget {
        qint64 elapsedMs{0};
        bool degraded{false};
        QString note;
    };
    static RenderBudget renderBudgetFor(const QSize &size, int opCount);

private:
    static QImage
    renderOperationsToImage(const QVector<KisAiStrokeOperation> &operations,
                            const QSize &canvasSize,
                            const QPainterPath &faceExclusionPath = QPainterPath(),
                            const QMap<QString, QPolygonF> &globalSilhouettes = QMap<QString, QPolygonF>());

    static void rasterizeOperation(QPainter &painter,
                                   const KisAiStrokeOperation &op,
                                   const QSize &canvasSize,
                                   int supersampleScale = 1,
                                   const QPainterPath &faceExclusionPath = QPainterPath());

    static void drawPathOperation(QPainter &painter,
                                  const KisAiStrokeOperation &op,
                                  const QSize &canvasSize,
                                  int supersampleScale = 1);

    static void drawFillOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize);

    static void drawGradientFillOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize);

    static void drawRibbonOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize);

    static void drawParticlesOperation(QPainter &painter,
                                       const KisAiStrokeOperation &op,
                                       const QSize &canvasSize,
                                       int supersampleScale = 1,
                                       const QPainterPath &faceExclusionPath = QPainterPath());

    static void drawAnimeEyeOperation(QPainter &painter,
                                      const KisAiStrokeOperation &op,
                                      const QSize &canvasSize,
                                      int supersampleScale = 1);

    static void drawAnimeMouthOperation(QPainter &painter,
                                        const KisAiStrokeOperation &op,
                                        const QSize &canvasSize,
                                        int supersampleScale = 1);

    static void drawHatchOperation(QPainter &painter,
                                   const KisAiStrokeOperation &op,
                                   const QSize &canvasSize,
                                   int supersampleScale = 1);

    static void drawMangaLinesOperation(QPainter &painter,
                                        const KisAiStrokeOperation &op,
                                        const QSize &canvasSize,
                                        int supersampleScale = 1);
};

#endif // KIS_AI_STROKE_RENDERER_H
