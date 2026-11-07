# PlaySpectra の構造 — アーキテクチャ、責務、依存、配置

この文書は `AGENTS.md` の第 5 節（構造と品質の必須ルール）の詳細です。判断の境界、製品不変条件、Definition of Done は `AGENTS.md` にあります。
この地図に当てはまらない配置を選んだら、同じ PR でこの文書を更新します（`AGENTS.md` 第 3 節）。

## 1. アーキテクチャ

### 1.1 アーキテクチャスタイル

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

### 1.2 入口と実行形態

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

### 1.3 データの流れと副作用

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

### 1.4 移行中の項目

- **SteamVR Adapter（📋計画）**: `driver/` は初期のスケルトンで、独自の暫定プロトコルを持つ。これを `devicecore/` に接続し、Windows で検証するのが残作業です（`docs/roadmap.md`）。`driver/` に独自プロトコルの機能を足さない。足すと devicecore への接続で捨てる二重実装になるためです。
- **Python と TypeScript の撤去（完了）**: 制御プレーンは Go に移植済み。CI が追跡中の `*.py`、`scripts/*.sh` からの Python 呼び出し、cgo を検査して拒否する。Node の `.mjs` クライアントは `scripts/` の統合テスト補助として残っている。

## 2. 責務の分割

### 2.1 分割の判断基準

パッケージ、ディレクトリ、ファイルの境界は、次の問いに順に答えて決めます。

1. **どのプロセスで動くか。** Go の制御プレーン、ランタイムのプロセス（devicecore と adapter シェル）、アプリのプロセス（レイヤー）のどれかに置く。プロセスをまたぐ呼び出しは TCP の NDJSON 境界だけにする。
2. **特定のランタイム、グラフィックス API、エンジンに依存するか。** ランタイム固有は adapter シェル、グラフィックス API 固有は capture backend の翻訳単位に置く。エンジン固有のコードはスコープ外として置かない。
3. **副作用（ソケット、GPU、ファイル、プロセス、時刻）を持つか。** 持つ部分は境界側に置き、補間、数学、ピクセル変換、判定は純粋な関数にしてユニットテストする（例: `pixel_convert`、`pose_animator`、`pose_override_pure.h`、`playspectra/math.go`）。
4. **変更の理由は何か（単一責任の原則、SRP）。** 変更の理由が 2 つあれば分割する。例: ワイヤのフレーミングの変更と、高水準操作の補間の変更の両方で同じ関数を直すなら、分割が誤っている。
5. **外部入力をパースするか。** パースとバリデーションは信頼境界（devicecore の `playspectra_proto`、レイヤーの `HandleRequest`、MCP の引数の抽出、`protocol.Client`）に集約する。

### 2.2 トップレベルのディレクトリ

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

### 2.3 Go パッケージの責務

パッケージの追加や責務の変更は、その変更と同じ PR でこの表を更新します（`AGENTS.md` 第 3 節）。

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

### 2.4 ネイティブコンポーネントの責務

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

### 2.5 ファイルの粒度

- 「別の理由で変わるものを別のファイルに」を唯一の基準にする。行数の削減や「1 関数 1 ファイル」を目的にしない。
- GPU、ランタイム、OpenXR に依存しない部分は、ユニットテストできるように別の翻訳単位かヘッダーオンリーに切り出す（例: `pose_override_pure.h` は `pose_override.cpp` の session / dispatch の依存なしにテストするために分けた）。
- ファイルの先頭に、そのファイルの責務と「なぜこの境界か」をコメントで書く（`layer/src/*.h` の既存の形）。
- Go の公開識別子は、パッケージの外から使われるものだけにする。テストのためだけに公開しない（同じパッケージの `_test.go` から使う）。

## 3. 依存関係の構築

### 3.1 依存の向きの決め方

- **依存は入口から Core へ、Core からインターフェースへ向ける。** `playspectra` は他の自前パッケージに依存せず、`protocol.Client` の具体型も知らない。具体型は入口（コンポジションルート）が注入する。
- **プロセスをまたぐ依存は、TCP の NDJSON 境界だけにする。** Go は C/C++ をリンクせず、レイヤーはランタイムにリンクせず、devicecore は Go を知らない。
- **ランタイム固有の処理が Core で必要になったら、`devicecore` の状態モデルとワイヤプロトコルに中立な形で表し、adapter シェルで変換する。** Core に `xrt_*` や `DriverPose_t` を持ち込まない。
- **汎用の plugin 機構や factory を、将来の拡張のためだけに作らない。** 必要になった時点で、必要な形だけを作る。

### 3.2 Go パッケージ間の依存の許可リスト

表にない依存は作りません。新しい自前パッケージ間の依存が必要になったら、3.1 の基準で向きを確認し、同じ PR でこの表を更新します。外部モジュールの追加は Owner の判断です（`AGENTS.md` 第 3 節）。現状は表のとおりです（`go list -f '{{.Imports}}' ./...` で確認）。

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

### 3.3 コンポジションルートと注入

1. `cmd/playspectra` が `protocol.Dial` で接続を作り、`playspectra.NewServer` に `Transport` と `CaptureTransport` を渡す。`mcp` は `Factory` を受け取り、最初の tool 呼び出しで `Server` を作る。
2. `playspectra` の関数は接続を自分で作らない。テストは同じインターフェースの fake を渡す。
3. devicecore はポート、ランタイム名、descriptor、ログ関数を `playspectra_control_config` で受け取る。Monado の fork は `PLAYSPECTRA_DEVICECORE_DIR` で devicecore のソースを取り込む。SteamVR の driver も同じソースを使う計画です。
4. レイヤーのフックは `kHooks[]`（`layer_entry.cpp`）に明示的に列挙する。

### 3.4 境界を越えるデータのスキーマ

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

## 4. 配置の地図

この節は「何を、どこに、どの形で置くか」の正本です。
レビューと定期リファクタリングは、この地図と第 2 節、第 3 節との照合で判断します。

### 4.1 配置表

| 対象 | 配置 | 形式 |
| --- | --- | --- |
| Go 実行ファイルのエントリポイント | `cmd/playspectra/main.go` | 「サブコマンドの解析 → Core の呼び出し → JSON の印字 → 終了コード」だけ |
| 高水準操作（move_head など） | `playspectra/server.go` の `Server` のメソッド | 補間した完全状態フレームを `SetState` で送る |
| 状態モデルと入力 path の一覧 | `playspectra/state.go` | `LeftInputPaths` / `RightInputPaths` |
| 姿勢の数学 | `playspectra/math.go`（Go）、`layer/src/xr_math.h`（レイヤー） | 副作用のない関数 |
| Scenario のステップの解釈 | `playspectra.Server.RunStep` / `RunScenario` | CLI と MCP は同じ関数を使う |
| MCP の tool | `mcp/server.go` | 引数の抽出、`Server` の呼び出し、結果の変換だけ |
| ワイヤの送受信 | `protocol/client.go` | |
| live スタックの自己検証 | `verify/<対象>.go` | `Report` に `Check` を積む。機械値で判定する（`implementation.md` 第 5 節） |
| デバイス状態とプロトコル | `devicecore/` | ランタイムの型を含めない |
| ランタイム型への変換 | adapter シェル（Monado は submodule の `drv_playspectra`） | Core の状態を読み、ネイティブ型に写す |
| フック | `layer/src/hooks_<クラスター>.cpp` | `kHooks[]` に登録 |
| グラフィックス API ごとの読み出し | `layer/src/capture_<api>.cpp` | `capture_backends.h` のフラットなインターフェース |
| API に依存しない読み出しの後段処理 | `layer/src/capture_common.cpp`、`pixel_convert.cpp` | ユニットテスト付き |
| レイヤーの制御コマンド | `layer/src/control_channel.cpp` の `Handle_<cmd>` と `kHandlers[]` | 例外を外へ出さず、`{ok:false, error}` を返す |
| E2E とセットアップの手順 | `scripts/<目的>.sh`、共通処理は `scripts/lib_*.sh` | bash。判定は機械値 |
| C のプローブ | `tools/` | |
| Scenario の例 | `tools/scenarios/*.json` | |
| Go の単体テスト | 対象と同じパッケージの `<対象>_test.go` | `implementation.md` 第 5 節 |
| レイヤーの単体テスト | `layer/tests/test_<対象>.cpp` と `playspectra_test` のソース列 | 純粋な部分だけ |
| 利用者向けの説明 | `docs/<題名>.md` | 3 段階の検証ラベルを付ける |
| 検証状態と証拠 | `docs/verification.md` | 環境、証拠、境界 |

### 4.2 配置の決定手順

上から順に判定し、最初に該当した場所に置きます。

1. 観測対象のアプリのプロセス内で動く必要があるか（swapchain、OpenXR の呼び出しの観測） → `layer/`
2. ランタイムがデバイス状態を同期的に問い合わせる経路か → `devicecore/`（ランタイム中立）と adapter シェル（変換だけ）
3. ワイヤの送受信か → `protocol/`
4. 高水準操作、補間、アサーション、Scenario、recording か → `playspectra/`
5. 入出力の変換（CLI の引数、MCP の JSON-RPC）か → `cmd/playspectra/`、`mcp/`
6. live スタックに対する検証の手順か → `verify/`（外部アプリ固有なら `vrapp/`）
7. 環境の準備や E2E の起動か → `scripts/`（Go の補助が要るなら `setuphelper/` と `playspectra internal`）
8. それ以外 → 2.3、2.4 の責務表で責務の合う場所

## 5. グラフィックス API とランタイムへの対応

### 5.1 方針

グラフィックス API ごと、ランタイムごとの処理は、**ファイルごとに分け、各実装を独立させます。**
各実装は「その API（ランタイム）でどんな処理があれば実現できるか」だけを考えて設計し、他の実装を考慮しません。
共通化を先に設計せず、書いた実装を比較して、意味が同じと分かった部分だけを共通化します（第 5.3 節）。
似た処理を 1 つの関数で扱うと、API の差が条件分岐の中に隠れ、ある API の修正が別の API を壊すためです。

### 5.2 層ごとの責務

| 層 | 責務 | 配置 |
| --- | --- | --- |
| 取得の dispatch | swapchain 画像と subimage の解決、どの backend を呼ぶかの決定、出力パスの採番 | `layer/src/capture.cpp` |
| API ごとの backend | GPU からの読み出し、API 固有の同期（D3D11 の keyed mutex など）、フォーマットの判定 | `layer/src/capture_d3d11.cpp`、`capture_d3d12.cpp`、`capture_vulkan.cpp` |
| 意味が同じ後段処理 | 行の詰め直し、色の変換、PNG への書き出し | `layer/src/capture_common.cpp`、`pixel_convert.cpp`、`dxgi_formats.h` |
| ランタイム中立のデバイス状態 | 状態、プロトコル、制御チャネル | `devicecore/` |
| ランタイムごとのシェル | Core の状態をネイティブ型に変換する | Monado: `drv_playspectra`（submodule）、SteamVR: `driver/`（計画） |

- backend のインターフェースはフラットにする（基本型の引数だけ。`capture.cpp` の内部型を翻訳単位の外へ出さない）。
- 新しいグラフィックス API に対応するときは、`capture_<api>.cpp` を足し、`capture_backends.h` に宣言を足し、`capture.cpp` の dispatch に 1 か所足す。他の backend を変えない。
- 未対応のフォーマットは明示的なエラーにする（`implementation.md` 第 1.2 節）。

### 5.3 共通化の手順

1. 2 つ以上の API（ランタイム）で、それぞれ独立に処理を書き、テストを通す。
2. 書いた処理を比較し、同じに見える部分を挙げる。
3. 挙げた部分ごとに、`implementation.md` 第 3.2 節の判定（「一方を変えたとき、もう一方も必ず同じ変更が必要か」）を行う。
4. 「必ず同じ変更が必要」なものだけを共通部分に移し、移した理由をファイルの先頭のコメントに書く。例: `capture_common` は 3 つの backend が重複していた末尾の 3 段階をまとめたもので、GPU からの読み出しが済んだ後は API に依存しない。
5. 偶然同じに見えるだけのもの（API ごとの同期の手順など）は、API ごとのファイルに書いたままにする。

