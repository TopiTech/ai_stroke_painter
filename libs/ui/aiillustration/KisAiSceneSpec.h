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
    QString expression {QStringLiteral("smile_open")}; // smile_open, smile_closed, neutral, half, closed
    QString gaze {QStringLiteral("front")}; // front, left, right, up
    QString hairStyle {QStringLiteral("long_hime")}; // long_hime, long_wavy, bob, twin_tails, short_messy, short_straight
    QColor hairColor {QColor(43, 58, 103)};
    QColor eyeColor {QColor(59, 130, 246)};
    QColor skinTone {QColor(255, 224, 192)};
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
 * V3 Phase 1: Meaning-only art direction (no coordinates).
 * The LLM decides WHAT/WHERE IN WORDS; the LayoutEngine owns geometry.
 */
struct KRITAUI_EXPORT KisAiSceneSpec
{
    QString prompt;
    QSize canvasSize {1024, 1024};
    KisAiSceneSubject subject;
    KisAiSceneHead head;
    KisAiSceneComposition composition;
    KisAiScenePalette palette;
    KisAiSceneLight light;
    KisAiSceneBackground background;
    KisAiSceneNegative negative;

    bool isCharacter() const { return subject.type == QLatin1String("character"); }
};

/**
 * Codec for the SceneSpec contract: schema, parsing, payload, canonical examples.
 */
class KRITAUI_EXPORT KisAiSceneSpecCodec
{
public:
    static QJsonObject sceneSpecJsonSchema();

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
        int artStyle = 0
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
