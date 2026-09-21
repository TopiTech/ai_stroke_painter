/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_RIG_LIBRARY_H
#define KIS_AI_RIG_LIBRARY_H

#include <QColor>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiSceneSpec.h"
#include "KisAiStrokeProgram.h"

/**
 * V5 R2: Rig DSL — parameter structs the LLM may tune, plus pure generators
 * that turn parameters into KisAiStrokeOperations with invariant guarantees
 * (symmetry, containment, clamped ranges, deterministic seeds).
 *
 * The LLM never emits coordinates through this path; it only fills the
 * parameter structs (via SceneSpec "rig" block). All geometry decisions that
 * would break a canonical anime look stay in code.
 */
struct KRITAUI_EXPORT KisAiEyeRigParams {
    qreal aperture{0.85}; // [0,1] 0 = closed, 1 = wide
    qreal irisRatio{0.62}; // [0.35,0.85] iris / eye height
    QString highlightShape; // twin_dot, streak, soft (empty = auto)
    bool doubleLid{true};
    QString gaze; // front, left, right, up
    QString expression; // open, smile, half, closed
};

struct KRITAUI_EXPORT KisAiBrowRigParams {
    bool enabled{true};
    qreal thicknessScale{1.0}; // [0.5,1.6]
    qreal angleScale{1.0}; // [0.4,1.6] >1 = more expressive arch
};

struct KRITAUI_EXPORT KisAiNoseRigParams {
    bool enabled{true};
    qreal shadowStrength{0.35}; // [0.1,0.6] — solid black is forbidden by design
};

struct KRITAUI_EXPORT KisAiMouthRigParams {
    QString expression; // smile, open_smile, small_open, closed_line, cat_mouth, pout
    qreal widthScale{1.0}; // [0.6,1.4]
    bool highlight{true};
};

struct KRITAUI_EXPORT KisAiHairRigParams {
    qreal strandDensity{0.55}; // [0,1]
    qreal flyaway{0.35}; // [0,1]
    int highlightBands{1}; // [0,3] main / sub / counter-light
};

struct KRITAUI_EXPORT KisAiBackdropRigParams {
    QString timeOfDay{QStringLiteral("day")}; // day, sunset, night
    QString weather{QStringLiteral("clear")}; // clear, cloudy, rain, snow
    QStringList props; // resolved into element slots (lamp, window_light, ...)
};

struct KRITAUI_EXPORT KisAiRigParameterSet {
    // Head geometry anchor (normalized canvas coordinates).
    QPointF headCenter{0.5, 0.38};
    qreal headWidth{0.33};
    qreal headHeight{0.42};
    // Facing id driving symmetric eye placement (front, front-right,
    // front-left, profile). Mirrors KisAiSceneSubject::facing vocabulary.
    QString facing{QStringLiteral("front")};

    KisAiEyeRigParams eyeLeft;
    KisAiEyeRigParams eyeRight;
    KisAiBrowRigParams brow;
    KisAiNoseRigParams nose;
    KisAiMouthRigParams mouth;
    KisAiHairRigParams hair;
    KisAiBackdropRigParams backdrop;

    // Colors derived from the spec (LightRig remains the single truth source).
    QColor hairColor{QColor(43, 58, 103)};
    QColor eyeColor{QColor(59, 130, 246)};
    QColor skinTone{QColor(255, 224, 192)};
    QColor lineColor{QColor(35, 35, 45)};
    QString lineWeight{QStringLiteral("standard")};

    // V7: Contrapposto & Upper-body Pose Dynamics
    qreal headTiltDeg{0.0}; // [-15, 15] head tilt in degrees
    qreal shoulderSlope{0.0}; // [-0.08, 0.08] vertical offset between shoulders
    qreal torsoTurn{0.0}; // [-0.10, 0.10] torso turn angle / depth shift
};

/**
 * V6 W1: canonical eye placement shared by the Layout engine and
 * accessory ornaments. Same math as the internal anchor helper so
 * Rig-driven eyes and lid-shadow/tear-trough ornaments coincide.
 */
struct KRITAUI_EXPORT KisAiEyePlacement {
    QPointF center;
    QSizeF size;
};

class KRITAUI_EXPORT KisAiRigLibrary
{
public:
    /**
     * Build a full parameter set from a SceneSpec. Applies the spec.rig
     * overrides on top of canonical defaults and clamps everything into
     * invariant-safe ranges (the clamp step is what keeps bad LLM output
     * harmless instead of broken).
     */
    static KisAiRigParameterSet parametersFromSpec(const KisAiSceneSpec &spec);

    /** Clamp-only entry point (used by patch application on tuned sets). */
    static KisAiRigParameterSet clamped(const KisAiRigParameterSet &params);

    /**
     * V6 W1: canonical eye pair placement (center + size) with facing
     * compensation applied.
     */
    static QPair<KisAiEyePlacement, KisAiEyePlacement> eyePlacements(const KisAiRigParameterSet &params);

    /**
     * V6 W1: apply SceneSpec camera hints (focal/tilt) as small clamped
     * head-geometry nudges. Call clamped() afterwards.
     */
    static void applyCameraAdjust(KisAiRigParameterSet &params, const KisAiSceneCameraV2 &camera);

    /**
     * V6 W1: apply style.detailLevel ornament budget. Low detail trims
     * highlight bands to at most one.
     */
    static void applyDetailBudget(KisAiRigParameterSet &params, qreal detailLevel);

    /**
     * Eye pair operations. Both eyes derive from one parameter struct family,
     * so symmetry is guaranteed by construction; aperture/iris/highlight are
     * mirrored deterministically.
     */
    static QVector<KisAiStrokeOperation> eyePairOps(const KisAiRigParameterSet &params, quint32 seed = 42);

    /**
     * V11 Lineart Mode: Pristine inking-only eye pair assembly without solid colored fills.
     * Generates upper lash with outer flick, double eyelid, uncolored iris contour,
     * pupil core, circular catchlight boundaries, and lower lid accents for coloring book readiness.
     */
    static QVector<KisAiStrokeOperation> eyePairLineartOps(const KisAiRigParameterSet &params, quint32 seed = 42);

    /** Double-lid crease lines (Lineart) for both eyes, if enabled. */
    static QVector<KisAiStrokeOperation> doubleLidOps(const KisAiRigParameterSet &params);

    /** Brows: tapered expressive strokes mirrored on both sides. */
    static QVector<KisAiStrokeOperation> browOps(const KisAiRigParameterSet &params);

    /** Nose: point + short shadow only (solid-black noses are lint-forbidden). */
    static QVector<KisAiStrokeOperation> noseOps(const KisAiRigParameterSet &params);

    /** Mouth assembly backed by the procedural AnimeMouth renderer. */
    static QVector<KisAiStrokeOperation> mouthOps(const KisAiRigParameterSet &params);

    /**
     * Hair highlight bands along the cranial dome: main / sub / counter
     * light ribbons. Band count comes from params.hair.highlightBands.
     */
    static QVector<KisAiStrokeOperation> hairHighlightOps(const KisAiRigParameterSet &params, quint32 seed = 42);

    /**
     * Weather + prop slots layered over the base background gradient:
     * rain streaks / snow dots / cloud masses / warm lamp bokeh.
     * Deterministic via seed. Never overlaps the face box.
     */
    static QVector<KisAiStrokeOperation>
    backdropWeatherOps(const KisAiRigParameterSet &params, const QSize &canvasSize, quint32 seed = 42);

    /** Map free-form narrative.time onto the canonical timeOfDay enum. */
    static QString narrativeTimeToTimeOfDay(const QString &narrativeTime, const QString &fallback);

    /**
     * Mountain rig (e.g. Mount Fuji / distant peaks):
     * Graceful exponential ridge curve, snow cap with fractal snowmelt ridges,
     * directional facet shading, and atmospheric haze wash.
     */
    static QVector<KisAiStrokeOperation>
    mountainOps(const KisAiSceneSpec &spec, const QSize &canvasSize, quint32 seed = 42);

    /**
     * Sakura tree rig:
     * Natural skeletal branching (trunk, primary and secondary branches),
     * layered organic watercolor petal clusters, and wind-blown floating petals.
     */
    static QVector<KisAiStrokeOperation>
    sakuraTreeOps(const KisAiSceneSpec &spec, const QSize &canvasSize, quint32 seed = 42);

    /**
     * Water surface rig:
     * Atmospheric aerial gradient wash, inverted soft reflection of sky and mountain,
     * and perspective-spaced ripple specular highlights.
     */
    static QVector<KisAiStrokeOperation>
    waterSurfaceOps(const KisAiSceneSpec &spec, const QSize &canvasSize, qreal horizonY = 0.62, quint32 seed = 42);

    /**
     * V7 Bezier Head Outline Generator:
     * High-continuity cubic Bezier curvature for smooth cheeks, defined jawline,
     * and refined chin contour, with optional tilt rotation.
     */
    static QPolygonF headOutlineBezier(
        const QPointF &headCenter,
        qreal headWidth,
        qreal headHeight,
        qreal tiltDeg = 0.0
    );

    /**
     * V7 Hierarchical 3D Hair Clump Dynamics:
     * Synthesizes volumetric ribbon hair strands with S-curve bezier flows,
     * sharp tapered tips, and ambient occlusion cast shadows.
     */
    static QVector<KisAiStrokeOperation> hierarchicalHairClumpOps(
        const KisAiRigParameterSet &params,
        const QSize &canvasSize,
        quint32 seed = 42
    );

    /**
     * V7 Procedural Drapery & Tension Folds:
     * Synthesizes clothing tension folds between anchor points (e.g. shoulders, neck, waist)
     * with delicate lineart and soft fold shading.
     * idSuffix が空でない場合は op id に付与し、同一プログラム内の重複を避ける。
     */
    static QVector<KisAiStrokeOperation> draperyFoldOps(
        const QPointF &origin,
        const QPointF &target,
        qreal widthPx,
        const QColor &clothColor,
        const QColor &shadowColor,
        const QString &idSuffix = QString());

    /** Stable sub-seed for a named rig part (deterministic across sessions). */
    static quint32 partSeed(const QString &partName, quint32 baseSeed);
};

#endif // KIS_AI_RIG_LIBRARY_H
