/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_PROGRAM_PATCH_H
#define KIS_AI_PROGRAM_PATCH_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiSceneSpec.h"
#include "KisAiStrokeProgram.h"

/**
 * V5 R3: Patch-based refinement.
 *
 * Instead of regenerating the whole stroke program each Goal Mode step, the
 * LLM returns a minimal list of patches. Geometry-structural layers stay
 * frozen; only rig parameters and whitelisted decorative ops are tunable.
 *
 * Patch paths:
 *   "/rig/eye_aperture"            → SceneSpec rig override (applied to the
 *                                    spec, then the program is re-laid-out)
 *   "/rig/<any SceneRigOverrides>” → same
 *   "/ops/add"                     → append a decorative operation (whitelist)
 *   "/ops/<id>/brush/opacity"      → adjust one existing op's opacity
 *   "/ops/<id>/brush/color"        → adjust one existing op's color
 *   "/ops/<id>/remove"             → remove one decorative op (whitelist)
 */
struct KRITAUI_EXPORT KisAiProgramPatch
{
    enum class Op {
        Replace,   ///< Set value at path.
        Add,       ///< Append decorative op (path "/ops/add").
        Remove     ///< Delete op by id (path "/ops/<id>/remove").
    };

    Op op {Op::Replace};
    QString path;
    QJsonValue value;
};

class KRITAUI_EXPORT KisAiProgramPatchCodec
{
public:
    /** Whitelisted decorative kinds for /ops/add (FX, Highlights, Background polish). */
    static bool isDecorativeLayer(const QString &layer);

    /** Whitelisted rig keys for /rig/... patches. */
    static bool isRigKey(const QString &key);

    /**
     * Build the Chat Completions payload requesting patches: current composite
     * image, the critique regions and the current rig parameter snapshot.
     */
    static QJsonObject buildPatchRequestPayload(
        const QString &model,
        const QString &prompt,
        const QString &imageBase64,
        const QJsonArray &critiqueRegions,
        const QJsonObject &currentRigState,
        const QString &customInstructions = QString());

    /** The system prompt that teaches the patch JSON contract. */
    static QString buildPatchSystemPrompt();

    /**
     * Parse a model response into patches. Accepts the full response body
     * (markdown fences / think tokens tolerated via sanitizeAndExtractJson).
     * Patches are individually validated; unknown paths are reported in
     * @p rejected and dropped.
     */
    static bool parsePatches(
        const QByteArray &responseBytes,
        QVector<KisAiProgramPatch> *outPatches,
        QStringList *rejected = nullptr,
        QString *errorMessage = nullptr);

    /**
     * Apply patches to a program. Structural safety:
     * - /rig/... patches are collected into a SceneRigOverrides delta (returned
     *   via @p rigDelta) and never touch existing ops directly.
     * - /ops/add only accepts decorative layers (FX, Highlights, Background).
     * - /ops/<id>/{opacity,color} only adjust brush values, never geometry.
     * - /ops/<id>/remove only removes decorative ops.
     * Rejected patches are listed with reasons. The base program is never
     * modified in place.
     */
    static KisAiStrokeProgram applyPatches(
        const KisAiStrokeProgram &base,
        const QVector<KisAiProgramPatch> &patches,
        KisAiSceneRigOverrides *rigDelta = nullptr,
        QStringList *rejected = nullptr);

    /** Serialize patches back to JSON (round-trip tests, telemetry). */
    static QJsonArray patchesToJson(const QVector<KisAiProgramPatch> &patches);
};

#endif // KIS_AI_PROGRAM_PATCH_H
