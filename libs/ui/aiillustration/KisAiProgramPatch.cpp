/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiProgramPatch.h"

#include "KisAiStrokeProgram.h"

#include <QColor>
#include <QJsonDocument>
#include <QJsonParseError>

namespace
{
constexpr int MAX_PATCHES_PER_RESPONSE = 64;
constexpr int MAX_PATCH_JSON_BYTES = 512 * 1024;

bool rigKeyToField(const QString &key, KisAiSceneRigOverrides &rig, const QJsonValue &value)
{
    if (key == QLatin1String("eye_aperture")) {
        rig.eyeAperture = qBound<qreal>(0.0, value.toDouble(rig.eyeAperture), 1.0);
    } else if (key == QLatin1String("iris_ratio")) {
        rig.irisRatio = qBound<qreal>(0.35, value.toDouble(rig.irisRatio), 0.85);
    } else if (key == QLatin1String("eye_highlight")) {
        const QString v = value.toString();
        if (v == QLatin1String("twin_dot") || v == QLatin1String("streak") || v == QLatin1String("soft"))
            rig.eyeHighlight = v;
        else
            return false;
    } else if (key == QLatin1String("double_lid")) {
        rig.doubleLid = value.toBool(rig.doubleLid);
    } else if (key == QLatin1String("hair_strand_density")) {
        rig.hairStrandDensity = qBound<qreal>(0.0, value.toDouble(rig.hairStrandDensity), 1.0);
    } else if (key == QLatin1String("hair_flyaway")) {
        rig.hairFlyaway = qBound<qreal>(0.0, value.toDouble(rig.hairFlyaway), 1.0);
    } else if (key == QLatin1String("hair_highlight_bands")) {
        rig.hairHighlightBands = qBound(0, int(value.toDouble(rig.hairHighlightBands)), 3);
    } else if (key == QLatin1String("mouth_width_scale")) {
        rig.mouthWidthScale = qBound<qreal>(0.6, value.toDouble(rig.mouthWidthScale), 1.4);
    } else if (key == QLatin1String("has_brows")) {
        rig.hasBrows = value.toBool(rig.hasBrows);
    } else {
        return false;
    }
    return true;
}

bool parseOpFromJson(const QJsonObject &obj, KisAiStrokeOperation *out, QString *why)
{
    const QString kind = obj.value(QStringLiteral("kind")).toString().toLower();
    if (kind == QLatin1String("particles"))
        out->kind = KisAiStrokeOperation::Kind::Particles;
    else if (kind == QLatin1String("gradient_fill") || kind == QLatin1String("gradientfill"))
        out->kind = KisAiStrokeOperation::Kind::GradientFill;
    else if (kind == QLatin1String("fill"))
        out->kind = KisAiStrokeOperation::Kind::Fill;
    else if (kind == QLatin1String("ribbon"))
        out->kind = KisAiStrokeOperation::Kind::Ribbon;
    else if (kind == QLatin1String("path"))
        out->kind = KisAiStrokeOperation::Kind::Path;
    else {
        if (why) *why = QStringLiteral("non-decorative kind '%1'").arg(kind);
        return false;
    }

    out->id = obj.value(QStringLiteral("id")).toString();
    if (out->id.trimmed().isEmpty()) {
        if (why) *why = QStringLiteral("missing id");
        return false;
    }

    out->layer = obj.value(QStringLiteral("layer")).toString(QStringLiteral("FX"));

    // Colors
    const QString colorStr = obj.value(QStringLiteral("color")).toString();
    if (!colorStr.isEmpty())
        out->brush.color = KisAiStrokeProgramCodec::parseColor(colorStr, out->brush.color);
    out->brush.opacity = qBound<qreal>(0.0, obj.value(QStringLiteral("opacity")).toDouble(out->brush.opacity), 1.0);

    if (out->kind == KisAiStrokeOperation::Kind::Particles) {
        const QJsonObject b = obj.value(QStringLiteral("bounds")).toObject();
        const qreal x = b.value(QStringLiteral("x")).toDouble(0.0);
        const qreal y = b.value(QStringLiteral("y")).toDouble(0.0);
        const qreal w = qBound<qreal>(0.0, b.value(QStringLiteral("w")).toDouble(0.2), 1.0);
        const qreal h = qBound<qreal>(0.0, b.value(QStringLiteral("h")).toDouble(0.2), 1.0);
        out->bounds = QRectF(x, y, w, h);
        out->particleShape = obj.value(QStringLiteral("particle_shape")).toString(QStringLiteral("sparkle"));
        out->particleCount = qBound(1, obj.value(QStringLiteral("count")).toInt(12), 64);
    } else if (out->kind == KisAiStrokeOperation::Kind::GradientFill) {
        const QJsonObject c = obj.value(QStringLiteral("center")).toObject();
        out->isRadial = obj.value(QStringLiteral("radial")).toBool(true);
        out->gradientCenter = QPointF(qBound<qreal>(0.0, c.value(QStringLiteral("x")).toDouble(0.5), 1.0),
                                      qBound<qreal>(0.0, c.value(QStringLiteral("y")).toDouble(0.5), 1.0));
        out->gradientRadius = qBound<qreal>(0.02, obj.value(QStringLiteral("radius")).toDouble(0.2), 0.9);
        const QString c0 = obj.value(QStringLiteral("color_inner")).toString(colorStr);
        out->gradientColors = {
            KisAiStrokeProgramCodec::parseColor(c0, out->brush.color),
            QColor(out->brush.color.red(), out->brush.color.green(), out->brush.color.blue(), 0)
        };
    } else {
        if (why) *why = QStringLiteral("kind '%1' not patch-addable (needs spine/polygon geometry)").arg(kind);
        return false;
    }

    return true;
}
} // namespace

bool KisAiProgramPatchCodec::isDecorativeLayer(const QString &layer)
{
    const QString l = KisAiStrokeProgramCodec::normalizeLayerName(layer);
    return l == QLatin1String("FX") || l == QLatin1String("Highlights") || l == QLatin1String("Background");
}

bool KisAiProgramPatchCodec::isRigKey(const QString &key)
{
    static const QStringList keys = {
        QStringLiteral("eye_aperture"), QStringLiteral("iris_ratio"),
        QStringLiteral("eye_highlight"), QStringLiteral("double_lid"),
        QStringLiteral("hair_strand_density"), QStringLiteral("hair_flyaway"),
        QStringLiteral("hair_highlight_bands"), QStringLiteral("mouth_width_scale"),
        QStringLiteral("has_brows")
    };
    return keys.contains(key);
}

QString KisAiProgramPatchCodec::buildPatchSystemPrompt()
{
    return QStringLiteral(
        "You are a refinement patch engine for a deterministic painting program. "
        "You receive: the current canvas image, a machine-readable critique list, and the current rig parameters. "
        "Return a JSON object {\"patches\": [...]} with MINIMAL corrective patches. "
        "Never regenerate the whole artwork.\n"
        "Patch forms:\n"
        "  {\"op\": \"replace\", \"path\": \"/rig/<key>\", \"value\": <number|bool|string>}  — rig keys: "
        "eye_aperture, iris_ratio, eye_highlight, double_lid, hair_strand_density, hair_flyaway, "
        "hair_highlight_bands, mouth_width_scale, has_brows\n"
        "  {\"op\": \"add\", \"path\": \"/ops/add\", \"value\": {<decorative op>}} — only FX / Highlights / Background "
        "layers, kinds: particles, gradient_fill. Example: {\"kind\": \"particles\", \"id\": \"fx_sparkle\", "
        "\"layer\": \"FX\", \"shape\": \"sparkle\", \"count\": 10, \"bounds\": {\"x\": 0.1, \"y\": 0.1, \"w\": 0.2, \"h\": 0.2}, "
        "\"color\": \"#ffe9a8\", \"opacity\": 0.6}\n"
        "  {\"op\": \"replace\", \"path\": \"/ops/<id>/brush/opacity\", \"value\": <0..1>}\n"
        "  {\"op\": \"replace\", \"path\": \"/ops/<id>/brush/color\", \"value\": \"#rrggbb\"}\n"
        "  {\"op\": \"remove\", \"path\": \"/ops/<id>/remove\"} — decorative ops only\n"
        "Rules: at most 12 patches. Never touch Lineart, Flats or Shading structure. "
        "Never invent coordinates for strokes. If the critique is empty, return an empty patch list.");
}

QJsonObject KisAiProgramPatchCodec::buildPatchRequestPayload(
    const QString &model,
    const QString &prompt,
    const QString &imageBase64,
    const QJsonArray &critiqueRegions,
    const QJsonObject &currentRigState,
    const QString &customInstructions)
{
    QJsonObject userContent;
    userContent.insert(QStringLiteral("prompt"), prompt.trimmed());
    userContent.insert(QStringLiteral("critique_regions"), critiqueRegions);
    userContent.insert(QStringLiteral("current_rig"), currentRigState);

    QJsonArray messages;
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("content"), buildPatchSystemPrompt()}});

    if (!imageBase64.trimmed().isEmpty()) {
        QJsonArray imageUrl;
        imageUrl.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"), QStringLiteral("Current canvas state:")}});
        imageUrl.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                                    {QStringLiteral("image_url"),
                                     QJsonObject{{QStringLiteral("url"),
                                                  QStringLiteral("data:image/jpeg;base64,%1").arg(imageBase64)}}}});
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), imageUrl}});
        userContent.insert(QStringLiteral("canvas_attached"), true);
    } else {
        userContent.insert(QStringLiteral("canvas_attached"), false);
    }

    if (!customInstructions.trimmed().isEmpty())
        userContent.insert(QStringLiteral("additional_instructions"), customInstructions.trimmed());

    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(userContent).toJson(QJsonDocument::Compact))}});

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), model.trimmed());
    payload.insert(QStringLiteral("messages"), messages);
    payload.insert(QStringLiteral("temperature"), 0.3);
    payload.insert(QStringLiteral("max_tokens"), 1024);

    if (KisAiStrokeProgramCodec::supportsJsonSchema(model)) {
        QJsonObject schemaObj;
        schemaObj.insert(QStringLiteral("name"), QStringLiteral("program_patches"));
        schemaObj.insert(QStringLiteral("strict"), true);
        QJsonObject schema;
        schema.insert(QStringLiteral("type"), QStringLiteral("object"));
        QJsonObject patchItem;
        patchItem.insert(QStringLiteral("type"), QStringLiteral("object"));
        QJsonObject patchProps;
        patchProps.insert(QStringLiteral("op"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                            {QStringLiteral("enum"), QJsonArray{QStringLiteral("replace"), QStringLiteral("add"), QStringLiteral("remove")}}});
        patchProps.insert(QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
        patchProps.insert(QStringLiteral("value"), QJsonObject{});
        patchItem.insert(QStringLiteral("properties"), patchProps);
        patchItem.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("op"), QStringLiteral("path")});
        QJsonArray anyOf;
        anyOf.append(patchItem);
        QJsonObject items;
        items.insert(QStringLiteral("anyOf"), anyOf);
        QJsonObject patches;
        patches.insert(QStringLiteral("type"), QStringLiteral("array"));
        patches.insert(QStringLiteral("items"), patchItem);
        schema.insert(QStringLiteral("properties"), QJsonObject{{QStringLiteral("patches"), patches}});
        schema.insert(QStringLiteral("additionalProperties"), false);
        schemaObj.insert(QStringLiteral("schema"), schema);
        QJsonObject responseFormat;
        responseFormat.insert(QStringLiteral("type"), QStringLiteral("json_schema"));
        responseFormat.insert(QStringLiteral("json_schema"), schemaObj);
        payload.insert(QStringLiteral("response_format"), responseFormat);
    } else {
        QJsonObject responseFormat;
        responseFormat.insert(QStringLiteral("type"), QStringLiteral("json_object"));
        payload.insert(QStringLiteral("response_format"), responseFormat);
    }

    return payload;
}

bool KisAiProgramPatchCodec::parsePatches(
    const QByteArray &responseBytes,
    QVector<KisAiProgramPatch> *outPatches,
    QStringList *rejected,
    QString *errorMessage)
{
    if (!outPatches) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Null output patch list.");
        return false;
    }
    outPatches->clear();
    if (rejected)
        rejected->clear();

    if (responseBytes.size() > MAX_PATCH_JSON_BYTES) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Patch response exceeded the size limit.");
        return false;
    }

    const QString jsonText = KisAiStrokeProgramCodec::sanitizeAndExtractJson(
        QString::fromUtf8(responseBytes));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Patch response is not valid JSON: %1").arg(parseError.errorString());
        return false;
    }

    QJsonArray patchArray = doc.object().value(QStringLiteral("patches")).toArray();
    if (patchArray.isEmpty()) {
        // Tolerate a bare array response.
        patchArray = doc.array();
    }

    for (const QJsonValue &v : patchArray) {
        if (outPatches->size() >= MAX_PATCHES_PER_RESPONSE) {
            if (rejected)
                rejected->append(QStringLiteral("patch limit (%1) reached").arg(MAX_PATCHES_PER_RESPONSE));
            break;
        }
        const QJsonObject obj = v.toObject();
        const QString opStr = obj.value(QStringLiteral("op")).toString(QStringLiteral("replace")).toLower();
        const QString path = obj.value(QStringLiteral("path")).toString().trimmed();

        KisAiProgramPatch patch;
        if (opStr == QLatin1String("replace"))
            patch.op = KisAiProgramPatch::Op::Replace;
        else if (opStr == QLatin1String("add"))
            patch.op = KisAiProgramPatch::Op::Add;
        else if (opStr == QLatin1String("remove"))
            patch.op = KisAiProgramPatch::Op::Remove;
        else {
            if (rejected) rejected->append(QStringLiteral("unknown op '%1'").arg(opStr));
            continue;
        }
        patch.path = path;
        patch.value = obj.value(QStringLiteral("value"));

        // Path validation happens here (cheap) so applyPatches stays honest.
        bool valid = false;
        if (patch.path.startsWith(QLatin1String("/rig/"))) {
            valid = isRigKey(patch.path.mid(5));
        } else if (patch.path == QLatin1String("/ops/add")) {
            valid = patch.op == KisAiProgramPatch::Op::Add;
        } else if (patch.path.startsWith(QLatin1String("/ops/"))) {
            valid = true; // finer validation at apply time (needs the base program)
        } else {
            valid = false;
        }

        if (!valid) {
            if (rejected) rejected->append(QStringLiteral("rejected path '%1'").arg(patch.path));
            continue;
        }
        outPatches->append(patch);
    }

    if (errorMessage)
        errorMessage->clear();
    return true;
}

KisAiStrokeProgram KisAiProgramPatchCodec::applyPatches(
    const KisAiStrokeProgram &base,
    const QVector<KisAiProgramPatch> &patches,
    KisAiSceneRigOverrides *rigDelta,
    QStringList *rejected)
{
    KisAiStrokeProgram result = base;
    KisAiSceneRigOverrides localRig;
    const bool trackRig = rigDelta != nullptr;
    if (trackRig)
        *rigDelta = KisAiSceneRigOverrides();

    auto reject = [rejected](const QString &reason) {
        if (rejected)
            rejected->append(reason);
    };

    for (const KisAiProgramPatch &patch : patches) {
        if (patch.path.startsWith(QLatin1String("/rig/"))) {
            const QString key = patch.path.mid(5);
            if (patch.op != KisAiProgramPatch::Op::Replace) {
                reject(QStringLiteral("/rig/%1: only replace allowed").arg(key));
                continue;
            }
            if (!rigKeyToField(key, localRig, patch.value)) {
                reject(QStringLiteral("/rig/%1: bad value").arg(key));
                continue;
            }
            if (trackRig) {
                // Only record keys this patch round actually changed.
                if (key == QLatin1String("eye_aperture")) rigDelta->eyeAperture = localRig.eyeAperture;
                else if (key == QLatin1String("iris_ratio")) rigDelta->irisRatio = localRig.irisRatio;
                else if (key == QLatin1String("eye_highlight")) rigDelta->eyeHighlight = localRig.eyeHighlight;
                else if (key == QLatin1String("double_lid")) rigDelta->doubleLid = localRig.doubleLid;
                else if (key == QLatin1String("hair_strand_density")) rigDelta->hairStrandDensity = localRig.hairStrandDensity;
                else if (key == QLatin1String("hair_flyaway")) rigDelta->hairFlyaway = localRig.hairFlyaway;
                else if (key == QLatin1String("hair_highlight_bands")) rigDelta->hairHighlightBands = localRig.hairHighlightBands;
                else if (key == QLatin1String("mouth_width_scale")) rigDelta->mouthWidthScale = localRig.mouthWidthScale;
                else if (key == QLatin1String("has_brows")) rigDelta->hasBrows = localRig.hasBrows;
            }
            continue;
        }

        if (patch.path == QLatin1String("/ops/add")) {
            KisAiStrokeOperation add;
            QString why;
            if (!parseOpFromJson(patch.value.toObject(), &add, &why)) {
                reject(QStringLiteral("/ops/add: %1").arg(why));
                continue;
            }
            if (!isDecorativeLayer(add.layer)) {
                reject(QStringLiteral("/ops/add: layer '%1' not decorative").arg(add.layer));
                continue;
            }
            // Dedup ids keep apply idempotent-ish and logs clean.
            bool exists = false;
            for (const KisAiStrokeOperation &op : result.operations) {
                if (op.id == add.id) { exists = true; break; }
            }
            if (exists) {
                reject(QStringLiteral("/ops/add: id '%1' already exists").arg(add.id));
                continue;
            }
            result.operations.append(add);
            continue;
        }

        // /ops/<id>/... adjustments
        const QString rest = patch.path.mid(5); // after "/ops/"
        const int slash = rest.indexOf(QLatin1Char('/'));
        const QString opId = slash > 0 ? rest.left(slash) : rest;
        const QString tail = slash > 0 ? rest.mid(slash + 1) : QString();

        int index = -1;
        for (int i = 0; i < result.operations.size(); ++i) {
            if (result.operations.at(i).id == opId) { index = i; break; }
        }
        if (index < 0) {
            reject(QStringLiteral("%1: op id not found").arg(patch.path));
            continue;
        }

        if (patch.op == KisAiProgramPatch::Op::Remove) {
            if (!isDecorativeLayer(result.operations.at(index).layer)
                && result.operations.at(index).kind != KisAiStrokeOperation::Kind::Particles) {
                reject(QStringLiteral("%1: only decorative ops removable").arg(patch.path));
                continue;
            }
            result.operations.removeAt(index);
            continue;
        }

        if (tail == QLatin1String("brush/opacity")) {
            result.operations[index].brush.opacity =
                qBound<qreal>(0.0, patch.value.toDouble(result.operations.at(index).brush.opacity), 1.0);
        } else if (tail == QLatin1String("brush/color")) {
            const QColor c = KisAiStrokeProgramCodec::parseColor(patch.value.toString(),
                                                                 result.operations.at(index).brush.color);
            result.operations[index].brush.color = c;
        } else {
            reject(QStringLiteral("%1: unsupported subpath").arg(patch.path));
        }
    }

    return result;
}

QJsonArray KisAiProgramPatchCodec::patchesToJson(const QVector<KisAiProgramPatch> &patches)
{
    QJsonArray arr;
    for (const KisAiProgramPatch &p : patches) {
        QJsonObject o;
        o.insert(QStringLiteral("op"),
                 p.op == KisAiProgramPatch::Op::Add ? QStringLiteral("add")
                 : p.op == KisAiProgramPatch::Op::Remove ? QStringLiteral("remove")
                 : QStringLiteral("replace"));
        o.insert(QStringLiteral("path"), p.path);
        o.insert(QStringLiteral("value"), p.value);
        arr.append(o);
    }
    return arr;
}
