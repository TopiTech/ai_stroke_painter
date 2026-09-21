/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeGraph.h"

#include "KisAiDeliberateStroke.h"
#include "KisAiInkStroke.h"

#include <QHash>
#include <QLineF>
#include <QSet>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace
{
int layerRank(const QString &layer)
{
    const QString l = KisAiStrokeProgramCodec::normalizeLayerName(layer);
    if (l == QLatin1String("Background"))
        return 0;
    if (l == QLatin1String("Flats"))
        return 1;
    if (l == QLatin1String("Shading"))
        return 2;
    if (l == QLatin1String("Lineart"))
        return 3;
    if (l == QLatin1String("Highlights"))
        return 4;
    if (l == QLatin1String("FX"))
        return 5;
    return 6;
}

QString pairKey(const QString &id)
{
    QString k = id.toLower();
    k.replace(QLatin1String("_left"), QLatin1String("_x"));
    k.replace(QLatin1String("_right"), QLatin1String("_x"));
    k.replace(QLatin1String("_l_"), QLatin1String("_x_"));
    k.replace(QLatin1String("_r_"), QLatin1String("_x_"));
    if (k.endsWith(QLatin1String("_l")))
        k.chop(2), k.append(QLatin1String("_x"));
    if (k.endsWith(QLatin1String("_r")))
        k.chop(2), k.append(QLatin1String("_x"));
    return k;
}

int sideRank(const QString &id)
{
    const QStringList parts = id.toLower().split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (parts.contains(QLatin1String("l")) || parts.contains(QLatin1String("left")))
        return 0;
    if (parts.contains(QLatin1String("r")) || parts.contains(QLatin1String("right")))
        return 1;
    return 2;
}

QVector<QPointF> parentPoints(const KisAiStrokeOperation &op)
{
    QVector<QPointF> pts;
    if (!op.points.isEmpty()) {
        pts.reserve(op.points.size());
        for (const KisAiStrokePoint &p : op.points)
            pts.append(p.pos);
        return pts;
    }
    if (!op.polygon.isEmpty()) {
        pts.reserve(op.polygon.size());
        for (const QPointF &p : op.polygon)
            pts.append(p);
        return pts;
    }
    if (!op.spine.isEmpty())
        return op.spine;
    return pts;
}

QPointF polyCentroid(const QPolygonF &poly)
{
    if (poly.isEmpty())
        return QPointF();
    QPointF acc;
    for (const QPointF &p : poly)
        acc += p;
    return acc / qreal(poly.size());
}

QPointF closestPointOnSegment(const QPointF &a, const QPointF &b, const QPointF &p)
{
    const QPointF ab = b - a;
    const qreal len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 < 1.0e-18)
        return a;
    const qreal t = qBound<qreal>(0.0, QPointF::dotProduct(p - a, ab) / len2, 1.0);
    return a + ab * t;
}

bool snapEndpointToAnchors(QPointF *pt, const QVector<QPointF> &anchors, bool closed, qreal snapNorm)
{
    if (!pt || anchors.isEmpty())
        return false;
    QPointF best = anchors.first();
    qreal bestD = 1.0e18;
    const int n = anchors.size();
    const int segs = closed ? n : qMax(0, n - 1);
    if (segs <= 0) {
        const QPointF d = anchors.first() - *pt;
        bestD = d.x() * d.x() + d.y() * d.y();
        best = anchors.first();
    } else {
        for (int i = 0; i < segs; ++i) {
            const QPointF a = anchors.at(i);
            const QPointF b = anchors.at((i + 1) % n);
            const QPointF c = closestPointOnSegment(a, b, *pt);
            const QPointF d = c - *pt;
            const qreal dist = d.x() * d.x() + d.y() * d.y();
            if (dist < bestD) {
                bestD = dist;
                best = c;
            }
        }
    }
    if (std::sqrt(bestD) <= snapNorm) {
        *pt = best;
        return true;
    }
    return false;
}
} // namespace

QString KisAiStrokeGraph::inferGroupId(const KisAiStrokeOperation &op)
{
    if (!op.groupId.isEmpty())
        return op.groupId;
    const QString id = op.id.toLower();
    const QStringList parts = id.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (id.contains(QLatin1String("eye")) || id.contains(QLatin1String("lash")) || id.contains(QLatin1String("iris"))
        || id.contains(QLatin1String("pupil")) || id.contains(QLatin1String("lid"))
        || id.contains(QLatin1String("crease"))) {
        if (parts.contains(QLatin1String("r")) || parts.contains(QLatin1String("right")) || op.eyeIsRight)
            return QStringLiteral("eye_r");
        return QStringLiteral("eye_l");
    }
    if (id.contains(QLatin1String("mouth")) || id.contains(QLatin1String("lip")))
        return QStringLiteral("mouth");
    if (id.contains(QLatin1String("brow"))) {
        if (parts.contains(QLatin1String("r")) || parts.contains(QLatin1String("right")))
            return QStringLiteral("brow_r");
        return QStringLiteral("brow_l");
    }
    if (id.contains(QLatin1String("hair_fringe")) || id.contains(QLatin1String("hair_clump")))
        return id.section(QLatin1Char('_'), 0, 2);
    if (id.contains(QLatin1String("face_contour")) || id.contains(QLatin1String("jaw"))
        || id.contains(QLatin1String("chin")))
        return QStringLiteral("jaw");
    if (id.contains(QLatin1String("hair")))
        return QStringLiteral("hair");
    return op.id.isEmpty() ? QStringLiteral("misc") : op.id;
}

QString KisAiStrokeGraph::inferParentId(const KisAiStrokeOperation &op)
{
    if (!op.parentId.isEmpty())
        return op.parentId;
    const QString id = op.id.toLower();
    const QString group = inferGroupId(op);
    if (id.contains(QLatin1String("lash_clump")))
        return QString(group).append(QLatin1String("_lash_upper"));
    if (id.contains(QLatin1String("lash_lower")))
        return QString(group).append(QLatin1String("_sclera"));
    if (id.contains(QLatin1String("hair_strand")) || id.contains(QLatin1String("flyaway")))
        return QStringLiteral("hair_fringe");
    return QString();
}

QString KisAiStrokeGraph::inferRole(const KisAiStrokeOperation &op)
{
    if (!op.role.isEmpty() && op.role != QLatin1String("auto"))
        return op.role;
    switch (op.kind) {
    case KisAiStrokeOperation::Kind::Fill:
    case KisAiStrokeOperation::Kind::GradientFill:
    case KisAiStrokeOperation::Kind::Ribbon:
        return KisAiInkRole::mass();
    case KisAiStrokeOperation::Kind::Path: {
        const QString layer = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
        if (layer == QLatin1String("Highlights") || layer == QLatin1String("FX"))
            return KisAiInkRole::accent();
        if (layer == QLatin1String("Shading"))
            return KisAiInkRole::internalFlow();
        return KisAiInkRole::contour();
    }
    case KisAiStrokeOperation::Kind::Particles:
    case KisAiStrokeOperation::Kind::MangaLines:
        return KisAiInkRole::accent();
    default:
        return KisAiInkRole::internalFlow();
    }
}

QVector<KisAiStrokeOperation> KisAiStrokeGraph::orderForCommit(const QVector<KisAiStrokeOperation> &ops,
                                                               const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> annotated = ops;
    for (KisAiStrokeOperation &op : annotated) {
        if (op.groupId.isEmpty())
            op.groupId = inferGroupId(op);
        if (op.parentId.isEmpty())
            op.parentId = inferParentId(op);
        if (op.role.isEmpty() || op.role == QLatin1String("auto"))
            op.role = inferRole(op);
    }

    struct Item {
        int index;
        int layer;
        int role;
        int clipDepth;
        QString pair;
        int side;
        bool face;
        qreal mass;
    };
    QHash<QString, int> byId;
    for (int i = 0; i < annotated.size(); ++i) {
        if (!annotated.at(i).id.isEmpty())
            byId.insert(annotated.at(i).id, i);
    }
    auto clipDepthOf = [&](const KisAiStrokeOperation &op) {
        int depth = 0;
        QString clip = op.clipToId;
        QSet<QString> seen;
        while (!clip.isEmpty() && byId.contains(clip) && !seen.contains(clip) && depth < 8) {
            seen.insert(clip);
            ++depth;
            clip = annotated.at(byId.value(clip)).clipToId;
        }
        return depth;
    };

    QVector<Item> items;
    items.reserve(annotated.size());
    const QVector<int> massOrder = KisAiDeliberateStroke::planStrokeOrder(annotated, canvasSize);
    QVector<qreal> massRank(annotated.size(), 0.0);
    for (int i = 0; i < massOrder.size(); ++i)
        massRank[massOrder.at(i)] = qreal(massOrder.size() - i);

    for (int i = 0; i < annotated.size(); ++i) {
        const KisAiStrokeOperation &op = annotated.at(i);
        items.append({i,
                      layerRank(op.layer),
                      KisAiInkRole::rank(op.role),
                      clipDepthOf(op),
                      pairKey(op.id),
                      sideRank(op.id),
                      KisAiDeliberateStroke::isFaceDetail(op.id),
                      massRank.at(i)});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.layer != b.layer)
            return a.layer < b.layer;
        if (a.role != b.role)
            return a.role < b.role;
        // Jaw / hair masses before facial details, otherwise pair-key
        // lexicographic order puts "eye_*" lashes ahead of "face_contour".
        if (a.face != b.face)
            return !a.face && b.face;
        // clipToId children (iris in sclera, pupil in iris) paint after parents.
        if (a.clipDepth != b.clipDepth)
            return a.clipDepth < b.clipDepth;
        if (a.pair != b.pair)
            return a.pair < b.pair;
        if (a.side != b.side)
            return a.side < b.side; // left then right of the same role
        if (!qFuzzyCompare(a.mass + 1.0, b.mass + 1.0))
            return a.mass > b.mass;
        return a.index < b.index;
    });
    QVector<KisAiStrokeOperation> ordered;
    ordered.reserve(annotated.size());
    for (const Item &it : items)
        ordered.append(annotated.at(it.index));
    return ordered;
}

QStringList KisAiStrokeGraph::groupIdsInOrder(const QVector<KisAiStrokeOperation> &ops)
{
    QStringList ids;
    for (const KisAiStrokeOperation &op : ops) {
        const QString g = op.groupId.isEmpty() ? inferGroupId(op) : op.groupId;
        if (!ids.contains(g))
            ids.append(g);
    }
    return ids;
}

QVector<KisAiStrokeOperation> KisAiStrokeGraph::opsInGroup(const QVector<KisAiStrokeOperation> &ops,
                                                           const QString &groupId)
{
    QVector<KisAiStrokeOperation> out;
    for (const KisAiStrokeOperation &op : ops) {
        const QString g = op.groupId.isEmpty() ? inferGroupId(op) : op.groupId;
        if (g == groupId)
            out.append(op);
    }
    return out;
}

int KisAiStrokeGraph::snapTStops(QVector<KisAiStrokeOperation> &ops, const QSize &canvasSize, qreal snapPx)
{
    QHash<QString, int> byId;
    for (int i = 0; i < ops.size(); ++i) {
        if (!ops.at(i).id.isEmpty())
            byId.insert(ops.at(i).id, i);
    }
    const qreal minDim = qMax<qreal>(1.0, qMin(canvasSize.width(), canvasSize.height()));
    const qreal snapNorm = snapPx / minDim;
    int snapped = 0;
    for (int i = 0; i < ops.size(); ++i) {
        KisAiStrokeOperation &op = ops[i];
        if (op.kind != KisAiStrokeOperation::Kind::Path || op.points.size() < 2 || op.parentId.isEmpty())
            continue;
        if (!byId.contains(op.parentId))
            continue;
        const int parentIndex = byId.value(op.parentId);
        if (parentIndex == i)
            continue;
        const KisAiStrokeOperation &parent = ops.at(parentIndex);
        const QVector<QPointF> anchors = parentPoints(parent);
        if (anchors.isEmpty())
            continue;
        const bool closedParent = parent.closed || parent.kind == KisAiStrokeOperation::Kind::Fill
            || parent.kind == KisAiStrokeOperation::Kind::GradientFill;
        if (snapEndpointToAnchors(&op.points.first().pos, anchors, closedParent, snapNorm))
            ++snapped;
        if (snapEndpointToAnchors(&op.points.last().pos, anchors, closedParent, snapNorm))
            ++snapped;
    }
    return snapped;
}

QStringList KisAiStrokeGraph::critiqueGroup(const QString &groupId, const QVector<KisAiStrokeOperation> &ops)
{
    QStringList warnings;
    if (groupId.startsWith(QLatin1String("eye"))) {
        QPointF left;
        QPointF right;
        bool hasL = false;
        bool hasR = false;
        for (const KisAiStrokeOperation &op : ops) {
            const QString g = op.groupId.isEmpty() ? inferGroupId(op) : op.groupId;
            if (op.kind == KisAiStrokeOperation::Kind::Fill
                && (op.id.contains(QLatin1String("sclera")) || op.id.contains(QLatin1String("iris")))) {
                const QPointF c = polyCentroid(op.polygon);
                if (g == QLatin1String("eye_l")) {
                    left = c;
                    hasL = true;
                } else if (g == QLatin1String("eye_r")) {
                    right = c;
                    hasR = true;
                }
            }
            if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
                if (op.eyeIsRight) {
                    right = op.eyeCenter;
                    hasR = true;
                } else {
                    left = op.eyeCenter;
                    hasL = true;
                }
            }
        }
        if (hasL && hasR && qAbs(left.y() - right.y()) > 0.02)
            warnings << QStringLiteral("eye-height-mismatch");
        if (hasL != hasR)
            warnings << QStringLiteral("single-eye-only");
    }
    return warnings;
}
