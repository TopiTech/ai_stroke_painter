/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_PHYSICAL_RENDERER_H
#define KIS_AI_PHYSICAL_RENDERER_H

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiStrokeProgram.h"

namespace KisAi
{

/**
 * V8 Phase 3: 単一レイヤー画像データ (線形 sRGB 空間・物理ブレンド対応)
 */
struct KRITAUI_EXPORT KisAiLayerImage {
    QString name;            ///< "Background", "Flats", "Shading", "Lineart", "Highlights", "FX"
    QImage image;            ///< RGBA16FPx4 (線形 sRGB) または ARGB32_Premultiplied
    QString blendMode;       ///< "normal", "multiply", "screen", "overlay", "soft_light", "color_dodge", "linear_burn"
    qreal opacityFactor{1.0};///< 不透明度倍率 [0, 1]
    QString clipToLayer;     ///< クリッピング先の親レイヤー名 (例: "Flats")
    QVector<QRectF> dirtyRegions; ///< 増分更新用領域

    bool isValid() const { return !image.isNull() && image.width() > 0 && image.height() > 0; }
};

/**
 * V8 Phase 3: 遅延合成グラフ (Composite Graph)
 *
 * 参照透明性に基づくノード評価を行い、プレビュー時は高速化、最終出力時は高精度物理合成を行う。
 */
struct KRITAUI_EXPORT KisAiCompositeGraph {
    QVector<KisAiLayerImage> layers;
    QImage backgroundImage;
    QSize size;
    QColorSpace colorSpace{QColorSpace::NamedColorSpace::SRgbLinear};

    /// 遅延合成グラフの評価 (線形空間で物理ブレンドを行い、sRGB 出力画像を返す)
    QImage evaluate() const;

    /// レイヤー名から該当レイヤーを検索
    int findLayerIndex(const QString &layerName) const;
    const KisAiLayerImage *findLayer(const QString &layerName) const;
};

/**
 * V8 Phase 3: 物理的レンダリングエンジン
 *
 * W3C CSS Compositing Level 1 準拠のブレンドモデルと線形 sRGB 空間 (RGBA16F) 演算により、
 * 暗部バンディング、色相ドリフト、二重シャドウを根絶する。
 */
class KRITAUI_EXPORT KisAiPhysicalRenderer
{
public:
    /// 色空間変換ユーティリティ
    static float srgbToLinear(float srgb);
    static float linearToSrgb(float linear);

    /// W3C CSS Compositing Level 1 ブレンド関数 (0.0〜1.0 のストレート RGB 値)
    static float blendMultiply(float cb, float cs);
    static float blendScreen(float cb, float cs);
    static float blendOverlay(float cb, float cs);
    static float blendSoftLight(float cb, float cs);
    static float blendColorDodge(float cb, float cs);
    static float blendLinearBurn(float cb, float cs);

    /// 単一ピクセルの物理ブレンド (Premultiplied RGBA 浮動小数点)
    /// src (cs, as), dst (cb, ab) -> result dst
    static void blendPixel(const QString &blendMode,
                           float srcR, float srcG, float srcB, float srcA,
                           float &dstR, float &dstG, float &dstB, float &dstA,
                           float opacity = 1.0f);

    /// レイヤー間物理合成 (dstImage 上に srcLayer を物理ブレンドで重ねる)
    /// clipMask が指定された場合、そのアルファ値でクリッピング (destination_in)
    static void compositeLayer(QImage &dstImage,
                               const KisAiLayerImage &srcLayer,
                               const QImage *clipMask = nullptr);

    /// RGBA16FPx4 (または RGBA32FPx4) のサポート有無
    static bool isHdrFormatSupported();

    /// 通常の sRGB ARGB32 画像を線形 RGBA16FPx4 (または RGBA32FPx4) 画像へ変換
    static QImage toLinearHdr(const QImage &srgbImage);

    /// 線形 RGBA16FPx4 / RGBA32FPx4 画像を sRGB ARGB32_Premultiplied 画像へ変換
    static QImage toSrgbLdr(const QImage &hdrImage);

    /// 4x スーパーサンプリング & ボックスフィルタによる高品質ダウンサンプリング
    static QImage downsampleBox(const QImage &src, const QSize &targetSize);

    /// ストロークプログラムから物理レンダリングによる画像を生成
    static QImage renderProgramToPhysicalImage(const KisAiStrokeProgram &program,
                                               const QSize &targetSize = QSize(),
                                               bool clipShadingToFlats = true,
                                               qreal trappingPx = -1.0,
                                               int superSampleFactor = 2);
};

} // namespace KisAi

#endif // KIS_AI_PHYSICAL_RENDERER_H
