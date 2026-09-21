/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_SCENE_SPEC_H
#define KIS_AI_SCENE_SPEC_H

#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

struct KRITAUI_EXPORT KisAiSceneSubject
{
    QString type {QStringLiteral("character")}; // character, landscape, creature, object
    QString poseId {QStringLiteral("three_quarter_bust")};
    QString facing {QStringLiteral("front")}; // front, front-right, front-left, profile
};

struct KRITAUI_EXPORT KisAiSceneHead
{
    QString expression {QStringLiteral("smile_open")}; // smile_open, smile_closed, neutral, half, closed, wink_left, wink_right, blush_shy, confident_smug
    QString gaze {QStringLiteral("front")}; // front, left, right, up
    QString hairStyle {QStringLiteral("long_hime")}; // long_hime, long_wavy, bob, twin_tails, short_messy, short_straight, pony_tail, half_up, wolf_cut, braided
    QString hairBangs {QStringLiteral("m_fringe")}; // m_fringe, straight_cut, swept_left, swept_right, see_through, blunt_bangs, center_part
    QColor hairColor {QColor(43, 58, 103)};
    QColor eyeColor {QColor(59, 130, 246)};
    QColor skinTone {QColor(255, 224, 192)};
    // V10 Fine-Grained Anatomy Handles
    qreal hairVolume {0.60}; // [0.0, 1.0]
    qreal hairFlyaway {0.35}; // [0.0, 1.0]
    qreal blushIntensity {0.50}; // [0.0, 1.0]
    QString eyeHighlightStyle {QStringLiteral("twin_dot")}; // twin_dot, radiant_sparkle, soft_diffuse, crescent
};

struct KRITAUI_EXPORT KisAiSceneClothing
{
    QString style {QStringLiteral("school_uniform")}; // school_uniform, sailor, hoodie, casual, dress, kimono
    QColor color {QColor(40, 48, 72)}; // primary clothing color
    QColor secondaryColor {QColor(245, 245, 250)}; // collar, trim, inner
    QColor accentColor {QColor(220, 50, 70)}; // ribbon, tie, accents
};

struct KRITAUI_EXPORT KisAiSceneComposition
{
    QString framing {QStringLiteral("bust_up")}; // face_closeup, bust_up, upper_body, full_body, wide
    QPointF headCenter {0.5, 0.38};
    qreal headHeight {0.42};
    QString depth {QStringLiteral("shallow")}; // flat, shallow, deep
};

struct KRITAUI_EXPORT KisAiScenePalette
{
    QString mood {QStringLiteral("soft_daylight")};
    QColor keyColor {QColor(100, 116, 139)};
    QVector<QColor> accents;
};

struct KRITAUI_EXPORT KisAiSceneLight
{
    QPointF direction {-0.5, -0.7}; // normalized-ish key-light vector (points toward the light)
    QString warmth {QStringLiteral("warm_key_cool_fill")};
    QString timeOfDay {QStringLiteral("day")}; // day, sunset, night
    // V10 Lighting Direction Handles
    qreal rimIntensity {0.40}; // [0.0, 1.0]
    qreal sssStrength {0.50}; // [0.0, 1.0]
    QString lightingStyle {QStringLiteral("soft_studio")}; // soft_studio, dramatic_backlight, komorebi_dappled, sunset_golden, neon_rim
};

struct KRITAUI_EXPORT KisAiSceneBackground
{
    QString type {QStringLiteral("simple_gradient")}; // simple_gradient, night_sky_town, sky_meadow, interior, abstract
    QStringList elements;
    QStringList forbid;
};

struct KRITAUI_EXPORT KisAiSceneNegative
{
    bool noParticlesOnFace {true};
    bool noText {true};
    bool noExtraLimbs {true};
};

/**
 * V10 Post-Processing Finish Directives.
 */
struct KRITAUI_EXPORT KisAiSceneFinishV3
{
    qreal bloomStrength {0.20}; // [0.0, 1.0]
    qreal grainIntensity {0.05}; // [0.0, 1.0]
    qreal vignetteStrength {0.15}; // [0.0, 1.0]
    QString toneMood {QStringLiteral("anime_vibrant")}; // anime_vibrant, cinematic_warm, pastel_dreamy, dark_noir
};

/**
 * V5 R1: SceneSpec v2 vocabulary — meaning-only fields that widen what the
 * flagship LLM can direct without ever touching geometry.
 * All fields are optional; missing blocks fall back to v3 defaults so old
 * SceneSpec JSON keeps parsing identically.
 */
struct KRITAUI_EXPORT KisAiSceneStyleV2
{
    QString artStyleId {QStringLiteral("anime_cel")}; // anime_cel, watercolor, impasto, ink_sketch, cyber_neon, fine_line
    QStringList customTags;   // Free-form art direction words (dictionary-matched downstream)
    QString lineWeight {QStringLiteral("standard")}; // delicate, standard, bold
    qreal detailLevel {0.6};  // [0,1] drives per-part detail budget in the LayoutEngine
};

struct KRITAUI_EXPORT KisAiSceneCameraV2
{
    QString focal {QStringLiteral("normal")}; // short, normal, long (head/body proportion feel)
    QString tilt {QStringLiteral("level")};   // level, high_angle, low_angle
};

struct KRITAUI_EXPORT KisAiSceneColorScriptV2
{
    QColor shadow {QColor(0, 0, 0, 0)};   // Invalid = derive from KisAiLightRig
    QColor midtone {QColor(0, 0, 0, 0)};
    QColor highlight {QColor(0, 0, 0, 0)};
    qreal accentWeight {0.25}; // intended accent area ratio [0,1]
};

struct KRITAUI_EXPORT KisAiSceneNarrativeV2
{
    QString time;        // Free-form (e.g. "golden_hour"); mapped onto timeOfDay
    QString weather;     // clear, cloudy, rain, snow (BackdropRig slots)
    QStringList props;   // Resolved into background element slots by BackdropRig
};

/**
 * V5 R2: LLM-tunable rig parameters. Only known keys survive validation;
 * the RigLibrary clamps every value into its invariant-safe range.
 */
struct KRITAUI_EXPORT KisAiSceneRigOverrides
{
    qreal eyeAperture {0.85};        // [0,1] 0 = closed, 1 = wide
    qreal irisRatio {0.62};          // [0.35,0.85] iris / eye height
    QString eyeHighlight {QString()}; // twin_dot, streak, soft, radiant_sparkle, crescent
    bool doubleLid {true};
    qreal hairStrandDensity {0.55};  // [0,1]
    qreal hairFlyaway {0.35};        // [0,1]
    int hairHighlightBands {1};      // [0,3] main/sub/counter light bands
    qreal mouthWidthScale {1.0};     // [0.6,1.4]
    bool hasBrows {true};
};

/**
 * V5 R5: deterministic N-best score for a candidate SceneSpec.
 */
struct KRITAUI_EXPORT KisAiSceneSpecScore
{
    qreal total {0.0};          // [0,1]
    qreal paletteHarmony {0.0}; // [0,1]
    qreal rigFeasibility {0.0}; // 1 - clampedRatio
    qreal intentMatch {0.0};    // prompt adherence via keyword overlap
    qreal negativeCompliance {1.0};
    QStringList notes;
};

/**
 * V3 Phase 1 / V4: Meaning-only art direction (no coordinates).
 * The LLM decides WHAT/WHERE IN WORDS; the LayoutEngine owns geometry.
 */
struct KRITAUI_EXPORT KisAiSceneSpec
{
    QString prompt;
    QSize canvasSize {1024, 1024};
    KisAiSceneSubject subject;
    KisAiSceneHead head;
    KisAiSceneClothing clothing;
    KisAiSceneComposition composition;
    KisAiScenePalette palette;
    KisAiSceneLight light;
    KisAiSceneBackground background;
    KisAiSceneNegative negative;

    // V5 R1/R2 additions (backward compatible: defaults keep old behaviour)
    KisAiSceneStyleV2 style;
    KisAiSceneCameraV2 camera;
    KisAiSceneColorScriptV2 colorScript;
    KisAiSceneNarrativeV2 narrative;
    KisAiSceneRigOverrides rig;

    // V10 additions
    KisAiSceneFinishV3 finish;

    bool isCharacter() const { return subject.type == QLatin1String("character"); }
};

/**
 * Codec for the SceneSpec contract: schema, parsing, payload, canonical examples.
 */
class KRITAUI_EXPORT KisAiSceneSpecCodec
{
public:
    static QJsonObject sceneSpecJsonSchema();

    /**
     * V5 R5: deterministic N-best scorer for candidate SceneSpecs.
     * Higher is better; all sub-scores are in [0,1] and independent of wall-clock.
     */
    static KisAiSceneSpecScore scoreSceneSpec(
        const KisAiSceneSpec &spec,
        const QStringList &candidateSpecs = QStringList());

    /**
     * V5 R5: pick the best candidate by deterministic score. Returns fallback
     * (or a default spec for the prompt) when candidates is empty.
     */
    static KisAiSceneSpec selectBestSpec(
        const QString &prompt,
        const QSize &canvasSize,
        const QVector<KisAiSceneSpec> &candidates);

    static bool parseSceneSpec(
        const QByteArray &responseBytes,
        KisAiSceneSpec *outSpec,
        QString *errorMessage = nullptr,
        QStringList *warnings = nullptr
    );

    static bool parseSceneSpecObject(
        const QJsonObject &rootObj,
        KisAiSceneSpec *outSpec,
        QStringList *warnings = nullptr
    );

    static QJsonObject buildSceneSpecPayload(
        const QString &model,
        const QString &prompt,
        const QSize &canvasSize,
        int artStyle = 0,
        const QString &reasoningEffort = QString(),
        const QString &customInstructions = QString(),
        bool enableStreaming = true,
        bool enforceJsonFormat = true,
        qreal temperature = 0.5,
        qreal topP = 1.0,
        int maxTokensOverride = 0,
        bool forceJsonObjectOnly = false
    );

    /**
     * Spec-level few-shot: beautiful canonical VALUES (not coordinates).
     * The model only nudges numbers instead of inventing geometry.
     */
    static QString canonicalSpecExample(const QString &domain);

    /**
     * Keyword-derived fallback spec so offline / error paths still paint
     * something principled without any LLM call.
     */
    static KisAiSceneSpec defaultSpecForPrompt(
        const QString &prompt,
        const QSize &canvasSize
    );
};

#endif // KIS_AI_SCENE_SPEC_H
