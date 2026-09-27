/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiImageGuidedScene.h"

#include "KisAiStrokeQualityUtils.h"

#include <QHash>
#include <QPoint>
#include <QtMath>

#include <cmath>
#include <limits>

namespace
{
constexpr qreal kMinRegionConfidence = 0.95;
constexpr int kMinMaskPixels = 64;
constexpr int kMinContourEdges = 48;
constexpr int kMaxMaskEdge = 1536;
constexpr int kMaxBoundaryEdges = 250000;

struct Edge {
    QPoint start;
    QPoint end;
    int direction{0}; // right, down, left, up
    bool visited{false};
};

quint64 vertexKey(const QPoint &p, int width)
{
    return quint64(p.y()) * quint64(width + 1) + quint64(p.x());
}

qreal signedArea(const QVector<QPointF> &poly)
{
    qreal area = 0.0;
    for (int i = 0; i < poly.size(); ++i) {
        const QPointF &a = poly.at(i);
        const QPointF &b = poly.at((i + 1) % poly.size());
        area += a.x() * b.y() - b.x() * a.y();
    }
    return area * 0.5;
}

QVector<QPointF> simplifyClosed(const QVector<QPointF> &loop)
{
    if (loop.size() < kMinContourEdges)
        return {};
    const QPointF first = loop.first();
    int farthest = 0;
    qreal maxDistance = 0.0;
    for (int i = 1; i < loop.size(); ++i) {
        const QPointF d = loop.at(i) - first;
        const qreal distance = d.x() * d.x() + d.y() * d.y();
        if (distance > maxDistance) {
            maxDistance = distance;
            farthest = i;
        }
    }
    if (maxDistance < 100.0 || farthest == 0 || farthest == loop.size() - 1)
        return {};
    const QVector<QPointF> forward = KisAiStrokeQualityUtils::simplifyRDP(loop.mid(0, farthest + 1), 0.65);
    QVector<QPointF> backward = loop.mid(farthest);
    backward.append(first);
    backward = KisAiStrokeQualityUtils::simplifyRDP(backward, 0.65);
    QVector<QPointF> result = forward;
    for (int i = 1; i + 1 < backward.size(); ++i)
        result.append(backward.at(i));
    return result.size() >= 3 ? result : QVector<QPointF>();
}

QVector<QPointF> outerContour(const QImage &mask)
{
    const int w = mask.width();
    const int h = mask.height();
    if (w < 12 || h < 12 || w > kMaxMaskEdge || h > kMaxMaskEdge)
        return {};

    auto inside = [&](int x, int y) -> bool {
        return x >= 0 && y >= 0 && x < w && y < h && mask.constScanLine(y)[x] >= 128;
    };

    QVector<Edge> edges;
    QHash<quint64, QVector<int>> outgoing;
    int filled = 0;
    auto addEdge = [&](int x0, int y0, int x1, int y1, int direction) {
        const int index = edges.size();
        edges.append({QPoint(x0, y0), QPoint(x1, y1), direction, false});
        outgoing[vertexKey(QPoint(x0, y0), w)].append(index);
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!inside(x, y))
                continue;
            ++filled;
            if (!inside(x, y - 1))
                addEdge(x, y, x + 1, y, 0);
            if (!inside(x + 1, y))
                addEdge(x + 1, y, x + 1, y + 1, 1);
            if (!inside(x, y + 1))
                addEdge(x + 1, y + 1, x, y + 1, 2);
            if (!inside(x - 1, y))
                addEdge(x, y + 1, x, y, 3);
            if (edges.size() > kMaxBoundaryEdges)
                return {};
        }
    }
    if (filled < kMinMaskPixels)
        return {};

    QVector<QPointF> best;
    qreal bestArea = 0.0;
    int exteriors = 0;
    for (int start = 0; start < edges.size(); ++start) {
        if (edges.at(start).visited)
            continue;
        QVector<QPointF> loop;
        int current = start;
        bool closed = false;
        while (current >= 0 && !edges.at(current).visited && loop.size() <= kMaxBoundaryEdges) {
            Edge &edge = edges[current];
            edge.visited = true;
            loop.append(QPointF(edge.start));
            if (edge.end == edges.at(start).start) {
                closed = true;
                break;
            }
            const QVector<int> nextEdges = outgoing.value(vertexKey(edge.end, w));
            int next = -1;
            int bestTurn = std::numeric_limits<int>::max();
            for (int possible : nextEdges) {
                if (edges.at(possible).visited)
                    continue;
                const int turn = (edges.at(possible).direction - edge.direction + 4) % 4;
                const int order = turn == 1 ? 0 : (turn == 0 ? 1 : (turn == 3 ? 2 : 3));
                if (order < bestTurn) {
                    bestTurn = order;
                    next = possible;
                }
            }
            current = next;
        }
        if (!closed || loop.size() < kMinContourEdges)
            continue;
        const qreal area = signedArea(loop);
        // Clockwise exterior paths have positive area in image coordinates;
        // counter-clockwise holes are intentionally left in the reference.
        if (area > 0.0) {
            ++exteriors;
            if (area > bestArea) {
                best = loop;
                bestArea = area;
            }
        }
    }
    // A disconnected mask is ambiguous: silently discarding its smaller
    // objects would misrepresent the region. Ask the segmenter to split it.
    if (exteriors != 1 || bestArea < kMinMaskPixels)
        return {};
    return simplifyClosed(best);
}
} // namespace

QRectF KisAiImageGuidedScene::imageRectPx() const
{
    if (source.isNull() || !canvasSize.isValid())
        return {};
    const QSize scaled = source.size().scaled(canvasSize, Qt::KeepAspectRatio);
    // Match the integer QPoint placement used by the actual image layer.
    return QRectF(QPointF((canvasSize.width() - scaled.width()) / 2, (canvasSize.height() - scaled.height()) / 2),
                  QSizeF(scaled));
}

bool KisAiImageGuidedScene::isValid() const
{
    return !source.isNull() && canvasSize.isValid() && !imageRectPx().isEmpty();
}

KisAiStrokeProgram KisAiImageGuidedContour::buildLineart(const KisAiImageGuidedScene &scene)
{
    KisAiStrokeProgram program;
    if (!scene.isValid())
        return program;
    program.canvasSize = scene.canvasSize;
    const QRectF target = scene.imageRectPx();
    QHash<QString, bool> ids;
    for (const KisAiGuidedRegion &region : scene.regions) {
        bool safeId = !region.id.isEmpty() && region.id.size() <= 64;
        for (const QChar c : region.id) {
            const ushort u = c.unicode();
            if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == '-'))
                safeId = false;
        }
        if (region.confidence < kMinRegionConfidence || !std::isfinite(region.confidence) || region.confidence > 1.0
            || !region.outlineApproved || !safeId || ids.contains(region.id)
            || region.mask.size() != scene.source.size()
            || (region.mask.format() != QImage::Format_Grayscale8 && region.mask.format() != QImage::Format_Alpha8))
            continue;
        ids.insert(region.id, true);
        const QVector<QPointF> points = outerContour(region.mask);
        if (points.size() < 3)
            continue;
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.layer = QStringLiteral("Lineart");
        op.id = QStringLiteral("guided_%1").arg(region.id);
        op.groupId = op.id;
        op.role = QStringLiteral("contour");
        op.closed = true;
        op.smooth = false; // Do not round away observed silhouette corners.
        op.brush.profile = QStringLiteral("gpen");
        op.brush.color = QColor(27, 24, 34);
        op.brush.sizeMode = QStringLiteral("px");
        op.brush.size = qBound<qreal>(1.2, qMin(scene.canvasSize.width(), scene.canvasSize.height()) * 0.002, 3.0);
        op.points.reserve(points.size());
        for (const QPointF &p : points) {
            const qreal x = (target.left() + p.x() * target.width() / scene.source.width()) / scene.canvasSize.width();
            const qreal y =
                (target.top() + p.y() * target.height() / scene.source.height()) / scene.canvasSize.height();
            op.points.append(KisAiStrokePoint(x, y, 0.8));
        }
        program.operations.append(op);
    }
    return program;
}
