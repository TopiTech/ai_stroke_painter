# AI Stroke Painter — V10 Coverage Ink 描画コア刷新計画

- 作成日: 2026-09-23
- 対象: [`libs/ui/aiillustration`](../libs/ui/aiillustration) + [`libs/ui/tests`](../libs/ui/tests) + [`CMakeLists.txt`](../CMakeLists.txt) + [`plans/`](.)
- 前提: V9 Atomic Ink（1本コミット・原子展開・順序制御）は実装済。V4 衛生基盤・V8 品質ベクトルも実装済
- 本書の位置づけ: **ストロークを「複数の図形を QPainter で重ねる」から「1本=1枚のカバレッジマスク+色を1回だけ合成する」へ置き換える**。`drawPathOperation` 以下の描画内核をほぼ全面置換した
- ユーザー確認済み方針: ①カバレッジベース独自ラスタライザ刷新（Krita ブラシエンジンは使わない）②優先品質は「線の均一性」「滑らかさ・継ぎ目解消」③数値ゲート維持・ピクセル差分は許容

---

## 0. 結論（診断と処方）

現状の根本的品質劣化は「**1本の線を複数回 drawPolygon / drawEllipse で重ねる**」ことから構造的に発生する。

| 症状 | 根拠 | 処方 |
| --- | --- | --- |
| 半透明線のビーディング（粒状ムラ） | `drawRoundJoins` が全サンプル点に円スタンプを個別合成 → 重なりでアルファ積算 | セグメント quad + join 円盤 + キャップを**カバレッジマスク内で Lighten (max) 合成**し、色は1回だけ塗る。等レベル合成は冪等なので積み上がりが構造的に発生しない |
| プロファイル内部の濃淡ムラ | 水彩=本体+縁、エアブラシ3層、ネオン4層、鉛筆3フィラメント等を Canvas 上で SourceOver 重ね | これらを**マスク内変調**（カバレッジレベル + Lighten/Multiply）に変換 |
| 急旋回・細線の継ぎ目・段差 | `renderFineLineStroke` の QPen フォールバック、エンベロープと曲線サンプルの乖離 | QPen 分岐を**廃止**。細線・太線・リボンを同一経路（`sampleStroke` の幅解決済みサンプル直結）に統一 |
| 長セグメントのファセット化 | Catmull-Rom 分割が `qBound(4, segLen/6, 24)` 固定上限 | **位置と幅の両方**を見て再帰2分する平坦度駆動テセレーション（深さ上限7）に置換 |

スローガン:

> 線は1回しか塗らない。継ぎ目はマスク内で潰す。分割数は曲率と幅が決める。

---

## 1. 実装アーキテクチャ — Coverage Ink Core

新モジュール [`KisAiStrokeCoverageRaster.h/.cpp`](../libs/ui/aiillustration/KisAiStrokeCoverageRaster.h):

```cpp
namespace KisAiStrokeCoverageRaster {
    struct StrokeSample { QPointF pos; qreal width; };   // 作業画像px・幅解決済み
    enum class TextureStyle { Solid, Airbrush, Watercolor, Bristle, Pencil,
                              Charcoal, Crayon, Marker, Neon, Splatter,
                              Stipple, Feathering };

    QVector<StrokeSample> sampleStroke(...);   // 平坦度駆動 Catmull-Rom + taper/速度/カリグラフィ幅
    void addStrokeCoverageShapes(QPainter&, ...); // quad + join 円盤 + キャップ + フィレット
    void paintStroke(QPainter&, ...);          // マスク生成 + プロファイル変調 + 色1回合成
}
```

### 1.1 描画フロー（1本ごと）

1. `sampleStroke()`: Catmull-Rom を「位置偏差 < 0.15px かつ幅偏差 < 0.25px」になるまで再帰2分（深さ上限7）。taper・速度変調・カリグラフィ幅まで各サンプルで解決
2. `buildStrokeCoverageShapes()`: セグメント quad + **全サンプル位置に join 円盤**（丸 join・丸/テーパーキャップを兼ねる）+ コーナーインクフィレット
3. マスク（グレースケール ARGB、bbox タイル）へ全ピースを **Lighten (max)** で合成 — 等レベルの重なりは冪等、AA 縁も積み上がらない
4. プロファイル変調: エアブラシ=径向グラデーション円盤（max 合成で滑らかなチューブ）、水彩=本体 0.65 + ウェットエッジ縁 0.92 + 紙目、鉛筆=本体 0.78 + フィラメント芯 0.84 等、いずれも**カバレッジレベル操作**
5. `compositeMask()`: マスク → 色付きタイルへ1回変換し `drawImage` **1回**。クリップ・合成モード（eraser は Clear→DestinationOut に読み替え、bbox 全面消去を防止）は呼び出し側の設定を維持
6. Neon のみ白熱芯（2色目）のため2回目の合成

### 1.2 呼び出し側の変更（`KisAiStrokeRenderer.cpp`）

- `drawPathOperation()`: サンプリング〜プロファイル描画（旧 約700行）を `sampleStroke` + `paintStroke` 呼び出しに全面置換
- `renderFineLineStroke()`: **削除**（QPen フォールバックごと）
- `drawRibbonOperation()`: 幅3点補間の結果を `StrokeSample` に載せて `paintStroke(Solid)` へ
- `drawFillOperation()` / `drawGradientFillOperation()`: 単一合成済みのため据え置き（スコープは線の均一性を優先）
- `KisAiStrokeQualityUtils::effectiveWidthPx` を公開（幅解決の単一実装化）

---

## 2. 実装ステップと結果

| ステップ | 内容 | 状態 |
| --- | --- | --- |
| S0 | golden32 ベースライン記録（`bench_gate_debug.txt` + benchmark_report.json + representative PNG） | 完了 |
| S1 | 幾何コア（`sampleStroke` / `addStrokeCoverageShapes`）+ `KisAiCoverageRasterTest` | 完了 |
| S2 | Path カバレッジ化（1回合成）+ ビーディング根絶テスト | 完了 |
| S3 | プロファイル ModulationRecipe 移植（12方式）+ 積み上がり禁止テスト | 完了 |
| S4 | Ribbon 統一 + 自己重なりテスト | 完了 |
| S5 | キャップ・丸 join・コーナーフィレット検証テスト | 完了 |
| S6 | `ctest -L AIStroke` 全15 PASS / golden32 ゲート PASS / 統合ビルド確認 | 完了 |

---

## 3. 受け入れ基準（すべて達成）

1. 半透明ストローク（opacity 0.5）の自己重なり・多層質感で α が 0.5 超に積み上がらない → `testNoAlphaBuildupOnSelfOverlap` / `testProfileTexturesModulateNotStack` / `testCornerPoolNoBeading`
2. 急旋回・キャップ・継ぎ目にカバレッジの隙間・段差が出ない → `testSharpCornerHasNoCoverageGap` / `testTaperedTipAndRoundCap`
3. 長い緩弧にファセット化が出ない（解析曲線からの乖離 ≤ 0.35px）→ `testLongSegmentFlatness`
4. 細線の QPen フォールバックがコード上に存在しない → `renderFineLineStroke` 削除済
5. `ctest -L AIStroke` 全件 PASS、`KisAiQualityBenchGateTest`（golden32 合格率 ≥95%・aggregate 非悪化）PASS、PSNR 整合テスト維持
6. 差分は `libs/ui/aiillustration` + `libs/ui/tests` + ビルド定義 + `plans` に閉じる

## 4. リスクと残件

- **見た目の変化**: 半透明プロファイルは従来より「均一」になる（積み上がり分だけ薄くなる箇所あり）。ピクセル差分許容方針で S0 ベースラインと目視比較済み運用
- **Fill / GradientFill**: 単一合成済みのため据え置き。フリンジ・紙目の完全マスク変調化は将来フェーズの任意項目
- **合成系（Multiply 二重影・マッハ帯）**: 優先度外のためスコープ外（V11 候補）
- **performance**: bbox タイル + ピース描画方式。1024px キャンバスで従来比ほぼ同等（テスト総時間33秒台）

## 5. 検証コマンド（Windows）

```powershell
# スタンドアロンテストビルド + 全テスト
./run-aistroke-ctest.bat
# 新コア単体（詳細ログ）
build-test\KisAiCoverageRasterTest.exe -v2 -o coverage-result.txt,txt
# ゴールデン32 ゲート
python tools/ai_quality_bench/run_bench.py
# 統合ビルド（アプリ + 全テスト + インストール）
powershell -File build-ai-stroke-painter.ps1
```
