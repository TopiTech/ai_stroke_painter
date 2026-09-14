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
struct KRITAUI_EXPORT KisAiEyeRigParams
{
    qreal aperture {0.85};           // [0,1] 0 = closed, 1 = wide
    qreal irisRatio {0.62};          // [0.35,0.85] iris / eye height
    QString highlightShape;          // twin_dot, streak, soft (empty = auto)
    bool doubleLid {true};
    QString gaze;                    // front, left, right, up
    QString expression;              // open, smile, half, closed
};

struct KRITAUI_EXPORT KisAiBrowRigParams
{
    bool enabled {true};
    qreal thicknessScale {1.0};      // [0.5,1.6]
    qreal angleScale {1.0};          // [0.4,1.6] >1 = more expressive arch
};

struct KRITAUI_EXPORT KisAiNoseRigParams
{
    bool enabled {true};
    qreal shadowStrength {0.35};     // [0.1,0.6] — solid black is forbidden by design
};

struct KRITAUI_EXPORT KisAiMouthRigParams
{
    QString expression;              // smile, open_smile, small_open, closed_line, cat_mouth, pout
    qreal widthScale {1.0};          // [0.6,1.4]
    bool highlight {true};
};

struct KRITAUI_EXPORT KisAiHairRigParams
{
    qreal strandDensity {0.55};      // [0,1]
    qreal flyaway {0.35};            // [0,1]
    int highlightBands {1};          // [0,3] main / sub / counter-light
};

struct KRITAUI_EXPORT KisAiBackdropRigParams
{
    QString timeOfDay {QStringLiteral("day")}; // day, sunset, night
    QString weather {QStringLiteral("clear")}; // clear, cloudy, rain, snow
    QStringList props;               // resolved into element slots (lamp, window_light, ...)
};

struct KRITAUI_EXPORT KisAiRigParameterSet
{
    // Head geometry anchor (normalized canvas coordinates).
    QPointF headCenter {0.5, 0.38};
    qreal headWidth {0.33};
    qreal headHeight {0.42};

    KisAiEyeRigParams eyeLeft;
    KisAiEyeRigParams eyeRight;
    KisAiBrowRigParams brow;
    KisAiNoseRigParams nose;
    KisAiMouthRigParams mouth;
    KisAiHairRigParams hair;
    KisAiBackdropRigParams backdrop;

    // Colors derived from the spec (LightRig remains the single truth source).
    QColor hairColor {QColor(43, 58, 103)};
    QColor eyeColor {QColor(59, 130, 246)};
    QColor skinTone {QColor(255, 224, 192)};
    QColor lineColor {QColor(35, 35, 45)};
    QString lineWeight {QStringLiteral("standard")};
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
     * Eye pair operations. Both eyes derive from one parameter struct family,
     * so symmetry is guaranteed by construction; aperture/iris/highlight are
     * mirrored deterministically.
     */
    static QVector<KisAiStrokeOperation> eyePairOps(
        const KisAiRigParameterSet &params, quint32 seed = 42);

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
    static QVector<KisAiStrokeOperation> hairHighlightOps(
        const KisAiRigParameterSet &params, quint32 seed = 42);

    /**
     * Weather + prop slots layered over the base background gradient:
     * rain streaks / snow dots / cloud masses / warm lamp bokeh.
     * Deterministic via seed. Never overlaps the face box.
     */
    static QVector<KisAiStrokeOperation> backdropWeatherOps(
        const KisAiRigParameterSet &params, const QSize &canvasSize, quint32 seed = 42);

    /** Map free-form narrative.time onto the canonical timeOfDay enum. */
    static QString narrativeTimeToTimeOfDay(const QString &narrativeTime, const QString &fallback);

    /** Stable sub-seed for a named rig part (deterministic across sessions). */
    static quint32 partSeed(const QString &partName, quint32 baseSeed);
};

#endif // KIS_AI_RIG_LIBRARY_H
