# AI Stroke Painter 開発者ガイド

この文書は、Krita のソース基盤から再構成した AI Stroke Painter を変更・ビルド・
配布する開発者向けの手順です。利用者向けの操作説明は [README.md](README.md) を
参照してください。

## 1. プロジェクトの境界

このリポジトリは、通常の Krita を設定で切り替える構成ではありません。ルートの
`CMakeLists.txt` が `AI_STROKE_PAINTER_APP=1` を常に定義し、AI Stroke Painter 専用の
実行ファイル・メニュー・リソース構成をビルドします。

保持しているのは、KRA/PNG 入出力、画像・色管理、キャンバス、Undo/Redo など、AI 生成
結果を編集可能なレイヤーとして扱うための Krita 基盤です。PyKrita、SIP、一般的な
ブラシエンジンの選択 UI、広範な形式プラグイン、テンプレート、ワークスペースは対象外です。

現在の配布・起動確認環境は Windows + Craft + MinGW GCC + Qt6 です。Qt6 は上流 Krita
で不安定版扱いのため、構成時に `-DALLOW_UNSTABLE=QT6` を必ず指定します。

## 2. ソース構成

| 場所 | 役割 |
| --- | --- |
| `CMakeLists.txt` | Qt/KF6 の検出、AI 専用コンパイル定義、依存機能の選択 |
| `krita/CMakeLists.txt` | アプリ名、アイコン、Windows ランチャー、インストール先 |
| `krita/main.cc` | アプリケーション起動、デスクトップ ID、AI 専用識別情報 |
| `libs/ui/aiillustration/KisAiIllustrationDocker` | AI Illustration ワークスペース（Dock）UI |
| `libs/ui/aiillustration/KisAiStrokeProgram` | LLM 用 StrokeProgram v2 スキーマ、プロンプト、JSON パーサー |
| `libs/ui/aiillustration/KisAiStrokeRenderer` | 座標ストロークのラスタライズ、Krita レイヤー群（Flats, Shading, Lineart, Highlights, FX）の構築 |
| `libs/ui/aiillustration/KisAiIllustrationRenderer` | 決定論的スケッチ生成およびエンドポイント検証 |
| `libs/ui/KisMainWindow.cpp` | AI 専用メニュー・Dock 構成、旧ブラシ警告の抑止 |
| `libs/ui/KisApplication.cpp` | 起動時のリソース初期化、`bin/data/krita` の探索 |
| `libs/resources/KisResourceLocator.cpp` | 標準バンドルの初回コピー・既存プロファイル復旧 |
| `krita/data/bundles/` | リソース DB 初期化に必要な標準バンドル |
| `krita/data/profiles/`, `shortcuts/`, `metadata/` | 最小限の Krita データ |
| `libs/ui/CMakeLists.txt` | AI Docker/Stroke/Renderer を `kritaui` に組み込むソース一覧 |

### Windows の実行ファイル構造

Windows では、Krita の実装本体を `bin/ai-stroke-painter.dll` にし、軽量な
`bin/ai-stroke-painter.exe` が DLL を起動します。DLL の依存ライブラリと Qt プラグインを
見つけるため、起動前に `C:\CraftRoot\bin`、`C:\CraftRoot\mingw64\bin`、配布先の
`bin` を `PATH` に追加します。

## 3. AI 生成の処理フロー

1. `KisAiIllustrationDocker` がプロンプトを正規化します（空白を整理し、12,000文字で切り詰め）。
2. 現在の View がなければ、指定サイズの RGB8 ラスタ―キャンバスを作成します。
3. **LLM 座標ストローク描画モード**:
   - `KisAiStrokeProgramCodec::buildChatCompletionsPayload()` がプロンプト・キャンバス寸法・作画戦略システムプロンプトから OpenAI Chat Completions (`/v1/chat/completions`) 互換の JSON リクエストを構築します。
   - レスポンスの思考タグ（`<think>`）除去やコードブロック抽出を行い、`StrokeProgram` をパースします。
   - `KisAiStrokeRenderer::renderProgramToLayers()` が、指定された座標・筆圧・スタイルに基づき、`Flats`, `Shading`, `Lineart`, `Highlights`, `FX` の独立した `KisPaintLayer` を自動作成してキャンバスへ直接描画します（Undo/Redo 対応）。
4. **ローカル座標ストローク描画モード**:
   - `KisAiStrokeProgramCodec::createDeterministicProgram()` がプロンプトをシードに決定論的な多層座標ストロークを生成し、上記同様にキャンバスへ描画します。
5. **画像モデル API モード**:
   - `QNetworkAccessManager` が OpenAI 互換の画像生成 API (`/v1/images/generations`) に POST し、`data[0].b64_json` を画像へデコードして新規レイヤーに追加します。
6. **ローカル・コンセプトスケッチモード**:
   - `KisAiIllustrationRenderer::createConceptImage()` が単一のコンセプト画像を生成してレイヤーに追加します。

API リクエストは次の形式です。

```json
{
  "model": "モデル名",
  "prompt": "正規化済みプロンプト",
  "size": "1024x1024",
  "response_format": "b64_json"
}
```

応答全体は32 MiB、デコード後の画像は24 MiB、総画素数は24メガピクセル、待機時間は
120秒が上限です。リダイレクトは手動扱いで、画像データを返さない応答や不正な画像は
レイヤーへ追加しません。

## 4. リソース初期化と起動警告

`krita/data/bundles/Krita_4_Default_Resources.bundle` は、AI UI でブラシを選ばせるため
ではなく、Krita のリソースキャッシュ DB に有効なストレージを1つ提供するために残しています。
配布版では次の場所へインストールされます。

```text
<package>\bin\data\krita\bundles\Krita_4_Default_Resources.bundle
```

現在の基準ファイルは 17,454,102 bytes、SHA-256 は
`4180F474052305E9DE6EAAC1D832C1DDEB0654142BCDE2EB2460629CF23796D0` です。

`KisApplication::initializeResources()` は AI ビルドで実行ファイルの隣の
`data/krita` をインストール済みリソースとして渡します。`KisResourceLocator` は初回
起動時だけでなく、`KRITA_RESOURCE_VERSION` が存在する既存プロファイルにも、基準バンドル
がなければコピーしてから DB を同期します。

標準 `KisMainWindow::slotStoragesWarning()` は、一般 Krita ではバンドルやブラシプリセット
不足時に QMessageBox を表示します。AI ビルドではブラシ選択フローが存在しないため、
この警告だけを無効化し、リソースロケーターによる DB 初期化は維持しています。

## 5. 高速ビルドおよびクリーンビルド（Windows/Craft）

以下はソースルートから実行する推奨配布ビルドです。
Unity Build（`-DAI_ENABLE_UNITY_BUILD=ON`）を有効にすることでヘッダー解析をまとめ、コンパイル時間を大幅に短縮できます。また、システムの論理コア数に合わせて並列ジョブ数（`$env:NUMBER_OF_PROCESSORS`）を割り当てます。

```powershell
$craftRoot = 'C:\CraftRoot'
$env:PATH = "$craftRoot\dev-utils\meson-venv\Scripts;$craftRoot\bin;$craftRoot\mingw64\bin;$craftRoot\dev-utils\bin;" + $env:PATH
$env:PKG_CONFIG_PATH = "$craftRoot\lib\pkgconfig"

cmake -S . -B build-ai -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_WITH_QT6=ON `
  -DALLOW_UNSTABLE=QT6 `
  -DAI_ENABLE_UNITY_BUILD=ON `
  -DBUILD_TESTING=ON `
  -DCMAKE_C_COMPILER="$craftRoot\mingw64\bin\gcc.exe" `
  -DCMAKE_CXX_COMPILER="$craftRoot\mingw64\bin\g++.exe" `
  -DCMAKE_PREFIX_PATH="$craftRoot" `
  -DCMAKE_INCLUDE_PATH="$craftRoot\include" `
  -DCMAKE_LIBRARY_PATH="$craftRoot\lib" `
  -DZLIB_ROOT="$craftRoot" `
  -DPNG_ROOT="$craftRoot"

# アプリケーション本体と AI ストローク単体テストを並列ビルド
cmake --build build-ai --target ai-stroke-painter KisAiStrokeProgramTest KisAiStrokeRendererTest -- -j$env:NUMBER_OF_PROCESSORS
cmake --install build-ai --prefix "$craftRoot\ai-stroke-painter"
```

### 単体テストの実行（CI / ローカル）

AI ストロークのパース・スキーマ生成・スプライン曲線補間・クリッピングマスクの回帰テストを実行します。

```powershell
ctest --test-dir build-ai -R KisAiStroke --output-on-failure
```

### ビルドが失敗したとき

- Qt6 の「not production-ready」エラーは `-DALLOW_UNSTABLE=QT6` の不足です。
- Zlib/PNG が見つからない場合は `C:\CraftRoot\include\zlib.h`、
  `C:\CraftRoot\include\png.h` と `C:\CraftRoot\lib` の import library を確認します。
- 依存関係や install script が古い場合は、ソースルート直下の `build-ai` だけを削除して
  構成からやり直します。削除前に配布先が完成していることを確認してください。

## 6. 実行確認

配布版の起動環境は次の通りです。

```powershell
$packageBin = 'C:\CraftRoot\ai-stroke-painter\bin'
$env:PATH = "$packageBin;C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;" + $env:PATH
$env:QT_PLUGIN_PATH = 'C:\CraftRoot\plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = 'C:\CraftRoot\plugins\platforms'
& "$packageBin\ai-stroke-painter.exe"
```

最低限、次の項目を手動または隔離プロファイルで確認します。

- 新規プロファイルの起動でエラーダイアログが出ず、AI Illustration Dock が表示される
- ローカルモードで同じプロンプトから同じコンセプト画像が生成される
- キャンバスがない状態から生成すると自動作成される
- 生成結果が新しいレイヤーになり、Undo で取り消せる
- API モードで成功・HTTP エラー・タイムアウト・中止を確認する
- プロファイルから `Krita_4_Default_Resources.bundle` を一時的に外して再起動し、自動復旧する

プロセスが起動直後に終了した場合は、`%LOCALAPPDATA%\ai-stroke-painter\krita.log` と
`krita-sysinfo.log` を確認します。古い実行ファイルを起動していないか、PowerShell の
`Get-Process ai-stroke-painter` で実行パスも確認してください。

## 7. API とセキュリティの保守ルール

- 外部 API は HTTPS のみ許可し、HTTP は localhost/ループバックの開発用だけにする。
- URL にユーザー名・パスワードを含めない。API キーを URL やコマンドライン引数へ置かない。
- API キーを `QSettings`、ログ、レイヤー名、クラッシュレポートへ書き込まない。
- エンドポイントとモデルはローカル設定へ保存できるが、キー入力欄はリクエスト後に消去する。
- 応答サイズ・画像サイズ・待機時間の上限を緩めるときは、メモリ使用量と DoS 耐性を確認する。
- 画像をレイヤーへ追加するときは、元画像の境界・色空間・透明度を維持し、現在ノードを
  直接上書きしない。

## 8. 変更時の注意

- AI 固有の処理は `AI_STROKE_PAINTER_APP` の条件分岐へ置き、通常 Krita のコードパスを
  不要に変更しない。
- メニューや Dock を変更した場合は `KisMainWindow::applyAiIllustrationMode()` の
  表示アクション一覧と、起動時に生成される XMLGUI の整合性を確認する。
- `krita/krita5.xmlgui` を変更する場合は、同ディレクトリの CMakeLists にある
  `KRITA5_XMLGUI_VERSION` と期待ハッシュも更新する。
- リソース配置を変更する場合は、`KisApplication.cpp` の探索先、
  `KisResourceLocator.cpp` の復旧先、`krita/data/*/CMakeLists.txt` の install 先を
  同時に確認する。
- 生成物・API キー・ユーザー設定を Git に追加しない。配布テスト用の一時プロファイルは
  `C:\CraftRoot` 配下に作成し、検証後に削除する。

## 9. ライセンス

Krita 由来ファイルの SPDX 表示とライセンスを維持してください。プロジェクト全体は、
個別の記載がない限り GPL-2.0-or-later です。新しい AI Stroke Painter 固有コードにも
著作権者・年・GPL-2.0-or-later の SPDX ヘッダーを付けます。
