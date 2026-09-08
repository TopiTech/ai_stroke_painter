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
- ローカルの決定論的コンセプトスケッチをオフラインで生成
- OpenAI 互換の画像生成 API から画像を取得
- 生成画像をプレビューし、現在のキャンバスへ新しいラスターレイヤーとして追加
- `.kra` の読み書き、PNG および Qt が扱える画像形式の入出力
- Undo/Redo、保存、別名保存、書き出し

ローカルモードは画像モデルを呼び出さず、プロンプトから再現可能な色・図形・
モチーフを構成します。本格的な画像生成には画像モデル API モードを使用してください。

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
3. **ローカル・コンセプトスケッチ** または **画像モデル API** を選びます。
4. **生成してレイヤーに追加** を押します。生成結果は透明背景の新規レイヤーに
   収まり、元のレイヤーを壊しません。

起動後は **ファイル**、**編集**、**AI**、**ヘルプ** の最小メニューだけを表示します。
AI ワークスペースを閉じた場合は、**AI → AI workspace** から再表示できます。

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
