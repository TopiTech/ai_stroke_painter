# AI Stroke Painter MVP

Krita 上で、筆圧付きストロークの描画計画を生成して編集可能なペイントレイヤーへ描く MVP です。オフラインのルールベース Planner に加え、OpenAI Chat Completions 互換 API を使う LLM Planner を選べます。

## できること

- 指示文、Seed、本数、キャンバス寸法から再現可能な `DrawingPlan` を生成
- 「髪」または「S字」を含む指示では髪の毛風の S 字カーブ、それ以外ではカーブ線を生成
- `AI Strokes (editable)` ペイントレイヤーへ `Node.paintLine` で筆圧 0.0–1.0 の線分として描画
- 同名レイヤーを再利用。停止ボタンは線分の途中ではなく安全な描画境界で反映
- 検証済みの schema version 付き JSON をユーザーデータ領域へ保存
- OpenAI 互換の `POST /chat/completions` から JSON 計画を取得し、描画前に検証

現在のブラシプリセットと前景色で描画します。ブラシプリセットの自動切替、画像生成、VLM 評価、連続ネイティブストロークは対象外です。

## 動作要件

- Krita 6.0 以降（`Node.paintLine` が必要）
- Python プラグインを有効にした Krita

## インストール

配布物の `dist/ai_stroke_painter.zip` を、Krita の **ツール > スクリプト > Pythonプラグインをインポート** から選択します。Krita を再起動後、**設定 > Kritaの設定 > Pythonプラグインマネージャ** で有効化し、さらに再起動します。最後に **設定 > ドッキングパネル > AI Stroke Painter MVP** を表示します。

ソースから ZIP を作る場合は、プロジェクトディレクトリで次を実行します。

```powershell
python build_plugin.py
```

生成される ZIP は manifest とプラグインパッケージを含むため、そのまま Krita にインポートできます。

## 使い方

1. Krita でドキュメントを開き、希望するブラシと前景色を選びます。
2. Docker に指示、Seed、本数を入力します。同じ入力とキャンバス寸法なら同じ計画になります。
3. **AIストロークを描画** を押します。必要なら **停止** を押します。
4. 既定では計画 JSON が保存され、Docker 下部に保存先が表示されます。

描画結果は通常のペイントレイヤー上のピクセルなので、Krita のレイヤー・消しゴム・Undo で編集できます。Krita のビルドによって Undo の粒度が細かくなる場合があります。

### OpenAI 互換 LLM を使う

1. Planner で **OpenAI 互換 LLM** を選びます。
2. Base URL に API のバージョン付きルート（例: `https://api.openai.com/v1`、またはローカルサーバーの `http://127.0.0.1:PORT/v1`）を設定します。フルの `/chat/completions` URL も指定できます。
3. Chat Completions 対応のモデル名を入力します。API Key は入力欄、または `OPENAI_API_KEY` 環境変数で渡します。

キーは設定ファイルや JSON 計画へ保存されません。互換性を優先して、リクエストは `model` と `messages` を用いる標準的な Chat Completions 形式です。LLM 出力は、schema・本数・座標範囲・筆圧などを検証し、満たさない場合は描画せずエラーにします。LLM モードの Seed はモデルへの指示の一部であり、出力の完全な再現性はプロバイダー側の機能に依存します。

## 設計

- `domain.py`: 検証・JSON 変換を備えた Krita 非依存の `Stroke` / `DrawingPlan`
- `planner.py`: 交換可能なルールベース Planner
- `llm_planner.py`: OpenAI 互換 Chat Completions Adapter と応答検証
- `ports.py`: Planner / Canvas / 将来の Native Bridge 境界
- `krita_adapter.py`: Krita 描画と対象レイヤー管理
- `docker.py`: Docker UI とユースケース制御
- `storage.py`: JSON の衝突しない保存と検証付き読込

将来は `PlannerPort` を LLM Adapter、`CanvasPort` を C++ 拡張または Krita フォーク側 IPC に差し替えられます。ドメイン JSON の `schema_version` はその契約です。

## 検証

Krita を起動せずに、決定性、キャンバス境界、圧力値域、JSON 往復、保存名衝突、レイヤー再利用、停止、ローカル HTTP サーバー経由の OpenAI 互換呼び出しを確認できます。

```powershell
Set-Location ..
python -m unittest ai_stroke_painter.self_test -v
```

## 安全性と制約

オフライン Planner は外部通信を行いません。OpenAI 互換 LLM モードでは、設定した Base URL に描画指示とキャンバス寸法を送信します。保存するユーザーデータは計画 JSON のみで、API キーは保存しません。連続したネイティブ 1 ストロークではなく、短い `paintLine` の列で近似しているため、ブラシや Krita ビルドにより見た目と Undo 粒度が変化する可能性があります。
