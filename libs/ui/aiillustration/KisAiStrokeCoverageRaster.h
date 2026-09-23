/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_COVERAGE_RASTER_H
#define KIS_AI_STROKE_COVERAGE_RASTER_H

#include <QColor>
#include <QPainter>
#include <QPointF>
#include <QSize>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiStrokeProgram.h"

/**
 * V10 Coverage Ink Core.
 *
 * ストローク描画を「複数図形の SourceOver 重ね」から
 * 「1本 = 1枚のカバレッジマスク + 色を1回だけ合成」へ置き換える。
 *
 * マスク内部のピース合成はすべて Lighten (max) で行う。等レベルのピースは
 * 重なっても冪等なので、自己重なり・プロファイル多層・AA 縁の重なりでの
 * アルファ積み上がり (ビーディング) が構造的に発生しない。色は最終の
 * drawImage で1回だけキャンバスへ乗る。
 */
namespace KisAiStrokeCoverageRaster {

/** 作業画像 (スーパーサンプル済み) ピクセル座標のサンプル。 */
struct StrokeSample {
    QPointF pos;
    qreal width {2.0};
};

/** マスク内変調 (プロファイル質感) の方式。 */
enum class TextureStyle {
    Solid, ///< 均一カバレッジ (gpen / fineliner / maru_pen / calligraphy / auto)
    Airbrush, ///< 径向減衰チューブ (コア + ソフトオーラ)
    Watercolor, ///< ウォッシュ + ウェットエッジ縁 + 紙目
    Bristle, ///< 筆毛ストリーム付き本体 (brush)
    Pencil, ///< グラファイト本体 + フィラメント芯
    Charcoal, ///< 粉状本体 + 炭片
    Crayon, ///< ワックス本体 + 粒ダブ
    Marker, ///< フラット本体 + 筆先エッジ
    Neon, ///< 多層グローチューブ (白熱芯は2回目の合成)
    Splatter, ///< 実体 + 飛沫
    Stipple, ///< 点描のみ (本体なし)
    Feathering ///< 多ストランド軽い線のみ (本体なし)
};

struct StrokeTexture {
    TextureStyle style {TextureStyle::Solid};
    quint32 seed {0};
    bool cornerFillets {false};
};

/** プロファイル名 (gpen / watercolor / ...) → テクスチャ方式の対応表。 */
KRITAUI_EXPORT TextureStyle textureStyleForProfile(const QString &profile);

/**
 * 平坦度駆動 Catmull-Rom サンプリング。
 * 各サンプルで taper / 速度変調 / カリグラフィ幅まで解決済みの幅を持つ。
 * scaledPts は作業画像ピクセル座標、pressures は [0,1]。
 */
KRITAUI_EXPORT QVector<StrokeSample> sampleStroke(const QVector<QPointF> &scaledPts,
                                                  const QVector<qreal> &pressures,
                                                  bool closed,
                                                  bool smooth,
                                                  const KisAiStrokeBrush &brush,
                                                  const QSize &workingSize,
                                                  int supersampleScale);

/**
 * ストローク形状ピース (セグメント quad + join 円盤 + キャップ + コーナー
 * フィレット) を target へ描く。呼び出し側がブラシと合成モード
 * (通常は不透明グレー + Lighten) を設定する。等レベル合成は冪等。
 */
KRITAUI_EXPORT void addStrokeCoverageShapes(QPainter &target,
                                            const QVector<StrokeSample> &samples,
                                            bool closed,
                                            bool cornerFillets);

/**
 * ストローク1本を「カバレッジマスク + 色1回合成」で描く。
 * painter のクリップ・合成モードは呼び出し側の設定をそのまま尊重する。
 */
KRITAUI_EXPORT void paintStroke(QPainter &painter,
                                const QVector<StrokeSample> &samples,
                                bool closed,
                                const KisAiStrokeBrush &brush,
                                const StrokeTexture &texture,
                                const QColor &color,
                                const QSize &workingSize,
                                int supersampleScale);

} // namespace KisAiStrokeCoverageRaster

#endif // KIS_AI_STROKE_COVERAGE_RASTER_H
