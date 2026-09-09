# AI Stroke Painter

AI Stroke Painter は、Krita のコード基盤をネイティブ C++ アプリケーション
として再構成した AI 向けイラスト制作環境です。Krita プラグインではなく、
PyKrita や Python 実行環境にも依存しません。

テキストで情景を指定し、生成結果を現在のキャンバスへ編集可能なラスターレイヤー
として追加できます。通常のブラシプリセットを選ぶための画面は意図的に省き、
プロンプトからキャンバスへ進むワークフローに集中しています。

## できること

- 右側の **AI Illustration** ワークスペースでプロンプトを入力
- 256–4096 px の新規キャンバスを作成
- **LLM 座標ストローク描画**: OpenAI Chat Completions 互換 API (`/v1/chat/completions`) から LLM が座標指定したストロークを、構図先行の作画計画、座標品質補正、Centripetal Catmull-Rom 曲線、単調な筆圧補間、筆圧入り抜き、2倍スーパーサンプリング、下地クリッピングを用いてレイヤー別（Flats, Shading, Lineart, Highlights, FX）に高品質自動描画
- **手動加筆・修正（ブラシ＆消しゴム）**: 生成されたイラストに対して、左側のツールボックス（ブラシ `B`、消しゴム `E`、スポイト `P`、移動 `T`、バケツ塗り `F`、投げ縄選択）や上部のブラシ設定ツールバー（サイズ・不透明度）を使って直感的に手動修正
- **レイヤーパネル**: AI が生成した各レイヤーを右側パネルで一覧表示し、表示/非表示（目のアイコン）、不透明度、合成モード（乗算・加算等）、並び替え、新規加筆レイヤー追加を自在に操作
- **カラーセレクター ＆ ブラシプリセット**: 色相環・三角形による直感的な色選びと、鉛筆・ペン・マーカー・エアブラシ・消しゴムプリセットの選択
- **ローカル座標ストローク描画**: オフラインで決定論的な多層座標ストロークを生成・描画
- **画像モデル API**: OpenAI 互換の画像生成 API (`/v1/images/generations`) から画像を取得してレイヤーに追加
- **ローカル・コンセプトスケッチ**: オフラインの決定論的コンセプト画像を生成
- `.kra` の読み書き、PNG および Qt が扱える画像形式の入出力
- 各レイヤーごとのブレンドモード（乗算・加算等）、Undo/Redo、保存、別名保存、書き出し

## 起動して最初の画像を作る

Craft でビルドした Windows 版は、次の PowerShell で起動します。

```powershell
$packageBin = 'C:\CraftRoot\ai-stroke-painter\bin'
$env:PATH = "$packageBin;C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;" + $env:PATH
$env:QT_PLUGIN_PATH = 'C:\CraftRoot\plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = 'C:\CraftRoot\plugins\platforms'
& "$packageBin\ai-stroke-painter.exe"
```

1. プロンプト欄に描きたい情景を入力します。
2. 必要なら幅・高さを変更し、**新しいキャンバス**を押します。キャンバスがない
   場合は **生成してレイヤーに追加** を押した時に自動作成されます。
3. 生成方式を選びます：
   - **LLM 座標ストローク描画 (Chat Completions)**: LLM が指定した座標に沿って各レイヤーへ滑らかなスプライン曲線ストロークを描画（推奨）
   - **ローカル座標ストローク描画**: API キー不要でオフラインで座標ストローク描画をテスト
   - **画像モデル API (DALL-E)**: 画像生成モデルのエンドポイントからラスター画像を取得
   - **ローカル・コンセプトスケッチ**: オフラインの単一スケッチ画像を生成
4. **生成してレイヤーに追加** を押します。
5. 生成結果がキャンバスと右側の **レイヤーパネル** に追加されます。レイヤーを選択し、左側の **ブラシツール** や上部の **消しゴム切替 (`E`)** で手動加筆・修正を行えます。

## LLM 座標ストローク描画の設定 (Chat Completions)

LLM 座標ストローク描画モードでは、次の項目を入力します。

- **エンドポイント**: `https://api.openai.com/v1/chat/completions` 等の OpenAI Chat Completions 互換 URL
- **モデル**: `gpt-4o`, `o3-mini`, `deepseek-chat`, ローカル LLM (Ollama, LM Studio) 等
- **API キー**: 現在のリクエストにだけ使用するキー（保存されず即座に消去されます）
- **ストローク予算**: LLM が形状と細部へ配分する座標ジオメトリの上限（既定値: 500）。内部では大形状を優先した操作目標へ変換されます

LLM から返された `StrokeProgram` JSON は、重複点・範囲外座標・退化形状・異常な筆圧/ブラシ値を描画前に補正します。その後 `Flats` (下塗り), `Shading` (陰影・乗算), `Lineart` (線画), `Highlights` (ハイライト・スクリーン), `FX` (効果) の各レイヤーを自動作成し、構造品質スコアとともにキャンバスへ直接レンダリングします。

## 画像モデル API の設定

画像モデル API モードでは、次の3項目を入力します。

- **エンドポイント**: `https://provider.example/v1/images/generations` のような
  OpenAI 互換の画像生成 URL
- **モデル**: プロバイダーが受け付ける画像モデル名
- **API キー**: 現在のリクエストにだけ使用するキー

アプリは次の JSON を POST します。

```json
{
  "model": "your-image-model",
  "prompt": "入力したプロンプト",
  "size": "1024x1024",
  "response_format": "b64_json"
}
```

応答は `data[0].b64_json` に base64 画像を含む OpenAI 互換形式が必要です。
エンドポイントとモデル名は次回起動のためローカル設定に保存されますが、API キーは
保存されず、リクエスト開始後に入力欄から消去されます。

外部ホストには HTTPS が必要です。`localhost`、`.localhost`、IPv4/IPv6 の
ループバックアドレスに限り、ローカル開発用として HTTP を使用できます。URL に
ユーザー名やパスワードを埋め込むことはできません。

安全上の上限として、応答全体は 32 MiB、デコード後の画像は 24 MiB、画像の総画素数は
24 メガピクセルまで、リクエストの待機時間は2分までです。上限を超えた応答や
リダイレクトは取り込みません。

## リソースバンドルについて

Krita のリソースデータベースを初期化するため、配布版には
`Krita_4_Default_Resources.bundle` を1つだけ同梱しています。これは内部初期化用で、
一般的なブラシ選択 UI を復活させるものではありません。

新しい配布版は、起動時に次を自動で行います。

- 初回起動時に標準バンドルをユーザーのリソースフォルダへコピー
- 以前の AI Stroke Painter で作られたプロファイルにバンドルがない場合も復旧
- AI 専用画面では、ブラシプリセット不足を知らせる Krita の旧ダイアログを表示しない

したがって、今回の「You don't have any resource bundles enabled」エラーを避けるために
設定フォルダを削除する必要はありません。古い実行ファイルが起動していないことを
確認し、`C:\CraftRoot\ai-stroke-painter` を新しい配布版で置き換えてください。

## 配布版の場所

既定の配布先は `C:\CraftRoot\ai-stroke-painter` です。主なファイルは次の通りです。

```text
C:\CraftRoot\ai-stroke-painter\bin\ai-stroke-painter.exe
C:\CraftRoot\ai-stroke-painter\bin\ai-stroke-painter.dll
C:\CraftRoot\ai-stroke-painter\bin\data\krita\bundles\Krita_4_Default_Resources.bundle
C:\CraftRoot\ai-stroke-painter\lib\kritaplugins\*.dll
```

配布版はソースツリーの外に置くため、生成オブジェクトがプロジェクトへ混ざりません。
開発者向けの再構成・ビルド・配布手順は [DEVELOPMENT.md](DEVELOPMENT.md) を参照してください。

## Craft で Windows 版をビルドする

`C:\CraftRoot` に Qt/KF6、MinGW GCC、Ninja、CMake の依存環境を準備したうえで、
この README があるディレクトリから実行します。Qt6 は Krita 側で不安定版扱いのため、
`-DALLOW_UNSTABLE=QT6` が必要です。配布ビルドではテストを無効にします。

```powershell
$craftRoot = 'C:\CraftRoot'
$env:PATH = "$craftRoot\dev-utils\meson-venv\Scripts;$craftRoot\bin;$craftRoot\mingw64\bin;$craftRoot\dev-utils\bin;" + $env:PATH
$env:PKG_CONFIG_PATH = "$craftRoot\lib\pkgconfig"

cmake -S . -B build-ai -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_WITH_QT6=ON `
  -DALLOW_UNSTABLE=QT6 `
  -DBUILD_TESTING=OFF `
  -DCMAKE_C_COMPILER="$craftRoot\mingw64\bin\gcc.exe" `
  -DCMAKE_CXX_COMPILER="$craftRoot\mingw64\bin\g++.exe" `
  -DCMAKE_PREFIX_PATH="$craftRoot" `
  -DCMAKE_INCLUDE_PATH="$craftRoot\include" `
  -DCMAKE_LIBRARY_PATH="$craftRoot\lib" `
  -DZLIB_ROOT="$craftRoot" `
  -DPNG_ROOT="$craftRoot"

cmake --build build-ai --target all --parallel 4
cmake --install build-ai --prefix "$craftRoot\ai-stroke-painter"
```

## プライバシーとライセンス

API キーは設定ファイルやレイヤーへ保存しません。ただし、画像モデル API を使う場合は
入力プロンプトとキーが指定したプロバイダーへ送信されます。組織のポリシーに従い、
機密情報をプロンプトへ入力しないでください。

このリポジトリには Krita 由来のコンポーネントが含まれます。各ファイルの著作権表示と
ライセンスを維持し、プロジェクト全体は個別の記載がない限り GPL-2.0-or-later として
扱います。
