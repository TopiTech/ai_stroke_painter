# AI Stroke Painter コードレビュー／改善提案レポート

作成日: 2026-08-25  
対象コミット: `11aa270`  
対象: Krita プラグイン本体、オフライン生成、画像変換、OpenAI-compatible LLM 生成、プレビュー、保存、品質検査

## 0. 実装後追記（2026-08-25）

本レポートを起点に実装と再レビューを行い、主経路で再現していた P1/P2 不具合は修正済みである。元の指摘本文は判断根拠として残し、この追記を現在の状態とする。

### 対応結果

- `BUG-01/05/06`: 反復計画を最大2,000本の累積成果物へ統合し、実描画の太さ・不透明度・レイヤーモードを JSON/SVG へ反映。保存失敗は描画成功と分離した。
- `BUG-02/03/04`, `LOGIC-01`: 野花・桜山景・バラ・FX の意味論を分離し、面積ベースの fill 予算、背景を覆わない FX、放射塗り、sparkle/bokeh を実装。内蔵プリセットは Auto 品質予算を既定化した。
- `BUG-07/08/09/10`: LLM の厳格 `json_schema`、決定的救済 ID、seed=0 保持、UIブラシ／パレット契約、累積品質による Goal 判定、意味要約履歴を実装した。
- `PRIV-01`: 参照画像は初回だけ送信し、参照画像と反復キャンバスを縮小・私的メタデータ除去。ログ本文とフルパスの記録も抑制した。
- `BUG-11`: 白紙拒否、外周連結背景マスク、色差エッジ、被写体内ハッチ、背景ハイライト抑制、透明背景内の単色範囲保持を実装した。
- `BUG-12`, `PERF-01`: 一定筆圧の多点線は Krita `paintPath` へまとめ、単発 active-layer は計画範囲だけを保存。反復セッションの全面保存には 512 MB の安全上限を設けた。
- `BUG-13`, `UX-01/02`, `OPS-01`: Planner/確認設定の永続化と項目別破損耐性、適用前プレビュー、固定アクション領域、実行中設定ロック、停止・終了待ち、分割受信時のキャンセル確認を実装した。
- `QA-01`: 全12内蔵プリセットを手動／Autoの双方で検査する24シナリオへ拡張し、題材、ブラシ、パレット、被覆、レイヤー、境界、決定性、描画コストを契約化した。

### 最終検証

- Ruff / Ruff format / Mypy / Pyrefly: pass
- 通常回帰テスト: 182件 pass
- Qt 非依存ヘッドレス回帰: 182件 pass（実Qt専用1件 skip）
- イラスト品質ゲート: 24/24 pass
- 配布 ZIP: `dist/ai_stroke_painter.zip`、170,762 bytes、26 entries、ZIP整合性および収録23 Pythonファイルの構文検査 pass
- SHA-256: `310cff657ba21f83278b3372e68c7cfbaf8d2e9e4e6fcd21ac17667ea03df5b1`

### 残る環境依存確認と将来改善

- この環境には Krita 実行ファイルがないため、Krita 6 実機での筆致・Undo・終了処理スモークテストは未実施である。配布前に `krita_smoke.py` を実行する。
- 任意題材を一般化する `SceneSpec`、active-layer 反復スナップショットのタイル／差分化、初回外部送信同意や費用見積り、詳細設定の折り畳みとアクセシビリティは、今回の再現不具合修正を越える次期設計項目として残る。
- Python の同期 HTTP 読み込み中はソケットの1回のブロッキング read 自体を即時中断できない。停止はチャンク境界、再試行待機、またはタイムアウト後に確定する。

## 1. 結論

現状は、ドメインモデルの検証、決定的な乱数、URL/API キー周辺の防御、ロールバック設計、ヘッドレステストなど、MVP の土台はかなり整っている。一方で「テストが通ること」と「ユーザーが選んだ題材を高品質に描けること」の間に大きな隔たりがある。

特に次の5点を、機能追加より先に修正すべきである。

1. 複数反復の最終 JSON/SVG が最終差分だけを保存し、キャンバス上の完成絵を再現できない。
2. 内蔵プリセットの一部が別題材のジェネレーターへ誤分類される。野花が人物顔、山と桜がバラと同一形状になる。
3. UI 既定の少ないストローク数へ落とす処理が、塗り走査線や左右一対の部品を意味単位でなく間引くため、横縞・白抜け・欠損を生む。
4. プレビュー、Krita 実描画、JSON/SVG の合成・消しゴム・倍率が一致せず、事前確認が完成結果を保証しない。
5. 保存失敗が描画失敗として扱われ、正常に描いたキャンバスまでロールバックされ得る。

したがって、現時点のリリース判定は「内部検証版としては可、品質を訴求する一般配布版としては要修正」とする。まず P1 を解消し、その後に題材解釈とレンダリング品質の再設計へ進むのが安全である。

## 2. 調査範囲と方法

### 実施した確認

- 全ソース、README、マニフェスト、ビルド／検査スクリプト、テスト群の静的レビュー
- `python check.py` による lint、format、型検査、通常／ヘッドレステスト、品質ゲートの実行
- 内蔵12プリセットを、UI が実際に設定する既定ストローク数と Auto の両方で生成・比較
- 同一 seed/count で座標列を比較し、異なる題材が同じ形状へ落ちていないか確認
- 実際の `PreviewWidget` でプリセットのコンタクトシートをラスタライズして目視確認
- 白、黒、灰色、透明画像を画像変換器へ入力する境界値確認
- Krita Adapter の呼び出しと公式 Node API の照合

### 自動検査結果

- Ruff / format / mypy: pass
- pyrefly: pass。ただし検査スクリプト上は48警告を非表示にしている
- 通常テスト: 168件 pass
- ヘッドレステスト: 168件 pass
- 現行品質ゲート: 6シナリオすべて pass

この結果は回帰防止の基礎として有用だが、品質ゲートは `count=None`、すなわち最大500本の Auto 予算でしか評価していない。内蔵 UI プリセットは通常30〜45本かつ `auto_count=False` なので、ユーザーが最初に見る経路を検査していない。

### 制約

この環境には Krita 実行ファイルがなく、実 Krita 上の筆致、Undo、レイヤー合成、終了処理は実行できていない。Qt プレビューと fake/stub を使った検証である。Krita 統合に関する指摘はコードと [Krita Node API](https://api.kde.org/legacy/krita/html/classNode.html) の照合に基づくため、最終的には対応 Krita 各版でのスモークテストが必要である。

## 3. 現状の構成と良い点

処理の主経路は概ね次のとおりである。

`Docker UI -> PlanWorker -> PlannerPort -> StrokeProgram v2 -> DrawingPlan -> CanvasPort/KritaCanvasAdapter`

維持すべき良い点は以下である。

- `domain.py` と `stroke_program.py` で入力範囲、不透明度、色、座標、上限を検証している。
- プロシージャル生成の多くは seed と `uuid5` により再現可能である。
- LLM 接続は API キーを永続化せず、非 HTTPS を原則拒否し、loopback のみ例外にしている。
- クロスオリジン redirect 拒否、ログの資格情報マスキング、応答サイズ制限がある。
- キャンセル／失敗時に作業レイヤーを戻すセッション設計がある。
- Planner と Canvas の Port 分離、Qt なしの headless import、fake Krita を使うテストがある。
- JSON/SVG 保存形式とプログラム v2 の互換レイヤーが用意されている。

今後の修正では、これらを壊さず「意味単位の計画」「実レンダリングと同じプレビュー」「生成と適用のトランザクション分離」を足すのがよい。

## 4. 優先度の定義

| 優先度 | 意味 |
|---|---|
| P0 | データ損失、資格情報漏えい、恒常的クラッシュ。今回は確定項目なし |
| P1 | 主経路で誤った絵、復元不能な出力、意図しない巻き戻し、重大なプライバシー問題を起こす |
| P2 | 品質、信頼性、費用、操作性を大きく下げるが回避策はある |
| P3 | 一貫性、アクセシビリティ、保守性、細部の改善 |

## 5. 重要指摘一覧

| ID | 優先度 | 分類 | 要約 |
|---|---:|---|---|
| BUG-01 | P1 | 保存／再現性 | 複数反復の保存物が最終差分だけになる |
| BUG-02 | P1 | 題材解釈 | 野花が人物、山と桜がバラになる |
| BUG-03 | P1 | 画質 | 本数制限が塗りと意味グループを破壊する |
| BUG-04 | P1 | 合成 | FX が強制背景で下絵を覆い、集中線は黒地に黒線になる |
| BUG-05 | P1 | トランザクション | JSON/SVG 保存失敗で成功した描画まで失敗扱いになる |
| BUG-06 | P1 | WYSIWYG | プレビュー、Krita、保存結果が一致しない |
| BUG-07 | P1 | LLM | UI のブラシ指定が LLM モードでは無視される |
| PRIV-01 | P1 | プライバシー／費用 | 参照画像の原本を毎反復 Base64 送信する |
| BUG-08 | P2 | LLM 検証 | 不正応答を架空の線・全面塗りへ救済する |
| BUG-09 | P2 | 再現性 | seed=0 の実ジオメトリと記録 seed がずれる |
| BUG-10 | P2 | 自動改善 | Goal 判定がモデルの自己申告だけで、履歴も不完全 |
| BUG-11 | P2 | 画像変換 | 白背景をハイライトとして大量に描き、白画像も成功する |
| BUG-12 | P2 | Krita | 1本の線を各区間 `paintLine` へ分割し、筆致が途切れる |
| PERF-01 | P2 | メモリ／速度 | active layer の全面スナップショットと指紋を反復する |
| BUG-13 | P2 | 設定 | planner mode が保存されず、復元した LLM 設定が解除される |
| UX-01 | P2 | UI | 生成とキャンバス適用が分離されず、有料呼び出し後すぐ描画する |
| UX-02 | P2 | UI | 実行ボタンと状態が長いスクロールの末尾、実行中も設定編集可能 |
| QA-01 | P1 | 品質保証 | 品質指標とテストが題材一致・白抜け・実際の既定値を検知しない |
| LOGIC-01 | P2 | 表現 | bokeh/sparkle、fill の幾何・間隔が宣言どおりの見た目にならない |
| OPS-01 | P2 | ライフサイクル | HTTP 中断と worker 終了待ちが弱く、閉じる時の競合余地がある |

## 6. 不具合の詳細

### BUG-01: 複数反復の保存物が最終差分だけになる

根拠:

- `docker.py:2121-2143` は反復ごとの plan を `_last_plan` で上書きし、プレビューだけ `accumulate=True` にしている。
- `docker.py:2208-2231` は各 plan をキャンバスへ追加描画する一方、最終時の `save_plan(plan)` / `save_svg(plan)` には現在の plan しか渡さない。

影響:

- 3段階生成なら、キャンバスには Flats + Shading + Lineart があるのに、保存 JSON/SVG は最終 Lineart/Highlights/FX だけになる。
- 保存物を再適用しても完成絵を復元できず、監査、再編集、バグ報告にも使えない。

推奨修正:

- 実行開始時に `GenerationSessionArtifact` を作り、反復 plan を順序付きで蓄積する。
- `combined_plan` は ID 衝突を検査し、キャンバス寸法、prompt、seed、iteration metadata を統合する。
- 最終保存は `combined_plan`、単一反復のデバッグ保存は `iteration_plan` と明示する。
- JSON に `render_options` を持たせるか、保存前に size/opacity 倍率を適用した materialized plan を作る。

受け入れテスト:

- iteration 1 に `flat-1`、iteration 2 に `line-1` を返す fake planner で、最終 JSON/SVG の双方に2本が含まれる。
- 保存物をロードして空キャンバスへ適用した結果が、元セッションの最終レンダーと一致する。

### BUG-02: プリセットと題材分類が一致しない

根拠:

- `procedural/__init__.py:57-159` は単一カテゴリを返し、未知語は `character` に落とす。
- 英単語は境界一致なので `wildflower` は `flower` に一致せず、内蔵「野花」プリセットは人物へ落ちる。
- `procedural/landscape.py:37-40,146-186` は `sakura` を `is_flower` とし、山や雲の語が同居していても汎用バラ螺旋を選ぶ。
- 分類は `if/elif` 的な単一 dispatch で、人物 + 背景、動物 + 都市の混合プロンプトを合成しない。

実測:

- `delicate watercolor wildflower garden with soft petals` は `character`。
- 同一 seed/count の野花と anime girl は、色以外の全座標列が一致した。
- 同一 seed/count の mountain+sakura と rose は、色以外の全座標列が一致した。
- sports car、still life、castle interior など未対応題材も人物になる。

推奨修正:

- 単一カテゴリではなく `SceneSpec(subjects, environment, effects, style, composition)` の multi-label 解析へ変更する。
- 内蔵プリセットには自由文だけでなく、明示的な generator/motif ID を保存する。表示文言の翻訳で挙動を変えない。
- `sakura_tree`、`wildflower_meadow`、`rose_bloom`、`mountain_range` を別 motif とし、同時に compose できるようにする。
- 未対応題材は人物へ偽装せず、「汎用抽象」「LLM利用案内」「未対応」のいずれかを明示する。

受け入れテスト:

- 12内蔵プリセットすべてに期待 motif を宣言し、分類契約テストを追加する。
- 山と桜には mountain、tree、blossom の semantic operation が全て含まれる。
- 野花と人物、山桜とバラの正規化座標列が同一でない。
- `anime girl in a mountain landscape` が subject と environment の両モジュールを含む。

### BUG-03: ストローク本数制限が塗りと意味グループを破壊する

根拠:

- `stroke_program.py:1136-1159` は各 operation をコンパイルした後、全ストロークを一括で削る。
- `procedural/base.py:439-540` はレイヤー比率だけで均等サンプリングし、同じ fill の連続走査線、左右の目、輪郭の閉路などの依存関係を知らない。
- operation ごとの最大予算も単純な等分で、余った予算を重要 operation へ再配分しない。

実測:

- anime girl の Auto は189本だが、内蔵既定40本では Flats が66本から10本へ減る。
- boy の推定 coverage は Auto 0.991 に対し既定35本で0.549。
- プレビュー目視では、面が横縞になり、白抜けと片側部品の欠落が顕著だった。

原因は「count」を最終 paint stroke の厳密上限として扱っていることにある。塗り1面が数十本へ展開される現在の表現では、30本は完成イラストの予算として不足している。

推奨修正:

- operation に `group_id`, `semantic_role`, `priority`, `min_budget`, `atomic`, `dependencies` を追加する。
- 先に必須シルエットと面を満たし、残予算を細部へ配る二段階 allocator にする。
- fill は「全部残す／低解像度で再コンパイルする」を選び、走査線の一部だけを削らない。
- 左右一対、閉曲線、瞳と白目などは atomic group として保持またはまとめて省略する。
- UI の「ストローク数」は厳密本数ではなく、Low/Standard/High の品質予算を主にし、詳細画面だけ上限値を見せる。
- 既定プリセットは修正まで Auto を標準にする。

受け入れテスト:

- 既定30〜45本相当の各プリセットで、必須 operation の `min_budget` を満たす。
- 不透明背景を持つプリセットは実ラスタ coverage 95%以上、連続する未塗り横帯なし。
- 左右対称部品の片側だけが残るケースを property test で禁止する。

### BUG-04: FX が背景を覆い、集中線が見えない

根拠:

- `procedural/__init__.py:213-293` は全カテゴリへ foundation を付与し、FX では `colors["lineart"]` の全面背景を作る。
- FX 本体も主に lineart 色を使うため、集中線は暗い背景上の暗い線になる。
- 既存作品へ効果だけ追加したい場合も、全面背景が下絵を覆う。

推奨修正:

- generator の出力モードを `standalone` と `overlay` に分け、FX は overlay を既定にする。
- overlay では foundation を作らず、対象キャンバスの平均明度／局所明度から高コントラスト色を選ぶ。
- standalone の FX 背景と線は WCAG 的な文字基準ではなく、少なくとも知覚色差 `Delta E` と luminance contrast を品質指標にする。

受け入れテスト:

- 集中線 overlay の透明画素率が十分高く、既存テスト画像が保持される。
- 線と背景の知覚明度差が閾値を超える。

### BUG-05: 保存失敗が描画トランザクションを巻き戻す

根拠:

- `docker.py:2158-2248` は `render()` と最終 `save_plan` / `save_svg` を同じ try に入れる。
- 保存例外は `render_error` となり、`docker.py:2249-2253` から worker へ render failure として通知される。
- worker 完了時は成功フラグが立たず、セッションを commit しない経路へ入る。

影響:

キャンバス描画が成功していても、ディスク容量不足、権限、パス問題など任意出力の失敗で全描画を失い得る。

推奨修正:

- `generate -> render -> commit -> export` のトランザクション境界を明確に分離する。
- render 成功時点でキャンバスセッションを commit し、export 失敗は非破壊の警告にする。
- 厳密に「保存も含めて原子的」を選べるモードを作る場合でも、既定はキャンバス保持とする。

受け入れテスト:

- `save_svg` が例外を投げても描画レイヤーは残り、UI は「描画成功／SVG保存失敗」を分けて表示する。

### BUG-06: プレビュー、実描画、保存物が一致しない

根拠:

- `docker.py:293-304` はレイヤー名から Shading=Multiply、Highlights/FX=Plus を常に適用するが、Krita の single/active layer モードでは同一通常レイヤーへ描く。
- `docker.py:295-298` は消しゴムを白い SourceOver 線として描く。透明化でも下層の露出でもない。
- `qt_compat.py:175` に DestinationOut の互換関数があるが使われていない。
- プレビュー背景は `docker.py:260-264` の固定白で、実キャンバス色を見ない。
- UI の size/opacity 倍率は `KritaCanvasAdapter.render` へ渡るが、保存 JSON/SVG は元 plan を使う。

推奨修正:

- preview を semantic layer ごとのオフスクリーンバッファで描き、実際に選んだ layer mode と blend mode で合成する。
- 消しゴムは対象レイヤーバッファへ DestinationOut を適用する。
- active canvas の縮小画像または背景色を preview ベースに使う。
- `RenderOptions` を単一の値オブジェクトにし、preview、Krita、export の三者へ同じものを渡す。
- ピクセル差分テスト用に Qt renderer と fake canvas renderer の共通 golden fixture を持つ。

受け入れテスト:

- normal/multiply/add/eraser と3 layer mode の fixture で、プレビューと基準レンダーのピクセル差が許容値内。
- 2倍サイズ、50% opacity の SVG を再描画した太さ／見た目がキャンバスと一致する。

### BUG-07: LLM モードでブラシ指定が無視される

根拠:

- `docker.py:530` は `brush_profile` を planner へ渡す。
- `llm_planner.py:393-407` の明示引数に `brush_profile` がなく、`**kwargs` からも読み出さない。
- request JSON と system instruction に選択ブラシが入らない。
- palette は文字列ガイダンスに留まり、色を選択 palette 内へ拘束しない。

推奨修正:

- PlannerPort の request DTO を導入し、UI オプションを `**kwargs` でなく型付きで渡す。
- LLM request に brush profile と palette lock 方針を含める。
- 応答後 sanitizer で brush profile を上書きできる `strict UI override` と、モデル提案を許す `advisory` を分ける。
- palette lock 時は色を Lab 空間の最近傍へ量子化する。

受け入れテスト:

- 同じ fake LLM 応答でも UI の gpen/watercolor 選択により最終 `brush_preset` が変わる。
- palette lock では全色が許可 palette または許容色差内に入る。

### PRIV-01: 参照画像原本を毎反復外部送信する

根拠:

- `llm_planner.py:497-519` は `image_data` と canvas をラベルなしの連続した `image_url` として Base64 添付する。
- reference は UI で選んだ原本 bytes であり、縮小、再エンコード、metadata 除去をしていない。
- 同じ reference を各反復で再送するため、最大25MBの入力が繰り返され得る。

影響:

- EXIF/GPS/端末情報などが原本に含まれる場合、そのまま外部 endpoint へ送信される。
- latency、通信量、課金入力を不必要に増やす。
- reference と current canvas の前に明示テキストラベルがなく、モデルが役割を取り違え得る。

推奨修正:

- 選択直後に decode、寸法／容量検証、向き補正、sRGB 化、最大辺縮小、PNG/JPEG 再エンコードを行い metadata を落とす。
- 各 image part の直前へ `REFERENCE IMAGE` / `CURRENT CANVAS` の text part を置く。
- 外部送信する endpoint origin、画像サイズ、反復回数を実行確認画面へ表示する。
- provider が画像参照 ID や cache を提供する場合のみ opt-in で再利用し、未対応時は費用見積りを表示する。
- debug log の完全 prompt とローカルパスは、コピー／保存時に既定で匿名化する。

受け入れテスト:

- EXIF 付き20MP JPEG を入力しても、送信 bytes は上限寸法以下かつ EXIF なし。
- message parts の順序とラベルを unit test する。

### BUG-08: LLM 不正応答を「成功した絵」に捏造する

根拠:

- `llm_planner.py:2147-2155` は path 座標欠落時に左上から右下の対角線を作る。
- `llm_planner.py:2176-2192` は fill 座標欠落時にキャンバス全面を作る。
- `llm_planner.py:2262-2280` は有効 operation がない場合に全面ピンク fill を作る。
- rescue ID の一部は `uuid4` で、同じ応答でも完全な再現性がない。
- `_get_stroke_program_json_schema()` は定義されているが API payload では使われず、`json_object` のみである。

影響:

schema 違反を明確に失敗させず、ユーザーのキャンバスへ無関係な線や全面塗りを適用する。fallback が「形式修復」ではなく「内容創作」になっている。

推奨修正:

- 構文修復と意味修復を分離する。キー別名、文字列数値、座標 clamp までは許容し、欠落ジオメトリは reject する。
- endpoint capability を判定し、対応時は strict JSON Schema structured output を使う。
- operation kind ごとの required/oneOf を定義する。
- malformed 応答時はキャンバスを変更せず、1回だけ schema エラーの要約付き再試行を行う。
- rescue を残す場合も seed と raw response hash から `uuid5` を作る。

受け入れテスト:

- points のない path、polygon のない fill、空 operations は明示エラーとなり、CanvasPort が一度も呼ばれない。

### BUG-09: seed=0 の実ジオメトリと記録 seed がずれる

根拠:

- `llm_planner.py:2344-2350` は `seed > 0` の時だけ要求 seed を採用し、0ならモデル返却 seed を採る。
- 後段で要求 seed=0 として plan metadata を整える経路があり、粒子等の乱数に使った seed と表示値が一致し得ない。

推奨修正:

- 0を有効 seed として常に `seed=seed` を使う。
- 自動 seed は UI 側で具体値を生成し、その値を request、program、plan、保存物へ一貫して記録する。

### BUG-10: Goal/Auto-Refine が独立した品質判定を持たない

根拠:

- 完了判定は plan metadata の `goal_reached`、すなわち生成モデルの自己申告を信頼する。
- text history は先頭15 stroke の端点中心で、fill も path 的な要約になり、キャンバス capture 失敗時の代替情報として不十分。
- phase 文には題材を問わず Petals/Petal Scatter が含まれ、無関係な花びらを誘発する。
- 外側の生成再試行と HTTP/parameter 再試行が重なり、1 iteration の呼び出し上限と費用が UI から分からない。

推奨修正:

- 完了条件を `model self score + deterministic validator + raster quality + required semantic roles` の合議にする。
- phase は SceneSpec から導出し、petal 等の motif 固有語を共通テンプレートから除く。
- history は compiled stroke の先頭抜粋でなく、semantic operation と各 layer のサムネイル／統計を送る。
- `max_requests`, `max_input_bytes`, `max_elapsed`, `max_estimated_cost` の実行 budget を設ける。
- 反復ごとに「前回との差分」のみを要求し、operation ID の重複、全面再塗りを拒否する。

### BUG-11: 画像変換が白背景をハイライトとして描く

根拠:

- `image_converter.py:508-531` は global `luminance > 0.90` かつ alpha > 0.5 を全て highlight とする。
- 白い紙／背景を subject のハイライトと区別しない。
- flat は `image_converter.py:481-505` の格子状横線で、領域分割や輪郭 mask がない。
- edge は輝度 Sobel だけで、色相だけが違う同明度境界、NMS、二重閾値 hysteresis を扱わない。

実測:

- 完全透明画像はエラーになる一方、完全白の不透明画像は成功し、35本の見えない白系 Flats/Highlights を返した。

推奨修正:

- alpha と境界接続性から背景候補を推定し、背景除外を opt-in/preview 付きで行う。
- 完全／ほぼ単色、エントロピー極小、エッジなし画像は警告または単色 fill 1個として扱う。
- edge は blur + Canny 相当（NMS、hysteresis）と chroma gradient を併用し、輪郭を trace 後に RDP/Bezier 化する。
- flats は superpixel/領域分割 + polygon/selection fill にし、hatch は領域 mask へ clip する。
- RGB 最近傍でなく Lab/OKLab で palette 量子化する。
- alpha の合成先を固定白にせず、実キャンバス背景またはユーザー指定色にする。

受け入れテスト:

- 白画像、単色画像、透明画像、白背景上の黒線画、同明度の赤緑境界を golden fixture にする。
- 生成 stroke が推定 subject mask 外へ出る率を測る。

### BUG-12: Krita の線を区間ごとに分断する

根拠:

- `krita_adapter.py:583-602` は1 Stroke の隣接点ごとに `Node.paintLine` を呼ぶ。
- 連続入力用 native bridge は任意であり、通常配布物には同梱されていない。

影響:

- ブラシエンジンの入力履歴、速度、間隔、join が区間ごとにリセットされ、継ぎ目やビーズ状アーティファクトが出やすい。
- 呼び出し数が point 数に比例し、UI responsiveness を落とす。

Krita の公式 Node API には `paintLine` に加えて `paintPath`, `paintPolygon`, `paintRectangle` がある。ただし `paintPath` は点ごとの pressure を受けないため、用途を分けるべきである。

推奨修正:

- 一定 pressure の塗り、輪郭、幾何 shape は `paintPath` / `paintPolygon` を利用する。
- 筆圧付き lineart は native continuous input を正式に同梱するか、Krita action/tool event を通す実装を設計する。
- fallback の segment 描画は点の簡略化、短区間の統合、端点 overlap を行い、アーティファクトを golden test する。

### PERF-01: active layer の全面 snapshot が大きい

根拠:

- `krita_adapter.py:732-763` は document 全幅・全高の `pixelData` を取得し、さらに fingerprint 時も全面 snapshot を再取得して hash 化する。

影響:

8K、16-bit、多チャンネルのキャンバスではメモリとコピー時間が大きく、反復描画で UI 停止やメモリ不足を起こし得る。

推奨修正:

- 既存 active layer を直接変更せず、一時 paint layer へ描いて成功時 merge する方式を優先する。
- 直接変更が必要なら stroke bounds の tile snapshot のみに限定する。
- 実行前に推定 bytes と対象レイヤーの paintAbility を preflight 表示する。

### BUG-13: 設定復元が planner mode と矛盾する

根拠:

- `docker.py:1378-1517` と `1519-1605` は多くの設定を保存するが `planner_mode` を扱わない。
- `auto_refine` / `goal_mode` を復元しても、既定 offline の `_update_planner_settings_state()` が `docker.py:1841-1855` で false に戻す。
- reset は planner mode と現在の API key field を明示的に戻さない。
- 全 load/save を1つの `contextlib.suppress(Exception)` で囲むため、途中の1項目が壊れると後続の復元が黙って止まる。

推奨修正:

- planner mode を最初に復元してから依存設定を復元する。
- 各設定を個別に parse/validate し、壊れたキーだけ既定値へ戻して debug log に残す。
- `closeEvent` でも設定保存し、reset の対象と「秘密情報は保持しない」を明示する。

### LOGIC-01: 宣言された粒子／塗り表現が見た目に反映されない

根拠:

- `stroke_program.py:1083-1130` は `bokeh` 分岐がなく、未知 shape と同じ単線になる。
- sparkle も一本の tapered line で、十字／星形にならない。
- radial fill は `stroke_program.py:885-920` で polygon との交点を求めず最大半径まで全方向へ伸ばすため、非円形 shape の外へはみ出す。
- contour fill は頂点平均中心への縮尺であり、凹 polygon の真の inward offset ではない。
- coverage 指標は point pressure と実 opacity を半径へ反映せず、低圧端や半透明 wash の白抜けを過小評価する。

推奨修正:

- bokeh は円形 dab、sparkle は atomic な複数軸 stroke として明示実装する。
- radial ray は polygon 境界との最近交点まで clip する。
- concave contour は polygon offset ライブラリ相当のアルゴリズム、または raster mask の距離変換を使う。
- fill spacing は有効ブラシ直径 × pressure × opacity/flow を基準にし、隣接ストロークの overlap 下限を設ける。

### OPS-01: 中断と終了処理が弱い

根拠:

- HTTP 要求は blocking read 中に cooperative cancel を観測できず、timeout まで止まらない。
- Docker の close/destructor で PlanWorker / connection worker を cancel し、短時間 join する明示処理が見当たらない。
- daemon 的 worker と Qt object lifetime の競合余地がある。

推奨修正:

- connect/read timeout を分離し、stream/chunk 間で cancel を確認する transport abstraction を導入する。
- `closeEvent` で新規 callback を遮断し、cancel、待機、session rollback/commit 方針を確定してから破棄する。
- 「停止要求済み／API応答待ち」を別状態として表示する。

## 7. UI/UX 改善提案

### 最優先: 生成と適用を分ける

現在は「実行」が API 呼び出し、preview 更新、Krita 変更まで一気に進む。次の4段階へ分ける。

1. 入力: prompt、参照画像、mode、品質を指定
2. 生成: plan と preview のみ作成。Krita は変更しない
3. 確認: 反復ごとの差分、レイヤー、本数、費用／時間目安、警告を表示
4. 適用: preflight 後にキャンバスへ一括 commit。保存はその後

主要 CTA は `プレビュー生成`、`キャンバスへ適用`、`停止` とし、破壊的な適用を明確にする。生成済み artifact は再適用できるようにする。

### 画面構成

- 上部を sticky action bar にし、実行状態と停止をスクロール位置に関係なく見せる。
- Basic には prompt、題材プリセット、mode、quality、seed、reference のみ置く。
- Advanced を「ブラシ」「レイヤー／出力」「LLM」「画像トレース」「デバッグ」へ折り畳む。
- 参照画像はファイル名だけでなく thumbnail、寸法、色空間、容量、送信予定サイズを表示する。
- 出力先は generation 時に選択可能にし、既定の hidden app-data への JSON/SVG 大量保存は「直近セッションの復旧用1件」に限定する。
- 保存済み plan/program の Load/Reapply UI を追加する。既存の `load_plan` / `load_program` を活用できる。

### 実行中のフィードバック

- 実行中は現在の run に反映されない設定を disable または「次回から反映」と表示する。
- `Step 2/3 · Shading · 42/120 strokes · API 1/最大3回` のような確定進捗を出す。
- 反復途中に「描画完了」と表示せず、「Step 1 適用済み、次の canvas 評価中」とする。
- エラーは `生成失敗`, `描画失敗`, `保存失敗`, `停止` を別状態にする。
- 実行前 preflight で document、view、active node、paintAbility、layer mode、推定 paint calls、snapshot memory を表示する。

### 安全性とプライバシー

- 外部 LLM モードでは endpoint origin、送信画像、反復上限を要約し、初回だけ同意を得る。
- reference image と current canvas の外部送信を別 checkbox にする。
- API 接続テストは「HTTP 200だが応答形式不明」を成功扱いせず、互換性警告として扱う。
- debug log の Copy は prompt/path/image metadata を匿名化する既定動作を持つ。

### アクセシビリティ／高 DPI

- 絵文字だけに意味を依存せず、テキストラベルと標準アイコン、accessibleName を付ける。
- QLabel buddy、keyboard shortcut、tab order を設定する。
- 複数 widget を詰めた横一列を避け、狭い dock と150〜200% scaling の screenshot test を追加する。

## 8. イラスト品質向上のためのロジック再設計

### 8.1 SceneSpec による意味分解

キーワードから1つの generator を選ぶ設計をやめ、以下を別々に抽出する。

```text
SceneSpec
  subjects:     [anime_girl, cat, car, ...]
  environment:  [mountain, city, interior, ...]
  motifs:       [sakura_tree, wildflowers, magic_circle, ...]
  effects:      [speed_lines, glow, petals, ...]
  style:        [watercolor, ink, cel_shading, ...]
  composition:  [portrait, wide, foreground/midground/background]
  palette/value_plan
```

各 module は semantic operation graph を返し、scene composer が座標領域、重なり、スケール、layer を調整する。これにより「猫 in cyberpunk city」「人物 beneath mountain」の両方を描ける。

### 8.2 ストロークではなく意味 operation を予算化

推奨 metadata:

```text
operation_id, semantic_role, group_id, priority,
min_quality_budget, preferred_budget, atomic,
depends_on, occludes, clip_mask, blend_mode
```

allocator は次の順で予算を配る。

1. 背景方針と主シルエット
2. 顔／主役の識別部品、主要環境
3. 面の完全な塗り
4. 陰影と接触影
5. 輪郭の整理
6. ハイライト、質感、FX

予算不足なら詳細 operation を丸ごと落とし、必須 operation を穴だらけにしない。count はコンパイル後に機械的に切るのではなく、operation ごとに低密度版を再コンパイルする。

### 8.3 塗りを stroke list だけで表現しない

品質上もっとも大きい横縞は、面を太い線の集合へ展開してから間引くことで起きる。Krita へ適用する段階では、用途ごとに primitive を使い分ける。

- 不透明ベタ面: polygon/selection fill
- 水彩 wash: mask 内に overlap 付きの連続筆致
- gradient: mask + 距離場／方向場
- hatch: mask clip された線
- lineart: 筆圧付き continuous path

StrokeProgram も `FillOperation` を DrawingPlan の線へ早期展開せず、renderer capability に応じて primitive のまま渡せる v3 を検討する。

### 8.4 値設計と色設計

- palette は色名の配列でなく、背景／主役／陰影／アクセントの役割を持つ `ColorPlan` にする。
- foreground と background の luminance range を先に割り当てる。
- palette lock、補色 accent、最大彩度面積、暗部の色相シフトをルール化する。
- raster 後に主役と背景の局所 contrast、黒つぶれ、白飛び、同色線を測る。

### 8.5 画像からストロークへの変換

推奨 pipeline:

```text
decode/orient/color-manage
 -> metadata stripping + resize
 -> alpha/background estimation
 -> denoise
 -> luminance + chroma edges
 -> contour graph + simplification/Bezier fit
 -> region segmentation + Lab palette
 -> structure-tensor stroke direction
 -> mask-clipped fills/hatches
 -> raster preview + quality validation
```

reference fidelity を重視する mode と、画風変換を重視する mode を分ける。現状の単一 pipeline に edge threshold、shading density、flats の全責任を持たせない。

### 8.6 LLM は geometry の直接発生器より SceneSpec/operation planner として使う

- LLM は SceneSpec、semantic operation、構図 box、palette roles を出す。
- deterministic compiler が座標検証、clip、symmetry、fill 完全性、budget を保証する。
- structured output 非対応 provider では、厳格 parse に失敗したら canvas を触らない。
- reference/current canvas をラベル付き縮小画像として渡す。
- critic は生成モデルの自己申告と分離し、required motif、raster metrics、差分量を検査する。
- repair は全面再生成でなく、`add/replace/delete operation_id` の patch とする。

### 8.7 品質評価をラスタ／意味ベースへ変える

`quality.py:39-125` の現指標は coverage 40%、layer 数20%、筆圧20%、断片10%、bounds10%である。全面背景だけで coverage と layer 数を稼げ、題材が間違っていても高得点になる。

最低限、以下を追加する。

- 実 pressure、opacity、eraser、blend mode を反映した256〜512pxラスタ coverage
- 大面積の未塗り帯、縞、重複 dab、out-of-mask paint
- foreground/background contrast と value histogram
- atomic group 完全性、左右部品、閉輪郭、required motif
- prompt/preset と semantic tags の一致
- 同 seed の決定性、異なる題材間の geometry collision
- reference mode の silhouette/edge/color similarity
- 実 Krita での golden image または許容差付き perceptual hash

## 9. 品質ゲートの作り直し

### 現行ゲートの盲点

- `quality_check.py:11-40` は6プロンプト、Auto count のみ。
- 内蔵12プリセットとその既定 count を使わない。
- 背景全面塗りで coverage が高くなる。
- prompt と出力題材の一致を見ない。
- preview/Krita/SVG の pixel parity を見ない。
- 実 Krita smoke は手動で、CI の結果に含まれない。

### 追加すべきテスト群

1. `test_builtin_preset_contracts.py`
   - prompt、明示 motif、既定 count、palette、brush、期待 layer を固定する。
2. `test_semantic_budgeting.py`
   - atomic group、fill completeness、dependency、低予算 degradation を検証する。
3. `test_renderer_parity.py`
   - preview、SVG、fake/native renderer の合成と倍率を比較する。
4. `test_generation_session.py`
   - multi-iteration accumulation、commit、export failure、cancel を検証する。
5. `test_image_converter_golden.py`
   - 白／透明／線画／写真／同明度色境界を固定する。
6. `test_llm_contract.py`
   - schema capability、malformed response、UI override、request budget、image labels を検証する。
7. `krita_smoke_matrix.md` または自動 harness
   - 対応 Krita 最小版／最新版、OS、single/active/multi layer、eraser、cancel、8K を確認する。

テスト fixture は「数値 score のみ」ではなく、small golden PNG、semantic manifest、性能上限をセットにする。golden 更新は自動上書きせず、差分画像をレビュー対象にする。

## 10. 実装エージェント向け作業分割と順序

### Workstream A: 保存とトランザクション（最初に実施）

対象: `docker.py`, `storage.py`, `svg_exporter.py`, session 関連テスト

- `GenerationSessionArtifact` と cumulative plan を導入
- render commit と export を分離
- render options を保存／materialize
- BUG-01、BUG-05 の回帰テスト

完了条件: 複数反復を保存・再適用でき、保存失敗でキャンバスが戻らない。

### Workstream B: プリセット意味論と予算配分

対象: `procedural/__init__.py`, 各 generator, `procedural/base.py`, `stroke_program.py`

- 内蔵 preset へ明示 motif ID
- wildflower、sakura tree、mountain composition の分離
- SceneSpec の最小版と multi-label compose
- operation-aware allocator、atomic group、fill 再コンパイル
- FX overlay mode

完了条件: 12プリセットの契約テストと既定品質 golden が pass。

### Workstream C: WYSIWYG preview と UI フロー

対象: `docker.py`, `qt_compat.py`, `svg_exporter.py`

- semantic layer buffer、eraser、blend mode parity
- Generate Preview / Apply の分離
- sticky actions、実行中 lock、段階別 status
- reference thumbnail/preflight/privacy summary

完了条件: preview と適用結果の parity fixture が passし、キャンバス変更前に確認できる。

### Workstream D: LLM 契約と自動改善

対象: `llm_planner.py`, PlannerPort/request DTO, LLM tests

- brush/palette option の型付き伝播
- schema capability と strict validation
- 内容を捏造する salvage の削除
- label/resized/metadata-free image parts
- independent goal validator と request budget

完了条件: malformed response が無描画で失敗し、選択した brush/palette が最終 plan に反映される。

### Workstream E: 画像変換 v2

対象: `image_converter.py`, raster/vector helper, golden fixtures

- blank/background 判定
- chroma-aware contour、領域分割、mask clip、Lab palette
- quality mode と fidelity mode

完了条件: 境界 fixture と reference similarity 指標を満たす。

### Workstream F: Krita adapter と性能

対象: `krita_adapter.py`, native bridge packaging, manual/real integration harness

- primitive ごとの API 使用、continuous line path
- temporary layer transaction または tile snapshot
- close/cancel lifecycle
- 対応 Krita 実機 matrix

完了条件: 線の継ぎ目、8K memory、cancel/rollback を実 Krita で検証済み。

Workstream A と B のデータ構造を先に確定し、その後 C/D/E/F を並行させる。特に preview を先に書き換えてから plan/session 形式を変えると二重作業になる。

## 11. 最小リリース判定基準

次の条件をすべて満たすまでは、README の「high quality」相当の訴求を弱めることを推奨する。

- P1 全項目が回帰テスト付きで解消
- 12内蔵プリセットを UI 既定値で golden review 済み
- 野花／山桜／集中線の誤出力が解消
- multi-iteration JSON/SVG が完成キャンバスを再現
- preview と Krita 結果の合成／消しゴム／倍率が一致
- 参照画像が metadata 除去・縮小後に明示同意付きで送信される
- 保存失敗、API失敗、cancel、document close でユーザー既存データが保護される
- 対応する実 Krita 最小版と最新版で smoke test 完了

## 12. 直近の着手チケット案

1. `fix/session-cumulative-export`: BUG-01 + export倍率
2. `fix/export-failure-commit`: BUG-05
3. `test/preset-contracts`: 現状の失敗を先に固定
4. `fix/preset-routing`: wildflower / sakura / mixed scene
5. `feat/semantic-budget`: BUG-03
6. `fix/fx-overlay`: BUG-04
7. `feat/preview-render-options-parity`: BUG-06
8. `fix/llm-request-contract`: BUG-07 + BUG-08 + seed=0
9. `feat/reference-sanitization`: PRIV-01
10. `feat/image-converter-v2`: BUG-11
11. `perf/krita-render-session`: BUG-12 + PERF-01 + OPS-01

各チケットでは、先に失敗する unit/golden test を追加し、修正後に `python check.py` と実 Krita smoke の該当項目を通すこと。
