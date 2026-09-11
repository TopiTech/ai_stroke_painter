/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_RENDERER_H
#define KIS_AI_STROKE_RENDERER_H

#include <QImage>
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
    static QVector<QPointF> generateCatmullRomSpline(
        const QVector<QPointF> &points,
        int subdivisions = 8,
        bool closed = false
    );

#ifndef AI_STROKE_STANDALONE
    /**
     * Render the stroke program into discrete paint layers on the specified Krita image.
     * Each layer group (Flats, Shading, Lineart, Highlights, FX) is created as an independent
     * KisPaintLayer and added via KisNodeCommandsAdapter so the entire generation is undoable.
     * When clipShadingToFlats is true, Shading and Highlights layers are automatically clipped
     * to the Flats silhouette to prevent color spills.
     */
    static bool renderProgramToLayers(
        KisImageWSP image,
        KisViewManager *viewManager,
        const KisAiStrokeProgram &program,
        QString *statusMessage = nullptr,
        bool clipShadingToFlats = true
    );

    /**
     * Render one program step while using Flats operations from an earlier step
     * as a clipping mask for Shading and Highlights.
     */
    static bool renderProgramToLayers(KisImageWSP image,
                                      KisViewManager *viewManager,
                                      const KisAiStrokeProgram &program,
                                      QString *statusMessage,
                                      bool clipShadingToFlats,
                                      const KisAiStrokeProgram *inheritedFlatsProgram);
#endif

    /**
     * Render the complete stroke program into a composite QImage (useful for previewing).
     * Respects layer blending modes (Multiply for Shading, Addition for Highlights) and clipping.
     */
    static QImage renderProgramToImage(
        const KisAiStrokeProgram &program,
        const QSize &targetSize,
        bool clipShadingToFlats = true
    );

    /** Render a program while using Flats operations from an earlier program as a clipping mask. */
    static QImage renderProgramToImage(const KisAiStrokeProgram &program,
                                       const QSize &targetSize,
                                       bool clipShadingToFlats,
                                       const KisAiStrokeProgram *inheritedFlatsProgram);

    /**
     * Encode a QImage into a JPEG Base64 Data URL (scaled down if exceeding maxDimension).
     */
    static QString captureImageBase64(
        const QImage &image,
        int maxDimension = 768,
        int quality = 80
    );

#ifndef AI_STROKE_STANDALONE
    /**
     * Capture the current state of a Krita image to a Base64 Data URL for Vision LLM input.
     */
    static QString captureCanvasBase64(
        KisImageWSP image,
        int maxDimension = 768,
        int quality = 80
    );
#endif

private:
    static QImage renderOperationsToImage(
        const QVector<KisAiStrokeOperation> &operations,
        const QSize &canvasSize
    );

    static void rasterizeOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void drawPathOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void drawFillOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void drawGradientFillOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void drawRibbonOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void drawParticlesOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void drawHatchOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void drawMangaLinesOperation(
        QPainter &painter,
        const KisAiStrokeOperation &op,
        const QSize &canvasSize
    );

    static void applySoftEdgeDiffusion(
        QImage &image,
        int radius
    );
};

#endif // KIS_AI_STROKE_RENDERER_H
