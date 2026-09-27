/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_IMAGE_GUIDED_SCENE_H
#define KIS_AI_IMAGE_GUIDED_SCENE_H

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiStrokeProgram.h"

/**
 * Provider-independent geometry contract. Masks are explicit, pixel-aligned
 * evidence in source-image coordinates, not edges inferred from texture.
 * An image-generation endpoint is NOT assumed to supply masks.
 */
struct KRITAUI_EXPORT KisAiGuidedRegion {
    QString id; // Stable, non-sensitive ASCII identifier.
    QImage mask; // Source-sized Grayscale8 or Alpha8: 0..255.
    qreal confidence{0.0}; // Independent semantic/segmentation confidence.
    bool outlineApproved{false}; // Explicitly approved for inking (never inferred from mask alone).
};

struct KRITAUI_EXPORT KisAiImageGuidedScene {
    QImage source; // Original image; never modified by extraction.
    QSize canvasSize;
    QVector<KisAiGuidedRegion> regions;

    // Same centered KeepAspectRatio placement as the existing image layer.
    QRectF imageRectPx() const;
    bool isValid() const;
};

/**
 * Conservative first stage of image-guided inking: trace only explicit,
 * high-confidence, connected object masks. When evidence is weak, return no
 * strokes; the caller must preserve the original image instead of inventing
 * a contour. No image models or network calls are made here.
 */
class KRITAUI_EXPORT KisAiImageGuidedContour
{
public:
    static KisAiStrokeProgram buildLineart(const KisAiImageGuidedScene &scene);
};

#endif // KIS_AI_IMAGE_GUIDED_SCENE_H
