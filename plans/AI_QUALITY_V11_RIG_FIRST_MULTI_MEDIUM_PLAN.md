# AI Quality V11 — Rig-first Multi-Medium Hardening (実装記録)

本計画 (llm-stroke-quality-boost-plan.md) の全フェーズ実装記録。V1〜V10 文書は凍結し、本ファイルを正とする。

## 方針

Rig-first + Vision-in-the-loop を正規化。LLM は意味 Spec + Rig/Medium パラメータ + 限定パッチのみを出し、
座標生成は LayoutEngine + RigLibrary + LightRig に一任。Raw 座標経路は legacy として受容・修復のみ。

## Phase A: 契約硬化

- `KisAiStrokeProgramCodec::strokeProgramJsonSchema()`: blend_mode enum に
  soft_light/darken/lighten を追加 (PhysicalRenderer の 9+ ブレンドに寄せる)、color_hex_format ヒント追加。
- `buildFlagshipDirectives()`: 先頭に Rig-First Contract (0 章) を追加。Unknown・空 id・named color・自由 blend を禁止明記。
- `KisAiFullStroke::buildPayload()`: FULL STROKE CONTRACT を RIG-FIRST 版に更新。
- `KisAiFullStrokeScene::drawingInstructions()`: 対称・T-stop・clip_to_id の Rig-first 指示を追加。
- `KisAiSceneSpecCodec`: `medium` ブロック新設 (medium/paper/brushwork/finish_strength)、
  rig ブロックに body_* 5 キー追加、score/intent に medium 証跡追加、defaultSpec に medium フォールバック追加。
- `OntologyApplier`: medium.* パス対応 + 水彩/厚塗り/鉛筆/木炭/紙目の既定 5 ルール追加。

## Phase B: Rig/Light 多画風

- `KisAiBodyRigParams` 新設 + `RigLibrary::bodyRigOps()` (胴・腕・手。顎→肩→腰アンカー、手は手首接続保証、
  全点 [0.02,0.98] クランプ)。`parametersFromSpec()` は framing/pose から既定化 + 明示 rig 上書き優先。
  `clamped()` に body 範囲 + pose 語彙検証。
- `KisAiLightSettings` に sssStrength/specularGain/edgeDarkening/mediumId を追加。
  `fromSpec()` で medium 別マテリアル応答 (水彩 SSS 抑制+湿式縁、impasto/neon スペキュラ増、pencil/ink SSS 殺し)。
  `synthesizeMaterialOptics()` は SSS/シーン不透明度にゲイン反映。
- `LayoutEngine::applyMediumPipeline()` 新設 (明示 medium 優先、空なら artStyleId) + pencil 分岐追加。
  `generateProgram()` は medium 解決経路に切替。characterProgram/lineart 由来の顔系に body rig を統合
  (upper_body/full_body のみ)。
- `PromptAnalyzer::SemanticSpec` に mediumId/paper/brushwork/canvasIntent を追加し style 分類を鏡像化。
- `ModelRouter`: `specCandidateCountForMedium()` / `critiqueRoundBudgetForMedium()` 追加
  (wash/impasto 系に +1、Fast は据置)。
- `brushPresetName()`: 1:1 プリセット ID 対応 (Ink_Gpen/Chalk_Charcoal/brush/Marker/Glow_Neon/Calligraphy 等)。

## Phase C: Deliberate/Atomic Ink v2

- `lintStroke()`: rig_arm_/rig_hand_ の detached-limb ガード追加。
- `StabilizeOperation/repairOperation`: detached-limb 修復 (画布内へ寄せ直し)。
- `StrokeGraph`: inferGroupId に torso/arm/hand 系、orderForCommit に bodyRank (torso→arm→hand) 追加。
  T-stop 1.2px・対称インターリーブは既存のまま body まで拡張 (group 解決経由)。
- `calculateTaper()`: watercolor/airbrush ( pooling belly + feathered ends)、neon (blunt tube) 追加。
- `QualityVector`: `gateThresholdForMedium()` (水彩 -0.08、impasto -0.05、ink/pencil -0.03、neon +0.02)、
  `forMedium()` (medium→プロファイル解決) 追加。

## Phase D: Vision 批判・パッチ反復

- `VisionCritic::selectCrops()`: hand/arm の prior 帯 + `hand_contour_predict` 予測クロップ追加 (予算末尾)。
- 批判語彙に hands/torso 追加 + エイリアス修復 (hand/arm/limb/finger→hands、body/shoulder→torso)。
  チェックリスト 10 (手・胴接続) + 11 (画風忠実度: 水彩/厚塗り/鉛筆・墨の質感観点) 追加。
- `hasConvergedStructural()` 新設 (PSNR 停滞 + 新規高優先度なしで収束)。
- `ProgramPatch`: /rig/ に body_* 5 キー、/medium/ (medium/paper/brushwork/finish_strength) 新設。
  isMediumKey/isMediumValueValid 検証、apply は ops 非接触 (relayout 側で再解決)。
- `RefinementLoop`: rigStateSnapshot に body+medium、applyRigPatchesAndRelayout は /rig//medium/ 両対応、
  medium 値は再検証の上 relaid.medium へ、rig body は delta 経由で relayout。merge の rig id に torso/arm/hand 追加。

## Phase E: レンダラー統合・仕上げ

- `KisAiStrokeRenderer::applyMediumFinish()` 新設 (FinishV3 × medium.finishStrength × medium 応答:
  水彩 bloom 0.45x、neon 1.6x、impasto 1.25x、pencil/ink 0.6x)。`applyFinishingPostProcess()` は既定係数で委譲
  (既存の見た目を維持)。
- `degradeNoteForMedium()` 新設 (wash 系は bloom 先捨て、glow 系は crop/round 先捨て)。
- `adaptiveSupersampleScale()`: wash 系は顔あり小キャンバスで 3x→2x。
- `PhysicalRenderer::renderProgramToPhysicalImage()`: wash 系は要求 ss-1 スタート (4096 bound 逓減は維持)。

## Phase F: テスト・基盤 (UI は次段階)

- 更新: `testBrushPresetMapping` (V11 ID)、`testFlagshipDirectivesAndTokenScaling` (Rig-First 断言)、
  `testStrokeProgramJsonSchema` (blend 拡張 + hex ヒント)。
- 新規:
  - V5 `testPatchBodyRigAndMediumKeys` (body/medium パース・検証・relayout・構造収束・hand 予測 crop)
  - V7 `testBodyRigOpsStructure` (8+ ops・クランプ拷問)、`testMediumPipelineResolution`
  - V10 `testBodyRigAnchoringAndSymmetry` (胴・腕・手対称 + full_body レイアウト統合)、
    `testMediumFinishAndQualityGates`
  - AtomicInk `testBodyRigGroupsOrderTorsoFirst` (torso→hand 順序)
  - Coverage `testWashAndNeonTaperProfiles`、Ontology `testMediumContractMapping`
- 検証: `cmake -B build-test -G Ninja -DAI_STROKE_STANDALONE_TESTS=ON` + `ctest --test-dir build-test`
  18/18 全緑 (41.8s)。
- 残作業 (次段階): Docker の画風プリセット UI・Rig/Finish スライダ・Vision 反復の進捗/差分プレビュー配線。
  フル Krita ビルドを要するため本 V11 では core のみとし、Docker は未接触。
