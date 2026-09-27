/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiFullStrokeScene.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
bool validId(const QString &s)
{
    if (s.isEmpty() || s.size() > 64)
        return false;
    for (QChar c : s) {
        const ushort ch = c.unicode();
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'
              || ch == '-'))
            return false;
    }
    return true;
}

QString ownerId(const KisAiStrokeOperation &op)
{
    return op.groupId.isEmpty() ? QString() : op.groupId;
}
} // namespace

bool KisAiFullStrokeScene::parse(const QJsonObject &json, KisAiFullStrokeScene *out, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("missing output"));
    const QJsonValue source = json.value(QStringLiteral("objects"));
    if (!source.isArray())
        return fail(QStringLiteral("objects must be an array"));
    const QJsonArray entries = source.toArray();
    if (entries.isEmpty() || entries.size() > 64)
        return fail(QStringLiteral("invalid object count"));
    KisAiFullStrokeScene candidate;
    QSet<QString> ids;
    for (const QJsonValue &value : entries) {
        if (!value.isObject())
            return fail(QStringLiteral("object must be a map"));
        const QJsonObject item = value.toObject();
        KisAiFullStrokeObject object;
        object.id = item.value(QStringLiteral("id")).toString();
        object.type = item.value(QStringLiteral("type")).toString();
        object.parentId = item.value(QStringLiteral("parent_id")).toString();
        object.behindId = item.value(QStringLiteral("behind_id")).toString();
        object.focalWeight = item.value(QStringLiteral("focal_weight")).toDouble(0.5);
        object.required = item.value(QStringLiteral("required")).toBool(true);
        const QJsonValue boundsValue = item.value(QStringLiteral("bounds"));
        if (!validId(object.id) || ids.contains(object.id) || object.type.isEmpty() || object.type.size() > 64
            || !boundsValue.isArray() || boundsValue.toArray().size() != 4 || !std::isfinite(object.focalWeight)
            || object.focalWeight < 0.0 || object.focalWeight > 1.0)
            return fail(QStringLiteral("invalid object fields"));
        const QJsonArray coords = boundsValue.toArray();
        for (const QJsonValue &v : coords) {
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < 0.0 || v.toDouble() > 1.0)
                return fail(QStringLiteral("invalid bounds"));
        }
        object.bounds =
            QRectF(coords.at(0).toDouble(), coords.at(1).toDouble(), coords.at(2).toDouble(), coords.at(3).toDouble());
        if (object.bounds.isEmpty() || object.bounds.right() > 1.0 || object.bounds.bottom() > 1.0)
            return fail(QStringLiteral("bounds outside canvas"));
        ids.insert(object.id);
        candidate.objects.append(object);
    }
    QHash<QString, QString> parent;
    for (const KisAiFullStrokeObject &object : candidate.objects) {
        if ((!object.parentId.isEmpty() && (!ids.contains(object.parentId) || object.parentId == object.id))
            || (!object.behindId.isEmpty() && (!ids.contains(object.behindId) || object.behindId == object.id)))
            return fail(QStringLiteral("unknown or self dependency"));
        parent.insert(object.id, object.parentId);
    }
    for (const KisAiFullStrokeObject &object : candidate.objects) {
        QSet<QString> seen;
        QString id = object.id;
        while (!id.isEmpty()) {
            if (seen.contains(id))
                return fail(QStringLiteral("parent cycle"));
            seen.insert(id);
            id = parent.value(id);
        }
    }
    *out = candidate;
    return true;
}

bool KisAiFullStrokeScene::parseResponse(const QByteArray &response, KisAiFullStrokeScene *out, QString *error)
{
    if (response.size() > 1024 * 1024) {
        if (error)
            *error = QStringLiteral("scene response too large");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("invalid scene JSON");
        return false;
    }
    QJsonObject scene = document.object();
    if (!scene.contains(QStringLiteral("objects"))) {
        const QJsonArray choices = scene.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty() || !choices.first().isObject()) {
            if (error)
                *error = QStringLiteral("missing scene choices");
            return false;
        }
        const QString content = choices.first()
                                    .toObject()
                                    .value(QStringLiteral("message"))
                                    .toObject()
                                    .value(QStringLiteral("content"))
                                    .toString();
        if (content.size() > 1024 * 1024) {
            if (error)
                *error = QStringLiteral("scene content too large");
            return false;
        }
        const QJsonDocument inner = QJsonDocument::fromJson(content.toUtf8(), &parseError);
        if (!inner.isObject()) {
            if (error)
                *error = QStringLiteral("invalid scene content");
            return false;
        }
        scene = inner.object();
    }
    return parse(scene, out, error);
}

QString KisAiFullStrokeScene::drawingInstructions() const
{
    QStringList lines;
    lines << QStringLiteral("[VERIFIED SCENE OBJECTS]");
    for (const KisAiFullStrokeObject &object : objects) {
        lines << QStringLiteral("id=%1 type=%2 rect=%3,%4,%5,%6 required=%7 focal=%8 behind=%9")
                     .arg(object.id, object.type)
                     .arg(object.bounds.x(), 0, 'f', 3)
                     .arg(object.bounds.y(), 0, 'f', 3)
                     .arg(object.bounds.width(), 0, 'f', 3)
                     .arg(object.bounds.height(), 0, 'f', 3)
                     .arg(object.required ? 1 : 0)
                     .arg(object.focalWeight, 0, 'f', 2)
                     .arg(object.behindId);
    }
    lines << QStringLiteral(
        "Draw required Flats silhouettes first. Tag operations with matching group_id values, then shade and ink with "
        "the same ids. Rig-first: mirror paired features about the face axis, snap T-stops onto parent "
        "contours, and clip shading/highlights via clip_to_id. Never add source images.");
    return lines.join(QLatin1Char('\n'));
}

QStringList KisAiFullStrokeScene::missingRequiredObjects(const KisAiStrokeProgram &program) const
{
    QSet<QString> painted;
    for (const KisAiStrokeOperation &op : program.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::Fill || op.kind == KisAiStrokeOperation::Kind::GradientFill
            || (op.kind == KisAiStrokeOperation::Kind::Path && op.closed))
            painted.insert(ownerId(op));
    }
    QStringList missing;
    for (const KisAiFullStrokeObject &object : objects) {
        if (object.required && !painted.contains(object.id))
            missing.append(object.id);
    }
    return missing;
}

KisAiStrokeProgram KisAiFullStrokeScene::prioritize(const KisAiStrokeProgram &program, int operationBudget) const
{
    if (operationBudget <= 0 || program.operations.size() <= operationBudget)
        return program;
    struct Group {
        QString id;
        QVector<KisAiStrokeOperation> operations;
        qreal priority{0.0};
        bool required{false};
        int firstIndex{0};
    };
    QVector<Group> groups;
    QHash<QString, int> indexes;
    for (int i = 0; i < program.operations.size(); ++i) {
        const KisAiStrokeOperation &op = program.operations.at(i);
        const QString id = ownerId(op).isEmpty() ? op.id : ownerId(op);
        if (!indexes.contains(id)) {
            indexes.insert(id, groups.size());
            Group g;
            g.id = id;
            g.firstIndex = i;
            for (const KisAiFullStrokeObject &object : objects) {
                if (object.id == id) {
                    g.priority = object.focalWeight;
                    g.required = object.required;
                    break;
                }
            }
            groups.append(g);
        }
        groups[indexes.value(id)].operations.append(op);
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        if (a.required != b.required)
            return a.required;
        return a.priority > b.priority;
    });
    QVector<Group> selected;
    int used = 0;
    for (const Group &group : groups) {
        if (used + group.operations.size() <= operationBudget) {
            selected.append(group);
            used += group.operations.size();
        }
    }
    if (selected.isEmpty() && !groups.isEmpty() && operationBudget > 0) {
        // Fallback: the operationBudget is smaller than any complete group.
        // Truncate the highest-priority group to fit the budget rather than dropping
        // all operations and returning a blank canvas.
        Group fallbackGroup = groups.first();
        fallbackGroup.operations = fallbackGroup.operations.mid(0, operationBudget);
        selected.append(fallbackGroup);
    }
    std::sort(selected.begin(), selected.end(), [](const Group &a, const Group &b) {
        return a.firstIndex < b.firstIndex;
    });
    KisAiStrokeProgram result = program;
    result.operations.clear();
    for (const Group &group : selected)
        result.operations.append(group.operations);
    return result;
}
