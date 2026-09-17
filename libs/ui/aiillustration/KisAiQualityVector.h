/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_QUALITY_VECTOR_H
#define KIS_AI_QUALITY_VECTOR_H

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

#include "KisAiStrokeProgram.h"

class KisAiSceneSpec;

/**
 * V8 Phase 1: 多軸品質ベクトル。
 *
 * 既存の KisAiStrokeProgramCodec::qualityScore() の 5 軸固定線形和は「シンボル合否」
 * を加算するだけで、視覚的破綻や幾何の真正らしさを捉えられなかった。
 *
 * このヘッダは評価を「8 軸構造メトリクス + 5 軸知覚メトリクス」の多次元空間に分解し、
 * 軸ごとの個別ゲート判定とテーマ別重みプリセットを実現する。
 */
namespace KisAi
{

/// 構造評価 8 軸。各軸は [0,1] の連続値。
struct KRITAUI_EXPORT StructuralMetrics {
    qreal layerCoverage{0.0}; ///< Flats/Shading/Lineart/Highlights の存在と量
    qreal silhouetteContinuity{0.0}; ///< シルエットの連結性 (穴や断裂の少なさ)
    qreal silhouetteArea{0.0}; ///< 実ポリゴン面積 (正規化座標での和)
    qreal colorHarmony{0.0}; ///< 色相・明度・彩度のバランス
    qreal strokeContinuity{0.0}; ///< 連続ストローク (3+ points) の割合
    qreal intentMatch{0.0}; ///< プロンプトキーワードの反映率
    qreal negativeCompliance{1.0}; ///< 否定語 (noParticlesOnFace 等) の遵守
    qreal symmetryAxisDeviation{1.0}; ///< 顔中心線からのミラーリング差分 (1.0=完全対称、0.0=完全非対称)
};

/// 知覚メトリクス 5 軸。ラスタライズ結果から計算。
struct KRITAUI_EXPORT PerceptualMetrics {
    qreal ssimAgainstReference{0.0}; ///< 構造類似度 (参照: SceneSpec から生成された予測画)
    qreal colorEntropy{0.0}; ///< 色相ヒストグラムのシャノンエントロピー
    qreal edgeDensityBalance{0.0}; ///< エッジ密度の空間的偏り (顔周辺に偏っていないか)
    qreal lineartThicknessStddev{0.0}; ///< 線画太さの標準偏差 (低すぎ=均一すぎ)
    qreal skinBandSmoothness{0.0}; ///< 肌色バンド内の明度勾配の滑らかさ
};

/// 多次元品質ベクトル。16 軸 + 動的重み + 合格フラグ。
struct KRITAUI_EXPORT QualityVector {
    StructuralMetrics structural;
    PerceptualMetrics perceptual;
    QMap<QString, qreal> weights; ///< "structural.silhouetteContinuity" -> 0.18

    /// 16 軸すべての名前一覧 (重みのキー、シリアライズ、デバッグ表示で利用)。
    static QStringList allAxisNames();

    /// 軸名から値を取得 (見つからなければ 0.0)。
    qreal axisValue(const QString &axisName) const;

    /// 軸名から重みを取得 (見つからなければ 1.0)。
    qreal axisWeight(const QString &axisName) const;

    /// 重み付きの合否判定。threshold は各軸個別閾値 (デフォルト 0.40)。
    bool isGatePassed(qreal perAxisThreshold = 0.40) const;

    /// 0-1 の集約スカラー (参考表示用)。
    qreal aggregate() const;

    /// 16 軸すべての値+重みを JSON にエクスポート (ベンチレポート用)。
    QJsonObject toJson() const;
};

/// テーマ別重みプリセット (UI の "Quality Profile" ドロップダウン用)
namespace QualityProfile
{

/// Anime Lineart Heavy: 線画の連続性・対称性を最重視。
KRITAUI_EXPORT QualityVector animeLineartHeavy();

/// Watercolor Soft: 色の多様性と知覚メトリクスを重視。
KRITAUI_EXPORT QualityVector watercolorSoft();

/// Photorealistic: 知覚メトリクス (SSIM / 肌バンド) を最重視。
KRITAUI_EXPORT QualityVector photorealistic();

/// Ink Sketch Bold: 構造メトリクスを高めに、線画ジッタを厳しく評価。
KRITAUI_EXPORT QualityVector inkSketchBold();

/// 組み込みプロファイル名 → デフォルトベクトル。
KRITAUI_EXPORT QualityVector forName(const QString &name);

} // namespace QualityProfile

/// 評価器: StrokeProgram + ラスタライズ画像 + SceneSpec から 16 軸を計算。
class KRITAUI_EXPORT QualityVectorEvaluator
{
public:
    /// 8 軸構造メトリクスを StrokeProgram と SceneSpec から計算。
    static StructuralMetrics evaluateStructural(const KisAiStrokeProgram &program,
                                                const KisAiSceneSpec *spec = nullptr);

    /// 5 軸知覚メトリクスをラスタライズ画像から計算。
    /// spec は任意。null の場合は対称性軸等の補助計算をスキップ。
    static PerceptualMetrics evaluatePerceptual(const QImage &renderedImage,
                                                const KisAiSceneSpec *spec = nullptr,
                                                const KisAiStrokeProgram *program = nullptr);

    /// 構造+知覚を一括評価。
    static QualityVector
    evaluate(const KisAiStrokeProgram &program, const QImage &renderedImage, const KisAiSceneSpec *spec = nullptr);
};

} // namespace KisAi

#endif // KIS_AI_QUALITY_VECTOR_H