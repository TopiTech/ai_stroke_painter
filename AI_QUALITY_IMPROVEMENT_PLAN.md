# LLM描画機能 描画クオリティ改善計画案

- ステータス: 案 (2026-09-11)
- 対象: `libs/ui/aiillustration` の LLM 座標ストローク描画パイプライン
  (Docker → StrokeProgramCodec → StrokeRenderer + PromptAnalyzer / QualityUtils)

## 1. 目的

LLM が生成する StrokeProgram v2 の構造品質と、最終的なラスタライズ結果の画質を
段的に引き上げる。既存の強み (堅牢な JSON パース/修復、Goal Mode の視覚フィード
バックループ、Flats/Shading/Lineart/Highlights/FX レイヤー階層、Undo/Redo 対応)
を壊さずに施策を重ね、効果は KPI で測定可能にする。

## 2. 現状のボトルネック (コード調査に基づく)

| # | 課題 | 現状 | 影響 |
| --- | --- | --- | --- |
| 1 | 自己修復ループが JSON 構文エラー限定 | `scheduleGoalStepRetry()` / `scheduleRetry()` は `m_lastJsonDiagnostic` から `[CRITICAL RETRY / JSON ERROR...]` を組むだけ。`refineForRendering()` が算出する `KisAiStrokeQualityReport` の warnings (例: "Flats layer is missing") は描画に使われず捨てられている | 幾何・構成の質が悪い出力がそのまま採用される |
| 2 | 構造化出力 (json_schema) 未使用 | `strokeProgramJsonSchema()` が存在するが、payload は `response_format: json_object` のみ | 型崩れ・欠損フィールドの発生源になり再試行コスト増 |
| 3 | 単発プロンプトで幾何を直接生成 | キーワード解析 (`KisAiPromptAnalyzer`) + 1回の Chat Completions で完成プログラムを要求 | 弱いモデルほど構図破綻・座標散漫が起きやすい |
| 4 | Goal Mode の継続性情報が不足 | `buildGoalStepPayload()` はスクリーンショット + フェーズ指示のみ。蓄積済みプログラムの幾何ダイジェスト未送信。`agentCritique` / `readinessScore` は UI 表示のみ、`recommendedAction` はパースだけされて次ステップに未還元 (KisAiStrokeProgram.cpp:1843, 1858) | ステップ間の重複描画・競合、エージェント判断の断絶 |
| 5 | Vision の detail が固定 low | KisAiStrokeProgram.cpp:3718 | 仕上げ段階の細部批評精度が上がらない |
| 6 | トラッピング未接続 | `KisAiStrokeQualityUtils::applyTrapping()` が実装・テスト済みなのに誰からも呼ばれていない | Flats と Lineart の間の白い隙間 (underfill seam) が残る |
| 7 | 色彩ハーモニー補正が LLM 出力に未適用 | `calculateHueShiftedShadow/Highlight` はプロシージャル合成専用。LLM の Shading/Highlights 色は素通り | 影・ハイライトの色がベース色と無関係になりがたい |
| 8 | 仕上げポストプロセスがプレビュー専用 | `applyFinishingPostProcess` (bloom/CA/vignette) は `renderProgramToImage` のみ (KisAiStrokeRenderer.cpp:347 付近)。実キャンバスの `renderProgramToLayers` には未適用 | プレビューと仕上がりに差が出る |
| 9 | 意味論ガードの実行時検査なし | "顔に hatch 禁止""無関係な manga_lines 禁止" はプロンプト上の約束のみ。純黒線画の補正 (refine 内) 以外は検査なし | ガイドライン違反出力がそのまま画になる |
| 10 | 品質スコアが構造指標のみ | `qualityScore()` はレイヤー被覆・プリミティブ多様性・操作数・細部量・画面占有率の加重 | シルエットの一貫性・焦点コントラストなど「見た目の質」を捉えられない |
| 11 | トークン予算推定が粗い | 操作あたり約160トークン想定。密な polygon では切り詰め → `repairTruncatedJson` → 自己修復ループのコスト増 | 失敗再試行が増え体感レイテンシ悪化 |

## 3. 改善施策

### P0: 即効施策 (工数目安: 各0.5〜2人日)

#### A1. 品質フィードバック自己修復ループ
- **内容**: `refineForRendering()` の `KisAiStrokeQualityReport` (score / warnings /
  droppedOperations) を保持し、構造品質が閾値未満 (例: score < 0.55 または Flats 欠落)
  の場合に JSON エラーと同様の再試行を発火。フィードバック文に warnings とレイヤー別
  操作数サマリ (`formatLayerSummary()`) を含め、「Flats を追加せよ」「退化操作を減らせ」
  のように具体的に指示する。既存の 2 段階再試行バジェットを「構文エラー / 品質エラー」
  で枠を分けて消費する。
- **対象**: `KisAiIllustrationDocker.{h,cpp}`, `KisAiStrokeProgram.{h,cpp}`
- **検証**: 質の悪い固定入力から警告文が生成される単体テスト。Goal/単発両モードで
  再試行フラグの伝播を確認。
- **効果**: 出足の悪い生成を 1 ラウンドで持ち直させる。最大の費用対効果。

#### A2. Flats トラッピングの接続
- **内容**: 描画直前に `applyTrapping()` を Flats 操作へ適用 (既定 1.5px、
  詳細設定 UI にスピンオプションを追加)。Shading/Highlights の Flats クリップ
  マスクにも展開した Flats を使用する。
- **対象**: `KisAiStrokeRenderer.cpp` (`renderProgramToImage` / `renderProgramToLayers`),
  `KisAiIllustrationDocker.cpp`
- **検証**: 既存 `KisAiStrokeRendererTest` のクリッピング系テストに seam 画素
  (Flats 境界の透明抜け) チェックを追加。
- **効果**: 白い隙間の恒久解消。レンダリング品質の見た目が即改善。

#### A3. Goal Mode のコンテキスト還元
- **内容**: `buildGoalStepPayload()` の user メッセージに次を追加:
  1. 直前ステップの `agent_critique` / `target_focus_area` / `recommended_action`
  2. 蓄積プログラム (`m_goalAccumulatedProgram`) の幾何ダイジェスト
     (レイヤー別操作数、操作 id + バウンディングボックスの重心、キャンバス占有率)。
     トークン節約のため id と要約のみで全文幾何は送らない。
- **対象**: `KisAiStrokeProgram.{h,cpp}`, `KisAiIllustrationDocker.cpp`
- **検証**: payload 生成の単体テスト (JSON に digest フィールドが含まれること)。
- **効果**: ステップ間の重複・競合防止、エージェント的な連続性の向上。

#### A4. Vision detail の設定化
- **内容**: `detail: low` 固定を廃止し、最終ステップ (finishing) では `high`、
  中間ステップは `low` を既定に。詳細設定にコンボで上書き可能に。最終ステップの
  キャプチャ解像度も 768 → 1024px へ引き上げ。
- **対象**: `KisAiStrokeProgram.cpp` (`buildGoalStepPayload`), `KisAiIllustrationDocker.cpp`
- **効果**: 仕上げ批評・微妙な色ズレの検出精度が向上 (トークン増は最終1回分のみ)。

#### A5. 意味論ランタイム Lint
- **内容**: `refineForRendering()` に軽量な意味論検査を追加:
  - Lineart の純黒補正 (既存) に加え、Shading の face 領域らしき bbox 内 hatch を
    `fill` (watercolor) へ自動変換
  - 対戦/アクション系キーワード無しプロンプトの `manga_lines` を警告付きで drop
  - Flats 未満の極小 polygon (面積 < 0.0001) の束を統合または drop
  違反は quality report warnings に積み、A1 の再試行フィードバックに流用。
- **対象**: `KisAiStrokeProgram.cpp` (`refineForRendering`), `KisAiPromptAnalyzer.{h,cpp}`
- **検証**: 違反プログラムの変換/ドロップを検証するテーブル駆動テスト。
- **効果**: 「 ugl y barcode lines」等の頻出失敗パターンを自動矯正。

#### A6. サンプリング既定値の見直し
- **内容**: 単発モードの既定 temperature を 0.7 → 0.5 に変更 (座標生成は決定論性が
  望ましい)。自己修復時の ≤0.20 は現状維持。seed フィールドを payload に載せ、
  対応プロバイダでは再現性を確保。
- **対象**: `KisAiIllustrationDocker.cpp`, `KisAiStrokeProgram.cpp`
- **効果**: 出力のばらつき低減。設定 UI からの変更は可能なまま。

### P1: 構造改善 (工数目安: 各2〜5人日)

#### B1. json_schema 構造化出力対応
- **内容**: `strokeProgramJsonSchema()` を `response_format: {type: "json_schema", ...}`
  として送るモードを追加。`supportsJsonFormat()` を拡張し、json_schema 非対応
  プロバイダでは json_object へ自動フォールバック。Goal Mode の 4 フェーズ指示
  フィールド (agent_critique 等) も schema に追加。
- **対象**: `KisAiStrokeProgram.{h,cpp}`, `KisAiIllustrationDocker.cpp`
- **検証**: payload 構造テスト。実プロバイダ接続時は接続テストボタンで capability 確認。

#### B2. 2段階生成 (Composition Plan → StrokeProgram)
- **内容**: 任意設定として 2 コール化。第1コールで構図プラン JSON
  (焦点・地平線・主要シルエット bbox・パレット・前景/中景/背景) を取得し、
  第2コールでプランを system コンテキストに注入して StrokeProgram を生成。
  弱いモデルほど効果が大きい。オフ (従来の1コール) を既定にし、詳細設定で選択。
- **対象**: `KisAiStrokeProgram.{h,cpp}` (plan payload / plan schema 追加),
  `KisAiIllustrationDocker.cpp`
- **検証**: プラン→プログラムの注入テスト。スタンドアロンテストの高速性を維持。
- **効果**: 構図破綻の根本対策。大きな品質レバー。

#### B3. 色彩ハーモニー正規化パス
- **内容**: refine 後に Shading/Highlights のブラシ色を、包含判定
  (polygon 重心が Flats polygon 内か) で対応する Flats ベース色から
  `calculateHueShiftedShadow/Highlight` で再導出するオプションを追加。
  LLM が明示的に彩度の強い色を指定した場合は尊重 (上書き不可フラグ)。
- **対象**: `KisAiStrokeQualityUtils.{h,cpp}`, `KisAiStrokeRenderer.cpp`
- **検証**: 含抱判定と色導出の単体テスト (既存 hair/foliage 合成テストと同型)。

#### B4. 実キャンバスへの仕上げパス
- **内容**: `renderProgramToLayers()` の完成ステップで bloom (Highlights/FX の
  輝度抽出 → 合成レイヤー追加) を Undo 可能な 1 レイヤーとして追加。CA/vignette は
  オプション扱い (ダイレクト描画は元に戻せないためレイヤー化が前提)。
- **対象**: `KisAiStrokeRenderer.{h,cpp}`, `KisAiIllustrationDocker.cpp`
- **検証**: レイヤー数・合成モード・Undo を確認する UI レベルテスト + 手動確認。

#### B5. qualityScore v2
- **内容**: 既存加重に追加: Flats シルエットの連続性 (最大 polygon 面積比)、
  筆圧分散、ストローク長の連続性 (2 点 fragment の比率)、ユニーク色相数、
  焦点域コントラスト密度。決定論を維持し、しきい値は定数としてテスト可能に。
- **対象**: `KisAiStrokeProgram.cpp` (`qualityScore`), テスト
- **効果**: A1 の自動再試行ゲートと Goal Mode の early-finish 判定精度が向上。

#### B6. 操作予算の強制と優先順位付け
- **内容**: 操作数が schema 上限 (160) を超えた場合、role/レイヤー重要度順に
  トリミング (Flats 被覆・焦点細部を優先し、断片 path を間引く)。トークン推定を
  操作あたり 220〜260 に上方修正し、切り詰め検知時は次回の `operation_target`
  を自動で段階減算。
- **対象**: `KisAiStrokeProgram.cpp` (refine / payload), `KisAiIllustrationDocker.cpp`

### P2: 中長期施策

#### C1. 評価ハーネス拡張
- 代表プロンプトセット (Character/Landscape/Cyberpunk/Creature/Botanical/MangaFx 各3本)
  を golden set 化し、`AI_STROKE_TEST_ARTIFACT_DIR` 出力 + 画素メトリクス
  (被覆率・色多様度・エッジ密度) のしきい値回帰を `ctest` に組み込む。
  LLM 実応答は録画フィクスチャ (JSON) として parse→refine→render の回帰に使用。

#### C2. PromptAnalyzer v2
- 多言語キーワード拡張、空間アンカー語 (「右側」「中央」「上部」等) の解析と
  プロンプトへの反映、B2 の第1コールで得た構図プランによる spec 上書き。

#### C3. role フィールドの活用または廃止
- `role` (`KisAiStrokeOperation.role`) はパースされるが描画に未使用。
  silhouette/detail の優先度付け (B6 と統合) に使うか、schema から外す。

#### C4. テレメトリ
- `logDebug` に修復率・再試行回数・スコア分布のカウンタを追加し、デバッグログの
  コピーでレポートできるように。API キーは記録しない (既存ルール順守)。

#### C5. グラデーション帯域対策
- `drawGradientFillOperation` に 1〜2% のディザノイズを追加し、空のバンディングを緩和。

## 4. ロードマップ

| フェーズ | 施策 | 想定効果 |
| --- | --- | --- |
| 第1週 | A1〜A6 | 自己修復率向上・シーム解消・重複防止。体感品質の底上げ |
| 第2〜4週 | B1〜B6 | 構造化出力と2段階生成で構図破綻を低減。スコアの信頼性向上 |
| 継続 | C1〜C5 | 回帰防止・計測基盤・磨き込み |

## 5. KPI と測定

- JSON 第一パース成功率 (golden set): ≥ 98%
- `refineForRendering` 平均スコア: ≥ 0.75 / droppedOperations 比率: < 5%
- 自己修復 (品質 + 構文) 再試行率: < 10%
- Flats 欠落警告の発生率: < 2%
- 画素メトリクス (被覆率 ≥ 0.70、色多様度 ≥ 120 色) を既存
  `testRepresentativeCompositionQualityMetrics` から golden set 全体へ拡大

## 6. 検証方針

- 単体テスト: `KisAiStrokeProgramTest` / `KisAiStrokeRendererTest` に施策ごとに
  1 件以上追加。高速スタンドアロンテスト (約1秒) を壊さない。
- 実行コマンドは `DEVELOPMENT.md` §5 準拠:
  - `ctest --test-dir build-ai -L AIStroke --output-on-failure --no-tests=error`
  - スタンドアロン: `cmake -B build-test -G Ninja -DAI_STROKE_STANDALONE_TESTS=ON ...`
- 手動確認: Goal Mode 4 ステップの連続描画、Undo/Redo、プレビューと実キャンバスの
  差分、プロバイダ 3 種 (OpenAI 互換 / ローカル LM Studio / json_schema 非対応) での
  フォールバック。

## 7. 実装上の注意

- AI 固有処理は `AI_STROKE_PAINTER_APP` 条件分岐を維持 (DEVELOPMENT.md §8)。
- 新規ファイルには SPDX (GPL-2.0-or-later) ヘッダーを付与。
- API キー・ユーザー設定をログへ出力しない。HTTPS/localhost ルールを維持。
- i18n: 新規ユーザー向け文字列は `i18n()` 経由で日本語メッセージを追加。
- 既存の JSON 修復系 (`repairJsonSyntax` / `repairTruncatedJson` /
  `extractOperationsFromRawText`) は資産。破壊的変更ではなく上位に品質ゲートを重ねる。
