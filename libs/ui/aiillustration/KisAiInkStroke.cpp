/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiInkStroke.h"

KisAiInkStroke KisAiInkStroke::fromOperation(const KisAiStrokeOperation &op)
{
    KisAiInkStroke ink;
    ink.op = op;
    ink.groupId = op.groupId;
    ink.parentId = op.parentId;
    ink.inkRole = op.role;
    if (ink.inkRole.isEmpty() || ink.inkRole == QLatin1String("auto")) {
        switch (op.kind) {
        case KisAiStrokeOperation::Kind::Fill:
        case KisAiStrokeOperation::Kind::GradientFill:
        case KisAiStrokeOperation::Kind::Ribbon:
            ink.inkRole = KisAiInkRole::mass();
            break;
        case KisAiStrokeOperation::Kind::Path:
            ink.inkRole = KisAiInkRole::contour();
            break;
        case KisAiStrokeOperation::Kind::Particles:
        case KisAiStrokeOperation::Kind::MangaLines:
            ink.inkRole = KisAiInkRole::accent();
            break;
        default:
            ink.inkRole = KisAiInkRole::internalFlow();
            break;
        }
    }
    return ink;
}

KisAiStrokeOperation KisAiInkStroke::toOperation() const
{
    KisAiStrokeOperation out = op;
    out.groupId = groupId;
    out.parentId = parentId;
    if (!inkRole.isEmpty())
        out.role = inkRole;
    return out;
}

QString KisAiStrokeCommitLog::summary() const
{
    return QStringLiteral("committed=%1 skipped=%2 repaired=%3 retried=%4")
        .arg(QString::number(committed), QString::number(skipped), QString::number(repaired), QString::number(retried));
}

namespace KisAiInkRole
{

QString mass()
{
    return QStringLiteral("mass");
}
QString contour()
{
    return QStringLiteral("contour");
}
QString internalFlow()
{
    return QStringLiteral("internal");
}
QString accent()
{
    return QStringLiteral("accent");
}
QString construction()
{
    return QStringLiteral("construction");
}

int rank(const QString &role)
{
    const QString r = role.toLower();
    if (r == QLatin1String("construction"))
        return 0;
    if (r == QLatin1String("mass"))
        return 1;
    if (r == QLatin1String("contour"))
        return 2;
    if (r == QLatin1String("internal"))
        return 3;
    if (r == QLatin1String("accent"))
        return 4;
    return 5;
}

} // namespace KisAiInkRole
