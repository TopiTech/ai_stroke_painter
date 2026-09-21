/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPhysicalRenderer.h"

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QVector>
#include <QtMath>
#include <QtNumeric>

#include <algorithm>
#include <cmath>

#include "KisAiStrokeCommitter.h"
#include "KisAiStrokeQualityUtils.h"
#include "KisAiStrokeRenderer.h"

namespace KisAi
{

namespace
{
QPointF scalePoint(const QPointF &pt, const QSize &size)
{
    return QPointF(pt.x() * size.width(), pt.y() * size.height());
}

QPolygonF scalePolygon(const QPolygonF &poly, const QSize &size)
{
    QPolygonF res;
    res.reserve(poly.size());
    for (const QPointF &p : poly) {
        res.append(scalePoint(p, size));
    }
    return res;
}
} // namespace

// ===========================================================================
// 色空間変換 (IEC 61966-2-1 sRGB <-> Linear sRGB)
// ===========================================================================

float KisAiPhysicalRenderer::srgbToLinear(float srgb)
{
    const float c = qBound(0.0f, srgb, 1.0f);
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float KisAiPhysicalRenderer::linearToSrgb(float linear)
{
    const float c = qBound(0.0f, linear, 1.0f);
    if (c <= 0.0031308f) {
        return c * 12.92f;
    }
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// ===========================================================================
// W3C CSS Compositing Level 1 物理ブレンド関数
// ===========================================================================

float KisAiPhysicalRenderer::blendMultiply(float cb, float cs)
{
    return cb * cs;
}

float KisAiPhysicalRenderer::blendScreen(float cb, float cs)
{
    return cb + cs - (cb * cs);
}

float KisAiPhysicalRenderer::blendOverlay(float cb, float cs)
{
    if (cb <= 0.5f) {
        return 2.0f * cb * cs;
    }
    return 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
}

float KisAiPhysicalRenderer::blendSoftLight(float cb, float cs)
{
    float d;
    if (cb <= 0.25f) {
        d = ((16.0f * cb - 12.0f) * cb + 4.0f) * cb;
    } else {
        d = std::sqrt(cb);
    }

    if (cs <= 0.5f) {
        return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
    }
    return cb + (2.0f * cs - 1.0f) * (d - cb);
}

float KisAiPhysicalRenderer::blendColorDodge(float cb, float cs)
{
    if (cb <= 0.0f) {
        return 0.0f;
    }
    if (cs >= 1.0f) {
        return 1.0f;
    }
    return std::min(1.0f, cb / (1.0f - cs));
}

float KisAiPhysicalRenderer::blendLinearBurn(float cb, float cs)
{
    return std::max(0.0f, cb + cs - 1.0f);
}

// ===========================================================================
// 単一ピクセル物理ブレンド (Premultiplied RGBA 浮動小数点)
// ===========================================================================

void KisAiPhysicalRenderer::blendPixel(const QString &blendMode,
                                       float srcR,
                                       float srcG,
                                       float srcB,
                                       float srcA,
                                       float &dstR,
                                       float &dstG,
                                       float &dstB,
                                       float &dstA,
                                       float opacity)
{
    if (!std::isfinite(srcA) || !std::isfinite(dstA) || !std::isfinite(opacity) || !std::isfinite(srcR)
        || !std::isfinite(srcG) || !std::isfinite(srcB) || !std::isfinite(dstR) || !std::isfinite(dstG)
        || !std::isfinite(dstB)) {
        return;
    }

    srcA = qBound(0.0f, srcA * opacity, 1.0f);
    if (srcA <= 1e-6f) {
        return; // ソースが完全に透明
    }

    // ソースのプレマルチプライド成分を不透明度に応じてスケーリング
    srcR *= opacity;
    srcG *= opacity;
    srcB *= opacity;

    if (dstA <= 1e-6f) {
        // バックドロップが完全に透明ならソースそのもの
        dstR = srcR;
        dstG = srcG;
        dstB = srcB;
        dstA = srcA;
        return;
    }

    // ストレートカラーの算出 (0.0〜1.0)
    const float invSrcA = 1.0f / srcA;
    const float invDstA = 1.0f / dstA;
    const float csR = qBound(0.0f, srcR * invSrcA, 1.0f);
    const float csG = qBound(0.0f, srcG * invSrcA, 1.0f);
    const float csB = qBound(0.0f, srcB * invSrcA, 1.0f);

    const float cbR = qBound(0.0f, dstR * invDstA, 1.0f);
    const float cbG = qBound(0.0f, dstG * invDstA, 1.0f);
    const float cbB = qBound(0.0f, dstB * invDstA, 1.0f);

    const QString mode = blendMode.trimmed().toLower();
    float bR = csR;
    float bG = csG;
    float bB = csB;

    if (mode == QLatin1String("multiply")) {
        bR = blendMultiply(cbR, csR);
        bG = blendMultiply(cbG, csG);
        bB = blendMultiply(cbB, csB);
    } else if (mode == QLatin1String("screen")) {
        bR = blendScreen(cbR, csR);
        bG = blendScreen(cbG, csG);
        bB = blendScreen(cbB, csB);
    } else if (mode == QLatin1String("overlay")) {
        bR = blendOverlay(cbR, csR);
        bG = blendOverlay(cbG, csG);
        bB = blendOverlay(cbB, csB);
    } else if (mode == QLatin1String("soft_light") || mode == QLatin1String("softlight")) {
        bR = blendSoftLight(cbR, csR);
        bG = blendSoftLight(cbG, csG);
        bB = blendSoftLight(cbB, csB);
    } else if (mode == QLatin1String("color_dodge") || mode == QLatin1String("colordodge")) {
        bR = blendColorDodge(cbR, csR);
        bG = blendColorDodge(cbG, csG);
        bB = blendColorDodge(cbB, csB);
    } else if (mode == QLatin1String("linear_burn") || mode == QLatin1String("linearburn")) {
        bR = blendLinearBurn(cbR, csR);
        bG = blendLinearBurn(cbG, csG);
        bB = blendLinearBurn(cbB, csB);
    }

    // W3C Compositing & Blending formula:
    // ao = as + ab * (1 - as)
    // Co = (1 - ab)*as*Cs + (1 - as)*ab*Cb + as*ab*B(Cb, Cs)
    const float outA = srcA + dstA * (1.0f - srcA);
    const float term1 = (1.0f - dstA) * srcA;
    const float term2 = (1.0f - srcA) * dstA;
    const float term3 = srcA * dstA;

    dstA = qBound(0.0f, outA, 1.0f);
    dstR = qBound(0.0f, term1 * csR + term2 * cbR + term3 * bR, dstA);
    dstG = qBound(0.0f, term1 * csG + term2 * cbG + term3 * bG, dstA);
    dstB = qBound(0.0f, term1 * csB + term2 * cbB + term3 * bB, dstA);
}

// ===========================================================================
// HDR フォーマット検出 & 変換
// ===========================================================================

bool KisAiPhysicalRenderer::isHdrFormatSupported()
{
    // Qt 6.2+ で Format_RGBA16FPx4_Premultiplied が利用可能
    return true;
}

QImage KisAiPhysicalRenderer::toLinearHdr(const QImage &srgbImage)
{
    if (srgbImage.isNull()) {
        return QImage();
    }

    const QImage argb = srgbImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = argb.width();
    const int h = argb.height();

    // 内部ストレージとして Format_RGBA32FPx4_Premultiplied または Format_RGBA16FPx4_Premultiplied を使用
    // float 配列演算が直接行いやすい Format_RGBA32FPx4_Premultiplied を優先
    QImage hdr(w, h, QImage::Format_RGBA32FPx4_Premultiplied);
    hdr.setColorSpace(QColorSpace(QColorSpace::NamedColorSpace::SRgbLinear));

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        float *dstLine = reinterpret_cast<float *>(hdr.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pixel = srcLine[x];
            const float a = qAlpha(pixel) / 255.0f;
            if (a <= 1e-6f) {
                dstLine[x * 4 + 0] = 0.0f;
                dstLine[x * 4 + 1] = 0.0f;
                dstLine[x * 4 + 2] = 0.0f;
                dstLine[x * 4 + 3] = 0.0f;
            } else {
                // ストレート色にしてから sRGB -> Linear 変換し、再び Premultiplied にする
                const float sR = (qRed(pixel) / 255.0f) / a;
                const float sG = (qGreen(pixel) / 255.0f) / a;
                const float sB = (qBlue(pixel) / 255.0f) / a;

                const float linR = srgbToLinear(sR) * a;
                const float linG = srgbToLinear(sG) * a;
                const float linB = srgbToLinear(sB) * a;

                dstLine[x * 4 + 0] = linR;
                dstLine[x * 4 + 1] = linG;
                dstLine[x * 4 + 2] = linB;
                dstLine[x * 4 + 3] = a;
            }
        }
    }
    return hdr;
}

QImage KisAiPhysicalRenderer::toSrgbLdr(const QImage &hdrImage)
{
    if (hdrImage.isNull()) {
        return QImage();
    }

    const int w = hdrImage.width();
    const int h = hdrImage.height();
    QImage ldr(w, h, QImage::Format_ARGB32_Premultiplied);
    ldr.setColorSpace(QColorSpace(QColorSpace::NamedColorSpace::SRgb));

    // RGBA32FPx4 からの変換
    QImage srcFp = hdrImage;
    if (srcFp.format() != QImage::Format_RGBA32FPx4_Premultiplied) {
        srcFp = srcFp.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
    }

    for (int y = 0; y < h; ++y) {
        const float *srcLine = reinterpret_cast<const float *>(srcFp.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(ldr.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float linR = srcLine[x * 4 + 0];
            const float linG = srcLine[x * 4 + 1];
            const float linB = srcLine[x * 4 + 2];
            const float a = qBound(0.0f, srcLine[x * 4 + 3], 1.0f);

            if (a <= 1e-6f) {
                dstLine[x] = 0;
            } else {
                const float straightLinR = qBound(0.0f, linR / a, 1.0f);
                const float straightLinG = qBound(0.0f, linG / a, 1.0f);
                const float straightLinB = qBound(0.0f, linB / a, 1.0f);

                const float sR = linearToSrgb(straightLinR);
                const float sG = linearToSrgb(straightLinG);
                const float sB = linearToSrgb(straightLinB);

                const int rInt = qBound(0, qRound(sR * a * 255.0f), 255);
                const int gInt = qBound(0, qRound(sG * a * 255.0f), 255);
                const int bInt = qBound(0, qRound(sB * a * 255.0f), 255);
                const int aInt = qBound(0, qRound(a * 255.0f), 255);

                dstLine[x] = qRgba(rInt, gInt, bInt, aInt);
            }
        }
    }
    return ldr;
}

// ===========================================================================
// レイヤー間物理合成 (dstImage 上に srcLayer を物理ブレンドで重ねる)
// ===========================================================================

void KisAiPhysicalRenderer::compositeLayer(QImage &dstImage, const KisAiLayerImage &srcLayer, const QImage *clipMask)
{
    if (!srcLayer.isValid() || dstImage.isNull()) {
        return;
    }

    const int w = dstImage.width();
    const int h = dstImage.height();

    QImage srcFp = srcLayer.image;
    if (srcFp.format() != QImage::Format_RGBA32FPx4_Premultiplied) {
        srcFp = toLinearHdr(srcFp);
    }
    if (dstImage.format() != QImage::Format_RGBA32FPx4_Premultiplied) {
        dstImage = toLinearHdr(dstImage);
    }

    QImage maskFp;
    if (clipMask && !clipMask->isNull()) {
        if (clipMask->format() != QImage::Format_RGBA32FPx4_Premultiplied) {
            maskFp = toLinearHdr(*clipMask);
        } else {
            maskFp = *clipMask;
        }
    }

    if (srcFp.size() != dstImage.size()) {
        srcFp = srcFp.scaled(dstImage.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (!maskFp.isNull() && maskFp.size() != dstImage.size()) {
        maskFp = maskFp.scaled(dstImage.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    const float opacity = qBound(0.0f, static_cast<float>(srcLayer.opacityFactor), 1.0f);
    const QString mode = srcLayer.blendMode;

    for (int y = 0; y < h; ++y) {
        float *dstLine = reinterpret_cast<float *>(dstImage.scanLine(y));
        const float *srcLine = reinterpret_cast<const float *>(srcFp.constScanLine(y));
        const float *maskLine = maskFp.isNull() ? nullptr : reinterpret_cast<const float *>(maskFp.constScanLine(y));

        for (int x = 0; x < w; ++x) {
            float sR = srcLine[x * 4 + 0];
            float sG = srcLine[x * 4 + 1];
            float sB = srcLine[x * 4 + 2];
            float sA = srcLine[x * 4 + 3];

            if (maskLine) {
                const float maskAlpha = maskLine[x * 4 + 3];
                sR *= maskAlpha;
                sG *= maskAlpha;
                sB *= maskAlpha;
                sA *= maskAlpha;
            }

            if (sA <= 1e-6f) {
                continue;
            }

            float &dR = dstLine[x * 4 + 0];
            float &dG = dstLine[x * 4 + 1];
            float &dB = dstLine[x * 4 + 2];
            float &dA = dstLine[x * 4 + 3];

            blendPixel(mode, sR, sG, sB, sA, dR, dG, dB, dA, opacity);
        }
    }
}

// ===========================================================================
// 遅延合成グラフ (Composite Graph)
// ===========================================================================

int KisAiCompositeGraph::findLayerIndex(const QString &layerName) const
{
    for (int i = 0; i < layers.size(); ++i) {
        if (layers.at(i).name.compare(layerName, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

const KisAiLayerImage *KisAiCompositeGraph::findLayer(const QString &layerName) const
{
    const int idx = findLayerIndex(layerName);
    return idx >= 0 ? &layers.at(idx) : nullptr;
}

QImage KisAiCompositeGraph::evaluate() const
{
    if (size.isEmpty() || size.width() <= 0 || size.height() <= 0) {
        return QImage();
    }

    // キャンバス初期化 (透明 HDR)
    QImage composite(size, QImage::Format_RGBA32FPx4_Premultiplied);
    composite.fill(Qt::transparent);

    if (!backgroundImage.isNull()) {
        const QImage bgHdr = KisAiPhysicalRenderer::toLinearHdr(
            backgroundImage.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        composite = bgHdr;
    }

    // レイヤーマップ作成 (クリッピング参照用)
    QMap<QString, QImage> layerMap;
    for (const KisAiLayerImage &layer : layers) {
        if (layer.isValid()) {
            QImage lyrHdr = layer.image;
            if (lyrHdr.format() != QImage::Format_RGBA32FPx4_Premultiplied) {
                lyrHdr = KisAiPhysicalRenderer::toLinearHdr(lyrHdr);
            }
            layerMap[layer.name.toLower()] = lyrHdr;
        }
    }

    // 各レイヤーを順次物理合成
    for (const KisAiLayerImage &layer : layers) {
        if (!layer.isValid()) {
            continue;
        }

        const QImage *clipMask = nullptr;
        if (!layer.clipToLayer.trimmed().isEmpty()) {
            const QString clipParent = layer.clipToLayer.trimmed().toLower();
            if (layerMap.contains(clipParent)) {
                clipMask = &layerMap[clipParent];
            }
        }

        KisAiPhysicalRenderer::compositeLayer(composite, layer, clipMask);
    }

    // 最終出力を sRGB LDR に変換
    return KisAiPhysicalRenderer::toSrgbLdr(composite);
}

// ===========================================================================
// スーパーサンプリング & ダウンサンプリング
// ===========================================================================

QImage KisAiPhysicalRenderer::downsampleBox(const QImage &src, const QSize &targetSize)
{
    if (src.isNull() || targetSize.isEmpty()) {
        return QImage();
    }
    if (src.size() == targetSize) {
        return src;
    }

    // QImage::scaled with Qt::SmoothTransformation implements an area-averaging box filter for downscaling
    return src.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// ===========================================================================
// ストロークプログラムから物理レンダリングによる画像を生成
// ===========================================================================

QImage KisAiPhysicalRenderer::renderProgramToPhysicalImage(const KisAiStrokeProgram &program,
                                                           const QSize &targetSize,
                                                           bool clipShadingToFlats,
                                                           qreal trappingPx,
                                                           int superSampleFactor)
{
    const bool hasExplicitTarget = !targetSize.isEmpty() && targetSize.width() >= 64 && targetSize.height() >= 64;
    QSize baseSize = hasExplicitTarget ? targetSize : program.canvasSize;
    if (baseSize.width() < 64 || baseSize.height() < 64) {
        baseSize = QSize(1024, 1024);
    }
    // renderProgramToImage() と同じ上限でモデル由来サイズを抑える。明示サイズは
    // 呼び出し元所有のため尊重する。
    if (!hasExplicitTarget) {
        constexpr int MAX_DERIVED_RENDER_EDGE = 4096;
        baseSize = baseSize.boundedTo(QSize(MAX_DERIVED_RENDER_EDGE, MAX_DERIVED_RENDER_EDGE));
    }

    // スーパーサンプリング解像度 (2x または 4x)
    int ssFactor = qBound(1, superSampleFactor, 4);
    QSize renderSize = baseSize * ssFactor;
    // RGBA16F/FP32 の作業バッファが爆発しないよう作業辺を制限する。
    // (例: 8k×4x は RGBA32FP で約16GBになる)
    constexpr int MAX_PHYSICAL_RENDER_EDGE = 4096;
    while ((renderSize.width() > MAX_PHYSICAL_RENDER_EDGE || renderSize.height() > MAX_PHYSICAL_RENDER_EDGE)
        && ssFactor > 1) {
        --ssFactor;
        renderSize = baseSize * ssFactor;
    }
    renderSize = renderSize.boundedTo(QSize(MAX_PHYSICAL_RENDER_EDGE, MAX_PHYSICAL_RENDER_EDGE));

    // トラッピング処理
    const qreal minDim = qMin(renderSize.width(), renderSize.height());
    const qreal effectiveTrapping = trappingPx < 0.0 ? qMax<qreal>(1.0, minDim / 1000.0 * 1.5) : trappingPx * ssFactor;
    KisAiStrokeProgram activeProgram = program;
    activeProgram.canvasSize = renderSize;
    if (effectiveTrapping > 0.0) {
        activeProgram = KisAiStrokeQualityUtils::applyTrapping(activeProgram, effectiveTrapping);
    }

    // レイヤー順
    const QStringList layerOrder = {QStringLiteral("Background"),
                                    QStringLiteral("Flats"),
                                    QStringLiteral("Shading"),
                                    QStringLiteral("Lineart"),
                                    QStringLiteral("Highlights"),
                                    QStringLiteral("FX")};

    // V9: explode composites before layer buckets so lashes land on Lineart,
    // then each layer image still goes through StrokeCommitter.
    QVector<KisAiStrokeOperation> expandedOps =
        KisAiStrokeRenderer::expandProceduralOperations(activeProgram.operations, renderSize);
    expandedOps = KisAiStrokeCommitter::prepareAtomicOps(expandedOps, renderSize);
    KisAiStrokeQualityUtils::applyLineartOcclusionWeights(expandedOps);

    QMap<QString, QVector<KisAiStrokeOperation>> layerBuckets;
    for (const KisAiStrokeOperation &op : expandedOps) {
        const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
        layerBuckets[lName].append(op);
    }

    // 顔除外マスク (FX 用)
    QPainterPath faceExclusionPath;
    for (const KisAiStrokeOperation &op : activeProgram.operations) {
        if (op.kind == KisAiStrokeOperation::Kind::AnimeEye) {
            const QPointF pt = scalePoint(op.eyeCenter, renderSize);
            const qreal ew = op.eyeSize.width() * renderSize.width();
            const qreal eh = op.eyeSize.height() * renderSize.height();
            faceExclusionPath.addEllipse(QRectF(pt.x() - ew * 1.5, pt.y() - eh * 1.5, ew * 3.0, eh * 3.0));
        } else if (op.kind == KisAiStrokeOperation::Kind::Fill) {
            const QString lowerId = op.id.toLower();
            if ((lowerId.contains(QLatin1String("skin")) || lowerId.contains(QLatin1String("face"))
                 || lowerId.contains(QLatin1String("head")))
                && op.polygon.size() >= 3) {
                faceExclusionPath.addPolygon(scalePolygon(op.polygon, renderSize));
            }
        }
    }

    // クロスレイヤーシルエット辞書
    QMap<QString, QPolygonF> globalSilhouettes;
    for (const KisAiStrokeOperation &op : expandedOps) {
        if (!op.id.isEmpty()) {
            if (op.polygon.size() >= 3) {
                globalSilhouettes[op.id] = op.polygon;
            } else if (op.kind == KisAiStrokeOperation::Kind::Path && op.points.size() >= 3 && op.closed) {
                QPolygonF poly;
                poly.reserve(op.points.size());
                for (const auto &p : op.points) {
                    poly.append(p.pos);
                }
                globalSilhouettes[op.id] = poly;
            }
        }
    }

    // 遅延合成グラフを構築
    KisAiCompositeGraph graph;
    graph.size = renderSize;

    for (const QString &layerKey : layerOrder) {
        if (!layerBuckets.contains(layerKey) || layerBuckets[layerKey].isEmpty()) {
            continue;
        }

        const QVector<KisAiStrokeOperation> &ops = layerBuckets[layerKey];
        const bool isShading = (layerKey.compare(QLatin1String("Shading"), Qt::CaseInsensitive) == 0);
        const bool isHighlights = (layerKey.compare(QLatin1String("Highlights"), Qt::CaseInsensitive) == 0);
        const bool isFx = (layerKey.compare(QLatin1String("FX"), Qt::CaseInsensitive) == 0);

        KisAiLayerImage lyr;
        lyr.name = layerKey;

        if (isShading) {
            // Dual shadow separation: Cast shadows and Form shadows
            QVector<KisAiStrokeOperation> formOps;
            QVector<KisAiStrokeOperation> castOps;
            for (const KisAiStrokeOperation &shOp : ops) {
                if (shOp.kind == KisAiStrokeOperation::Kind::Fill
                    && KisAiStrokeQualityUtils::isCastShadow(shOp.polygon, renderSize)) {
                    castOps.append(shOp);
                } else {
                    formOps.append(shOp);
                }
            }

            QImage formImage;
            if (!formOps.isEmpty()) {
                formImage = KisAiStrokeRenderer::renderOperationsToImage(formOps,
                                                                         renderSize,
                                                                         QPainterPath(),
                                                                         globalSilhouettes);
                const int formDiffusionRadius = qMax(4, qRound(qMin(renderSize.width(), renderSize.height()) * 0.010));
                KisAiStrokeRenderer::applySoftEdgeDiffusion(formImage, formDiffusionRadius);
            } else {
                formImage = QImage(renderSize, QImage::Format_ARGB32_Premultiplied);
                formImage.fill(Qt::transparent);
            }

            if (!castOps.isEmpty()) {
                QImage castImage = KisAiStrokeRenderer::renderOperationsToImage(castOps,
                                                                                renderSize,
                                                                                QPainterPath(),
                                                                                globalSilhouettes);
                KisAiStrokeRenderer::applySoftEdgeDiffusion(castImage, 1);
                QPainter p(&formImage);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.drawImage(0, 0, castImage);
            }

            lyr.image = toLinearHdr(formImage);
            lyr.blendMode = QStringLiteral("multiply");
            if (clipShadingToFlats) {
                lyr.clipToLayer = QStringLiteral("Flats");
            }
        } else if (isHighlights) {
            const QImage hlImg =
                KisAiStrokeRenderer::renderOperationsToImage(ops, renderSize, QPainterPath(), globalSilhouettes);
            lyr.image = toLinearHdr(hlImg);

            bool hasDodge = false;
            for (const auto &hop : ops) {
                if (hop.blendMode == QLatin1String("color_dodge")) {
                    hasDodge = true;
                    break;
                }
            }
            lyr.blendMode = hasDodge ? QStringLiteral("color_dodge") : QStringLiteral("screen");
            if (clipShadingToFlats) {
                lyr.clipToLayer = QStringLiteral("Flats");
            }
        } else if (isFx) {
            const QImage fxImg =
                KisAiStrokeRenderer::renderOperationsToImage(ops, renderSize, faceExclusionPath, globalSilhouettes);
            lyr.image = toLinearHdr(fxImg);
            lyr.blendMode = QStringLiteral("screen");
        } else {
            // Background, Flats, Lineart
            const QImage baseImg =
                KisAiStrokeRenderer::renderOperationsToImage(ops, renderSize, QPainterPath(), globalSilhouettes);
            lyr.image = toLinearHdr(baseImg);
            lyr.blendMode = QStringLiteral("normal");
        }

        graph.layers.append(lyr);
    }

    // 物理合成評価
    QImage resultRender = graph.evaluate();

    // ダウンサンプリング (ssFactor > 1 の場合)
    if (ssFactor > 1 && resultRender.size() != baseSize) {
        resultRender = downsampleBox(resultRender, baseSize);
    }

    // フィニッシング後処理
    if (program.stepPhase.compare(QLatin1String("finishing"), Qt::CaseInsensitive) == 0
        || (program.goalReached && program.currentStep >= program.totalSteps)) {
        KisAiStrokeRenderer::applyFinishingPostProcess(resultRender);
    }

    return resultRender;
}

} // namespace KisAi
