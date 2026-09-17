/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_PERCEPTUAL_REPAIRER_H
#define KIS_AI_PERCEPTUAL_REPAIRER_H

#include <QColor>
#include <QImage>
#include <QPointF>
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
 * V8 Phase 2: 知覚補正 (Perceptual Repairer).
 *
 * 既存 `KisAiStrokeProgramCodec::refineForRendering()` は「幾何を破壊しない安全補正」
 * に徹するため、ラスタライズ結果から発見できる本質的な視覚破綻を修正できない。
 *
 * このクラスは **第 2 段階の補正** として、以下を自動検出 / 修正する:
 * - Flats 穴 (Flats の連結性欠落)
 * - Lineart と Flats のシーム (境界ギャップ)
 * - Shading はみ出し (Flats 外への侵食)
 * - 顔領域ハッチ (バーコード状の肌崩壊)
 * - 左右非対称 (顔中心軸からのずれ)
 * - カラーバンディング (8bit 量子化ノイズ)
 * - 線画太さジッタ (隣接ストロークの太さ変動)
 *
 * `requiresUserConsent=true` の修正は Plan に記録するだけで適用せず、
 * Docker の承認ダイアログでユーザーが個別許可する。
 */
namespace KisAi
{

/// 知覚 Issue 種別
struct KRITAUI_EXPORT PerceptualIssue {
    enum Type {
        FlatsHole,
        LineartFlatsGap,
        ShadingOverSpill,
        HatchOnFace,
        AsymmetryEye,
        AsymmetryMouth,
        ColorBanding,
        LineartThicknessJitter
    };

    Type type;
    QRectF region; ///< 正規化座標 [0,1] の影響領域
    qreal severity; ///< [0,1]
    QString description; ///< 人間可読の説明
    bool requiresUserConsent; ///< true なら Docker の承認待ち
};

/// 1 つの修正案
struct KRITAUI_EXPORT PerceptualFix {
    PerceptualIssue issue;
    enum Action {
        DropOp, ///< op を drop
        ReplaceOpKind, ///< op の kind を Fill に降格
        InsertPolygonFill, ///< 新規 Fill op を挿入
        ClipPolygon, ///< 既存ポリゴンをクリップ
        SmoothControlPoints, ///< 制御点を平滑化
        AddDither, ///< ディザ op を追加
        ResizeOp, ///< op.brush.size 等を調整
        NoOp ///< 何もしない（提案のみ）
    };
    Action action;
    int targetOpIndex; ///< -1 なら新規挿入
    QString targetOpId; ///< ID による指定 (空なら index を使用)
    QPolygonF newPolygon; ///< InsertPolygonFill / ClipPolygon で使用
    QString replaceKindName; ///< ReplaceOpKind で使用 ("Fill", "GradientFill"...)
    QColor newColor; ///< 色置換で使う
    qreal newOpacity{1.0};
    qreal newSize{0.0};
    QString description;
};

/// 修正計画 (診断結果と推奨される Fix のリスト)
struct KRITAUI_EXPORT PerceptualRepairPlan {
    QVector<PerceptualIssue> issues;
    QVector<PerceptualFix> fixes;
    qreal expectedImprovement{0.0}; ///< QualityVector 期待改善量
    bool hasUserConsentRequired{false};
    QStringList autoFixSummaries; ///< 自動適用される修正の概要
    QStringList consentSummaries; ///< ユーザー承認待ちの概要

    int issueCount(PerceptualIssue::Type t) const;
    int totalIssues() const
    {
        return issues.size();
    }
    int autoFixCount() const
    {
        return autoFixSummaries.size();
    }
    int consentFixCount() const
    {
        return consentSummaries.size();
    }
};

/// 診断器 + 適用器
class KRITAUI_EXPORT KisAiPerceptualRepairer
{
public:
    /// ラスタライズ結果から問題を診断し、修正計画を返す。
    /// program は変更しない (const)。
    static PerceptualRepairPlan
    diagnose(const KisAiStrokeProgram &program, const QImage &renderedImage, const KisAiSceneSpec *spec = nullptr);

    /// 計画を適用して新しい StrokeProgram を返す。
    /// requiresUserConsent=true の Fix は無視し、autoFix のみ適用する。
    /// 完全に空のプログラムの場合、空プログラムを返す。
    static KisAiStrokeProgram
    apply(const KisAiStrokeProgram &program, const PerceptualRepairPlan &plan, bool includeConsentFixes = false);

    /// 診断 + 自動適用を 1 ステップで実行 (consent fix は Plan に残す)
    static KisAiStrokeProgram autoRepair(const KisAiStrokeProgram &program,
                                         const QImage &renderedImage,
                                         const KisAiSceneSpec *spec = nullptr,
                                         PerceptualRepairPlan *outPlan = nullptr);
};

} // namespace KisAi

#endif // KIS_AI_PERCEPTUAL_REPAIRER_H