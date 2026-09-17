/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_QUALITY_UTILS_H
#define KIS_AI_STROKE_QUALITY_UTILS_H

#include <QColor>
#include <QPainter>
#include <QPointF>
#include <QPolygonF>
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
 * Advanced geometric, artistic brush simulation, color harmony, and
 * vector stroke quality enhancement utilities for AI Stroke Painter.
 */
class KRITAUI_EXPORT KisAiStrokeQualityUtils
{
public:
    // =========================================================================
    // 1. Geometry, Resampling & Smoothing Utilities
    // =========================================================================

    /**
     * Resample stroke points with uniform arc-length intervals (stepPx).
     * Eliminates density irregularities from LLM outputs while preserving
     * pressure interpolation and shape fidelity.
     */
    static QVector<KisAiStrokePoint> resampleEquidistant(
        const QVector<KisAiStrokePoint> &points,
        qreal stepPx,
        bool closed = false
    );

    /**
     * Classic Ramer-Douglas-Peucker (RDP) polyline simplification.
     * Removes colinear and jittery micro-noise below epsilonPx.
     */
    static QVector<QPointF> simplifyRDP(
        const QVector<QPointF> &points,
        qreal epsilonPx
    );

    /**
     * Corner-preserving adaptive spline smoothing for polygons.
     * Detects sharp corners (angle sharper than cornerAngleThresholdDeg)
     * and keeps them crisp while smoothing gentle curves.
     */
    static QPolygonF smoothPolygonCornerPreserving(
        const QPolygonF &polygon,
        qreal cornerAngleThresholdDeg = 135.0,
        int subdivisions = 4
    );

    /**
     * Inset or outset a polygon by a distance in pixels.
     * Positive distance expands outward (trapping), negative contracts inward.
     * Miter limit is applied to prevent spike blowups on sharp vertices.
     */
    static QPolygonF offsetPolygon(
        const QPolygonF &polygon,
        qreal distancePx
    );

    /**
     * Compute discrete curvature values (1/R) along a point sequence.
     */
    static QVector<qreal> computeCurvatures(const QVector<QPointF> &points);

    // =========================================================================
    // 2. Stroke Envelope & Quad Mesh Generation
    // =========================================================================

    struct StrokeEnvelopeSegment {
        QPointF leftStart;
        QPointF leftEnd;
        QPointF rightStart;
        QPointF rightEnd;
        QPointF centerStart;
        QPointF centerEnd;
        qreal widthStart {1.0};
        qreal widthEnd {1.0};

        QPolygonF toQuad() const {
            QPolygonF quad;
            quad.reserve(4);
            quad << leftStart << leftEnd << rightEnd << rightStart;
            return quad;
        }
    };

    /**
     * Generate continuous quad segments along the stroke spine without
     * self-intersection bowties or twist artifacts on sharp turns.
     */
    static QVector<StrokeEnvelopeSegment> generateStrokeEnvelope(
        const QVector<KisAiStrokePoint> &points,
        const KisAiStrokeBrush &brush,
        const QSize &canvasSize,
        bool closed = false,
        int supersampleScale = 1
    );

    /**
     * Calculate profile-specific taper multiplier for a given normalized progress [0.0, 1.0].
     */
    static qreal calculateTaper(
        qreal globalT,
        const QString &profile,
        bool isClosed = false
    );

    // =========================================================================
    // 3. Artistic Brush Dynamics & Procedural Textures
    // =========================================================================

    /**
     * Generate multi-strand offset spine polylines for realistic bristle/oil brush marks.
     */
    static QVector<QVector<QPointF>> generateBristleStrands(
        const QVector<KisAiStrokePoint> &spine,
        int strandCount,
        qreal maxSpreadPx,
        quint32 seed
    );

    /**
     * Modulate stroke width based on movement tangent vector relative to a fixed nib angle
     * (e.g. 45 degrees for calligraphy / chisel pen).
     */
    static qreal calculateCalligraphyWidth(
        const QPointF &tangent,
        qreal baseWidthPx,
        qreal nibAngleDeg = 45.0,
        qreal thinRatio = 0.20
    );

    /**
     * Render professional manga halftone screen (dot screen or line screen) inside a polygon.
     */
    static void drawHalftonePattern(
        QPainter &painter,
        const QPolygonF &polygon,
        const QColor &color,
        qreal dotSpacingPx = 8.0,
        qreal dotRadiusPx = 2.5,
        qreal angleDeg = 45.0,
        bool lineScreen = false
    );

    // =========================================================================
    // 4. Color & Lighting Harmonies
    // =========================================================================

    /**
     * Calculate artist-grade hue-shifted shadow color.
     * Warm key lights shift shadows toward cool purple/blue; cool lights shift toward warm amber.
     */
    static QColor calculateHueShiftedShadow(
        const QColor &baseColor,
        const QColor &ambientShadowTint = QColor(35, 40, 65),
        qreal shadowDepth = 0.35
    );

    /**
     * Calculate artist-grade hue-shifted highlight color.
     */
    static QColor calculateHueShiftedHighlight(
        const QColor &baseColor,
        const QColor &keyLightTint = QColor(255, 252, 240),
        qreal intensity = 0.50
    );

    // =========================================================================
    // 5. Stroke Program Optimization & Trapping
    // =========================================================================

    /**
     * Apply trapping (slight dilation of Flats polygons) to tuck under Lineart strokes,
     * permanently fixing unwanted white gaps and underfill seams.
     */
    static KisAiStrokeProgram applyTrapping(
        const KisAiStrokeProgram &program,
        qreal trappingPx = 1.5
    );

    /**
     * V3 Phase 0.2: Unite overlapping hair Flats fills into continuous
     * silhouettes. LLMs often emit hair as dozens of small overlapping
     * circles ("bubble/afro" artifact); intersecting candidates are merged
     * via boolean union while isolated ones are preserved untouched.
     */
    static QVector<KisAiStrokeOperation> uniteOverlappingHairFlats(
        const QVector<KisAiStrokeOperation> &operations
    );

    /**
     * V3 Phase 2: Normalize Lineart stroke weights into a 3-tier hierarchy
     * (outer contours 0.008 / structure 0.005 / details 0.003, closed +0.001).
     * Opt-in pass used by the LayoutEngine; refineForRendering() never calls
     * it implicitly so hand-tuned LLM sizes survive. Returns adjusted count.
     */
    static int applyLineartHierarchy(
        QVector<KisAiStrokeOperation> &operations
    );

    /**
     * V3 Phase 2: Map a brush profile to its Krita preset counterpart
     * (e.g. gpen -> Pencil-2) for the native rasterization path.
     */
    static QString brushPresetName(const QString &profile);

    /**
     * Fill empty brush.presetHint fields from the profile mapping.
     * Returns the number of hints assigned.
     */
    static int assignBrushPresetHints(
        KisAiStrokeProgram &program
    );

    // =========================================================================
    // 6. Algorithmic Detail Synthesizers (Phase 3)
    // =========================================================================

    struct HairClumpSynthesis {
        KisAiStrokeOperation mainMass;
        QVector<KisAiStrokeOperation> strands;
        QVector<KisAiStrokeOperation> flyaways;
        KisAiStrokeOperation highlightHalo;
    };

    /**
     * Synthesize rich organic hair details from a single spine ribbon operation.
     * Generates flowing internal strands, flyaway wisps, and specular angel halo highlights.
     */
    static HairClumpSynthesis synthesizeHairClump(
        const KisAiStrokeOperation &ribbonOp,
        const QSize &canvasSize,
        quint32 seed = 42
    );

    /**
     * Synthesize multi-layered foliage and petal clusters from a rough canopy volume polygon.
     * Generates organic cloud-like clusters, ambient occlusion under-shading, and drifting petals.
     */
    static QVector<KisAiStrokeOperation> synthesizeFoliageClusters(
        const KisAiStrokeOperation &fillOp,
        const QSize &canvasSize,
        quint32 seed = 42
    );

    /**
     * Determine if a shading polygon is a sharp Cast Shadow (e.g. bangs, folds)
     * or a soft Form Shadow (e.g. cheek curve, torso roundness).
     */
    static bool isCastShadow(
        const QPolygonF &polygon,
        const QSize &canvasSize
    );

    // =========================================================================
    // 7. Advanced Artistic Fidelity & Inking Utilities (V5)
    // =========================================================================

    /**
     * Corner Inking / Ambient Occlusion dots.
     * Detects stroke-stroke intersections and sharp corners (< 125 deg)
     * and generates small ink pooling dots to simulate real pen bleed.
     */
    static QVector<KisAiStrokeOperation> generateCornerInkingDots(
        const QVector<KisAiStrokeOperation> &operations,
        const QSize &canvasSize
    );

    /**
     * Intelligent Colored Lineart (色トレス).
     * Modulates dark ink line colors based on the underlying Flats color.
     * Skin contours become deep coral/mahogany, hair contours become deep harmonic tones.
     */
    static QColor calculateHarmonicLineColor(
        const QColor &baseInkColor,
        const QColor &underlyingFlatsColor,
        bool isSkin
    );

    /**
     * Subsurface Scattering (SSS) fringe for skin form shadows.
     * Generates a warm, vibrant coral/crimson feather edge along the terminator of skin shadows.
     */
    static QVector<KisAiStrokeOperation> generateSkinSssFringe(
        const KisAiStrokeOperation &shadingOp,
        const QSize &canvasSize
    );

    /**
     * Facial Contour Beautifier.
     * Snaps and smooths coarse jaw/chin/cheek paths into elegant, proportional anime facial curves.
     */
    static QVector<KisAiStrokePoint> beautifyFacialContour(
        const QVector<KisAiStrokePoint> &rawPoints,
        const QSize &canvasSize
    );

    /**
     * Rim light generator.
     * Generates delicate specular rim highlight strokes along the outer silhouette edges facing key/back light.
     */
    static QVector<KisAiStrokeOperation> generateRimLightStrokes(
        const QVector<KisAiStrokeOperation> &operations,
        const QSize &canvasSize,
        const QPointF &lightDir = QPointF(0.707, -0.707)
    );

    /**
     * Procedural cheek blush and soft glow generator.
     * Injects soft radial cheek blush when eyes/face are present.
     */
    static QVector<KisAiStrokeOperation> generateProceduralBlush(
        const QVector<KisAiStrokeOperation> &operations,
        const QSize &canvasSize
    );

    /**
     * Generate film / paper grain noise overlay.
     */
    static QImage generateFilmGrain(
        const QSize &size,
        qreal intensity = 0.08,
        quint32 seed = 1337
    );

    /**
     * Generate cinematic vignette image.
     */
    static QImage generateVignetteImage(
        const QSize &size,
        qreal strength = 0.15
    );

    /**
     * Generate atmospheric ambient overlay gradient.
     */
    static QImage generateAmbientOverlay(
        const QSize &size,
        int artStyle = 0,
        int timeOfDay = 0
    );

    // =========================================================================
    // 8. V7 Organic Brush Inking & Dynamic Beautification
    // =========================================================================

    /**
     * Fast deterministic 1D/2D value noise for procedural bristle jitter and paper texture.
     */
    static qreal noise1D(qreal t, quint32 seed = 42);

    /**
     * V7 Stroke Beautifier & Stabilizer.
     * Removes micro-jitter, repairs degenerate loops, smooths curvature,
     * and shapes dynamic pressure taper profiles along raw input points.
     */
    static QVector<KisAiStrokePoint> stabilizeAndBeautifyStroke(
        const QVector<KisAiStrokePoint> &points,
        bool closed = false
    );

    /**
     * V7 Lineart Occlusion & Light Direction Weighting.
     * Modulates Lineart stroke widths based on whether they fall on the shadow side
     * (thicker, heavier) vs light side (thinner, delicate) or outer silhouettes.
     */
    static void applyLineartOcclusionWeights(
        QVector<KisAiStrokeOperation> &operations,
        const QPointF &lightDir = QPointF(-0.5, -0.7)
    );

    /**
     * V7 Inking Corner Fillet / Ink Pooling Polygon.
     * Computes a smooth organic wedge at sharp corners to simulate ink surface tension.
     */
    static QPolygonF generateCornerInkingPolygon(
        const QPointF &pPrev,
        const QPointF &pCurr,
        const QPointF &pNext,
        qreal strokeWidthPx
    );
};

#endif // KIS_AI_STROKE_QUALITY_UTILS_H
