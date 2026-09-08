/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeRenderer.h"

#ifndef AI_STROKE_STANDALONE
#include "KisDocument.h"
#include "KisPart.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "kis_image.h"
#include "kis_node_commands_adapter.h"
#include "kis_paint_layer.h"
#include <KoCompositeOpRegistry.h>
#include <klocalizedstring.h>
#else
#define i18n(str, ...) QStringLiteral(str)
#endif

#include <QColor>
#include <QLinearGradient>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRandomGenerator>
#include <QStringList>

#include <cmath>

namespace
{
constexpr qreal PI = 3.14159265358979323846;

QPointF scalePoint(const QPointF &normPt, const QSize &canvasSize)
{
    return QPointF(normPt.x() * canvasSize.width(), normPt.y() * canvasSize.height());
}

QPolygonF scalePolygon(const QPolygonF &normPoly, const QSize &canvasSize)
{
    QPolygonF poly;
    poly.reserve(normPoly.size());
    for (const QPointF &pt : normPoly) {
        poly.append(scalePoint(pt, canvasSize));
    }
    return poly;
}

qreal effectiveBrushWidth(const KisAiStrokeBrush &brush, qreal pressure, const QSize &canvasSize)
{
    const qreal baseDim = qMin(canvasSize.width(), canvasSize.height());
    qreal sz = 8.0;
    if (brush.sizeMode == QLatin1String("px")) {
        sz = brush.size;
    } else {
        sz = brush.size * baseDim;
    }
    sz = qMax<qreal>(1.0, sz * qBound<qreal>(0.05, pressure, 1.0));
    return sz;
}

struct SampledStrokePoint
{
    QPointF pos;
    qreal width {2.0};
};

} // namespace

QVector<QPointF> KisAiStrokeRenderer::generateCatmullRomSpline(
    const QVector<QPointF> &points,
    int subdivisions,
    bool closed
)
{
    if (points.size() < 2) return points;
    if (points.size() == 2 && !closed) return points;

    QVector<QPointF> result;
    const int n = points.size();
    const int segments = closed ? n : (n - 1);

    for (int i = 0; i < segments; ++i) {
        QPointF p0, p1, p2, p3;
        if (closed) {
            p0 = points.at((i - 1 + n) % n);
            p1 = points.at(i);
            p2 = points.at((i + 1) % n);
            p3 = points.at((i + 2) % n);
        } else {
            p1 = points.at(i);
            p2 = points.at(i + 1);
            p0 = (i > 0) ? points.at(i - 1) : (p1 + (p1 - p2));
            p3 = (i + 2 < n) ? points.at(i + 2) : (p2 + (p2 - p1));
        }

        const int steps = qMax(2, subdivisions);
        for (int step = 0; step < steps; ++step) {
            const qreal t = qreal(step) / steps;
            const qreal t2 = t * t;
            const qreal t3 = t2 * t;

            const qreal x = 0.5 * ((2.0 * p1.x()) +
                                   (-p0.x() + p2.x()) * t +
                                   (2.0 * p0.x() - 5.0 * p1.x() + 4.0 * p2.x() - p3.x()) * t2 +
                                   (-p0.x() + 3.0 * p1.x() - 3.0 * p2.x() + p3.x()) * t3);

            const qreal y = 0.5 * ((2.0 * p1.y()) +
                                   (-p0.y() + p2.y()) * t +
                                   (2.0 * p0.y() - 5.0 * p1.y() + 4.0 * p2.y() - p3.y()) * t2 +
                                   (-p0.y() + 3.0 * p1.y() - 3.0 * p2.y() + p3.y()) * t3);

            result.append(QPointF(x, y));
        }
    }

    if (!closed) {
        result.append(points.last());
    }

    return result;
}

QImage KisAiStrokeRenderer::renderProgramToImage(
    const KisAiStrokeProgram &program,
    const QSize &targetSize,
    bool clipShadingToFlats
)
{
    const QSize size = targetSize.isValid() ? targetSize : program.canvasSize;
    QImage compositeImage(size, QImage::Format_ARGB32_Premultiplied);
    compositeImage.fill(Qt::transparent);

    // Standard layer sequence for illustration rendering
    const QStringList layerOrder = {
        QStringLiteral("Flats"),
        QStringLiteral("Shading"),
        QStringLiteral("Lineart"),
        QStringLiteral("Highlights"),
        QStringLiteral("FX")
    };

    QMap<QString, QVector<KisAiStrokeOperation>> layerBuckets;
    for (const KisAiStrokeOperation &op : program.operations) {
        QString lName = op.layer.trimmed();
        if (lName.isEmpty()) lName = QStringLiteral("Lineart");
        layerBuckets[lName].append(op);
    }

    QStringList orderedLayers;
    for (const QString &stdLayer : layerOrder) {
        if (layerBuckets.contains(stdLayer)) {
            orderedLayers.append(stdLayer);
        }
    }
    for (auto it = layerBuckets.constBegin(); it != layerBuckets.constEnd(); ++it) {
        if (!orderedLayers.contains(it.key())) {
            orderedLayers.append(it.key());
        }
    }

    QImage flatsImage;
    bool hasFlats = false;

    QPainter compPainter(&compositeImage);
    compPainter.setRenderHint(QPainter::Antialiasing, true);
    compPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const QString &layerKey : orderedLayers) {
        const QVector<KisAiStrokeOperation> &ops = layerBuckets[layerKey];
        if (ops.isEmpty()) continue;

        QImage layerImage(size, QImage::Format_ARGB32_Premultiplied);
        layerImage.fill(Qt::transparent);

        {
            QPainter painter(&layerImage);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            for (const KisAiStrokeOperation &op : ops) {
                rasterizeOperation(painter, op, size);
            }
        }

        const bool isFlats = (layerKey.compare(QLatin1String("Flats"), Qt::CaseInsensitive) == 0);
        const bool isShading = (layerKey.compare(QLatin1String("Shading"), Qt::CaseInsensitive) == 0);
        const bool isHighlights = (layerKey.compare(QLatin1String("Highlights"), Qt::CaseInsensitive) == 0);

        if (isFlats) {
            flatsImage = layerImage;
            hasFlats = true;
        } else if (clipShadingToFlats && hasFlats && (isShading || isHighlights)) {
            QPainter clipPainter(&layerImage);
            clipPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            clipPainter.drawImage(0, 0, flatsImage);
        }

        if (isShading) {
            compPainter.setCompositionMode(QPainter::CompositionMode_Multiply);
        } else if (isHighlights) {
            compPainter.setCompositionMode(QPainter::CompositionMode_Plus);
        } else {
            compPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }

        compPainter.drawImage(0, 0, layerImage);
    }

    return compositeImage;
}

#ifndef AI_STROKE_STANDALONE
bool KisAiStrokeRenderer::renderProgramToLayers(
    KisImageWSP image,
    KisViewManager *viewManager,
    const KisAiStrokeProgram &program,
    QString *statusMessage,
    bool clipShadingToFlats
)
{
    if (!image || !viewManager || program.operations.isEmpty()) {
        if (statusMessage) {
            *statusMessage = i18n("描画先のキャンバスまたはストロークデータが無効です。");
        }
        return false;
    }

    const QRect bounds = image->bounds();
    if (bounds.isEmpty()) {
        if (statusMessage) {
            *statusMessage = i18n("キャンバスの寸法が無効です。");
        }
        return false;
    }

    const QSize canvasSize = bounds.size();

    const QStringList layerOrder = {
        QStringLiteral("Flats"),
        QStringLiteral("Shading"),
        QStringLiteral("Lineart"),
        QStringLiteral("Highlights"),
        QStringLiteral("FX")
    };

    QMap<QString, QVector<KisAiStrokeOperation>> layerBuckets;
    for (const KisAiStrokeOperation &op : program.operations) {
        QString lName = op.layer.trimmed();
        if (lName.isEmpty()) lName = QStringLiteral("Lineart");
        layerBuckets[lName].append(op);
    }

    QStringList orderedLayers;
    for (const QString &stdLayer : layerOrder) {
        if (layerBuckets.contains(stdLayer)) {
            orderedLayers.append(stdLayer);
        }
    }
    for (auto it = layerBuckets.constBegin(); it != layerBuckets.constEnd(); ++it) {
        if (!orderedLayers.contains(it.key())) {
            orderedLayers.append(it.key());
        }
    }

    KisNodeSP root = image->root();
    KisNodeSP aboveNode = root->lastChild();
    KisNodeCommandsAdapter adapter(viewManager);

    QImage flatsImage;
    bool hasFlats = false;
    int layersAdded = 0;

    for (const QString &layerKey : orderedLayers) {
        const QVector<KisAiStrokeOperation> &ops = layerBuckets[layerKey];
        if (ops.isEmpty()) continue;

        QImage layerImage(canvasSize, QImage::Format_ARGB32_Premultiplied);
        layerImage.fill(Qt::transparent);

        {
            QPainter painter(&layerImage);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

            for (const KisAiStrokeOperation &op : ops) {
                rasterizeOperation(painter, op, canvasSize);
            }
        }

        const bool isFlats = (layerKey.compare(QLatin1String("Flats"), Qt::CaseInsensitive) == 0);
        const bool isShading = (layerKey.compare(QLatin1String("Shading"), Qt::CaseInsensitive) == 0);
        const bool isHighlights = (layerKey.compare(QLatin1String("Highlights"), Qt::CaseInsensitive) == 0);

        if (isFlats) {
            flatsImage = layerImage;
            hasFlats = true;
        } else if (clipShadingToFlats && hasFlats && (isShading || isHighlights)) {
            // Apply clipping mask to Flats silhouette:
            // Shading and Highlights will stay strictly within the filled regions of Flats
            QPainter clipPainter(&layerImage);
            clipPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            clipPainter.drawImage(0, 0, flatsImage);
        }

        const QString layerTitle = QStringLiteral("AI: %1").arg(layerKey);
        KisPaintLayerSP layer = new KisPaintLayer(image, layerTitle, OPACITY_OPAQUE_U8);
        layer->paintDevice()->convertFromQImage(layerImage, nullptr);

        if (isShading) {
            layer->setCompositeOpId(COMPOSITE_MULT);
        } else if (isHighlights) {
            layer->setCompositeOpId(COMPOSITE_ADD);
        } else {
            layer->setCompositeOpId(COMPOSITE_OVER);
        }

        adapter.addNode(layer, root, aboveNode);
        aboveNode = layer;
        ++layersAdded;
    }

    if (viewManager && viewManager->document()) {
        viewManager->document()->setModified(true);
    } else {
        image->setModifiedWithoutUndo();
    }

    if (statusMessage) {
        *statusMessage = i18n("%1 個の AI 作画レイヤーを生成しました（%2 ストローク）。", layersAdded, program.operations.size());
    }

    return layersAdded > 0;
}
#endif

void KisAiStrokeRenderer::rasterizeOperation(
    QPainter &painter,
    const KisAiStrokeOperation &op,
    const QSize &canvasSize
)
{
    painter.save();

    if (op.brush.isEraser) {
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
    } else {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    switch (op.kind) {
    case KisAiStrokeOperation::Kind::Path:
        drawPathOperation(painter, op, canvasSize);
        break;
    case KisAiStrokeOperation::Kind::Fill:
        drawFillOperation(painter, op, canvasSize);
        break;
    case KisAiStrokeOperation::Kind::GradientFill:
        drawGradientFillOperation(painter, op, canvasSize);
        break;
    case KisAiStrokeOperation::Kind::Ribbon:
        drawRibbonOperation(painter, op, canvasSize);
        break;
    case KisAiStrokeOperation::Kind::Particles:
        drawParticlesOperation(painter, op, canvasSize);
        break;
    default:
        break;
    }

    painter.restore();
}

void KisAiStrokeRenderer::drawPathOperation(
    QPainter &painter,
    const KisAiStrokeOperation &op,
    const QSize &canvasSize
)
{
    if (op.points.isEmpty()) return;

    QColor color = op.brush.color;
    color.setAlphaF(qBound<qreal>(0.0, op.brush.opacity * color.alphaF(), 1.0));

    // Single point: render as crisp dab / dot
    if (op.points.size() == 1) {
        const QPointF pt = scalePoint(op.points.at(0).pos, canvasSize);
        const qreal r = effectiveBrushWidth(op.brush, op.points.at(0).pressure, canvasSize) * 0.5;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(pt, r, r);
        return;
    }

    const int n = op.points.size();

    // Scale input points and extract pressures
    QVector<QPointF> scaledPts;
    QVector<qreal> pressures;
    scaledPts.reserve(n);
    pressures.reserve(n);
    for (const KisAiStrokePoint &pt : op.points) {
        scaledPts.append(scalePoint(pt.pos, canvasSize));
        pressures.append(qBound<qreal>(0.05, pt.pressure, 1.0));
    }

    // Subdivide and interpolate curve using Catmull-Rom spline
    const int subdivisions = (op.smooth && n >= 3) ? 8 : 4;
    const int segments = op.closed ? n : (n - 1);

    QVector<SampledStrokePoint> curveSamples;
    curveSamples.reserve(segments * subdivisions + 1);

    for (int i = 0; i < segments; ++i) {
        QPointF p0, p1, p2, p3;
        qreal pr0, pr1, pr2, pr3;

        if (op.closed) {
            const int idx0 = (i - 1 + n) % n;
            const int idx1 = i;
            const int idx2 = (i + 1) % n;
            const int idx3 = (i + 2) % n;
            p0 = scaledPts.at(idx0); pr0 = pressures.at(idx0);
            p1 = scaledPts.at(idx1); pr1 = pressures.at(idx1);
            p2 = scaledPts.at(idx2); pr2 = pressures.at(idx2);
            p3 = scaledPts.at(idx3); pr3 = pressures.at(idx3);
        } else {
            p1 = scaledPts.at(i);     pr1 = pressures.at(i);
            p2 = scaledPts.at(i + 1); pr2 = pressures.at(i + 1);
            p0 = (i > 0) ? scaledPts.at(i - 1) : (p1 + (p1 - p2));
            pr0 = (i > 0) ? pressures.at(i - 1) : pr1;
            p3 = (i + 2 < n) ? scaledPts.at(i + 2) : (p2 + (p2 - p1));
            pr3 = (i + 2 < n) ? pressures.at(i + 2) : pr2;
        }

        for (int step = 0; step < subdivisions; ++step) {
            const qreal t = qreal(step) / subdivisions;
            const qreal t2 = t * t;
            const qreal t3 = t2 * t;

            // Interpolate position
            const qreal x = 0.5 * ((2.0 * p1.x()) +
                                   (-p0.x() + p2.x()) * t +
                                   (2.0 * p0.x() - 5.0 * p1.x() + 4.0 * p2.x() - p3.x()) * t2 +
                                   (-p0.x() + 3.0 * p1.x() - 3.0 * p2.x() + p3.x()) * t3);

            const qreal y = 0.5 * ((2.0 * p1.y()) +
                                   (-p0.y() + p2.y()) * t +
                                   (2.0 * p0.y() - 5.0 * p1.y() + 4.0 * p2.y() - p3.y()) * t2 +
                                   (-p0.y() + 3.0 * p1.y() - 3.0 * p2.y() + p3.y()) * t3);

            // Interpolate pressure
            const qreal p = 0.5 * ((2.0 * pr1) +
                                   (-pr0 + pr2) * t +
                                   (2.0 * pr0 - 5.0 * pr1 + 4.0 * pr2 - pr3) * t2 +
                                   (-pr0 + 3.0 * pr1 - 3.0 * pr2 + pr3) * t3);

            // Global arc progress for tapering
            const qreal globalT = (qreal(i) + t) / qreal(segments);

            // Natural stroke taper at start and end
            qreal taper = 1.0;
            if (!op.closed) {
                constexpr qreal TAPER_LEN = 0.15;
                if (globalT < TAPER_LEN) {
                    taper = 0.08 + 0.92 * std::sin((globalT / TAPER_LEN) * (PI * 0.5));
                } else if (globalT > (1.0 - TAPER_LEN)) {
                    taper = 0.08 + 0.92 * std::sin(((1.0 - globalT) / TAPER_LEN) * (PI * 0.5));
                }
            }

            const qreal strokeW = effectiveBrushWidth(op.brush, p * taper, canvasSize);
            curveSamples.append({QPointF(x, y), strokeW});
        }
    }

    if (!op.closed) {
        curveSamples.append({scaledPts.last(), effectiveBrushWidth(op.brush, pressures.last() * 0.08, canvasSize)});
    }

    const int sampleCount = curveSamples.size();
    if (sampleCount < 2) return;

    // Generate polygonal envelope for smooth varied thickness without stepped overlaps
    QVector<QPointF> leftEdge;
    QVector<QPointF> rightEdge;
    leftEdge.reserve(sampleCount);
    rightEdge.reserve(sampleCount);

    for (int i = 0; i < sampleCount; ++i) {
        const QPointF &curr = curveSamples.at(i).pos;
        const qreal halfW = qMax<qreal>(0.5, curveSamples.at(i).width * 0.5);

        QPointF tangent;
        if (i == 0) {
            tangent = curveSamples.at(1).pos - curr;
        } else if (i == sampleCount - 1) {
            tangent = curr - curveSamples.at(i - 1).pos;
        } else {
            tangent = curveSamples.at(i + 1).pos - curveSamples.at(i - 1).pos;
        }

        const qreal tLen = std::hypot(tangent.x(), tangent.y());
        QPointF normal(0.0, 1.0);
        if (tLen > 0.0001) {
            normal = QPointF(-tangent.y() / tLen, tangent.x() / tLen);
        }

        leftEdge.append(curr + normal * halfW);
        rightEdge.append(curr - normal * halfW);
    }

    QPolygonF ribbonPoly;
    ribbonPoly.reserve(sampleCount * 2);
    for (const QPointF &p : leftEdge) {
        ribbonPoly.append(p);
    }
    for (int i = rightEdge.size() - 1; i >= 0; --i) {
        ribbonPoly.append(rightEdge.at(i));
    }

    const QString profile = op.brush.profile.toLower();

    // Brush profile texture & dynamics
    if (profile == QLatin1String("airbrush")) {
        // Multi-pass soft falloff
        painter.setPen(Qt::NoPen);
        QColor cOuter = color; cOuter.setAlphaF(color.alphaF() * 0.12);
        QColor cMid = color;   cMid.setAlphaF(color.alphaF() * 0.35);

        // Outer soft glow
        QPolygonF outerPoly;
        for (int i = 0; i < sampleCount; ++i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width * 1.8;
            QPointF tangent = (i < sampleCount - 1) ? (curveSamples.at(i + 1).pos - curr) : (curr - curveSamples.at(i - 1).pos);
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            QPointF normal = (tLen > 0.0001) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);
            outerPoly.append(curr + normal * w);
        }
        for (int i = sampleCount - 1; i >= 0; --i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width * 1.8;
            QPointF tangent = (i < sampleCount - 1) ? (curveSamples.at(i + 1).pos - curr) : (curr - curveSamples.at(i - 1).pos);
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            QPointF normal = (tLen > 0.0001) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);
            outerPoly.append(curr - normal * w);
        }
        painter.setBrush(cOuter);
        painter.drawPolygon(outerPoly);

        // Mid and core
        painter.setBrush(cMid);
        painter.drawPolygon(ribbonPoly);
        painter.setBrush(color);
        painter.drawPolygon(ribbonPoly);

    } else if (profile == QLatin1String("watercolor")) {
        // Transparent wash with subtle water-fringe contour
        painter.setPen(Qt::NoPen);
        QColor washColor = color;
        washColor.setAlphaF(color.alphaF() * 0.75);
        painter.setBrush(washColor);
        painter.drawPolygon(ribbonPoly);

        // Darkened fringe boundary
        QColor fringe = color;
        fringe.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.95, 1.0));
        QPen fringePen(fringe, 1.0);
        painter.setPen(fringePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(leftEdge);
        painter.drawPolyline(rightEdge);

    } else {
        // G-Pen / Default: solid crisp anti-aliased contour
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(ribbonPoly);
    }
}

void KisAiStrokeRenderer::drawFillOperation(
    QPainter &painter,
    const KisAiStrokeOperation &op,
    const QSize &canvasSize
)
{
    if (op.polygon.size() < 3) return;

    QPolygonF poly = scalePolygon(op.polygon, canvasSize);

    // Smooth jagged polygon vertices if smooth requested
    if (op.smooth && poly.size() >= 3) {
        poly = generateCatmullRomSpline(poly, 4, true);
    }

    QColor color = op.brush.color;
    color.setAlphaF(qBound<qreal>(0.0, op.brush.opacity * color.alphaF(), 1.0));

    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(poly);
}

void KisAiStrokeRenderer::drawGradientFillOperation(
    QPainter &painter,
    const KisAiStrokeOperation &op,
    const QSize &canvasSize
)
{
    if (op.polygon.size() < 3) return;

    QPolygonF poly = scalePolygon(op.polygon, canvasSize);
    if (op.smooth && poly.size() >= 3) {
        poly = generateCatmullRomSpline(poly, 4, true);
    }

    const QRectF b = poly.boundingRect();

    const qreal rad = op.angleDeg * PI / 180.0;
    const QPointF center = b.center();
    const qreal len = qMax(b.width(), b.height()) * 0.6;
    const QPointF p1 = center - QPointF(std::cos(rad) * len, std::sin(rad) * len);
    const QPointF p2 = center + QPointF(std::cos(rad) * len, std::sin(rad) * len);

    QLinearGradient grad(p1, p2);
    if (!op.gradientColors.isEmpty()) {
        const int count = op.gradientColors.size();
        for (int i = 0; i < count; ++i) {
            const qreal pos = (count > 1) ? qreal(i) / (count - 1) : 0.0;
            grad.setColorAt(pos, op.gradientColors.at(i));
        }
    } else {
        grad.setColorAt(0.0, op.brush.color);
        grad.setColorAt(1.0, Qt::transparent);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawPolygon(poly);
}

void KisAiStrokeRenderer::drawRibbonOperation(
    QPainter &painter,
    const KisAiStrokeOperation &op,
    const QSize &canvasSize
)
{
    if (op.spine.size() < 2) return;

    const qreal baseDim = qMin(canvasSize.width(), canvasSize.height());

    // Scale spine and smooth
    QVector<QPointF> scaledSpine;
    scaledSpine.reserve(op.spine.size());
    for (const QPointF &pt : op.spine) {
        scaledSpine.append(scalePoint(pt, canvasSize));
    }

    if (scaledSpine.size() >= 3) {
        scaledSpine = generateCatmullRomSpline(scaledSpine, 6, false);
    }

    const int n = scaledSpine.size();
    if (n < 2) return;

    QVector<QPointF> leftEdge;
    QVector<QPointF> rightEdge;
    leftEdge.reserve(n);
    rightEdge.reserve(n);

    for (int i = 0; i < n; ++i) {
        const QPointF &curr = scaledSpine.at(i);
        QPointF tangent;
        if (i == 0) {
            tangent = scaledSpine.at(1) - curr;
        } else if (i == n - 1) {
            tangent = curr - scaledSpine.at(n - 2);
        } else {
            tangent = scaledSpine.at(i + 1) - scaledSpine.at(i - 1);
        }

        const qreal tLen = std::hypot(tangent.x(), tangent.y());
        QPointF normal(0.0, 1.0);
        if (tLen > 0.0001) {
            normal = QPointF(-tangent.y() / tLen, tangent.x() / tLen);
        }

        const qreal t = qreal(i) / (n - 1);
        qreal widthNorm;
        if (t <= 0.5) {
            widthNorm = op.widthStart + (op.widthMid - op.widthStart) * (t * 2.0);
        } else {
            widthNorm = op.widthMid + (op.widthEnd - op.widthMid) * ((t - 0.5) * 2.0);
        }
        const qreal halfW = qMax<qreal>(0.5, widthNorm * baseDim * 0.5);

        leftEdge.append(curr + normal * halfW);
        rightEdge.append(curr - normal * halfW);
    }

    QPolygonF ribbonPoly;
    ribbonPoly.reserve(n * 2);
    for (const QPointF &p : leftEdge) {
        ribbonPoly.append(p);
    }
    for (int i = rightEdge.size() - 1; i >= 0; --i) {
        ribbonPoly.append(rightEdge.at(i));
    }

    QColor color = op.brush.color;
    color.setAlphaF(qBound<qreal>(0.0, op.brush.opacity * color.alphaF(), 1.0));

    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(ribbonPoly);
}

void KisAiStrokeRenderer::drawParticlesOperation(
    QPainter &painter,
    const KisAiStrokeOperation &op,
    const QSize &canvasSize
)
{
    const QRectF normBounds = op.bounds.isValid() ? op.bounds : QRectF(0.0, 0.0, 1.0, 1.0);
    const QRectF area(normBounds.left() * canvasSize.width(),
                      normBounds.top() * canvasSize.height(),
                      normBounds.width() * canvasSize.width(),
                      normBounds.height() * canvasSize.height());

    const int count = qBound(1, op.particleCount, 300);
    QRandomGenerator rng(qHash(op.id.isEmpty() ? QStringLiteral("particles") : op.id));

    painter.setPen(Qt::NoPen);
    painter.setBrush(op.brush.color);

    for (int i = 0; i < count; ++i) {
        const qreal x = area.left() + rng.generateDouble() * area.width();
        const qreal y = area.top() + rng.generateDouble() * area.height();
        const qreal r = qMax<qreal>(2.0, canvasSize.width() * (0.003 + rng.generateDouble() * 0.007));

        if (op.particleShape == QLatin1String("sparkle") || op.particleShape == QLatin1String("star")) {
            QPainterPath star;
            star.moveTo(x, y - r * 1.6);
            star.quadTo(x, y, x + r * 1.6, y);
            star.quadTo(x, y, x, y + r * 1.6);
            star.quadTo(x, y, x - r * 1.6, y);
            star.quadTo(x, y, x, y - r * 1.6);
            painter.drawPath(star);
        } else if (op.particleShape == QLatin1String("petal")) {
            painter.save();
            painter.translate(x, y);
            painter.rotate(rng.bounded(360));
            painter.drawEllipse(QPointF(0, 0), r * 1.4, r * 0.7);
            painter.restore();
        } else {
            painter.drawEllipse(QPointF(x, y), r, r);
        }
    }
}
