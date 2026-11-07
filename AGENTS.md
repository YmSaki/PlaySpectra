# AGENTS.md — PlaySpectra 開発ルール

このファイルは、このリポジトリで設計、実装、レビューを行うすべての開発者と AI エージェントが従うルールです。
禁止事項の一覧ではなく、アーキテクチャ上の判断基準、責務の分割、ファイルの配置、依存関係の構築方法を、設計と実装でそのまま使える手順として定めます。
用語は OpenXR とプロトコル仕様の語彙（`XrGraphicsBindingD3D12KHR`、`set_state`、`writer` / `observer`、STAGE など）をそのまま使います。言い換えによる意味のずれを避けるためです。
ルールを変えるときは、このファイルを更新する PR を出してから変えます。口頭やチャットだけで変えると、次のセッションが古いルールで作業するためです。

**参照の順序**

- 新機能の設計: 第 1 節から第 7 節を順に読み、第 7 節の手順で配置を決める。
- 新しいグラフィックス API、ランタイム、入力経路への対応: 第 9 節を読み、第 7 節の手順で配置を決める。
- 実装: 第 8 節から第 13 節を参照する。
- レビューと定期リファクタリング: 第 15 節の手順で、第 3 節から第 6 節と照合する。

## 1. 前提

- **プロダクト**: PlaySpectra は「VR 版 Playwright」です。Playwright がブラウザにキーボードとマウスの合成入力を注入して画面を観察するのと同じく、VR HMD とコントローラーを触ったのと同じ入力を VR アプリに注入し、描画された画面を観察します。用途は AI による VR アプリの操作と、OpenXR アプリの自動テストです。**コアループ（入力注入 + 画面取得）が動くことが最優先の要件**です（`CLAUDE.md`）。
- **要求の正本**: `CLAUDE.md` の north star と 2 軸のスコープ規則。設計と仕様の正本は `docs/` 配下です。
    - `docs/architecture.md`: 2 つの関心事（デバイス状態の操作と観測 / アプリの計装）、3 プロセスの配布境界、責務の境界。
    - `docs/device-core-spec.md`: `VirtualDeviceState` と `:52702` の NDJSON ワイヤプロトコル（protocol_version 1）。§7 に未実装の項目がある。
    - `docs/steamvr-adapter.md`: SteamVR Adapter の設計。仮説（H1〜H3）を含み、📋設計・開発中。
    - `docs/verification.md`: 主張ごとの検証状態と証拠。状態を変えるときは、ここに再現できるコマンドか一次証拠を足す。
    - `docs/mcp-tools.md`、`docs/scenario-format.md`: MCP tool と JSON Scenario の外部契約。
    - `docs/roadmap.md`、`docs/go-port-compatibility-plan.md`: 計画と工程資料。権威は上の文書に置く。計画は製品の規範ではなく、それで上書きすると契約がずれるためです。
- **仕様が沈黙しているとき**: 文書どうしが食い違うか、必要な判断を決めていなければ、該当箇所を引用して未解決として報告する。過去のコード、テスト、Git の履歴は「何が行われたか」を示すだけで「何が決まったか」を示さないので、それで欠落を埋めない。判断はユーザー（Owner）に求める。
- **検証済みと未検証を分ける**: 文書、設計、実装に主張を書く前に、それが「このリポジトリ内の実測ログ、テスト結果、一次ソースを指させる**事実**」か「学習知識由来の**仮説**」かを分類する（`CLAUDE.md`）。仮説は仮説ラベルと検証方法を付けて書き、仕様の決定は事実の上にだけ置く。仮説の上に実装するときは「検証装置としての暫定実装」と明示する。外部ランタイムの挙動を推測で断言し、実測と真逆だった事故が実際に起きたためです。
- **文書の役割分担**:

    | 文書 | 置くもの |
    | --- | --- |
    | `AGENTS.md`（このファイル） | 設計と実装の判断基準、責務、依存、配置、規約、レビューの手順 |
    | `CLAUDE.md` | プロジェクトの核となる要求と、見失ってはいけない教訓。実装の詳細は書かない |
    | `README.md`、`docs/readme.ja.md` | 利用者向けの入口。✅実装+自動テスト済み / 🟡実機で部分検証済み / 📋設計・開発中 の 3 段階で検証境界を示す |
    | `docs/` | 設計と仕様の正本、検証マトリクス、利用者向けの詳細 |
    | `docs/testing.md` | ローカルゲートと CI の構成、個別のテストコマンド |
    | `.claude/rules/` | パス別の実測済みの罠（CMake、Windows interop、scripts）と、横断のレビュー観点（`review-checklist.md`） |

    内容が食い違ったら、要求は `CLAUDE.md`、プロトコルは `docs/device-core-spec.md`、検証状態は `docs/verification.md`、設計規則はこのファイルを優先し、食い違った側を同じ PR か別 Issue で直す。
- **作業記録**: 作業中の計画や工程記録を `.claude/` に置くときは、`.gitignore` が公開しない場所（`.claude/rules/` と `.claude/settings.json` 以外）に置き、追跡される文書とコードから参照しない（`CLAUDE.md`）。参照先が fresh clone に存在しないためです。
- **ライセンス**: MPL-2.0。追跡するソースの先頭に MPL-2.0 のヘッダーと `SPDX-License-Identifier: MPL-2.0` を書く。第三者のコードと配布物の表示は `THIRD_PARTY_NOTICES.md` に書く。

### 1.1 アーキテクチャ上の制約となる製品不変条件

次の不変条件は例外のない製品要件です。以降の構造は、これらを特定のコンポーネントや機構で保証する前提で決めています。

| 不変条件 | 内容 | 保証する機構 |
| --- | --- | --- |
| エンジン非依存 | 入力注入と観測は OpenXR の抽象化レベルで行う。Unity、Unreal などエンジン固有のプラグインや統合コードを必須の経路に置かない。Playwright がブラウザエンジンの内部を気にしないのと同じ位置に立つためです | OpenXR API レイヤー（`layer/`）と Runtime Adapter（`devicecore/` + ランタイム側シェル） |
| グラフィックス API の完全性 | 画面取得は OpenXR が定義する全グラフィックスバインディング（`XrGraphicsBindingD3D11KHR` / `D3D12KHR` / `VulkanKHR`）で動く。1 つでも欠けると「そのアプリは撮れない」不完全品になるので、どれも後回しにしない（`CLAUDE.md` のスコープの 2 軸） | `layer/src/capture_backends.h` の API ごとの backend |
| 入力経路の完全性 | 入力注入は全アクション種別（bool / float / vec2 / pose）、両手、頭部姿勢を扱う | `VirtualDeviceState`（`devicecore/`）と Go の Core の高水準操作 |
| 無言の壊れた結果を出さない | 未対応のフォーマット、MSAA、デバイスなしなどでは `{ok:false, error}` を返し、空白や全ゼロの画像を成功として返さない | 各 capture backend の戻り値の JSON |
| 観測対象のアプリを落とさない | レイヤーはアプリのプロセス内で動くので、制御チャネルとフックから例外を外へ漏らさない。ツールが観測対象を落とすと、観測そのものが成り立たないためです | `layer/src/control_channel.cpp` の `HandleRequest`（全体を try で囲む） |
| 操作と観測の分離 | 操作（デバイス状態）は Runtime Adapter の `127.0.0.1:52702`、観測（screenshot、recording、action discovery、診断）はレイヤーの `127.0.0.1:52700`。レイヤーの入力 override はテスト補助で、主経路ではない | `docs/architecture.md`、`devicecore/playspectra_control.c`、`layer/src/control_channel.cpp` |
| 完全スナップショット | `set_state` は接続中のデバイスの全可変フィールドを毎回含み、前回値を継承しない。writer は同時に 1 接続、observer は複数 | `devicecore/playspectra_proto.c`、`playspectra_control.c`（`docs/device-core-spec.md` §2.2、§5.3） |
| devicecore のランタイム中立 | `devicecore/` は Monado、SteamVR、OpenXR の型を知らない。ランタイム固有の値（ポート、名前、descriptor、ログ）は adapter が `playspectra_control_config` で注入する | `devicecore/README.md` |
| 配布単位の分離 | 実行時の 3 プロセス（Go の `playspectra`、Runtime Adapter を含むランタイム、レイヤーを読み込んだアプリ）は TCP の NDJSON 境界だけで通信する。Go 実行ファイルは C/C++ の成果物をリンクせず、cgo を使わない | `docs/architecture.md` の Distribution boundary、CI の cgo 検査 |
| 入口の同値 | CLI、JSON Scenario、MCP は同じ Core（`playspectra.Server`）を呼ぶ対等な入口で、業務ロジックを持たない | `cmd/playspectra`、`mcp/`、`playspectra/` |
| 主張と証拠 | 振る舞いを「対応済み」「検証済み」と書くのは、`docs/verification.md` に再現できる証拠があるときだけ。ユニットテストが通ったことを、ランタイムやエンジンへの対応の主張に変えない | `docs/verification.md`、`docs/testing.md` の Verification discipline |
| 深度は任意 | 既定の画面取得は利き目のカラー画像。深度マップは「出してもいい」nice-to-have で、必須仕様にしない。生の左右ステレオは `eye` パラメータで選べるように残す | `CLAUDE.md`、`Screenshot(ctx, eye, ...)` |

### 1.2 トレードオフの優先順位

設計上の選択肢で迷ったら、上位を優先します。

1. **観測の正しさ**: 無言で誤った画像、状態、検証結果を返すより、明示的なエラーで失敗する。観測対象のアプリは落とさない。
2. **OpenXR 抽象化レベルの完全性**: 全グラフィックス API、全入力経路を揃える。エンジン固有のコードで穴を埋めない（`CLAUDE.md` のスコープの 2 軸）。
3. **外部契約の維持**: `:52702` のワイヤプロトコル、`:52700` のコマンド、CLI の引数と終了コード、MCP の tool 名、スキーマ、既定値、結果の形、JSON Scenario と recording の形式を変えない。変える必要があれば、仕様の文書を先に更新する PR として扱う（第 14 節）。
4. **テスタビリティ**: GPU、ランタイム、ネットワークに触れない純粋な部分を切り出し、ホストのユニットテストで検証できるようにする（第 6.1 節）。
5. **シンプルさ**: コアループに不要な汎用化はしない（YAGNI）。設計レビューで出た指摘は「コアループに必須」か「将来の拡張」かを分類し、後者をタスクの主軸に混ぜない（`CLAUDE.md`）。ただし知識の重複は排除する（第 11 節）。

## 2. アーキテクチャ

### 2.1 アーキテクチャスタイル

**3 つのプロセスに分かれた配布単位**と、その内側の**依存が一方向のレイヤー構成**で構成します。
ランタイムごとの差は Runtime Adapter に、グラフィックス API ごとの差は capture backend に閉じ込めます（Ports and Adapters）。
OpenXR はアプリとランタイムの境界を標準化しますが、ランタイムとデバイスドライバーの境界は標準化しないので、共通のドライバー ABI は作れないためです（`docs/architecture.md`）。

| コンポーネント | 責務 | 配置 | 言語 |
| --- | --- | --- | --- |
| 入口 | 引数、JSON Scenario、MCP の JSON-RPC を Core の呼び出しに変換し、結果を JSON と終了コードに変換する | `cmd/playspectra/`、`mcp/` | Go |
| Core | 高水準操作（move_head、look、walk_forward など）から補間した完全状態フレームへの変換、アサーション、Scenario の実行、recording / replay、セッション管理 | `playspectra/` | Go |
| ワイヤクライアント | NDJSON/TCP のフレーミング、`request_id` の対応付け、最大メッセージサイズ | `protocol/` | Go |
| 検証と補助 | live スタックに対する自己検証（`playspectra verify ...`）、外部アプリのハーネス、PNG の統計、セットアップ補助 | `verify/`、`vrapp/`、`pngstats/`、`setuphelper/` | Go |
| Virtual Device Core | `VirtualDeviceState`、NDJSON のパースとフレーム同期の判定、TCP 制御チャネル（`:52702`） | `devicecore/` | C |
| Runtime Adapter のシェル | Core の状態をランタイムのネイティブ型に変換する。Monado は `drv_playspectra`、SteamVR は計画中 | `runtime/monado-playspectra`（submodule）、`driver/`（SteamVR の初期スケルトン） | C / C++ |
| OpenXR 計装レイヤー | アプリのプロセス内でフレーム取得、録画、action discovery、診断、テスト用 override を行い、`:52700` で制御を受ける | `layer/` | C++ |

### 2.2 入口と実行形態

| 入口 | 責務 | 備考 |
| --- | --- | --- |
| `playspectra cmd <operation>` | 1 つの高水準操作を実行し、結果と状態を JSON で出す | `--cmd` は旧 CLI との互換 |
| `playspectra run <scenario.json>` | JSON Scenario を実行し、アサーションの集計を出す | 形式は `docs/scenario-format.md` |
| `playspectra mcp` | stdio の MCP サーバー。13 tool。最初の tool 呼び出しで接続する | 名前、スキーマ、既定値、結果の形は互換テストで固定（`docs/mcp-tools.md`） |
| `playspectra record` / `replay` | observer として状態フレームを記録し、writer として再生する | |
| `playspectra doctor` / `image-stats` / `session` / `version` | 到達性の診断、PNG の統計、プロセス管理、版の表示 | |
| `playspectra verify <name>` | live スタックに対する自己検証（`server`、`record`、`frame`、`reset`、`multiobs`、`coupling`、`mcp`、`vrapp`） | `:52702` などの live な相手が必要 |
| `playspectra internal <op>` | セットアップとテストのスクリプトが使う補助 | 利用者向けの契約ではない |
| レイヤーの制御チャネル（`:52700`） | `status`、`screenshot`、`start_recording` / `stop_recording`、`actions`、`input`、`pose*`、`head*`、`haptics`、`reset` など | 1 行 1 JSON。未知の `cmd` は `{ok:false}` |

- 終了コードは `0` 成功、`1` 実行時の失敗（アサーション失敗、到達不能、検証の NG など）、`2` usage の誤りを使う。
- どの入口も対話入力を求めない。AI エージェントと CI から同じ結果で呼べるようにするためです。
- 実行ファイルは `playspectra` の 1 つです。ポートは `PLAYSPECTRA_MONADO_PORT`（既定 52702）と `PLAYSPECTRA_PORT`（既定 52700）で変えられる。

### 2.3 データの流れと副作用

```text
CLI / JSON Scenario / MCP ──▶ playspectra.Server（Core） ──Transport──▶ protocol.Client
                                       │                                    │ NDJSON/TCP :52702
                                       │                                    ▼
                                       │                    devicecore（ランタイムのプロセス内）
                                       │                                    │ adapter シェル
                                       │                                    ▼
                                       │                    OpenXR ランタイム（Monado）──▶ VR アプリ
                                       │ CaptureTransport (NDJSON/TCP :52700)                │
                                       └────────────────────▶ OpenXR 計装レイヤー（アプリのプロセス内）
                                                                swapchain を読み PNG を書く
```

- 状態の正本はランタイム側の devicecore にある。Go の Core は `sequence` を所有し、完全状態フレームを送り、`get_state` で読み戻す。
- 画面は、レイヤーがアプリの描画した swapchain 画像を `xrEndFrame` で読み、`PLAYSPECTRA_CAPTURE_DIR` に PNG として書く。PNG のような大きなデータは `:52702` の経路に載せない（最大 1 MiB、`docs/device-core-spec.md` §5.1）。
- 副作用（ソケット、ファイル、外部プロセス、時刻、sleep）は Go では `Transport` / `CaptureTransport` とオプションで注入し、補間、数学、状態の組み立て、判定は入出力を引数で受け取る関数にする。

### 2.4 移行中の項目

- **SteamVR Adapter（📋計画）**: `driver/` は初期のスケルトンで、独自の暫定プロトコルを持つ。これを `devicecore/` に接続し、Windows で検証するのが残作業です（`docs/roadmap.md`）。`driver/` に独自プロトコルの機能を足さない。足すと devicecore への接続で捨てる二重実装になるためです。
- **Python と TypeScript の撤去（完了）**: 制御プレーンは Go に移植済み。CI が追跡中の `*.py`、`scripts/*.sh` からの Python 呼び出し、cgo を検査して拒否する。Node の `.mjs` クライアントは `scripts/` の統合テスト補助として残っている。

## 3. 責務の分割

### 3.1 分割の判断基準

パッケージ、ディレクトリ、ファイルの境界は、次の問いに順に答えて決めます。

1. **どのプロセスで動くか。** Go の制御プレーン、ランタイムのプロセス（devicecore と adapter シェル）、アプリのプロセス（レイヤー）のどれかに置く。プロセスをまたぐ呼び出しは TCP の NDJSON 境界だけにする。
2. **特定のランタイム、グラフィックス API、エンジンに依存するか。** ランタイム固有は adapter シェル、グラフィックス API 固有は capture backend の翻訳単位に置く。エンジン固有のコードはスコープ外として置かない。
3. **副作用（ソケット、GPU、ファイル、プロセス、時刻）を持つか。** 持つ部分は境界側に置き、補間、数学、ピクセル変換、判定は純粋な関数にしてユニットテストする（例: `pixel_convert`、`pose_animator`、`pose_override_pure.h`、`playspectra/math.go`）。
4. **変更の理由は何か（単一責任の原則、SRP）。** 変更の理由が 2 つあれば分割する。例: ワイヤのフレーミングの変更と、高水準操作の補間の変更の両方で同じ関数を直すなら、分割が誤っている。
5. **外部入力をパースするか。** パースとバリデーションは信頼境界（devicecore の `playspectra_proto`、レイヤーの `HandleRequest`、MCP の引数の抽出、`protocol.Client`）に集約する。

### 3.2 トップレベルのディレクトリ

```text
cmd/playspectra/   Go 実行ファイルの入口（main）
playspectra/       Go の Core（Server、状態モデル、数学、recording、セッション）
protocol/          NDJSON/TCP のクライアント
mcp/               stdio MCP サーバー（と verify 用のクライアント）
verify/            live スタックに対する自己検証
vrapp/             外部アプリ（VRAppDummyGame）のハーネス
pngstats/          PNG のピクセル統計
setuphelper/       セットアップとテストのスクリプトが使う補助
devicecore/        ランタイム中立の Virtual Device Core（C）
runtime/           Runtime Adapter を含むランタイム（monado-playspectra は submodule）
driver/            SteamVR Adapter の初期スケルトン（C++、計画中）
layer/             OpenXR 計装レイヤー（C++）と、そのホストユニットテスト
tools/             C のプローブと JSON Scenario の例
scripts/           セットアップ、統合テスト、E2E のスクリプト（bash と Node .mjs）
docs/              設計と仕様の正本、利用者向けの文書
.claude/rules/     公開しているパス別の規約とレビュー観点
.github/workflows/ CI（ci.yml）とリリース（release.yml）
```

| ディレクトリ | 置くもの | 置かないもの |
| --- | --- | --- |
| Go のパッケージ | 制御プレーンのコードと `_test.go` | C/C++ へのリンク、cgo、外部モジュールへの依存 |
| `devicecore/` | ランタイム中立の状態とプロトコル | Monado、SteamVR、OpenXR の型 |
| `runtime/monado-playspectra` | Monado の fork と `drv_playspectra` のシェル | devicecore の複製（`PLAYSPECTRA_DEVICECORE_DIR` で取り込む） |
| `layer/` | アプリのプロセス内の計装 | ランタイムへのリンク、エンジン固有の処理 |
| `scripts/` | 環境の準備、E2E の起動と判定 | 製品のロジック、Python |
| `docs/` | 正本と利用者向けの説明 | 検証されていない主張を事実として書くこと |
| `/build/`、`layer/build/`、`third_party/`、`bin/` | 生成物と、スクリプトで再現する外部依存（追跡しない） | ソース |

### 3.3 Go パッケージの責務

パッケージの追加や責務の変更は、先にこの表を更新する PR で行います（第 7 節）。

| パッケージ | 責務 | 持たない責務（担当） |
| --- | --- | --- |
| `playspectra` | `Server`（高水準操作、補間、`set_state` / `get_state`、アサーション、`wait_for`、capture 参照、Scenario の実行と集計）、状態モデルと入力 path の一覧、数学、recording / replay、`SessionRecord` によるプロセス管理。外部とは `Transport` / `CaptureTransport` インターフェースだけで話す | ソケットの実装（`protocol`）、引数の解析と印字（`cmd/playspectra`）、JSON-RPC（`mcp`） |
| `protocol` | NDJSON/TCP の接続、フレーミング、`request_id`、`ProtocolVersion`、`MaxMessageSize` | 状態の意味づけ（`playspectra`） |
| `mcp` | stdio の JSON-RPC、tool 一覧と入力スキーマ、引数の抽出、`Server` の呼び出し、結果の変換。`Client` は `verify mcp` が MCP サーバーを子プロセスとして駆動するために使う | 業務ロジック（`playspectra`） |
| `verify` | live スタックに対する検証の手順と `Report` / `Check` | 製品の振る舞いそのもの |
| `vrapp` | VRAppDummyGame を `[VRTEST]` の stdin/stdout 契約で駆動するハーネスとスイート | アプリ本体（別リポジトリ） |
| `pngstats` | グラフィックス API や画像ライブラリに依存しない PNG のデコードと統計 | 取得（レイヤー） |
| `setuphelper` | vcxproj の変換、hello_xr のパッチ、PE の検査、TCP 待ちなど、スクリプトの補助 | 製品の実行経路 |
| `cmd/playspectra` | サブコマンドの解析、`protocol.Client` の生成と `Server` への注入（コンポジションルート）、JSON の印字、終了コード | 業務ロジック |

### 3.4 ネイティブコンポーネントの責務

| コンポーネント | 責務 | 持たない責務（担当） |
| --- | --- | --- |
| `devicecore/playspectra_proto` | NDJSON のパース、フレーム同期の判定、content signature | ソケット（`playspectra_control`） |
| `devicecore/playspectra_state` | 共有の参照カウント付き `VirtualDeviceState`（姿勢、入力、haptics のキュー） | ランタイム型への変換（adapter シェル） |
| `devicecore/playspectra_control` | `:52702` の TCP 制御チャネル（`hello`、`set_state`、`get_state`、`status`、`reset`）、writer 排他 | ポート番号や名前の決定（adapter が config で注入） |
| `devicecore/ps_os` | Win32 / pthread の mutex とスレッドの最小 shim | |
| `layer/src/layer_entry.cpp`、`layer_dispatch` | ローダーとの交渉、フックの dispatch 表（`kHooks[]`）、次レイヤーの関数表とインスタンス状態 | 個々のフックの処理 |
| `layer/src/hooks_*.cpp` | フックのクラスター（`hooks_capture`: swapchain とフレーム、`hooks_locate`: 頭部とコントローラーの姿勢、`hooks_action`: action と teardown） | 共有状態の所有（`layer_state`、`action_registry`） |
| `layer/src/capture*.cpp` | `capture.cpp` が swapchain 画像と subimage を解決し、API ごとの backend（`capture_d3d11` / `capture_d3d12` / `capture_vulkan`）を呼ぶ。共通の後段処理は `capture_common` | ピクセル変換の純粋部分（`pixel_convert`） |
| `layer/src/control_channel.cpp` | `:52700` の NDJSON サーバーと、コマンドから handler への dispatch | 状態の保持（`layer_state`） |
| `layer/src/input_inject`、`pose_override`、`pose_animator` | テスト補助の入力と姿勢の override（CA あり / なしの経路）と、`durationMs` の補間 | 主経路の入力（Runtime Adapter） |

### 3.5 ファイルの粒度

- 「別の理由で変わるものを別のファイルに」を唯一の基準にする。行数の削減や「1 関数 1 ファイル」を目的にしない。
- GPU、ランタイム、OpenXR に依存しない部分は、ユニットテストできるように別の翻訳単位かヘッダーオンリーに切り出す（例: `pose_override_pure.h` は `pose_override.cpp` の session / dispatch の依存なしにテストするために分けた）。
- ファイルの先頭に、そのファイルの責務と「なぜこの境界か」をコメントで書く（`layer/src/*.h` の既存の形）。
- Go の公開識別子は、パッケージの外から使われるものだけにする。テストのためだけに公開しない（同じパッケージの `_test.go` から使う）。

## 4. 依存関係の構築

### 4.1 依存の向きの決め方

- **依存は入口から Core へ、Core からインターフェースへ向ける。** `playspectra` は他の自前パッケージに依存せず、`protocol.Client` の具体型も知らない。具体型は入口（コンポジションルート）が注入する。
- **プロセスをまたぐ依存は、TCP の NDJSON 境界だけにする。** Go は C/C++ をリンクせず、レイヤーはランタイムにリンクせず、devicecore は Go を知らない。
- **ランタイム固有の処理が Core で必要になったら、`devicecore` の状態モデルとワイヤプロトコルに中立な形で表し、adapter シェルで変換する。** Core に `xrt_*` や `DriverPose_t` を持ち込まない。
- **汎用の plugin 機構や factory を、将来の拡張のためだけに作らない。** 必要になった時点で、必要な形だけを作る。

### 4.2 Go パッケージ間の依存の許可リスト

表にない依存は作りません。新しい依存が必要になったら、4.1 の基準で向きを確認し、この表を更新する PR を先に出します。現状は表のとおりです（`go list -f '{{.Imports}}' ./...` で確認）。

| パッケージ | 依存してよい自前パッケージ |
| --- | --- |
| `playspectra`、`protocol`、`pngstats`、`setuphelper` | なし |
| `mcp` | `playspectra` |
| `verify` | `playspectra`、`protocol`、`mcp` |
| `vrapp` | `playspectra`、`protocol`、`pngstats` |
| `cmd/playspectra` | すべて |

- `go.mod` は外部モジュールを `require` しない（標準ライブラリだけ）。外部モジュールを足すのは、標準ライブラリで実現できない理由を PR に書いてからにする。実行ファイルを依存なしで配布するためです。
- cgo（`import "C"`、`#cgo`）は使わない。CI が検査して拒否する。

ネイティブの外部依存も使用範囲を固定します。

| 依存 | 使用してよい範囲 | 取得方法 |
| --- | --- | --- |
| OpenXR SDK のヘッダー、nlohmann_json、lodepng、Vulkan-Headers | `layer/` | `layer/CMakeLists.txt` の FetchContent でタグを固定 |
| googletest | `layer/` のテストターゲット `playspectra_test` | 同上（`PLAYSPECTRA_BUILD_TESTS=ON`） |
| cJSON | `devicecore/` と Monado | Monado submodule の `src/external/cjson` |
| D3D11 / D3D12 / DXGI | `layer/src/capture_d3d11.cpp`、`capture_d3d12.cpp`（Windows だけでビルド） | OS |

### 4.3 コンポジションルートと注入

1. `cmd/playspectra` が `protocol.Dial` で接続を作り、`playspectra.NewServer` に `Transport` と `CaptureTransport` を渡す。`mcp` は `Factory` を受け取り、最初の tool 呼び出しで `Server` を作る。
2. `playspectra` の関数は接続を自分で作らない。テストは同じインターフェースの fake を渡す。
3. devicecore はポート、ランタイム名、descriptor、ログ関数を `playspectra_control_config` で受け取る。Monado の fork は `PLAYSPECTRA_DEVICECORE_DIR` で devicecore のソースを取り込む。SteamVR の driver も同じソースを使う計画です。
4. レイヤーのフックは `kHooks[]`（`layer_entry.cpp`）に明示的に列挙する。

### 4.4 境界を越えるデータのスキーマ

境界を越えるデータのスキーマは、それぞれ 1 か所で定義します（Single Source of Truth）。

| 境界 | スキーマの所有者 | 対になる側 |
| --- | --- | --- |
| `:52702` の NDJSON（`VirtualDeviceState`、`hello`、`set_state` など） | `docs/device-core-spec.md` と `devicecore/playspectra_proto` | `protocol`、`playspectra` |
| `:52700` のレイヤーのコマンド | `layer/src/control_channel.cpp` の handler 表 | `playspectra.Server` の capture 経路、`scripts/*.mjs` |
| capture backend の結果 JSON | `layer/src/capture_backends.h` のコメント | `capture.cpp`、制御チャネル |
| MCP の tool 名、スキーマ、既定値、結果 | `mcp/server.go`（互換テストで固定） | `docs/mcp-tools.md` |
| JSON Scenario と recording | `docs/scenario-format.md` と `playspectra` | `tools/scenarios/*.json` |
| VRAppDummyGame の `[VRTEST]` 契約 | 別リポジトリのアプリ | `vrapp/driver.go` |
| OpenXR ローダーのマニフェスト | `layer/manifest/XrApiLayer_playspectra.json` | ローダー |

## 5. 配置の地図

この節は「何を、どこに、どの形で置くか」の正本です。
レビューと定期リファクタリングは、この地図と第 3 節、第 4 節との照合で判断します。

### 5.1 配置表

| 対象 | 配置 | 形式 |
| --- | --- | --- |
| Go 実行ファイルのエントリポイント | `cmd/playspectra/main.go` | 「サブコマンドの解析 → Core の呼び出し → JSON の印字 → 終了コード」だけ |
| 高水準操作（move_head など） | `playspectra/server.go` の `Server` のメソッド | 補間した完全状態フレームを `SetState` で送る |
| 状態モデルと入力 path の一覧 | `playspectra/state.go` | `LeftInputPaths` / `RightInputPaths` |
| 姿勢の数学 | `playspectra/math.go`（Go）、`layer/src/xr_math.h`（レイヤー） | 副作用のない関数 |
| Scenario のステップの解釈 | `playspectra.Server.RunStep` / `RunScenario` | CLI と MCP は同じ関数を使う |
| MCP の tool | `mcp/server.go` | 引数の抽出、`Server` の呼び出し、結果の変換だけ |
| ワイヤの送受信 | `protocol/client.go` | |
| live スタックの自己検証 | `verify/<対象>.go` | `Report` に `Check` を積む。機械値で判定する（第 13 節） |
| デバイス状態とプロトコル | `devicecore/` | ランタイムの型を含めない |
| ランタイム型への変換 | adapter シェル（Monado は submodule の `drv_playspectra`） | Core の状態を読み、ネイティブ型に写す |
| フック | `layer/src/hooks_<クラスター>.cpp` | `kHooks[]` に登録 |
| グラフィックス API ごとの読み出し | `layer/src/capture_<api>.cpp` | `capture_backends.h` のフラットなインターフェース |
| API に依存しない読み出しの後段処理 | `layer/src/capture_common.cpp`、`pixel_convert.cpp` | ユニットテスト付き |
| レイヤーの制御コマンド | `layer/src/control_channel.cpp` の `Handle_<cmd>` と `kHandlers[]` | 例外を外へ出さず、`{ok:false, error}` を返す |
| E2E とセットアップの手順 | `scripts/<目的>.sh`、共通処理は `scripts/lib_*.sh` | bash。判定は機械値 |
| C のプローブ | `tools/` | |
| Scenario の例 | `tools/scenarios/*.json` | |
| Go の単体テスト | 対象と同じパッケージの `<対象>_test.go` | 第 13 節 |
| レイヤーの単体テスト | `layer/tests/test_<対象>.cpp` と `playspectra_test` のソース列 | 純粋な部分だけ |
| 利用者向けの説明 | `docs/<題名>.md` | 3 段階の検証ラベルを付ける |
| 検証状態と証拠 | `docs/verification.md` | 環境、証拠、境界 |

### 5.2 配置の決定手順

上から順に判定し、最初に該当した場所に置きます。

1. 観測対象のアプリのプロセス内で動く必要があるか（swapchain、OpenXR の呼び出しの観測） → `layer/`
2. ランタイムがデバイス状態を同期的に問い合わせる経路か → `devicecore/`（ランタイム中立）と adapter シェル（変換だけ）
3. ワイヤの送受信か → `protocol/`
4. 高水準操作、補間、アサーション、Scenario、recording か → `playspectra/`
5. 入出力の変換（CLI の引数、MCP の JSON-RPC）か → `cmd/playspectra/`、`mcp/`
6. live スタックに対する検証の手順か → `verify/`（外部アプリ固有なら `vrapp/`）
7. 環境の準備や E2E の起動か → `scripts/`（Go の補助が要るなら `setuphelper/` と `playspectra internal`）
8. それ以外 → 3.3、3.4 の責務表で責務の合う場所

## 6. 場面ごとの設計

### 6.1 採用する設計パターン

場面ごとに使うパターンを固定します。ここにない場面で新しいパターンが必要になったら、ユーザーの判断を受けてからこの表に追加します。

| 場面 | パターン | 実装方法 |
| --- | --- | --- |
| ランタイムへの入力 | 共通 Core + Runtime Adapter | devicecore を各ランタイムのプロセスに組み込み、シェルがネイティブ型に変換する |
| グラフィックス API ごとの読み出し | 1 つの dispatch + API ごとの backend | `capture.cpp` が解決し、`capture_<api>.cpp` を呼ぶ。backend の間で内部型を渡さない |
| 操作の入口 | 対等な入口 + 共通 Core | CLI、Scenario、MCP は `playspectra.Server` を呼ぶ |
| 外部との通信 | インターフェースの注入 | `Transport` / `CaptureTransport`。テストは fake |
| 状態の送信 | 完全スナップショット | `set_state` に全フィールドを入れる。部分更新（`patch_state`）は未実装 |
| 純粋な処理と副作用 | Functional Core, Imperative Shell | 数学、補間、ピクセル変換、判定を副作用から分ける |
| レイヤーの制御コマンド | 名前から handler への表 | `kHandlers[]` |
| 失敗の返却 | 明示的なエラー | ワイヤは `ok:false` と `error_type`、レイヤーは `{ok:false, error}`、Go は `error` |

採用しないパターン: エンジン固有のプラグイン、モードフラグで意味の違う処理を 1 関数にまとめること、将来の拡張のためだけのインターフェースや factory、何でも入る `utils`。

### 6.2 エラー処理

- `:52702` のエラーは `error_type` の 3 分類（`protocol_error`、`validation_error`、`conflict_error`）で返す（`docs/device-core-spec.md` §5.2）。新しい分類が必要なら、先に仕様の文書を更新する。
- レイヤーは制御チャネルとフックから例外を外へ出さない。失敗は `{ok:false, error}` で返す。取得に失敗したときに、空白や全ゼロの画像を成功として返さない。
- Go のライブラリは `error` を返し、panic で失敗を伝えない。入口が終了コード（第 2.2 節）に変換する。
- 処理方法が定まらない状況では、成功に見える既定値で代用せず、明示的なエラーにする。受け取った値を使わずに定数で成功を返す（例: 未接続なのに `applied:true`）ことは、観測の正しさ（第 1.2 節の 1）への違反です。

### 6.3 外部入力と信頼境界

- 未知のフィールドは無視する（tolerant reader）が、`set_state` の必須フィールドの欠落は `validation_error` にする（§5.1）。
- メッセージは最大 1 MiB。超えたら `protocol_error` で接続を閉じる。
- JSON で届いた四元数は単位長の保証がないので、使う前に正規化する（`layer/src/xr_math.h`）。
- 文書の仕様と実装の差（入力の完全性の検証、`runtime_reserved` の分離、descriptor の `controllers` / `fov` など）は `docs/device-core-spec.md` §7 に未実装として書かれている。実装済みのように書かない。

### 6.4 決定論とタイミング

- `frame_synchronized` は論理ステップを揃えるプロトコルの経路です。アプリ全体の決定論（delta time、GPU のスケジューリング、物理、非同期ロード、フレーム落ち）は未検証なので、決定論的と書かない（`docs/architecture.md`）。
- live なアプリを相手にする検証は、接続直後に 1 回だけ問い合わせて判定しない。準備が整うまでリトライか条件待ちをする。

### 6.5 安全性

- 制御チャネルは `127.0.0.1` に bind する。外部のホストから操作できないようにするためです。
- 外部プロセスは実行ファイルと引数を分けて起動し、シェルの文字列を組み立てない（`playspectra.SessionRecord` はコマンド文字列を保存しない）。
- ダウンロードしたものは、PE ヘッダー、サイズ、形式を機械的に検査する。サードパーティの clone はタグで固定する（`.claude/rules/setup-scripts.md`）。
- ログと出力に認証情報や環境変数の値を含めない。

## 7. 新機能の設計手順

1. **関連する正本を読む。** `CLAUDE.md`、`docs/architecture.md`、該当する仕様（`docs/device-core-spec.md` など）を読み、仕様が決めていることと決めていないことを分ける。決めていないことは推測で埋めず、ユーザーに確認する。
2. **コアループに必須か、将来の拡張かを分類する。** OpenXR の構造体と関数で表される普遍的な話（全グラフィックス API、全入力経路）はコア必須。特定エンジンの内部の話はスコープ外。ユーザーが「〜してもいい」と許容表現で言ったものだけを nice-to-have にする（`CLAUDE.md`）。
3. **機能を関心事ごとに分解する。** 「アプリのプロセス内の観測」「ランタイムのデバイス経路」「ワイヤ」「高水準操作と判定」「入出力の変換」「検証の手順」に分ける。
4. **関心事ごとに配置を決める。** 5.2 の手順で判定する。1 つの機能が Go、devicecore、レイヤーにまたがるのは通常のことです。
5. **外部契約への影響を確認する。** ワイヤ、レイヤーのコマンド、CLI、MCP、Scenario の形式を変えるなら、仕様の文書（第 4.4 節の所有者）を同じ PR か先の PR で更新する。
6. **依存を確認する。** 4.2 の許可リストにあるか確認する。なければ 4.1 の基準で向きを合わせる。
7. **既存のパッケージに収まらない場合だけ、新しいパッケージやコンポーネントを作る。** 条件は、3.3、3.4 のどの行にも該当しない、単独でテストできる、4.2 に循環なく置ける、の 3 つ。先に表を更新する PR を出す。
8. **足りない基盤は別 Issue に切り出す。** 基盤の PR を先に出し、その上に機能の PR を積む。機能の PR の中で基盤を作ると、基盤の設計が 1 つの機能の都合で決まるためです。
9. **純粋な部分から実装し、検証の境界を書く。** 数学や変換とそのユニットテスト → 境界（ワイヤ、レイヤー） → Core → 入口と互換テスト → live の検証。live で確かめていない部分は 📋 または「未検証」と書く。

例として、「コントローラーの squeeze を一定時間握る高水準操作 `squeeze` を追加する」機能は次のように配置します（説明のための例で、計画ではありません）。

| 関心事 | 内容 | 配置 |
| --- | --- | --- |
| 仕様 | `/input/squeeze/value` は既に `VirtualDeviceState` の入力にある。ワイヤの変更は不要 | `docs/device-core-spec.md` §2.4 を確認するだけ |
| 高水準操作 | 値を補間して完全状態フレームを送り、時間が過ぎたら戻す | `playspectra/server.go` に `Server.Squeeze`。既存の `SetInput` で足りるなら新設しない |
| Scenario | ステップ名から `Server.Squeeze` への対応 | `Server.RunStep` |
| CLI | `playspectra cmd squeeze` の引数 | `cmd/playspectra/main.go` |
| MCP | tool の名前、スキーマ、既定値 | `mcp/server.go`、`docs/mcp-tools.md` |
| テスト | fake の `Transport` で送られたフレームの値を、命令した値と照合する | `playspectra/server_test.go`、`mcp/server_test.go` |
| 検証 | live の Monado で `get_state` が命令値になることを確かめるまでは未検証と書く | `docs/verification.md` |

## 8. コーディング規約

### 8.1 共通

- 識別子とコードのコメントは英語で書く（現在の Go、`layer/`、`driver/` はすべて英語）。`devicecore/` と仕様の文書には日本語のコメントと記述がある。言語の統一のためだけに既存のファイルを書き換えない。
- 文字コードは UTF-8。改行は `.gitattributes` に従い、`*.sh` は LF。
- 追跡するソースの先頭に MPL-2.0 のヘッダーと SPDX 行を書く。
- マジックナンバーは名前付き定数にし、値の根拠（仕様の節、実測）をコメントに書く。

### 8.2 Go

- フォーマットは `gofmt`、検査は `go vet ./...`。Go の版は `go.mod` の 1.22。
- 実行ファイルは `go build -o build/ ./cmd/playspectra` で作る。`-o playspectra` にすると、同名の `playspectra/` パッケージのディレクトリと衝突するためです。
- cgo を使わない。標準ライブラリだけを使う（第 4.2 節）。
- 外部と話す関数は `context.Context` を最初の引数に取る。
- 命名は Go の標準の規約に従う（パッケージは小文字 1 語、公開は UpperCamelCase、非公開は lowerCamelCase）。

### 8.3 C / C++

- `devicecore/` は C で書き、ランタイムの型を含めない。OS の差は `ps_os` に閉じ込める。
- `layer/` は C++ で書く。制御チャネルとフックから例外を外へ出さない。
- 新しい MSVC のビルドターゲットには `/utf-8` を付ける（`.claude/rules/build-cmake.md`）。
- Windows の interop（D3D11 の keyed mutex、MinGW での COM の IID の参照など）は、`.claude/rules/native-win-interop.md` の実測済みの手順に従う。D3D11 固有の同期を D3D12 や Vulkan に予防的に足さない。
- clangd 用の compile database は `scripts/gen_compile_commands.sh` で作る。

### 8.4 スクリプト

- bash で書き、先頭で `set -u` 以上を付ける（既存は `set -u`、`set -eu`、`set -uo pipefail`）。パイプの失敗を判定に使うスクリプトでは `pipefail` を付ける。Python を使わない（CI が拒否する）。
- live の相手と話すクライアントは Node の `.mjs` か `playspectra` のサブコマンドを使う。
- 新しいダウンロードは `curl -fSL` を使い、取得後に機械的に検査する（`.claude/rules/setup-scripts.md`）。
- 実行ファイルの解決は `scripts/lib_playspectra.sh` を使い、ソースのルートで `playspectra` を探さない。

## 9. グラフィックス API とランタイムへの対応

### 9.1 方針

グラフィックス API ごと、ランタイムごとの処理は、**ファイルごとに分け、各実装を独立させます。**
各実装は「その API（ランタイム）でどんな処理があれば実現できるか」だけを考えて設計し、他の実装を考慮しません。
共通化を先に設計せず、書いた実装を比較して、意味が同じと分かった部分だけを共通化します（第 9.3 節）。
似た処理を 1 つの関数で扱うと、API の差が条件分岐の中に隠れ、ある API の修正が別の API を壊すためです。

### 9.2 層ごとの責務

| 層 | 責務 | 配置 |
| --- | --- | --- |
| 取得の dispatch | swapchain 画像と subimage の解決、どの backend を呼ぶかの決定、出力パスの採番 | `layer/src/capture.cpp` |
| API ごとの backend | GPU からの読み出し、API 固有の同期（D3D11 の keyed mutex など）、フォーマットの判定 | `layer/src/capture_d3d11.cpp`、`capture_d3d12.cpp`、`capture_vulkan.cpp` |
| 意味が同じ後段処理 | 行の詰め直し、色の変換、PNG への書き出し | `layer/src/capture_common.cpp`、`pixel_convert.cpp`、`dxgi_formats.h` |
| ランタイム中立のデバイス状態 | 状態、プロトコル、制御チャネル | `devicecore/` |
| ランタイムごとのシェル | Core の状態をネイティブ型に変換する | Monado: `drv_playspectra`（submodule）、SteamVR: `driver/`（計画） |

- backend のインターフェースはフラットにする（基本型の引数だけ。`capture.cpp` の内部型を翻訳単位の外へ出さない）。
- 新しいグラフィックス API に対応するときは、`capture_<api>.cpp` を足し、`capture_backends.h` に宣言を足し、`capture.cpp` の dispatch に 1 か所足す。他の backend を変えない。
- 未対応のフォーマットは明示的なエラーにする（第 6.2 節）。

### 9.3 共通化の手順

1. 2 つ以上の API（ランタイム）で、それぞれ独立に処理を書き、テストを通す。
2. 書いた処理を比較し、同じに見える部分を挙げる。
3. 挙げた部分ごとに、第 11 節の判定（「一方を変えたとき、もう一方も必ず同じ変更が必要か」）を行う。
4. 「必ず同じ変更が必要」なものだけを共通部分に移し、移した理由をファイルの先頭のコメントに書く。例: `capture_common` は 3 つの backend が重複していた末尾の 3 段階をまとめたもので、GPU からの読み出しが済んだ後は API に依存しない。
5. 偶然同じに見えるだけのもの（API ごとの同期の手順など）は、API ごとのファイルに書いたままにする。

## 10. コメント

- ファイルの先頭に、責務、境界（このファイルがしないこと）、その境界にした理由を書く。読み手が本文より先に役割を知るためです。
- 実装内のコメントには、処理内容（what）ではなく理由（why）を書く。非自明な選択には、理由と根拠（仕様の文書と節、実測、Issue 番号）を書く。
    - 良い例: `// 2>&1 is load-bearing: Monado logs the enumeration to stderr; probe's stdout never contains the device names (measured).`
    - 悪い例: `// see spec`
- 仕様の節番号で参照するときは、内容も 1 文で書く。節番号だけでは、改版で番号がずれたときに追跡できないためです。
- 開発工程の経緯（どのマイルストーン、どのラウンドで誰が直したか）はコードの外（PR、Issue、`docs/`）に置く。現在の契約と経緯が混ざると、実装と食い違うコメントになるためです。
- 実測で確かめた事実と仮説をコメントでも区別する。仮説には検証方法を添える。
- 古くなったコメントは、コードと同じ PR で直す。コードを移動したら、説明のコメントも一緒に移す。コメントアウトしたコードは残さない。

## 11. 共通化の基準（DRY）

- **知識の重複は排除する。** 知識とは、同じ規則やドメイン知識（ワイヤの形式、入力 path の一覧、四元数の規約、PNG の書き出しなど）のこと。同じ意味と同じ変更理由を持つと**確認できた**時点で、その契約を所有する場所に抽出する。
- **偶発的な重複は抽出しない。** コードの形が同じでも、変更理由が異なるなら別の知識として扱う。判定基準は「一方を変更したとき、もう一方も必ず同じ変更が必要か」。偶発的な重複を共通化すると、呼び出し元の都合で共通関数に引数や分岐が増えるためです。
    - 例: Go の `playspectra/math.go` とレイヤーの `xr_math.h` は別のプロセスと言語の実装で、プロセスをまたいで共有しない。
- **共通化は比較の後に行う。** 1 か所目を書く時点で、将来の共通化を見越した抽象を作らない。
- 抽出したものには単体テストを付ける。何でも入る `utils` は作らない。

## 12. CLI、Scenario、MCP の設計

- 入口は業務ルールを持たない。引数を `Server` の呼び出しに変換し、結果を出力に変換するだけにする。
- 操作を追加、変更するときは、CLI、Scenario、MCP のどの経路に出すかを決め、出す経路のテストと文書を同じ PR で更新する。
- MCP の tool 名、スキーマ、既定値、説明、結果の形は互換テストで固定されている。変えるときは `docs/mcp-tools.md` とテストを同じ PR で更新する。
- `screenshot` はレイヤーが読み込まれたアプリを必要とし、それ以外の tool は操作のチャネルだけを必要とする。この前提を tool の説明と結果に反映する。
- 実装していない機能を、実装したように見せない。

## 13. テスト

- Go の単体テストは、対象と同じパッケージの `<対象>_test.go` に置く。外部との通信は `Transport` などの fake で置き換え、ネットワークなしで動かす。
- レイヤーの単体テストは、GPU、ランタイム、ソケットに依存しない部分だけを `layer/tests/` に置く。新しいソースの依存を足したら、`playspectra_test` のソース列も更新し、テストターゲットのビルドと実行を確認する（本体のビルドだけではテストのリンク切れを見逃すためです）。
- devicecore のプロトコルは `devicecore/playspectra_proto_test.c` でテストする。
- live の検証（`playspectra verify ...`、`scripts/run_*_monado.sh`、`scripts/integration_test.sh`）は、実アプリとランタイムが必要で、CI では動かない。結果は `docs/verification.md` に環境と証拠とともに書く。
- 判定は機械値で行う（例: 観測したフレーム数 > 0、命令した座標との誤差）。プロセスの生存時間や初期化ログの 1 行を根拠にしない。
- アサーションは「変化したか」ではなく「命令した値になったか」を見る。わざと間違った値を注入したら落ちるかを確かめる。
- 測定の範囲（サンプリングする行数、タイムアウト）が対象の取りうる値をカバーしているか確かめる。
- 検証できない項目は、理由を出力して明示的に SKIP にし、集計に数える。終了コードは FAIL だけで決め、SKIP を PASS に化けさせない（`scripts/run_all_tests.sh` の `ALL GREEN` と `GREEN WITH SKIPS`）。
- 横断のレビュー観点は `.claude/rules/review-checklist.md` にある。

## 14. 開発プロセス

1. 着手前に、関連する正本（第 1 節）を読む。新機能は第 7 節の手順で設計する。
2. **1 つの PR は 1 つの目的に限る。** 機能追加、仕様変更、不具合の修正、依存の更新、リファクタリング、文書だけの変更を混ぜない。
3. **移動（ファイルの移動、名前の変更）と論理の変更は別の PR にする。** 移動の PR は差分が大きく、論理の差分が埋もれるためです。名前を変えたら、旧い名前の `#include` と参照が残っていないかを grep で全数確認する。
4. **既存の振る舞いを変えない。** リファクタリングは外部契約（第 1.2 節の 3）を変えずに行う。振る舞いを変える必要がある場合は、別の Issue と PR にする。
5. **足りない基盤と露出した問題は別の Issue にする。** 作業中に見つけた不具合や仕様の欠陥は、再現条件と影響を書いた Issue にし、作業中の PR に混ぜない。
6. **リリース作業の邪魔をしない。** リリースの準備中は、リリースのブランチや CI と同じファイルを触る PR を同時に出さない。
7. Definition of Done:
    - ローカルゲート（第 17 節）が成功している。SKIP があれば、どのスイートかを PR に書く
    - CI が成功している
    - 追加、変更した処理にテストがある
    - 新しいコードが第 3 節から第 5 節の責務、依存、配置に準拠している
    - 必要な文書（このファイルの表、`docs/`、`README.md` の検証ラベル）が更新されている
    - live で確かめていない部分が PR と文書に「未検証」と明記されている

## 15. 定期リファクタリングとコードレビュー

### 15.1 方針

CI はビルド、テスト、`go vet`、cgo と Python の混入を検査します。構造のドリフトは日々の PR ではブロックせず、計画 Issue にまとめ、現在の位置から第 5 節の地図が示す位置へ移動して直します。
地図が明確であれば、どのドリフトも移動（Move）、分割（Split）、抽出（Extract）、名前の変更（Rename）の小さなリファクタリングで直せます。そのため、地図の曖昧さを残さないことを最優先にします。

### 15.2 サイクルの手順

1. 計画 Issue を起票し、基準のコミット、環境、ゲートの結果を固定する。
2. コードベース全体を第 3 節から第 5 節と照合し、ドリフトを計画 Issue に列挙する（形式は 15.3）。
3. ドリフトを「1 PR に 1 種類のリファクタリング」にまとめて直す。
4. 地図に欠落や矛盾が見つかったら、コードより先にこのファイルを直す PR を出す。
5. すべて解消したら計画 Issue をクローズする。

### 15.3 ドリフトの記述形式

| 項目 | 内容 |
| --- | --- |
| 現在の位置 | ファイルと関数、または型 |
| あるべき位置と形式 | 第 5 節の表に当てはめた結果 |
| 根拠 | このファイルの節番号、仕様の文書と節 |
| 修正方法 | Move、Split、Extract、Rename のいずれか |
| 影響範囲 | 変更するファイルと、振る舞いを固定するテスト |

### 15.4 重大度

| 重大度 | 対象 | 扱い |
| --- | --- | --- |
| 即時修正 | 製品不変条件（1.1）、依存の向き（4.1、4.2）、観測の正しさ（6.2）、外部契約（1.2 の 3）、検証されていない主張を事実として書くこと | その PR で差し戻す。マージ済みなら定期サイクルを待たずに直す |
| 次回サイクルで修正 | 配置、形式、命名、知識の重複、コメント | PR はマージしてよい。ドリフトを計画 Issue に記録する |

### 15.5 レビューの規則

- **レビューの前に、対象、目的、固定する判断軸、除外を書く。** レビュー中は宣言した軸を変えない。軸を変えると、新しい基準の陰に失敗が隠れるためです。
- 宣言した軸の中の失敗では差し戻す。軸の中の重要な問題はすべて報告する。
- 宣言した軸の外で重大な違反を見つけたら、軸外のブロッカーとして報告する。
- 直接の観察と推論を分ける。指摘にはファイルと行、またはコマンドの出力を証拠として付ける。レビュアーどうしで事実が食い違ったら、grep や実行の結果で裁く。
- 指摘には、このファイルの節番号か `.claude/rules/review-checklist.md` のレンズを根拠として書く。根拠を示せない好みは指摘にしない。
- 文書の数値は、引用元の生データから機械的に数えて照合する。
- 外部のレビュー（設計レビューのエージェントなど）の指摘は、コアループに必須かどうかを分類してから取り込む（`CLAUDE.md`）。

## 16. GitHub の運用

- **ブランチ**: `master` が既定でリリース元。作業は `master` からトピックのブランチ（既存の例: `feat/`、`fix/`、`docs/`、`refactor/`、`chore/`）を切り、PR で `master` にマージする。`master` に直接 push しない。
- **コミットメッセージ**: Conventional Commits の形（`feat:`、`fix:`、`docs:`、`test:`、`refactor:`、`build:`、`ci:`、`chore:`）を使う。
- **PR**: 1 つの目的に限り、検証の証拠（実行したコマンドと結果、SKIP、未検証の範囲）を本文に書く。PR テンプレートは現在ない。
- **リリース**: `v*` のタグで `release.yml` が CI のゲートを通してから、Windows と Linux のパッケージ（Go 実行ファイル、Monado ランタイム、レイヤー）を作り、smoke test の後に GitHub Release に添付する。
- **GitHub Actions**（`ci.yml`、push to `master` と PR で動く）:
    - `go-tests`（ubuntu / windows）: `go test`、`go vet`、cgo なしのビルド、Python と cgo の混入の検査
    - `layer-tests`（windows）: `playspectra_test` のビルドと CTest（`loader_test` を除く）
    - `layer-build`（ubuntu / windows）: 配布用の `playspectra_layer` のビルド
    - `monado-build`（ubuntu / windows）: Monado と devicecore のビルド、stage-install、`monado-cli probe` の smoke
    - live の E2E は CI に含まれない

## 17. よく使うコマンド

```sh
# ローカルゲート（PR を出す前に実行する。GPU、HMD、ランタイムは不要）
bash scripts/run_all_tests.sh

# Go だけ
go test ./...
go vet ./...
CGO_ENABLED=0 go build -o build/ ./cmd/playspectra

# レイヤーの単体テスト
cmake -S layer -B layer/build -DPLAYSPECTRA_BUILD_TESTS=ON
cmake --build layer/build --target playspectra_test
ctest --test-dir layer/build -E loader_test --output-on-failure

# レイヤー本体
cmake --build layer/build --target playspectra_layer

# Monado（submodule が必要）
git submodule update --init --recursive
task bootstrap:linux   # Linux / WSL: 依存の導入から Monado のビルドまで

# live の検証（Monado の :52702 が必要）
playspectra verify server
playspectra verify frame
playspectra verify mcp
```

- `run_all_tests.sh` の最後の行が `ALL GREEN` か `GREEN WITH SKIPS` かを確認する。SKIP は失敗ではないが、スキップしたスイートは検証されていない。
- 同じ `layer/build` を Windows と WSL で使い回さない。CMake のキャッシュが絶対パスを持つためです。
- 個別のテストと E2E のコマンドは `docs/testing.md` を読む。

## 18. 指示と文書の書き方

- 規則は「取る行動 → 理由 → 起こりうる状況と、それぞれの行動」の順で書く。短く書くと「X する。理由は Y。Z が起こりうるので、W の場合は V する」。
- 禁止は、同じ目的を果たす行動と、その理由と、場合ごとの行動で表す。例: 「推測を書かない」ではなく、「主張は実測か一次ソースを指せるときだけ事実として書く。推測が実測と真逆だったことがあるため。指せないときは仮説ラベルと検証方法を付ける」。
- 書き直した規則は、古い禁止が防いでいた状況で読み手が何をするかを問うて確かめる。
- 行動、理由、条件のどれも持たない文は削る。指示ファイルのすべての文は毎回のセッションで読まれるためです。
- 利用者向けの文書は ✅ / 🟡 / 📋 の 3 段階で検証境界を示し、「できること」と「できる予定」を混ぜない。
- これらの規則は指示に適用する。製品についての事実の記述（機能の説明、検証の記録、仕様）は事実の記述のままにする。
