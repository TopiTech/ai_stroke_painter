/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeRenderer.h"
#include "KisAiDeliberateStroke.h"
#include "KisAiStrokeQualityUtils.h"

#ifndef AI_STROKE_STANDALONE
#include "KisDocument.h"
#include "KisPart.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "kis_image.h"
#include "kis_node_commands_adapter.h"
#include "kis_paint_layer.h"
#include "kis_group_layer.h"
#include <KoCompositeOpRegistry.h>
#include <klocalizedstring.h>
#include <kundo2magicstring.h>
#include <QApplication>
#include <QThread>
#else
#define i18n(str, ...) QStringLiteral(str)
#endif

#include <QBuffer>
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

qreal effectiveBrushWidth(const KisAiStrokeBrush &brush, qreal pressure, const QSize &canvasSize, int supersampleScale = 1)
{
    const qreal baseDim = qMin(canvasSize.width(), canvasSize.height());
    const qreal scale = qMax(1, supersampleScale);
    qreal sz = 8.0;
    if (brush.sizeMode == QLatin1String("px")) {
        // The rasterizer paints on a supersampled working image and downscales it
        // afterwards, so an absolute px width must be enlarged by the same factor
        // to keep the requested device-pixel width on the final image.
        sz = brush.size * scale;
    } else {
        sz = brush.size * baseDim;
    }
    sz = qMax<qreal>(1.0, sz * qBound<qreal>(0.05, pressure, 1.0));
    return sz;
}

struct SampledStrokePoint {
    QPointF pos;
    qreal width{2.0};
};

qreal knotInterval(const QPointF &a, const QPointF &b)
{
    // alpha=0.5 (centripetal): (squared distance)^(alpha/2).
    const QPointF delta = b - a;
    return qMax<qreal>(1.0e-4, std::pow(delta.x() * delta.x() + delta.y() * delta.y(), 0.25));
}

QPointF interpolateAtKnot(const QPointF &a, const QPointF &b, qreal ta, qreal tb, qreal t)
{
    const qreal denominator = tb - ta;
    if (qAbs(denominator) < 1.0e-8)
        return a;
    return a * ((tb - t) / denominator) + b * ((t - ta) / denominator);
}

QPointF centripetalPoint(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal u)
{
    const qreal t0 = 0.0;
    const qreal t1 = t0 + knotInterval(p0, p1);
    const qreal t2 = t1 + knotInterval(p1, p2);
    const qreal t3 = t2 + knotInterval(p2, p3);
    const qreal t = t1 + qBound<qreal>(0.0, u, 1.0) * (t2 - t1);

    const QPointF a1 = interpolateAtKnot(p0, p1, t0, t1, t);
    const QPointF a2 = interpolateAtKnot(p1, p2, t1, t2, t);
    const QPointF a3 = interpolateAtKnot(p2, p3, t2, t3, t);
    const QPointF b1 = interpolateAtKnot(a1, a2, t0, t2, t);
    const QPointF b2 = interpolateAtKnot(a2, a3, t1, t3, t);
    return interpolateAtKnot(b1, b2, t1, t2, t);
}

} // namespace

QVector<QPointF>
KisAiStrokeRenderer::generateCatmullRomSpline(const QVector<QPointF> &points, int subdivisions, bool closed)
{
    if (points.size() < 2)
        return points;
    if (points.size() == 2 && !closed)
        return points;

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
            result.append(centripetalPoint(p0, p1, p2, p3, t));
        }
        // Include t = 1 (== p2, the next knot) so each span reaches its end;
        // sampling only [0, 1) left every segment ~1/subdivisions short and
        // produced visible polygonal faceting between spans.
        result.append(p2);
    }

    if (!closed) {
        // The final segment already ended at points.last(); avoid a duplicate.
        if (result.isEmpty() || result.last() != points.last()) {
            result.append(points.last());
        }
    }

    return result;
}

QVector<KisAiStrokeOperation> KisAiStrokeRenderer::expandProceduralOperations(
    const QVector<KisAiStrokeOperation> &operations,
    const QSize &canvasSize)
{
    // V3 Phase 0.2: First fuse overlapping hair Flats patches ("bubble/afro"
    // artifact) into continuous silhouettes before strand synthesis.
    const QVector<KisAiStrokeOperation> unified =
        KisAiStrokeQualityUtils::uniteOverlappingHairFlats(operations);

    QVector<KisAiStrokeOperation> expanded;
    expanded.reserve(unified.size() * 2);

    for (const KisAiStrokeOperation &op : unified) {
        if (op.kind == KisAiStrokeOperation::Kind::Ribbon &&
            (op.brush.profile.compare(QLatin1String("hair"), Qt::CaseInsensitive) == 0 ||
             op.brush.profile.compare(QLatin1String("hair_strand"), Qt::CaseInsensitive) == 0 ||
             op.id.contains(QLatin1String("hair"), Qt::CaseInsensitive))) {
            const KisAiStrokeQualityUtils::HairClumpSynthesis syn =
                KisAiStrokeQualityUtils::synthesizeHairClump(op, canvasSize);
            expanded.append(syn.mainMass);
            for (const KisAiStrokeOperation &strand : syn.strands) {
                expanded.append(strand);
            }
            for (const KisAiStrokeOperation &fly : syn.flyaways) {
                expanded.append(fly);
            }
            if (!syn.highlightHalo.points.isEmpty()) {
                expanded.append(syn.highlightHalo);
            }
        } else if (op.kind == KisAiStrokeOperation::Kind::Fill &&
                   !op.id.contains(QLatin1String("shadow"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("shade"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("skin"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("neck"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("blush"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("chin"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("face"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("clothing"), Qt::CaseInsensitive) &&
                   !op.id.contains(QLatin1String("hair"), Qt::CaseInsensitive) &&
                   (op.brush.profile.compare(QLatin1String("foliage"), Qt::CaseInsensitive) == 0 ||
                    op.brush.profile.compare(QLatin1String("leaves"), Qt::CaseInsensitive) == 0 ||
                    op.brush.profile.compare(QLatin1String("petals"), Qt::CaseInsensitive) == 0 ||
                    op.id.contains(QLatin1String("sakura"), Qt::CaseInsensitive) ||
                    op.id.contains(QLatin1String("tree_canopy"), Qt::CaseInsensitive) ||
                    op.id.contains(QLatin1String("foliage"), Qt::CaseInsensitive) ||
                    op.id.contains(QLatin1String("petal"), Qt::CaseInsensitive))) {
            const QVector<KisAiStrokeOperation> foliage =
                KisAiStrokeQualityUtils::synthesizeFoliageClusters(op, canvasSize);
            expanded.append(foliage);
        } else {
            expanded.append(op);
        }
    }
    return expanded;
}

QImage KisAiStrokeRenderer::renderProgramToImage(const KisAiStrokeProgram &program,
                                                 const QSize &targetSize,
                                                 bool clipShadingToFlats,
                                                 qreal trappingPx)
{
    return renderProgramToImage(program, targetSize, clipShadingToFlats, nullptr, trappingPx);
}

QImage KisAiStrokeRenderer::renderProgramToImage(const KisAiStrokeProgram &program,
                                                 const QSize &targetSize,
                                                 bool clipShadingToFlats,
                                                 const KisAiStrokeProgram *inheritedFlatsProgram,
                                                 qreal trappingPx)
{
    const bool hasExplicitTarget = !targetSize.isEmpty() && targetSize.width() >= 64 && targetSize.height() >= 64;
    QSize size = hasExplicitTarget ? targetSize : program.canvasSize;
    // program.canvasSize is model-supplied metadata, so bound it before it sizes a
    // QImage allocation. An explicit target size is caller-owned (document or
    // preview size) and is honoured as given.
    if (!hasExplicitTarget) {
        constexpr int MAX_DERIVED_RENDER_EDGE = 4096;
        size = size.boundedTo(QSize(MAX_DERIVED_RENDER_EDGE, MAX_DERIVED_RENDER_EDGE));
    }
    if (size.width() < 64 || size.height() < 64) {
        size = QSize(1024, 1024);
    }

    // A2: Apply Flats trapping (slight dilation) to tuck under Lineart strokes and eliminate white underfill seams
    const qreal minDim = qMin(size.width(), size.height());
    const qreal effectiveTrapping = trappingPx < 0.0 ? qMax<qreal>(1.0, minDim / 1000.0 * 1.5) : trappingPx;
    KisAiStrokeProgram activeProgram = program;
    if (effectiveTrapping > 0.0) {
        activeProgram = KisAiStrokeQualityUtils::applyTrapping(program, effectiveTrapping);
    }

    QImage compositeImage(size, QImage::Format_ARGB32_Premultiplied);
    compositeImage.fill(Qt::transparent);

    // Standard layer sequence for illustration rendering
    const QStringList layerOrder = {QStringLiteral("Background"),
                                    QStringLiteral("Flats"),
                                    QStringLiteral("Shading"),
                                    QStringLiteral("Lineart"),
                                    QStringLiteral("Highlights"),
                                    QStringLiteral("FX")};

    const QVector<KisAiStrokeOperation> expandedOps = expandProceduralOperations(activeProgram.operations, size);
    QMap<QString, QVector<KisAiStrokeOperation>> layerBuckets;
    for (const KisAiStrokeOperation &op : expandedOps) {
        const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
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
    if (clipShadingToFlats && inheritedFlatsProgram) {
        QVector<KisAiStrokeOperation> inheritedFlats;
        for (const KisAiStrokeOperation &op : inheritedFlatsProgram->operations) {
            if (KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Flats")) {
                inheritedFlats.append(op);
            }
        }
        if (!inheritedFlats.isEmpty()) {
            flatsImage = renderOperationsToImage(inheritedFlats, size);
            hasFlats = true;
        }
    }

    // Phase 1: Build Face & Feature Exclusion Mask to prevent particles from falling on facial features
    QPainterPath faceExclusionPath;
    for (const KisAiStrokeOperation &op : activeProgram.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
            const QPointF pt = scalePoint(op.eyeCenter, size);
            const qreal ew = op.eyeSize.width() * size.width();
            const qreal eh = op.eyeSize.height() * size.height();
            faceExclusionPath.addEllipse(QRectF(pt.x() - ew * 1.5, pt.y() - eh * 1.5, ew * 3.0, eh * 3.0));
        } else if (op.kind == KisAiStrokeOperation::Kind::Fill) {
            const QString lowerId = op.id.toLower();
            if ((lowerId.contains(QLatin1String("skin")) || lowerId.contains(QLatin1String("face")) || lowerId.contains(QLatin1String("head"))) && op.polygon.size() >= 3) {
                faceExclusionPath.addPolygon(scalePolygon(op.polygon, size));
            }
        }
    }

    QPainter compPainter(&compositeImage);
    compPainter.setRenderHint(QPainter::Antialiasing, true);
    compPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const QString &layerKey : orderedLayers) {
        const QVector<KisAiStrokeOperation> &ops = layerBuckets[layerKey];
        if (ops.isEmpty())
            continue;

        const bool isFlats = (layerKey.compare(QLatin1String("Flats"), Qt::CaseInsensitive) == 0);
        const bool isShading = (layerKey.compare(QLatin1String("Shading"), Qt::CaseInsensitive) == 0);
        const bool isHighlights = (layerKey.compare(QLatin1String("Highlights"), Qt::CaseInsensitive) == 0);
        const bool isFx = (layerKey.compare(QLatin1String("FX"), Qt::CaseInsensitive) == 0);

        QImage layerImage = isFx ? renderOperationsToImage(ops, size, faceExclusionPath) : renderOperationsToImage(ops, size);

        if (isFlats) {
            flatsImage = layerImage;
            hasFlats = true;
        } else if (isShading) {
            // Dual shadow separation (Phase 3): separate sharp cast shadows and soft form shadows
            QVector<KisAiStrokeOperation> formOps;
            QVector<KisAiStrokeOperation> castOps;
            formOps.reserve(ops.size());
            castOps.reserve(ops.size());

            for (const KisAiStrokeOperation &shOp : ops) {
                if (shOp.kind == KisAiStrokeOperation::Kind::Fill &&
                    KisAiStrokeQualityUtils::isCastShadow(shOp.polygon, size)) {
                    castOps.append(shOp);
                } else {
                    formOps.append(shOp);
                }
            }

            QImage formImage;
            if (!formOps.isEmpty()) {
                formImage = renderOperationsToImage(formOps, size);
                applySoftEdgeDiffusion(formImage, 4);
            } else {
                formImage = QImage(size, QImage::Format_ARGB32_Premultiplied);
                formImage.fill(Qt::transparent);
            }

            if (!castOps.isEmpty()) {
                QImage castImage = renderOperationsToImage(castOps, size);
                applySoftEdgeDiffusion(castImage, 1);
                QPainter p(&formImage);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.drawImage(0, 0, castImage);
            }

            layerImage = formImage;

            if (clipShadingToFlats && hasFlats) {
                QPainter clipPainter(&layerImage);
                clipPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                clipPainter.drawImage(0, 0, flatsImage);
            }
        } else if (isHighlights) {
            if (clipShadingToFlats && hasFlats) {
                QPainter clipPainter(&layerImage);
                clipPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                clipPainter.drawImage(0, 0, flatsImage);
            }
        }

        if (isShading) {
            compPainter.setCompositionMode(QPainter::CompositionMode_Multiply);
        } else if (isHighlights) {
            compPainter.setCompositionMode(QPainter::CompositionMode_Screen);
        } else if (isFx) {
            compPainter.setCompositionMode(QPainter::CompositionMode_Plus);
        } else {
            compPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }

        compPainter.drawImage(0, 0, layerImage);
    }

    compPainter.end();

    if (program.stepPhase.compare(QLatin1String("finishing"), Qt::CaseInsensitive) == 0 ||
        (program.goalReached && program.currentStep >= program.totalSteps)) {
        applyFinishingPostProcess(compositeImage);
    }

    return compositeImage;
}

#ifndef AI_STROKE_STANDALONE
bool KisAiStrokeRenderer::renderProgramToLayers(KisImageWSP image,
                                                KisViewManager *viewManager,
                                                const KisAiStrokeProgram &program,
                                                QString *statusMessage,
                                                bool clipShadingToFlats,
                                                qreal trappingPx)
{
    return renderProgramToLayers(image, viewManager, program, statusMessage, clipShadingToFlats, nullptr, trappingPx);
}

bool KisAiStrokeRenderer::renderProgramToLayers(KisImageWSP image,
                                                KisViewManager *viewManager,
                                                const KisAiStrokeProgram &program,
                                                QString *statusMessage,
                                                bool clipShadingToFlats,
                                                const KisAiStrokeProgram *inheritedFlatsProgram,
                                                qreal trappingPx)
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

    // A2: Apply Flats trapping (slight dilation) to tuck under Lineart strokes and eliminate white underfill seams
    const qreal minDim = qMin(canvasSize.width(), canvasSize.height());
    const qreal effectiveTrapping = trappingPx < 0.0 ? qMax<qreal>(1.0, minDim / 1000.0 * 1.5) : trappingPx;
    KisAiStrokeProgram activeProgram = program;
    activeProgram.canvasSize = canvasSize;
    if (effectiveTrapping > 0.0) {
        activeProgram = KisAiStrokeQualityUtils::applyTrapping(activeProgram, effectiveTrapping);
    }

    const QStringList layerOrder = {QStringLiteral("Background"),
                                    QStringLiteral("Flats"),
                                    QStringLiteral("Shading"),
                                    QStringLiteral("Lineart"),
                                    QStringLiteral("Highlights"),
                                    QStringLiteral("FX")};

    const QVector<KisAiStrokeOperation> expandedOps = expandProceduralOperations(activeProgram.operations, canvasSize);
    QMap<QString, QVector<KisAiStrokeOperation>> layerBuckets;
    for (const KisAiStrokeOperation &op : expandedOps) {
        const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
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

    if (layerBuckets.isEmpty()) {
        if (statusMessage) {
            *statusMessage = i18n("描画可能なストローク操作がありませんでした。");
        }
        return false;
    }

    // Guard against deadlocks: if the user is currently drawing a brush stroke or
    // the image scheduler is processing background stroke jobs, calling beginMacro()
    // directly would invoke KisLegacyUndoAdapter's barrierLock() which deadlocks
    // the GUI thread because mouse/tablet release events cannot be dispatched.
    // We request stroke completion and spin the event loop safely to allow pending
    // input events to drain without freezing the UI.
    // Every acquired lock must reach unlock(): holding it while returning false
    // would leave the image barrier-locked for the rest of the session.
    bool locked = image->tryBarrierLock();
    if (!locked) {
        image->requestStrokeEnd();
        constexpr int MAX_ATTEMPTS = 50;
        for (int attempts = 0; !locked && attempts < MAX_ATTEMPTS; ++attempts) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(20);
            locked = image->tryBarrierLock();
        }
        if (!locked) {
            if (statusMessage) {
                *statusMessage = i18n("キャンバスが描画中のため、AIストロークをレイヤーに追加できませんでした。少し待ってから再度お試しください。");
            }
            return false;
        }
    }
    image->unlock();

    KisNodeSP root = image->root();
    KisNodeSP aboveNode = root->lastChild();
    KisNodeCommandsAdapter adapter(viewManager);
    adapter.beginMacro(kundo2_i18n("AI Illustration"));

    // Group layer for clean encapsulation with pass-through mode enabled
    // so that child layers with non-normal composite modes (e.g. Shading with Multiply
    // and Highlights with Screen) blend directly through to layers beneath the group.
    // V3 Phase 3.1: Single Artboard enforcement.
    // When executing Goal Mode (totalSteps > 1) or iterative refinements, reuse existing
    // "🎨 AI Illustration" group layer instead of proliferating groups per step.
    KisGroupLayerSP group = nullptr;
    if (program.totalSteps > 1) {
        KisNodeSP candidate = root->firstChild();
        while (candidate) {
            if (candidate->inherits("KisGroupLayer") && candidate->name().startsWith(QStringLiteral("🎨 AI"))) {
                group = dynamic_cast<KisGroupLayer*>(candidate.data());
                break;
            }
            candidate = candidate->nextSibling();
        }
    }

    if (!group) {
        const QString groupTitle = (program.totalSteps > 1)
            ? QStringLiteral("🎨 AI Illustration")
            : QStringLiteral("🎨 AI: %1").arg(program.prompt.left(24).trimmed());
        group = new KisGroupLayer(image.data(), groupTitle, OPACITY_OPAQUE_U8);
        group->setPassThroughMode(true);
        adapter.addNode(group, root, aboveNode);
    }
    KisNodeSP childAboveNode = nullptr;

    QImage flatsImage;
    bool hasFlats = false;
    if (clipShadingToFlats && inheritedFlatsProgram) {
        QVector<KisAiStrokeOperation> inheritedFlats;
        for (const KisAiStrokeOperation &op : inheritedFlatsProgram->operations) {
            if (KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Flats")) {
                inheritedFlats.append(op);
            }
        }
        if (!inheritedFlats.isEmpty()) {
            flatsImage = renderOperationsToImage(inheritedFlats, canvasSize);
            hasFlats = true;
        }
    }
    // Phase 1: Build Face & Feature Exclusion Mask to prevent particles from falling on facial features
    QPainterPath faceExclusionPath;
    for (const KisAiStrokeOperation &op : activeProgram.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
            const QPointF pt = scalePoint(op.eyeCenter, canvasSize);
            const qreal ew = op.eyeSize.width() * canvasSize.width();
            const qreal eh = op.eyeSize.height() * canvasSize.height();
            faceExclusionPath.addEllipse(QRectF(pt.x() - ew * 1.5, pt.y() - eh * 1.5, ew * 3.0, eh * 3.0));
        } else if (op.kind == KisAiStrokeOperation::Kind::Fill) {
            const QString lowerId = op.id.toLower();
            if ((lowerId.contains(QLatin1String("skin")) || lowerId.contains(QLatin1String("face")) || lowerId.contains(QLatin1String("head"))) && op.polygon.size() >= 3) {
                faceExclusionPath.addPolygon(scalePolygon(op.polygon, canvasSize));
            }
        }
    }

    int layersAdded = 0;

    // Accumulate the composite the same way renderProgramToImage() does, so the
    // finishing-step bloom map can be derived from it instead of re-running the
    // whole rasterization pipeline a second time.
    QImage bloomSource;
    const bool needsBloomSource =
        program.stepPhase.compare(QLatin1String("finishing"), Qt::CaseInsensitive) == 0 ||
        (program.goalReached && program.currentStep >= program.totalSteps);
    if (needsBloomSource) {
        bloomSource = QImage(canvasSize, QImage::Format_ARGB32_Premultiplied);
        bloomSource.fill(Qt::transparent);
    }

    for (const QString &layerKey : orderedLayers) {
        const QVector<KisAiStrokeOperation> &ops = layerBuckets[layerKey];
        if (ops.isEmpty())
            continue;

        const bool isFlats = (layerKey.compare(QLatin1String("Flats"), Qt::CaseInsensitive) == 0);
        const bool isShading = (layerKey.compare(QLatin1String("Shading"), Qt::CaseInsensitive) == 0);
        const bool isHighlights = (layerKey.compare(QLatin1String("Highlights"), Qt::CaseInsensitive) == 0);
        const bool isBackground = (layerKey.compare(QLatin1String("Background"), Qt::CaseInsensitive) == 0);
        const bool isFx = (layerKey.compare(QLatin1String("FX"), Qt::CaseInsensitive) == 0);

        QImage layerImage = isFx ? renderOperationsToImage(ops, canvasSize, faceExclusionPath) : renderOperationsToImage(ops, canvasSize);

        if (isFlats) {
            flatsImage = layerImage;
            hasFlats = true;
        } else if (isShading) {
            // Dual shadow separation (Phase 3): separate sharp cast shadows and soft form shadows
            QVector<KisAiStrokeOperation> formOps;
            QVector<KisAiStrokeOperation> castOps;
            formOps.reserve(ops.size());
            castOps.reserve(ops.size());

            for (const KisAiStrokeOperation &shOp : ops) {
                if (shOp.kind == KisAiStrokeOperation::Kind::Fill &&
                    KisAiStrokeQualityUtils::isCastShadow(shOp.polygon, canvasSize)) {
                    castOps.append(shOp);
                } else {
                    formOps.append(shOp);
                }
            }

            QImage formImage;
            if (!formOps.isEmpty()) {
                formImage = renderOperationsToImage(formOps, canvasSize);
                applySoftEdgeDiffusion(formImage, 4);
            } else {
                formImage = QImage(canvasSize, QImage::Format_ARGB32_Premultiplied);
                formImage.fill(Qt::transparent);
            }

            if (!castOps.isEmpty()) {
                QImage castImage = renderOperationsToImage(castOps, canvasSize);
                applySoftEdgeDiffusion(castImage, 1);
                QPainter p(&formImage);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.drawImage(0, 0, castImage);
            }

            layerImage = formImage;

            if (clipShadingToFlats && hasFlats) {
                QPainter clipPainter(&layerImage);
                clipPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                clipPainter.drawImage(0, 0, flatsImage);
            }
        } else if (clipShadingToFlats && hasFlats && isHighlights) {
            // Apply clipping mask to Flats silhouette:
            // Highlights will stay strictly within the filled regions of Flats
            QPainter clipPainter(&layerImage);
            clipPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            clipPainter.drawImage(0, 0, flatsImage);
        }

        if (needsBloomSource && !bloomSource.isNull()) {
            QPainter bloomPainter(&bloomSource);
            if (isShading) {
                bloomPainter.setCompositionMode(QPainter::CompositionMode_Multiply);
            } else if (isHighlights) {
                bloomPainter.setCompositionMode(QPainter::CompositionMode_Screen);
            } else if (isFx) {
                bloomPainter.setCompositionMode(QPainter::CompositionMode_Plus);
            } else {
                bloomPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            }
            bloomPainter.drawImage(0, 0, layerImage);
        }

        const QString layerTitle = QStringLiteral("AI: %1").arg(layerKey);
        KisPaintLayerSP layer = nullptr;
        KisNodeSP existingChild = group->firstChild();
        while (existingChild) {
            if (existingChild->inherits("KisPaintLayer") && existingChild->name() == layerTitle) {
                layer = dynamic_cast<KisPaintLayer*>(existingChild.data());
                break;
            }
            existingChild = existingChild->nextSibling();
        }

        if (layer) {
            layer->paintDevice()->clear();
            layer->paintDevice()->convertFromQImage(layerImage, nullptr);
            layer->setDirty(bounds);
        } else {
            layer = new KisPaintLayer(image, layerTitle, OPACITY_OPAQUE_U8);
            layer->paintDevice()->convertFromQImage(layerImage, nullptr);

            if (isShading) {
                layer->setCompositeOpId(COMPOSITE_MULT);
                layer->setColorLabelIndex(7); // Purple
            } else if (isHighlights) {
                // A2b: Screen blend mode matching preview and preventing harsh blowout/disappearance
                layer->setCompositeOpId(COMPOSITE_SCREEN);
                layer->setColorLabelIndex(3); // Yellow
            } else if (isFlats) {
                layer->setCompositeOpId(COMPOSITE_OVER);
                layer->setColorLabelIndex(1); // Blue
            } else if (isBackground) {
                layer->setCompositeOpId(COMPOSITE_OVER);
                layer->setColorLabelIndex(8); // Grey
            } else if (layerKey.compare(QLatin1String("Lineart"), Qt::CaseInsensitive) == 0) {
                layer->setCompositeOpId(COMPOSITE_OVER);
                layer->setColorLabelIndex(4); // Orange
            } else if (isFx) {
                layer->setCompositeOpId(COMPOSITE_ADD); // Additive blending for floating particles & sparkles
                layer->setColorLabelIndex(2); // Green (FX)
            } else {
                layer->setCompositeOpId(COMPOSITE_OVER);
                layer->setColorLabelIndex(2); // Green (FX)
            }

            layer->setDirty(bounds);
            adapter.addNode(layer, group, childAboveNode);
        }
        childAboveNode = layer;
        ++layersAdded;
    }

    if (program.stepPhase.compare(QLatin1String("finishing"), Qt::CaseInsensitive) == 0 ||
        (program.goalReached && program.currentStep >= program.totalSteps)) {
        // B4: Generate isolated, non-destructive Cinematic Bloom Layer on real canvas.
        // The composite was accumulated while rendering the layers above, so no
        // second full rasterization pass is needed here.
        QImage compPreview = bloomSource;
        if (compPreview.isNull()) {
            compPreview = renderProgramToImage(activeProgram, canvasSize, clipShadingToFlats, inheritedFlatsProgram, effectiveTrapping);
        }
        if (!compPreview.isNull()) {
            QImage bloomGlow = generateBloomMap(compPreview, 0.40, 8);
            if (!bloomGlow.isNull()) {
                KisPaintLayerSP bloomLayer = nullptr;
                KisNodeSP exBloom = group->firstChild();
                while (exBloom) {
                    if (exBloom->name() == QStringLiteral("🎨 AI: Bloom FX")) {
                        bloomLayer = dynamic_cast<KisPaintLayer*>(exBloom.data());
                        break;
                    }
                    exBloom = exBloom->nextSibling();
                }
                if (bloomLayer) {
                    bloomLayer->paintDevice()->clear();
                    bloomLayer->paintDevice()->convertFromQImage(bloomGlow, nullptr);
                    bloomLayer->setDirty(bounds);
                } else {
                    // 40% opacity Screen blending for soft atmospheric glow
                    bloomLayer = new KisPaintLayer(image, QStringLiteral("🎨 AI: Bloom FX"), qRound(0.40 * 255));
                    bloomLayer->paintDevice()->convertFromQImage(bloomGlow, nullptr);
                    bloomLayer->setCompositeOpId(COMPOSITE_SCREEN);
                    bloomLayer->setColorLabelIndex(3); // Yellow
                    bloomLayer->setDirty(bounds);
                    adapter.addNode(bloomLayer, group, childAboveNode);
                }
                childAboveNode = bloomLayer;
                ++layersAdded;
            }
        }

        // V3 Phase 2.5: Color Grading Real-canvas Layer
        QImage gradeImg(canvasSize, QImage::Format_ARGB32_Premultiplied);
        gradeImg.fill(Qt::transparent);
        {
            QPainter pGrade(&gradeImg);
            QLinearGradient grad(0, 0, 0, canvasSize.height());
            grad.setColorAt(0.0, QColor(30, 45, 80, 28));   // cool ambient overhead
            grad.setColorAt(1.0, QColor(255, 210, 160, 22)); // warm bounce light
            pGrade.fillRect(QRect(QPoint(0, 0), canvasSize), grad);
        }
        KisPaintLayerSP gradeLayer = nullptr;
        KisNodeSP exGrade = group->firstChild();
        while (exGrade) {
            if (exGrade->name() == QStringLiteral("🎨 AI: Grade")) {
                gradeLayer = dynamic_cast<KisPaintLayer*>(exGrade.data());
                break;
            }
            exGrade = exGrade->nextSibling();
        }
        if (gradeLayer) {
            gradeLayer->paintDevice()->clear();
            gradeLayer->paintDevice()->convertFromQImage(gradeImg, nullptr);
            gradeLayer->setDirty(bounds);
        } else {
            gradeLayer = new KisPaintLayer(image, QStringLiteral("🎨 AI: Grade"), qRound(0.50 * 255));
            gradeLayer->paintDevice()->convertFromQImage(gradeImg, nullptr);
            gradeLayer->setCompositeOpId(COMPOSITE_OVER);
            gradeLayer->setColorLabelIndex(6); // Violet
            gradeLayer->setDirty(bounds);
            adapter.addNode(gradeLayer, group, childAboveNode);
        }
        childAboveNode = gradeLayer;
        ++layersAdded;
    }

    adapter.endMacro();

    image->refreshGraphAsync();
    if (viewManager) {
        viewManager->updateGUI();
    }

    if (viewManager && viewManager->document()) {
        viewManager->document()->setModified(true);
    } else {
        image->setModifiedWithoutUndo();
    }

    if (statusMessage) {
        *statusMessage =
            i18n("%1 個の AI 作画レイヤーを生成しました（%2 ストローク）。", layersAdded, program.operations.size());
    }

    return layersAdded > 0;
}
#endif

QImage KisAiStrokeRenderer::renderOperationsToImage(const QVector<KisAiStrokeOperation> &operations,
                                                    const QSize &canvasSize,
                                                    const QPainterPath &faceExclusionPath)
{
    // D0: meaning-aware supersampling — faces/eyes deserve 3x on modest
    // canvases while plain backgrounds stay cheap. Memory-bounded.
    const int scale = KisAiDeliberateStroke::adaptiveSupersampleScale(operations, canvasSize);
    const QSize workingSize(canvasSize.width() * scale, canvasSize.height() * scale);
    QImage working(workingSize, QImage::Format_ARGB32_Premultiplied);
    working.fill(Qt::transparent);

    QPainterPath scaledFacePath = faceExclusionPath;
    if (scale != 1 && !scaledFacePath.isEmpty()) {
        QTransform tr;
        tr.scale(scale, scale);
        scaledFacePath = tr.map(scaledFacePath);
    }

    // D1: deliberate paint order — large masses first, facial details last.
    const QVector<KisAiStrokeOperation> orderedOps =
        KisAiDeliberateStroke::orderOperationsForRendering(operations, canvasSize);

    {
        QPainter painter(&working);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const KisAiStrokeOperation &op : orderedOps) {
            // D1: per-stroke gate — degenerate/off-canvas strokes never reach ink.
            const KisAiStrokeLintReport lint = KisAiDeliberateStroke::lintStroke(op, canvasSize);
            if (lint.drop)
                continue;
            rasterizeOperation(painter, op, workingSize, scale, scaledFacePath);
        }
    }

    if (scale == 1)
        return working;
    return working.scaled(canvasSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void KisAiStrokeRenderer::rasterizeOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize, int supersampleScale, const QPainterPath &faceExclusionPath)
{
    painter.save();

    if (op.brush.isEraser) {
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
    } else {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    switch (op.kind) {
    case KisAiStrokeOperation::Kind::Path:
        drawPathOperation(painter, op, canvasSize, supersampleScale);
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
        drawParticlesOperation(painter, op, canvasSize, supersampleScale, faceExclusionPath);
        break;
    case KisAiStrokeOperation::Kind::Hatch:
        drawHatchOperation(painter, op, canvasSize, supersampleScale);
        break;
    case KisAiStrokeOperation::Kind::MangaLines:
        drawMangaLinesOperation(painter, op, canvasSize, supersampleScale);
        break;
    case KisAiStrokeOperation::Kind::AnimeEye:
        drawAnimeEyeOperation(painter, op, canvasSize, supersampleScale);
        break;
    default:
        break;
    }

    painter.restore();
}

static bool renderFineLineStroke(
    QPainter &painter,
    const QVector<SampledStrokePoint> &curveSamples,
    const KisAiStrokeOperation &op,
    const QColor &color,
    const QSize &canvasSize,
    int supersampleScale)
{
    Q_UNUSED(canvasSize);
    const int sampleCount = curveSamples.size();
    if (sampleCount < 2)
        return false;

    const QString profile = op.brush.profile.toLower();
    const bool isExplicitFine = (profile == QLatin1String("fineliner") || profile == QLatin1String("maru_pen")
        || profile == QLatin1String("feathering") || profile == QLatin1String("stipple"));

    // Check maximum and average stroke width
    qreal maxW = 0.0;
    qreal avgW = 0.0;
    for (const SampledStrokePoint &s : curveSamples) {
        maxW = qMax(maxW, s.width);
        avgW += s.width;
    }
    avgW /= qMax(1, sampleCount);

    const bool isSpecialEffect = (profile == QLatin1String("airbrush") || profile == QLatin1String("neon")
        || profile == QLatin1String("splatter") || profile == QLatin1String("watercolor")
        || profile == QLatin1String("charcoal") || profile == QLatin1String("crayon")
        || profile == QLatin1String("marker"));

    const bool isThinStroke = (maxW <= 2.6 * supersampleScale && !isSpecialEffect);

    if (!isExplicitFine && !isThinStroke) {
        return false; // Let ribbonPoly handle wider painterly strokes
    }

    // --- High-fidelity Subpixel Vector Path Rendering ---
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (profile == QLatin1String("stipple")) {
        // Delicate ink stipples along curve
        QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/stipple")));
        painter.setPen(Qt::NoPen);
        QColor dotCol = color;
        const int dotCount = qBound(sampleCount * 2, sampleCount * 4, 300);
        for (int i = 0; i < dotCount; ++i) {
            const int idx = rng.bounded(sampleCount);
            const QPointF &pt = curveSamples.at(idx).pos;
            const qreal w = qMax<qreal>(0.8, curveSamples.at(idx).width);
            const qreal spread = (rng.generateDouble() - 0.5) * w * 1.6;
            const qreal r = qMax<qreal>(0.4, w * 0.22 * (0.6 + rng.generateDouble() * 0.6));
            dotCol.setAlphaF(qBound<qreal>(0.1, color.alphaF() * (0.4 + rng.generateDouble() * 0.6), 1.0));
            painter.setBrush(dotCol);
            painter.drawEllipse(pt + QPointF(spread, spread * 0.8), r, r);
        }
        return true;
    }

    if (profile == QLatin1String("feathering")) {
        // Multi-strand layered sketch touch with organic filament tremor
        QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/feather")));
        const int strands = 3;
        for (int s = 0; s < strands; ++s) {
            QPolygonF strandPath;
            strandPath.reserve(sampleCount);
            const qreal lateral = (rng.generateDouble() - 0.5) * avgW * 0.7;
            for (int i = 0; i < sampleCount; ++i) {
                const qreal jitter = (rng.generateDouble() - 0.5) * 0.35;
                strandPath.append(curveSamples.at(i).pos + QPointF(lateral + jitter, jitter));
            }
            QColor featherCol = color;
            featherCol.setAlphaF(qBound<qreal>(0.05, color.alphaF() * 0.42, 1.0));
            QPen pen(featherCol, qMax<qreal>(0.5, avgW * 0.45), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(strandPath);
        }
        return true;
    }

    // For fineliner, maru_pen, or thin strokes:
    // Render with varying width segment-by-segment for natural pressure taper,
    // avoiding polygon bowtie spikes entirely.
    for (int i = 0; i < sampleCount - 1; ++i) {
        const QPointF &p1 = curveSamples.at(i).pos;
        const QPointF &p2 = curveSamples.at(i + 1).pos;
        const qreal w = qMax<qreal>(0.5, (curveSamples.at(i).width + curveSamples.at(i + 1).width) * 0.5);

        // Sharp tapering for fineliner/maru_pen at endpoints (natural flick / 抜き)
        qreal tipFactor = 1.0;
        if ((profile == QLatin1String("fineliner") || profile == QLatin1String("maru_pen")) && !op.closed) {
            if (i < 3) {
                tipFactor = qBound<qreal>(0.35, (i + 1) / 3.0, 1.0);
            } else if (i > sampleCount - 4) {
                tipFactor = qBound<qreal>(0.30, (sampleCount - 1 - i) / 3.0, 1.0);
            }
        }

        QPen pen(color, w * tipFactor, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(p1, p2);
    }

    return true;
}

void KisAiStrokeRenderer::drawPathOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize, int supersampleScale)
{
    if (op.points.isEmpty())
        return;

    QColor color = op.brush.color;
    color.setAlphaF(qBound<qreal>(0.0, op.brush.opacity * color.alphaF(), 1.0));

    // Single point: render as crisp dab / dot
    if (op.points.size() == 1) {
        const QPointF pt = scalePoint(op.points.at(0).pos, canvasSize);
        const qreal r = effectiveBrushWidth(op.brush, op.points.at(0).pressure, canvasSize, supersampleScale) * 0.5;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(pt, r, r);
        return;
    }

    // D0: deliberate pre-pass — jitter removal + uniform resampling so long
    // LLM spans and dense facial clusters share one clean representation.
    const QVector<KisAiStrokePoint> stablePoints =
        KisAiDeliberateStroke::stabilizeStroke(op.points, canvasSize, op.closed,
                                               KisAiStrokeProgramCodec::stableSeed(op.id));
    const KisAiStrokeLintReport lint = KisAiDeliberateStroke::lintStroke(op, canvasSize);
    if (lint.drop) {
        return; // micro/off-canvas/degenerate strokes never reach ink
    }

    const int n = stablePoints.size();

    // Scale input points and extract pressures
    QVector<QPointF> scaledPts;
    QVector<qreal> pressures;
    scaledPts.reserve(n);
    pressures.reserve(n);
    for (const KisAiStrokePoint &pt : stablePoints) {
        scaledPts.append(scalePoint(pt.pos, canvasSize));
        pressures.append(qBound<qreal>(0.05, pt.pressure, 1.0));
    }
    // Adapt sampling to on-canvas segment length. Long LLM spans otherwise
    // facet visibly while tiny facial details are needlessly oversampled.
    const int segments = op.closed ? n : (n - 1);

    QVector<SampledStrokePoint> curveSamples;
    // subdivisions peaks at 24 for smooth paths; 12 under-reserved and forced
    // mid-loop reallocation on every long stroke.
    curveSamples.reserve(segments * 24 + 1);
    // D2-1: speed-coupled ink — slow passages pool darker/wider, fast
    // passages skip thinner (deterministic, from segment length).
    qreal segLenAvg = 0.0;
    {
        qreal total = 0.0;
        for (int i = 0; i < segments; ++i) {
            const QPointF a = (i < scaledPts.size()) ? scaledPts.at(i % scaledPts.size()) : QPointF();
            const QPointF b = scaledPts.at((i + 1) % scaledPts.size());
            total += std::hypot(b.x() - a.x(), b.y() - a.y());
        }
        segLenAvg = segments > 0 ? total / segments : 1.0;
    }

    for (int i = 0; i < segments; ++i) {
        QPointF p0, p1, p2, p3;
        qreal pr1, pr2;

        if (op.closed) {
            const int idx0 = (i - 1 + n) % n;
            const int idx1 = i;
            const int idx2 = (i + 1) % n;
            const int idx3 = (i + 2) % n;
            p0 = scaledPts.at(idx0);
            p1 = scaledPts.at(idx1);
            pr1 = pressures.at(idx1);
            p2 = scaledPts.at(idx2);
            pr2 = pressures.at(idx2);
            p3 = scaledPts.at(idx3);
        } else {
            p1 = scaledPts.at(i);
            pr1 = pressures.at(i);
            p2 = scaledPts.at(i + 1);
            pr2 = pressures.at(i + 1);
            p0 = (i > 0) ? scaledPts.at(i - 1) : (p1 + (p1 - p2));
            p3 = (i + 2 < n) ? scaledPts.at(i + 2) : (p2 + (p2 - p1));
        }

        const qreal segmentLength = std::hypot(p2.x() - p1.x(), p2.y() - p1.y());
        const int subdivisions =
            op.smooth && n >= 3 ? qBound(4, qCeil(segmentLength / 6.0), 24) : qBound(1, qCeil(segmentLength / 8.0), 16);
        for (int step = 0; step < subdivisions; ++step) {
            const qreal t = qreal(step) / subdivisions;
            const QPointF position = op.smooth && n >= 3 ? centripetalPoint(p0, p1, p2, p3, t) : p1 + (p2 - p1) * t;

            // Smoothstep is monotone, so malformed pressure anchors cannot
            // create negative widths or bulges between samples.
            const qreal pressureT = op.smooth ? t * t * (3.0 - 2.0 * t) : t;
            const qreal p = pr1 + (pr2 - pr1) * pressureT;

            // Global arc progress for tapering
            const qreal globalT = (qreal(i) + t) / qreal(segments);

            // Natural stroke taper according to brush profile
            const qreal taper = KisAiStrokeQualityUtils::calculateTaper(globalT, op.brush.profile, op.closed);

            // D2-1 speed coupling: +8% width pooled when slow, −12% skipped when fast.
            const qreal speedRatio = segLenAvg > 1.0e-6 ? segmentLength / segLenAvg : 1.0;
            const qreal speedGain = qBound<qreal>(0.88, 1.0 + (1.0 - qMin<qreal>(speedRatio, 2.0)) * 0.10, 1.08);

            qreal strokeW = effectiveBrushWidth(op.brush, p * taper, canvasSize, supersampleScale) * speedGain;

            // If calligraphy profile, modulate width based on tangent vector
            if (op.brush.profile.compare(QLatin1String("calligraphy"), Qt::CaseInsensitive) == 0) {
                const QPointF tangent = (i < segments - 1) ? (scaledPts.at(i + 1) - scaledPts.at(i)) : (p2 - p1);
                strokeW = KisAiStrokeQualityUtils::calculateCalligraphyWidth(tangent, strokeW, 45.0, 0.20);
            }

            curveSamples.append({position, strokeW});
        }
    }

    if (!op.closed) {
        const qreal endTaper = KisAiStrokeQualityUtils::calculateTaper(1.0, op.brush.profile, false);
        curveSamples.append({scaledPts.last(), effectiveBrushWidth(op.brush, pressures.last() * endTaper, canvasSize, supersampleScale)});
    }

    const int sampleCount = curveSamples.size();
    if (sampleCount < 2)
        return;

    // High-precision subpixel fine line rendering for delicate ink strokes and thin contours
    if (renderFineLineStroke(painter, curveSamples, op, color, canvasSize, supersampleScale)) {
        return;
    }

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
    const auto drawRoundJoins = [&](const QColor &joinColor, qreal radiusScale) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(joinColor);
        for (const SampledStrokePoint &sample : curveSamples) {
            const qreal radius = qMax<qreal>(0.25, sample.width * 0.5 * radiusScale);
            painter.drawEllipse(sample.pos, radius, radius);
        }
    };

    // Brush profile texture & dynamics
    if (profile == QLatin1String("airbrush")) {
        // Multi-pass soft falloff
        painter.setPen(Qt::NoPen);
        QColor cOuter = color;
        cOuter.setAlphaF(color.alphaF() * 0.12);
        QColor cMid = color;
        cMid.setAlphaF(color.alphaF() * 0.35);

        // Outer soft glow
        QPolygonF outerPoly;
        for (int i = 0; i < sampleCount; ++i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width * 1.8;
            QPointF tangent =
                (i < sampleCount - 1) ? (curveSamples.at(i + 1).pos - curr) : (curr - curveSamples.at(i - 1).pos);
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            QPointF normal = (tLen > 0.0001) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);
            outerPoly.append(curr + normal * w);
        }
        for (int i = sampleCount - 1; i >= 0; --i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width * 1.8;
            QPointF tangent =
                (i < sampleCount - 1) ? (curveSamples.at(i + 1).pos - curr) : (curr - curveSamples.at(i - 1).pos);
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            QPointF normal = (tLen > 0.0001) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);
            outerPoly.append(curr - normal * w);
        }
        painter.setBrush(cOuter);
        painter.drawPolygon(outerPoly);

        // Mid body
        painter.setBrush(cMid);
        painter.drawPolygon(ribbonPoly);

        // Inner core (narrower width)
        QPolygonF innerPoly;
        innerPoly.reserve(sampleCount * 2);
        for (int i = 0; i < sampleCount; ++i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width * 0.45;
            QPointF tangent =
                (i < sampleCount - 1) ? (curveSamples.at(i + 1).pos - curr) : (curr - curveSamples.at(i - 1).pos);
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            QPointF normal = (tLen > 0.0001) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);
            innerPoly.append(curr + normal * w);
        }
        for (int i = sampleCount - 1; i >= 0; --i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width * 0.45;
            QPointF tangent =
                (i < sampleCount - 1) ? (curveSamples.at(i + 1).pos - curr) : (curr - curveSamples.at(i - 1).pos);
            const qreal tLen = std::hypot(tangent.x(), tangent.y());
            QPointF normal = (tLen > 0.0001) ? QPointF(-tangent.y() / tLen, tangent.x() / tLen) : QPointF(0, 1);
            innerPoly.append(curr - normal * w);
        }
        painter.setBrush(color);
        painter.drawPolygon(innerPoly);
        if (!curveSamples.isEmpty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            const qreal rStart = qMax<qreal>(0.25, curveSamples.first().width * 0.45 * 0.9);
            painter.drawEllipse(curveSamples.first().pos, rStart, rStart);
            const qreal rEnd = qMax<qreal>(0.25, curveSamples.last().width * 0.45 * 0.9);
            painter.drawEllipse(curveSamples.last().pos, rEnd, rEnd);
        }

    } else if (profile == QLatin1String("watercolor")) {
        // Transparent wash with subtle water-fringe contour (wet edge effect)
        painter.setPen(Qt::NoPen);
        QColor washColor = color;
        washColor.setAlphaF(color.alphaF() * 0.65);
        painter.setBrush(washColor);
        painter.drawPolygon(ribbonPoly);

        // Darkened wet-edge fringe boundary along stroke contours
        QColor fringe = color;
        fringe.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.92, 1.0));
        QPen fringePen(fringe, qMax<qreal>(0.8, effectiveBrushWidth(op.brush, 0.5, canvasSize, supersampleScale) * 0.12));
        painter.setPen(fringePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(leftEdge);
        painter.drawPolyline(rightEdge);
        drawRoundJoins(fringe, 0.4);

        // D2-2: paper grain — sparse deterministic tooth inside the wash.
        {
            QRandomGenerator grain(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/paper")));
            painter.setPen(Qt::NoPen);
            QColor grainDot = color;
            grainDot.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.10, 1.0));
            painter.setBrush(grainDot);
            const QRectF washBounds = ribbonPoly.boundingRect();
            const int grainCount = qBound(8, sampleCount * 2, 120);
            for (int g = 0; g < grainCount; ++g) {
                const qreal gx = washBounds.left() + grain.generateDouble() * washBounds.width();
                const qreal gy = washBounds.top() + grain.generateDouble() * washBounds.height();
                if (!ribbonPoly.containsPoint(QPointF(gx, gy), Qt::OddEvenFill))
                    continue;
                const qreal gr = qMax<qreal>(0.4, effectiveBrushWidth(op.brush, 0.3, canvasSize, supersampleScale) * 0.08);
                painter.drawEllipse(QPointF(gx, gy), gr, gr);
            }
        }

    } else if (profile == QLatin1String("brush")) {
        // Rich artistic hair/oil brush mark with bristle strands and stroke direction
        painter.setPen(Qt::NoPen);
        QColor bodyColor = color;
        bodyColor.setAlphaF(color.alphaF() * 0.72);
        painter.setBrush(bodyColor);
        painter.drawPolygon(ribbonPoly);
        drawRoundJoins(bodyColor, 0.95);

        // Bristle strands layering
        QVector<KisAiStrokePoint> strokeSpine;
        strokeSpine.reserve(sampleCount);
        for (const SampledStrokePoint &sample : curveSamples) {
            strokeSpine.append(KisAiStrokePoint(sample.pos.x(), sample.pos.y(), 0.8));
        }

        const qreal avgW = effectiveBrushWidth(op.brush, 0.7, canvasSize, supersampleScale);
        const quint32 brushSeed = KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/bristles"));
        const auto strands = KisAiStrokeQualityUtils::generateBristleStrands(strokeSpine, 5, avgW * 0.40, brushSeed);

        QColor strandColor = color;
        strandColor.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.35, 1.0));
        QPen strandPen(strandColor, qMax<qreal>(0.6, avgW * 0.15), Qt::SolidLine, Qt::RoundCap);
        painter.setPen(strandPen);
        painter.setBrush(Qt::NoBrush);

        for (const QVector<QPointF> &strandPath : strands) {
            if (strandPath.size() >= 2) {
                painter.drawPolyline(strandPath);
            }
        }

    } else if (profile == QLatin1String("calligraphy")) {
        // Elegant flat chisel nib with angle-modulated variation
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(ribbonPoly);

        // Crisp chisel edge
        QColor edgeColor = color;
        edgeColor.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.50, 1.0));
        QPen edgePen(edgeColor, qMax<qreal>(0.8, effectiveBrushWidth(op.brush, 0.4, canvasSize, supersampleScale) * 0.20));
        painter.setPen(edgePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(leftEdge);

    } else if (profile == QLatin1String("charcoal")) {
        // Soft powdery charcoal with porous tooth texture and carbon flecks
        QColor charcoalBase = color;
        charcoalBase.setAlphaF(color.alphaF() * 0.60);
        painter.setPen(Qt::NoPen);
        painter.setBrush(charcoalBase);
        painter.drawPolygon(ribbonPoly);

        QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/charcoal")));
        painter.setPen(Qt::NoPen);

        for (int i = 0; i < sampleCount; ++i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width;
            for (int k = 0; k < 4; ++k) {
                const qreal rx = (rng.generateDouble() - 0.5) * w * 1.1;
                const qreal ry = (rng.generateDouble() - 0.5) * w * 1.1;
                const qreal rDot = qMax<qreal>(0.4, w * 0.12 * (0.4 + rng.generateDouble() * 0.6));
                QColor fleck = color;
                fleck.setAlphaF(qBound<qreal>(0.0, color.alphaF() * (0.2 + rng.generateDouble() * 0.5), 1.0));
                painter.setBrush(fleck);
                painter.drawEllipse(curr + QPointF(rx, ry), rDot, rDot);
            }
        }

    } else if (profile == QLatin1String("pencil")) {
        // A translucent graphite body plus deterministic fine filaments gives
        // hatching and sketch lines tooth without random frame-to-frame noise.
        QColor graphite = color;
        graphite.setAlphaF(color.alphaF() * 0.78);
        painter.setPen(Qt::NoPen);
        painter.setBrush(graphite);
        painter.drawPolygon(ribbonPoly);
        drawRoundJoins(graphite, 1.0);

        QRandomGenerator grain(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/pencil")));
        QColor filament = color;
        filament.setAlphaF(color.alphaF() * 0.28);
        QPen filamentPen(filament,
                         qMax<qreal>(0.45, effectiveBrushWidth(op.brush, 0.3, canvasSize, supersampleScale) * 0.18),
                         Qt::SolidLine,
                         Qt::RoundCap);
        painter.setPen(filamentPen);
        painter.setBrush(Qt::NoBrush);
        for (int pass = 0; pass < 3; ++pass) {
            QPolygonF filamentPath;
            filamentPath.reserve(sampleCount);
            const qreal offset = (grain.generateDouble() - 0.5) * effectiveBrushWidth(op.brush, 0.7, canvasSize, supersampleScale) * 0.45;
            for (int i = 0; i < sampleCount; ++i) {
                const QPointF normal = leftEdge.at(i) - rightEdge.at(i);
                const qreal normalLength = std::hypot(normal.x(), normal.y());
                const QPointF unitNormal = normalLength > 1.0e-5 ? normal / normalLength : QPointF();
                const qreal jitter = (grain.generateDouble() - 0.5) * 0.35;
                filamentPath.append(curveSamples.at(i).pos + unitNormal * (offset + jitter));
            }
            painter.drawPolyline(filamentPath);
        }

    } else if (profile == QLatin1String("marker")) {
        // Semi-flat marker with overlapping accumulation and chisel-like stroke body
        QColor markerColor = color;
        markerColor.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.72, 1.0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(markerColor);
        painter.drawPolygon(ribbonPoly);

        // Chisel edge accent
        QColor edgeColor = color;
        edgeColor.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.40, 1.0));
        QPen edgePen(edgeColor, qMax<qreal>(1.0, effectiveBrushWidth(op.brush, 0.5, canvasSize, supersampleScale) * 0.25));
        painter.setPen(edgePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(leftEdge);

    } else if (profile == QLatin1String("crayon")) {
        // Grainy, waxy textured crayon with grain dabs along contour
        QColor baseCrayon = color;
        baseCrayon.setAlphaF(color.alphaF() * 0.65);
        painter.setPen(Qt::NoPen);
        painter.setBrush(baseCrayon);
        painter.drawPolygon(ribbonPoly);

        QRandomGenerator grain(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/crayon")));
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < sampleCount; ++i) {
            const QPointF &curr = curveSamples.at(i).pos;
            const qreal w = curveSamples.at(i).width;
            for (int d = 0; d < 3; ++d) {
                const qreal rx = (grain.generateDouble() - 0.5) * w;
                const qreal ry = (grain.generateDouble() - 0.5) * w;
                const qreal dotR = qMax<qreal>(0.5, w * 0.15 * (0.5 + grain.generateDouble() * 0.5));
                QColor dotColor = color;
                dotColor.setAlphaF(qBound<qreal>(0.0, color.alphaF() * (0.3 + grain.generateDouble() * 0.5), 1.0));
                painter.setBrush(dotColor);
                painter.drawEllipse(curr + QPointF(rx, ry), dotR, dotR);
            }
        }

    } else if (profile == QLatin1String("neon")) {
        // Multi-pass neon tube: outer soft aura -> medium halo -> intense core -> bright white center
        painter.setPen(Qt::NoPen);

        // 1. Broad soft aura
        QColor cAura = color;
        cAura.setAlphaF(color.alphaF() * 0.18);
        painter.setBrush(cAura);
        for (const SampledStrokePoint &sample : curveSamples) {
            painter.drawEllipse(sample.pos, sample.width * 1.6, sample.width * 1.6);
        }

        // 2. Medium glow
        QColor cHalo = color;
        cHalo.setAlphaF(color.alphaF() * 0.45);
        painter.setBrush(cHalo);
        for (const SampledStrokePoint &sample : curveSamples) {
            painter.drawEllipse(sample.pos, sample.width * 0.9, sample.width * 0.9);
        }

        // 3. Colored core
        painter.setBrush(color);
        painter.drawPolygon(ribbonPoly);
        drawRoundJoins(color, 0.9);

        // 4. White hot filament center line
        QColor whiteCore(255, 255, 255);
        whiteCore.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.90, 1.0));
        QPen whitePen(whiteCore, qMax<qreal>(1.0, effectiveBrushWidth(op.brush, 0.5, canvasSize, supersampleScale) * 0.28),
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(whitePen);
        painter.setBrush(Qt::NoBrush);
        QPolygonF spinePoly;
        spinePoly.reserve(sampleCount);
        for (const SampledStrokePoint &sample : curveSamples) {
            spinePoly.append(sample.pos);
        }
        painter.drawPolyline(spinePoly);

    } else if (profile == QLatin1String("splatter")) {
        // Solid ink base path with fine ink splatters radiating outward
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(ribbonPoly);
        drawRoundJoins(color, 1.0);

        QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/splatter")));
        const int splatterCount = qBound(6, sampleCount * 2, 80);
        for (int s = 0; s < splatterCount; ++s) {
            const int sampleIdx = rng.bounded(sampleCount);
            const QPointF origin = curveSamples.at(sampleIdx).pos;
            const qreal w = curveSamples.at(sampleIdx).width;

            const qreal dist = w * (0.8 + rng.generateDouble() * 2.2);
            const qreal angle = rng.generateDouble() * 2.0 * PI;
            const QPointF dropPos = origin + QPointF(std::cos(angle) * dist, std::sin(angle) * dist);
            const qreal dropR = qMax<qreal>(0.6, w * (0.08 + rng.generateDouble() * 0.20));

            QColor dropColor = color;
            dropColor.setAlphaF(qBound<qreal>(0.0, color.alphaF() * (0.5 + rng.generateDouble() * 0.5), 1.0));
            painter.setBrush(dropColor);
            painter.drawEllipse(dropPos, dropR, dropR);
        }

    } else {
        // G-Pen / Default: solid crisp anti-aliased contour
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(ribbonPoly);
        drawRoundJoins(color, 1.0);
    }
}

void KisAiStrokeRenderer::drawFillOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize)
{
    if (op.polygon.size() < 3)
        return;

    QPolygonF poly = scalePolygon(op.polygon, canvasSize);

    // Smooth jagged polygon vertices to eliminate raw low-poly faceting
    if (poly.size() >= 3) {
        poly = KisAiStrokeQualityUtils::smoothPolygonCornerPreserving(poly, 135.0, 4);
    }

    QColor color = op.brush.color;
    color.setAlphaF(qBound<qreal>(0.0, op.brush.opacity * color.alphaF(), 1.0));

    const QString lowerId = op.id.toLower();
    const bool isBlush = lowerId.contains(QLatin1String("blush")) || lowerId.contains(QLatin1String("cheek"));
    const bool isHair = lowerId.contains(QLatin1String("hair"));
    const bool isSkin = lowerId.contains(QLatin1String("skin")) || lowerId.contains(QLatin1String("face"));
    const bool isNose = lowerId.contains(QLatin1String("nose"));

    // Phase 1: Special treatment for Blush: soft radial wash instead of harsh circular boundary
    if (isBlush) {
        const QRectF b = poly.boundingRect();
        QRadialGradient blushGrad(b.center(), qMax(b.width(), b.height()) * 0.55);
        QColor core = color;
        core.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.35, 0.45));
        QColor edge = color;
        edge.setAlphaF(0.0);
        blushGrad.setColorAt(0.0, core);
        blushGrad.setColorAt(0.5, core);
        blushGrad.setColorAt(1.0, edge);
        painter.setPen(Qt::NoPen);
        painter.setBrush(blushGrad);
        painter.drawPolygon(poly);
        return;
    }

    // Phase 1: Special treatment for Nose shading: prevent ugly black holes
    // (op.layer must be compared via the normalized name; every other branch in
    // this pipeline operates on normalized layer names).
    if (isNose && KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Shading") && color.value() < 50) {
        color.setRgb(120, 75, 65, qBound(0, qRound(color.alphaF() * 255 * 0.4), 80));
    }

    if (op.fillStyle.compare(QLatin1String("scanline"), Qt::CaseInsensitive) == 0) {
        // Comic halftone screen fill
        KisAiStrokeQualityUtils::drawHalftonePattern(painter, poly, color, 8.0, 2.5, op.angleDeg != 0.0 ? op.angleDeg : 45.0, false);
    } else if (op.fillStyle.compare(QLatin1String("wash"), Qt::CaseInsensitive) == 0 ||
               op.brush.profile.compare(QLatin1String("watercolor"), Qt::CaseInsensitive) == 0) {
        // Watercolor wash with subtle vertical illumination gradient
        const QRectF b = poly.boundingRect();
        QLinearGradient washGrad(b.topLeft(), b.bottomLeft());
        QColor colTop = color;
        colTop.setAlphaF(color.alphaF() * 0.95);
        QColor colBot = color;
        colBot.setAlphaF(color.alphaF() * 0.70);
        washGrad.setColorAt(0.0, colTop);
        washGrad.setColorAt(1.0, colBot);

        painter.setPen(Qt::NoPen);
        painter.setBrush(washGrad);
        painter.drawPolygon(poly);

        // Phase 1 fix: Do NOT draw dark fringe on hair or skin patches!
        // It was creating dozens of bubble/scale boundaries (the "afro/bubbles" look).
        if (!isHair && !isSkin && !isBlush) {
            QColor fringe = color;
            fringe.setAlphaF(qBound<qreal>(0.0, color.alphaF() * 0.85, 1.0));
            QPen fringePen(fringe, 0.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(fringePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(poly);
        }
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(poly);
    }
}

void KisAiStrokeRenderer::drawGradientFillOperation(QPainter &painter,
                                                    const KisAiStrokeOperation &op,
                                                    const QSize &canvasSize)
{
    QPolygonF poly;
    if (op.polygon.size() >= 3) {
        poly = scalePolygon(op.polygon, canvasSize);
        if (op.smooth && poly.size() >= 3) {
            poly = KisAiStrokeQualityUtils::smoothPolygonCornerPreserving(poly, 135.0, 4);
        }
    } else {
        poly = QPolygonF(QRectF(0, 0, canvasSize.width(), canvasSize.height()));
    }

    const QRectF b = poly.boundingRect();
    const qreal brushOpacity = qBound<qreal>(0.0, op.brush.opacity, 1.0);

    if (op.isRadial || op.fillStyle.compare(QLatin1String("radial"), Qt::CaseInsensitive) == 0) {
        QPointF centerPt;
        if (op.gradientCenter != QPointF(0.5, 0.5) || op.polygon.isEmpty()) {
            centerPt = scalePoint(op.gradientCenter, canvasSize);
        } else {
            centerPt = b.center();
        }
        const qreal r = qMax(5.0, op.gradientRadius * qMin(canvasSize.width(), canvasSize.height()));
        QRadialGradient radGrad(centerPt, r);

        if (!op.gradientColors.isEmpty()) {
            const int count = op.gradientColors.size();
            for (int i = 0; i < count; ++i) {
                const qreal pos = (count > 1) ? qreal(i) / (count - 1) : 0.0;
                QColor col = op.gradientColors.at(i);
                col.setAlphaF(qBound<qreal>(0.0, col.alphaF() * brushOpacity, 1.0));
                radGrad.setColorAt(pos, col);
            }
            if (count == 1) {
                QColor col = op.gradientColors.at(0);
                col.setAlphaF(qBound<qreal>(0.0, col.alphaF() * brushOpacity, 1.0));
                radGrad.setColorAt(1.0, col);
            }
        } else {
            QColor col = op.brush.color;
            col.setAlphaF(qBound<qreal>(0.0, col.alphaF() * brushOpacity, 1.0));
            radGrad.setColorAt(0.0, col);
            radGrad.setColorAt(1.0, Qt::transparent);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(radGrad);
        painter.drawPolygon(poly);
        return;
    }

    QPointF p1, p2;
    if (op.points.size() >= 2) {
        p1 = scalePoint(op.points.first().pos, canvasSize);
        p2 = scalePoint(op.points.last().pos, canvasSize);
    } else {
        const qreal rad = op.angleDeg * PI / 180.0;
        const QPointF center = b.center();
        const qreal len = qMax(b.width(), b.height()) * 0.6;
        p1 = center - QPointF(std::cos(rad) * len, std::sin(rad) * len);
        p2 = center + QPointF(std::cos(rad) * len, std::sin(rad) * len);
    }

    QLinearGradient grad(p1, p2);
    if (!op.gradientColors.isEmpty()) {
        const int count = op.gradientColors.size();
        for (int i = 0; i < count; ++i) {
            const qreal pos = (count > 1) ? qreal(i) / (count - 1) : 0.0;
            QColor col = op.gradientColors.at(i);
            col.setAlphaF(qBound<qreal>(0.0, col.alphaF() * brushOpacity, 1.0));
            grad.setColorAt(pos, col);
        }
        if (count == 1) {
            QColor col = op.gradientColors.at(0);
            col.setAlphaF(qBound<qreal>(0.0, col.alphaF() * brushOpacity, 1.0));
            grad.setColorAt(1.0, col);
        }
    } else {
        QColor col = op.brush.color;
        col.setAlphaF(qBound<qreal>(0.0, col.alphaF() * brushOpacity, 1.0));
        grad.setColorAt(0.0, col);
        grad.setColorAt(1.0, Qt::transparent);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawPolygon(poly);

    // D2-5: 1.5% deterministic dither kills 8-bit Mach banding on smooth skies.
    {
        const QRect bounds = poly.boundingRect().toAlignedRect().intersected(
            QRect(QPoint(0, 0), canvasSize));
        if (!bounds.isEmpty() && bounds.width() * bounds.height() < 4096 * 4096) {
            QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(
                op.id + QStringLiteral("/dither")));
            painter.setPen(Qt::NoPen);
            const int dabStep = 3;
            for (int y = bounds.top(); y <= bounds.bottom(); y += dabStep) {
                for (int x = bounds.left(); x <= bounds.right(); x += dabStep) {
                    if (!poly.containsPoint(QPointF(x + 0.5, y + 0.5), Qt::OddEvenFill))
                        continue;
                    const int n = int(rng.generateDouble() * 255.0);
                    if (n < 4) { // ~1.5%: sparse light lift
                        painter.setBrush(QColor(255, 255, 255, 10));
                        painter.drawPoint(x, y);
                    } else if (n > 251) { // ~1.5%: sparse dark dip
                        painter.setBrush(QColor(0, 0, 0, 10));
                        painter.drawPoint(x, y);
                    }
                }
            }
        }
    }
}

void KisAiStrokeRenderer::drawRibbonOperation(QPainter &painter,
                                              const KisAiStrokeOperation &op,
                                              const QSize &canvasSize)
{
    QVector<QPointF> rawSpine = op.spine;
    if (rawSpine.isEmpty() && !op.points.isEmpty()) {
        rawSpine.reserve(op.points.size());
        for (const KisAiStrokePoint &pt : op.points) {
            rawSpine.append(pt.pos);
        }
    }
    if (rawSpine.size() < 2)
        return;

    const qreal baseDim = qMin(canvasSize.width(), canvasSize.height());

    // D0: stabilize the spine (jitter removal) before any envelope math.
    QVector<KisAiStrokePoint> spinePts;
    spinePts.reserve(rawSpine.size());
    for (const QPointF &pt : rawSpine)
        spinePts.append(KisAiStrokePoint(pt.x(), pt.y(), 0.8));
    const QVector<KisAiStrokePoint> stableSpine =
        KisAiDeliberateStroke::stabilizeStroke(spinePts, canvasSize, false,
                                               KisAiStrokeProgramCodec::stableSeed(op.id));
    QVector<QPointF> stableRaw;
    stableRaw.reserve(stableSpine.size());
    for (const KisAiStrokePoint &pt : stableSpine)
        stableRaw.append(pt.pos);

    // Scale spine and smooth
    QVector<QPointF> scaledSpine;
    scaledSpine.reserve(stableRaw.size());
    for (const QPointF &pt : stableRaw) {
        scaledSpine.append(scalePoint(pt, canvasSize));
    }

    if (scaledSpine.size() >= 3) {
        scaledSpine = generateCatmullRomSpline(scaledSpine, 6, false);
    }

    const int n = scaledSpine.size();
    if (n < 2)
        return;

    // D2-4: curvature-coupled width — tighten on sharp bends, widen on runs.
    QVector<qreal> spineCurves;
    if (n >= 3) {
        spineCurves = KisAiStrokeQualityUtils::computeCurvatures(scaledSpine);
    } else {
        spineCurves = QVector<qreal>(n, 0.0);
    }
    qreal curveMax = 0.0;
    for (qreal c : spineCurves)
        curveMax = qMax(curveMax, qAbs(c));
    const qreal curveScale = curveMax > 1.0e-9 ? curveMax : 1.0;

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
        const qreal curveT = spineCurves.isEmpty()
            ? 0.0
            : qAbs(spineCurves.at(qMin(i, spineCurves.size() - 1))) / curveScale;
        widthNorm *= (1.0 - 0.20 * qBound<qreal>(0.0, curveT, 1.0));
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

void KisAiStrokeRenderer::drawParticlesOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize, int supersampleScale, const QPainterPath &faceExclusionPath)
{
    const QRectF normBounds = op.bounds.isValid() ? op.bounds : QRectF(0.0, 0.0, 1.0, 1.0);
    const QRectF area(normBounds.left() * canvasSize.width(),
                      normBounds.top() * canvasSize.height(),
                      normBounds.width() * canvasSize.width(),
                      normBounds.height() * canvasSize.height());

    // A zero count is legal: parse-time total-budgeting zeroes over-budget
    // particle operations, and such an operation must draw nothing.
    if (op.particleCount <= 0)
        return;
    const int count = qBound(1, op.particleCount, 60); // Bound count to prevent blizzard storm
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id.isEmpty() ? QStringLiteral("particles") : op.id));

    painter.setPen(Qt::NoPen);
    QColor partColor = op.brush.color;
    partColor.setAlphaF(qBound<qreal>(0.0, partColor.alphaF() * op.brush.opacity * 0.85, 0.90));
    painter.setBrush(partColor);

    const qreal baseDiameter = effectiveBrushWidth(op.brush, 0.8, canvasSize, supersampleScale);

    for (int i = 0; i < count; ++i) {
        const qreal x = area.left() + rng.generateDouble() * area.width();
        const qreal y = area.top() + rng.generateDouble() * area.height();

        // Phase 1 fix: Skip drawing particles inside the face/feature exclusion zone
        if (!faceExclusionPath.isEmpty() && faceExclusionPath.contains(QPointF(x, y))) {
            continue;
        }

        const qreal depthScale = 0.40 + rng.generateDouble() * 1.10;
        const qreal r = qMax<qreal>(0.75, baseDiameter * 0.45 * depthScale);

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
        } else if (op.particleShape == QLatin1String("bokeh")) {
            QRadialGradient glow(QPointF(x, y), r * 1.8);
            QColor core = partColor;
            QColor edge = partColor;
            edge.setAlpha(0);
            glow.setColorAt(0.0, core);
        glow.setColorAt(0.55, core);
            glow.setColorAt(1.0, edge);
            painter.setBrush(glow);
            painter.drawEllipse(QPointF(x, y), r * 1.8, r * 1.8);
            painter.setBrush(partColor);
        } else {
            painter.drawEllipse(QPointF(x, y), r, r);
        }
    }
}

void KisAiStrokeRenderer::drawHatchOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize, int supersampleScale)
{
    if (op.polygon.size() < 3)
        return;

    QPolygonF poly = scalePolygon(op.polygon, canvasSize);
    if (op.smooth && poly.size() >= 3) {
        poly = KisAiStrokeQualityUtils::smoothPolygonCornerPreserving(poly, 135.0, 4);
    }

    const QRectF b = poly.boundingRect();
    if (b.isEmpty())
        return;

    painter.save();
    QPainterPath clipPath;
    clipPath.addPolygon(poly);
    painter.setClipPath(clipPath);

    QColor color = op.brush.color;
    color.setAlphaF(qBound<qreal>(0.0, op.brush.opacity * color.alphaF() * 0.70, 1.0));

    const qreal minDim = qMin(canvasSize.width(), canvasSize.height());
    // Use delicate, fine linework instead of heavy wireframe bars
    const qreal penWidth = qMax<qreal>(0.5, effectiveBrushWidth(op.brush, 0.5, canvasSize, supersampleScale) * 0.20);
    QPen pen(color, penWidth, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(pen);

    const qreal spacing = qMax<qreal>(2.0, op.spacing * minDim);
    const QPointF center = b.center();
    const qreal radius = std::hypot(b.width(), b.height()) * 0.55;
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/hatch")));

    auto drawPass = [&](qreal angleDeg) {
        const qreal rad = angleDeg * PI / 180.0;
        const QPointF dir(std::cos(rad), std::sin(rad));
        const QPointF norm(-std::sin(rad), std::cos(rad));

        // Bound the line count like drawHalftonePattern does: a tiny spacing
        // over a huge bounding radius must not translate into hundreds of
        // thousands of clipped drawLine calls per pass. Beyond the cap the
        // hatch is already visually solid, so widening the spacing keeps the
        // coverage without the runaway work.
        constexpr int MAX_HATCH_LINES = 2000;
        const qreal rawNumLines = radius * 2.0 / spacing;
        int numLines = std::isfinite(rawNumLines) ? qRound(rawNumLines) : 0;
        if (numLines > MAX_HATCH_LINES) {
            numLines = MAX_HATCH_LINES;
        }
        const qreal effectiveSpacing = numLines > 0 ? qMax(spacing, radius * 2.0 / numLines) : spacing;
        for (int i = -numLines; i <= numLines; ++i) {
            const QPointF lineMid = center + norm * (i * effectiveSpacing);
            // Slight organic tremor/jitter to avoid sterile mechanical appearance
            const qreal wobble = (rng.generateDouble() - 0.5) * effectiveSpacing * 0.15;
            const QPointF p1 = lineMid - dir * radius + norm * wobble;
            const QPointF p2 = lineMid + dir * radius - norm * wobble;
            painter.drawLine(p1, p2);
        }
    };

    drawPass(op.angleDeg);
    if (op.crossHatch) {
        drawPass(op.angleDeg + 90.0);
    }

    painter.restore();
}

void KisAiStrokeRenderer::drawMangaLinesOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize, int supersampleScale)
{
    // gradientCenter defaults to (0.5, 0.5) per KisAiStrokeOperation struct.
    // Since (0, 0) is a valid coordinate (top-left), we cannot use isNull() to
    // detect "not set". Instead, we always use the value as-is after refinement.
    const QPointF center = scalePoint(op.gradientCenter, canvasSize);
    const qreal baseDim = qMin(canvasSize.width(), canvasSize.height());
    const qreal rInner = qMax<qreal>(5.0, op.innerRadius * baseDim);
    const qreal rOuter = qMax<qreal>(rInner + 10.0, op.outerRadius * baseDim);
    const int count = qBound(4, op.density, 180);
    const qreal jitter = qBound<qreal>(0.0, op.lineLengthJitter, 0.8);

    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/manga")));

    QColor lineColor = op.brush.color;
    lineColor.setAlphaF(qBound<qreal>(0.0, op.brush.opacity * lineColor.alphaF(), 1.0));

    const qreal lineWidth = effectiveBrushWidth(op.brush, 0.8, canvasSize, supersampleScale);

    for (int i = 0; i < count; ++i) {
        const qreal baseAngle = (2.0 * PI * i) / count;
        const qreal angleJitter = (rng.generateDouble() - 0.5) * (2.0 * PI / count) * 0.4;
        const qreal angle = baseAngle + angleJitter;

        const qreal cosA = std::cos(angle);
        const qreal sinA = std::sin(angle);

        const qreal innerDist = rInner * (1.0 + (rng.generateDouble() - 0.5) * jitter * 0.8);
        const qreal outerDist = rOuter * (1.0 + (rng.generateDouble() - 0.5) * jitter);

        const QPointF pStart = center + QPointF(cosA * innerDist, sinA * innerDist);
        const QPointF pEnd = center + QPointF(cosA * outerDist, sinA * outerDist);

        const QPointF perp(-sinA, cosA);
        const qreal halfW = lineWidth * (0.6 + rng.generateDouble() * 0.8);

        QPolygonF wedge;
        wedge << pStart
              << (pEnd + perp * halfW)
              << (pEnd - perp * halfW);

        painter.setPen(Qt::NoPen);
        painter.setBrush(lineColor);
        painter.drawPolygon(wedge);
    }
}

QString KisAiStrokeRenderer::captureImageBase64(const QImage &image, int maxDimension, int quality)
{
    if (image.isNull() || maxDimension <= 0) {
        return QString();
    }

    // QImage::save() documents that an out-of-range quality gives undefined
    // results, and scaling to a non-positive dimension yields a null image.
    const int boundedQuality = qBound(-1, quality, 100);

    QImage scaled = image;
    if (qMax(image.width(), image.height()) > maxDimension) {
        scaled = image.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (scaled.isNull()) {
        return QString();
    }

    QImage rgb(scaled.size(), QImage::Format_RGB32);
    rgb.fill(Qt::white);
    QPainter p(&rgb);
    p.drawImage(0, 0, scaled);
    p.end();

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!rgb.save(&buffer, "JPEG", boundedQuality) || bytes.isEmpty()) {
        return QString();
    }

    return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
}

#ifndef AI_STROKE_STANDALONE
QString KisAiStrokeRenderer::captureCanvasBase64(KisImageWSP image, int maxDimension, int quality)
{
    if (!image) {
        return QString();
    }
    const QRect bounds = image->bounds();
    if (bounds.isEmpty()) {
        return QString();
    }
    // Never invoke image->waitForDone() on the GUI thread: if the user is drawing
    // or asynchronous projection updates are pending, it blocks the event loop
    // and causes permanent deadlocks. convertToQImage directly reads projection tiles.
    const QImage canvasImg = image->convertToQImage(bounds, nullptr);
    return captureImageBase64(canvasImg, maxDimension, quality);
}
#endif

void KisAiStrokeRenderer::applySoftEdgeDiffusion(QImage &image, int radius)
{
    if (image.isNull() || radius <= 0) {
        return;
    }

    const int w = image.width();
    const int h = image.height();
    if (w < 4 || h < 4) {
        return;
    }

    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    } else if (image.format() == QImage::Format_ARGB32) {
        // Straight-alpha pixels bleed colour into a box filter that assumes
        // premultiplied math, producing halos around transparent regions.
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    const int boundedRadius = qBound(1, radius, qMax(1, qMin(w - 1, h - 1)));
    QImage temp(image.size(), image.format());
    const int diameter = boundedRadius * 2 + 1;
    const qreal invDiv = 1.0 / diameter;

    // Horizontal pass
    for (int y = 0; y < h; ++y) {
        const QRgb *srcRow = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        QRgb *dstRow = reinterpret_cast<QRgb *>(temp.scanLine(y));

        int sumA = 0;
        int sumR = 0;
        int sumG = 0;
        int sumB = 0;
        for (int i = -boundedRadius; i <= boundedRadius; ++i) {
            const int cx = qBound(0, i, w - 1);
            const QRgb c = srcRow[cx];
            sumA += qAlpha(c);
            sumR += qRed(c);
            sumG += qGreen(c);
            sumB += qBlue(c);
        }

        for (int x = 0; x < w; ++x) {
            dstRow[x] = qRgba(qBound(0, qRound(sumR * invDiv), 255),
                              qBound(0, qRound(sumG * invDiv), 255),
                              qBound(0, qRound(sumB * invDiv), 255),
                              qBound(0, qRound(sumA * invDiv), 255));

            const int xRemove = qBound(0, x - boundedRadius, w - 1);
            const int xAdd = qBound(0, x + boundedRadius + 1, w - 1);
            const QRgb cRem = srcRow[xRemove];
            const QRgb cAdd = srcRow[xAdd];
            sumA += qAlpha(cAdd) - qAlpha(cRem);
            sumR += qRed(cAdd) - qRed(cRem);
            sumG += qGreen(cAdd) - qGreen(cRem);
            sumB += qBlue(cAdd) - qBlue(cRem);
        }
    }

    // Vertical pass
    for (int x = 0; x < w; ++x) {
        int sumA = 0;
        int sumR = 0;
        int sumG = 0;
        int sumB = 0;
        for (int i = -boundedRadius; i <= boundedRadius; ++i) {
            const int cy = qBound(0, i, h - 1);
            const QRgb c = reinterpret_cast<const QRgb *>(temp.constScanLine(cy))[x];
            sumA += qAlpha(c);
            sumR += qRed(c);
            sumG += qGreen(c);
            sumB += qBlue(c);
        }

        for (int y = 0; y < h; ++y) {
            reinterpret_cast<QRgb *>(image.scanLine(y))[x] =
                qRgba(qBound(0, qRound(sumR * invDiv), 255),
                      qBound(0, qRound(sumG * invDiv), 255),
                      qBound(0, qRound(sumB * invDiv), 255),
                      qBound(0, qRound(sumA * invDiv), 255));

            const int yRemove = qBound(0, y - boundedRadius, h - 1);
            const int yAdd = qBound(0, y + boundedRadius + 1, h - 1);
            const QRgb cRem = reinterpret_cast<const QRgb *>(temp.constScanLine(yRemove))[x];
            const QRgb cAdd = reinterpret_cast<const QRgb *>(temp.constScanLine(yAdd))[x];
            sumA += qAlpha(cAdd) - qAlpha(cRem);
            sumR += qRed(cAdd) - qRed(cRem);
            sumG += qGreen(cAdd) - qGreen(cRem);
            sumB += qBlue(cAdd) - qBlue(cRem);
        }
    }
}

QImage KisAiStrokeRenderer::generateBloomMap(const QImage &image, qreal intensity, int radius)
{
    if (image.isNull() || radius <= 0 || intensity <= 0.0) {
        return QImage();
    }

    const int w = image.width();
    const int h = image.height();
    if (w < 8 || h < 8) {
        return QImage();
    }

    QImage src = image;
    if (src.format() != QImage::Format_ARGB32_Premultiplied && src.format() != QImage::Format_ARGB32) {
        src = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    // Step 1: Extract bright highlights (luminance > 170)
    QImage bright(w, h, QImage::Format_ARGB32_Premultiplied);
    bright.fill(Qt::transparent);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcRow = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *dstRow = reinterpret_cast<QRgb *>(bright.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = srcRow[x];
            const int a = qAlpha(c);
            if (a < 15) {
                dstRow[x] = 0;
                continue;
            }
            const int r = qRed(c);
            const int g = qGreen(c);
            const int b = qBlue(c);
            const int lum = (299 * r + 587 * g + 114 * b) / 1000;
            if (lum > 170) {
                const qreal factor = qreal(lum - 170) / (255.0 - 170.0);
                dstRow[x] = qRgba(qRound(r * factor), qRound(g * factor), qRound(b * factor), qRound(a * factor));
            } else {
                dstRow[x] = 0;
            }
        }
    }

    // Step 2: Diffuse the bright pass using 2-pass separable blur
    applySoftEdgeDiffusion(bright, radius);
    applySoftEdgeDiffusion(bright, qMax(2, radius / 2));

    // Step 3: Modulate by intensity
    const qreal boundedIntensity = qBound<qreal>(0.0, intensity, 2.0);
    if (boundedIntensity != 1.0) {
        for (int y = 0; y < h; ++y) {
            QRgb *dstRow = reinterpret_cast<QRgb *>(bright.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb c = dstRow[x];
                const int a = qAlpha(c);
                if (a == 0) continue;
                dstRow[x] = qRgba(qBound(0, qRound(qRed(c) * boundedIntensity), 255),
                                  qBound(0, qRound(qGreen(c) * boundedIntensity), 255),
                                  qBound(0, qRound(qBlue(c) * boundedIntensity), 255),
                                  qBound(0, qRound(a * boundedIntensity), 255));
            }
        }
    }

    return bright;
}

void KisAiStrokeRenderer::applyBloomEffect(QImage &image, qreal intensity, int radius)
{
    const QImage bloomMap = generateBloomMap(image, intensity, radius);
    if (bloomMap.isNull()) {
        return;
    }

    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    const int w = image.width();
    const int h = image.height();
    for (int y = 0; y < h; ++y) {
        const QRgb *bloomRow = reinterpret_cast<const QRgb *>(bloomMap.constScanLine(y));
        QRgb *dstRow = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb cBloom = bloomRow[x];
            const int aBloom = qAlpha(cBloom);
            if (aBloom == 0) continue;

            const QRgb cSrc = dstRow[x];
            const int r = qMin(255, qRed(cSrc) + qRed(cBloom));
            const int g = qMin(255, qGreen(cSrc) + qGreen(cBloom));
            const int b = qMin(255, qBlue(cSrc) + qBlue(cBloom));
            const int a = qMax(qAlpha(cSrc), aBloom);
            dstRow[x] = qRgba(r, g, b, a);
        }
    }
}

void KisAiStrokeRenderer::applyChromaticAberration(QImage &image, int shiftPx)
{
    if (image.isNull() || shiftPx <= 0) {
        return;
    }

    const int w = image.width();
    const int h = image.height();
    if (w < 4 || h < 4) {
        return;
    }

    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    const QImage copy = image.copy();
    const int boundedShift = qBound(1, shiftPx, qMax(1, w / 16));

    for (int y = 0; y < h; ++y) {
        const QRgb *srcRow = reinterpret_cast<const QRgb *>(copy.constScanLine(y));
        QRgb *dstRow = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int xR = qBound(0, x - boundedShift, w - 1);
            const int xB = qBound(0, x + boundedShift, w - 1);
            const QRgb cR = srcRow[xR];
            const QRgb cG = srcRow[x];
            const QRgb cB = srcRow[xB];
            dstRow[x] = qRgba(qRed(cR), qGreen(cG), qBlue(cB), qAlpha(cG));
        }
    }
}

void KisAiStrokeRenderer::applyVignette(QImage &image, qreal strength)
{
    if (image.isNull() || strength <= 0.0) {
        return;
    }

    const int w = image.width();
    const int h = image.height();
    if (w < 4 || h < 4) {
        return;
    }

    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    const qreal cx = (w - 1) * 0.5;
    const qreal cy = (h - 1) * 0.5;
    const qreal invMaxDistSq = 1.0 / qMax<qreal>(1.0, cx * cx + cy * cy);
    const qreal boundStrength = qBound<qreal>(0.0, strength, 1.0);

    for (int y = 0; y < h; ++y) {
        const qreal dy = y - cy;
        const qreal dySq = dy * dy;
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const qreal dx = x - cx;
            const qreal distSqNorm = (dx * dx + dySq) * invMaxDistSq;
            if (distSqNorm > 0.20) {
                const qreal t = qBound<qreal>(0.0, (distSqNorm - 0.20) / 0.80, 1.0);
                const qreal v = t * t * (3.0 - 2.0 * t) * boundStrength;
                const qreal factor = 1.0 - v;
                const QRgb c = row[x];
                row[x] = qRgba(qRound(qRed(c) * factor),
                               qRound(qGreen(c) * factor),
                               qRound(qBlue(c) * factor),
                               qAlpha(c));
            }
        }
    }
}

void KisAiStrokeRenderer::applyFinishingPostProcess(QImage &image)
{
    applyBloomEffect(image, 0.40, 6);
    applyChromaticAberration(image, 1);
    applyVignette(image, 0.12);
}

void KisAiStrokeRenderer::drawAnimeEyeOperation(QPainter &painter, const KisAiStrokeOperation &op, const QSize &canvasSize, int supersampleScale)
{
    Q_UNUSED(supersampleScale);
    const QPointF centerPt = scalePoint(op.eyeCenter, canvasSize);
    const qreal w = op.eyeSize.width() * canvasSize.width();
    const qreal h = op.eyeSize.height() * canvasSize.height();
    if (w < 4.0 || h < 4.0)
        return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal outerSign = op.eyeIsRight ? 1.0 : -1.0;
    const qreal innerSign = -outerSign;
    const QRectF eyeRect(centerPt.x() - w * 0.5, centerPt.y() - h * 0.5, w, h);

    // 1. Sclera (白目)
    QPainterPath scleraPath;
    scleraPath.moveTo(centerPt.x() - w * 0.48, centerPt.y());
    scleraPath.quadTo(centerPt.x(), centerPt.y() - h * 0.52, centerPt.x() + w * 0.48, centerPt.y());
    scleraPath.quadTo(centerPt.x(), centerPt.y() + h * 0.45, centerPt.x() - w * 0.48, centerPt.y());

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(250, 250, 255));
    painter.drawPath(scleraPath);

    // Upper Sclera Shadow (gentle ambient shadow cast by eyelid)
    QLinearGradient scleraGrad(centerPt.x(), eyeRect.top(), centerPt.x(), centerPt.y());
    scleraGrad.setColorAt(0.0, QColor(185, 190, 215, 160));
    scleraGrad.setColorAt(0.50, QColor(220, 225, 240, 50));
    scleraGrad.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.setBrush(scleraGrad);
    painter.drawPath(scleraPath);

    // 2. Iris (虹彩)
    const qreal irisW = w * 0.62;
    const qreal irisH = h * 0.82;
    const QRectF irisRect(centerPt.x() - irisW * 0.5, centerPt.y() - irisH * 0.46, irisW, irisH);
    QPainterPath irisPath;
    irisPath.addEllipse(irisRect);

    painter.save();
    painter.setClipPath(scleraPath);

    QLinearGradient irisGrad(irisRect.center().x(), irisRect.top(), irisRect.center().x(), irisRect.bottom());
    QColor darkTop = op.eyeIrisColor.darker(280);
    darkTop.setAlpha(255);
    QColor midColor = op.eyeIrisColor;
    QColor bottomColor = op.eyeSecondaryColor.isValid() ? op.eyeSecondaryColor : op.eyeIrisColor.lighter(150);

    irisGrad.setColorAt(0.0, darkTop);
    irisGrad.setColorAt(0.35, darkTop.lighter(120));
    irisGrad.setColorAt(0.70, midColor);
    irisGrad.setColorAt(1.0, bottomColor);

    painter.setPen(Qt::NoPen);
    painter.setBrush(irisGrad);
    painter.drawPath(irisPath);

    // Lower crescent reflection inside iris
    QPainterPath crescentPath;
    const QRectF cresRect(irisRect.left() + irisW * 0.10, irisRect.top() + irisH * 0.45, irisW * 0.80, irisH * 0.50);
    crescentPath.addEllipse(cresRect);
    QLinearGradient cresGrad(cresRect.center().x(), cresRect.top(), cresRect.center().x(), cresRect.bottom());
    cresGrad.setColorAt(0.0, QColor(bottomColor.red(), bottomColor.green(), bottomColor.blue(), 0));
    cresGrad.setColorAt(1.0, QColor(bottomColor.red(), bottomColor.green(), bottomColor.blue(), 180));
    painter.setBrush(cresGrad);
    painter.drawPath(crescentPath);

    // 3. Pupil (瞳孔)
    const qreal pupilW = irisW * 0.38;
    const qreal pupilH = irisH * 0.44;
    const QRectF pupilRect(centerPt.x() - pupilW * 0.5, centerPt.y() - pupilH * 0.40, pupilW, pupilH);
    QColor pupilColor = darkTop.darker(150);
    pupilColor.setAlpha(240);
    painter.setBrush(pupilColor);
    painter.drawEllipse(pupilRect);

    // 4. Catchlights (ハイライト)
    const qreal hlR = qMax<qreal>(2.0, irisW * 0.16);
    const QPointF hlPos(centerPt.x() - irisW * 0.22, centerPt.y() - irisH * 0.22);
    painter.setBrush(QColor(255, 255, 255, 245));
    if (op.eyeStyle == QLatin1String("sparkle")) {
        QPainterPath star;
        star.moveTo(hlPos.x(), hlPos.y() - hlR * 1.5);
        star.quadTo(hlPos.x(), hlPos.y(), hlPos.x() + hlR * 1.5, hlPos.y());
        star.quadTo(hlPos.x(), hlPos.y(), hlPos.x(), hlPos.y() + hlR * 1.5);
        star.quadTo(hlPos.x(), hlPos.y(), hlPos.x() - hlR * 1.5, hlPos.y());
        star.quadTo(hlPos.x(), hlPos.y(), hlPos.x(), hlPos.y() - hlR * 1.5);
        painter.drawPath(star);
    } else {
        painter.drawEllipse(hlPos, hlR, hlR);
    }

    const qreal subHlR = hlR * 0.55;
    const QPointF subHlPos(centerPt.x() + irisW * 0.20, centerPt.y() + irisH * 0.18);
    painter.setBrush(QColor(255, 255, 255, 210));
    painter.drawEllipse(subHlPos, subHlR, subHlR);

    painter.restore(); // end Sclera clip

    // 5. Upper Eyelash & Eyeline (上まつ毛・アイライン)
    QColor lashColor = darkTop.darker(130);
    lashColor.setAlpha(255);
    const qreal lashThickness = qMax<qreal>(2.2, h * 0.09);

    // Main sweeping upper lash line: from inner corner (目頭) over pupil to outer corner (目尻)
    QPainterPath lashPath;
    lashPath.moveTo(centerPt.x() + innerSign * w * 0.44, centerPt.y() + h * 0.02);
    lashPath.quadTo(centerPt.x() + outerSign * w * 0.05, centerPt.y() - h * 0.56,
                    centerPt.x() + outerSign * w * 0.46, centerPt.y() - h * 0.12);
    // Outer wing flick (目尻の跳ね上げ・キャットアイ)
    lashPath.quadTo(centerPt.x() + outerSign * w * 0.54, centerPt.y() - h * 0.22,
                    centerPt.x() + outerSign * w * 0.58, centerPt.y() - h * 0.30);

    QPen lashPen(lashColor, lashThickness, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(lashPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(lashPath);

    // Accent upper lash tip (繊細なまつ毛のアクセント)
    QPainterPath accentLash;
    accentLash.moveTo(centerPt.x() + outerSign * w * 0.38, centerPt.y() - h * 0.35);
    accentLash.quadTo(centerPt.x() + outerSign * w * 0.46, centerPt.y() - h * 0.44,
                      centerPt.x() + outerSign * w * 0.50, centerPt.y() - h * 0.48);
    QPen accentPen(lashColor, qMax<qreal>(1.2, lashThickness * 0.45), Qt::SolidLine, Qt::RoundCap);
    painter.setPen(accentPen);
    painter.drawPath(accentLash);

    // 6. Double Eyelid crease (二重まぶた: 自然な平行二重ライン)
    QPainterPath creasePath;
    creasePath.moveTo(centerPt.x() + innerSign * (w * 0.26), centerPt.y() - (h * 0.60));
    creasePath.quadTo(centerPt.x() + outerSign * (w * 0.05), centerPt.y() - (h * 0.68),
                      centerPt.x() + outerSign * (w * 0.35), centerPt.y() - (h * 0.54));
    QColor creaseColor = lashColor;
    creaseColor.setAlpha(185);
    QPen creasePen(creaseColor, qMax<qreal>(1.0, lashThickness * 0.32), Qt::SolidLine, Qt::RoundCap);
    painter.setPen(creasePen);
    painter.drawPath(creasePath);

    // 7. Lower Eyelash (下まつ毛: 目尻側のソフトで繊細なアクセント)
    QPainterPath lowerLash;
    lowerLash.moveTo(centerPt.x() + outerSign * (w * 0.12), centerPt.y() + (h * 0.46));
    lowerLash.quadTo(centerPt.x() + outerSign * (w * 0.28), centerPt.y() + (h * 0.48),
                     centerPt.x() + outerSign * (w * 0.40), centerPt.y() + (h * 0.36));
    QColor lowerLashColor = lashColor;
    lowerLashColor.setAlpha(175);
    QPen lowerPen(lowerLashColor, qMax<qreal>(1.0, lashThickness * 0.35), Qt::SolidLine, Qt::RoundCap);
    painter.setPen(lowerPen);
    painter.drawPath(lowerLash);

    painter.restore();
}


