# AGENTS.md — PlaySpectra 開発ルール

このファイルは、このリポジトリで設計、実装、レビューを行うすべての開発者と AI エージェントが常に従うルールです。
禁止事項の一覧ではなく、判断の境界、製品不変条件、構造と品質の必須ルール、Definition of Done を、設計と実装でそのまま使える形で定めます。
具体的な配置表、言語ごとの規約、手順は `docs/development/` に置き、作業の種類に応じて第 7 節の表で読みます。1 ファイルに全部を書くと、エージェントの読み込み上限（例: Codex の `project_doc_max_bytes` 既定 32 KiB）で後半の規則が届かなくなるためです。このファイルはその上限に収める。
用語は OpenXR とプロトコル仕様の語彙（`XrGraphicsBindingD3D12KHR`、`set_state`、`writer` / `observer`、STAGE など）をそのまま使います。言い換えによる意味のずれを避けるためです。
ルールを変えるときは、このファイルか `docs/development/` を更新する PR を出してから変えます。口頭やチャットだけで変えると、次のセッションが古いルールで作業するためです。

## 1. 前提

- **プロダクト**: PlaySpectra は「VR 版 Playwright」です。Playwright がブラウザにキーボードとマウスの合成入力を注入して画面を観察するのと同じく、VR HMD とコントローラーを触ったのと同じ入力を VR アプリに注入し、描画された画面を観察します。用途は AI による VR アプリの操作と、OpenXR アプリの自動テストです。**コアループ（入力注入 + 画面取得）が動くことが最優先の要件**です（`CLAUDE.md`）。
- **要求の正本**: `CLAUDE.md` の north star と 2 軸のスコープ規則。設計と仕様の正本は `docs/` 配下です。
    - `docs/architecture.md`: 2 つの関心事（デバイス状態の操作と観測 / アプリの計装）、3 プロセスの配布境界、責務の境界。
    - `docs/device-core-spec.md`: `VirtualDeviceState` と `:52702` の NDJSON ワイヤプロトコル（protocol_version 1）。§7 に未実装の項目がある。
    - `docs/steamvr-adapter.md`: SteamVR Adapter の設計。仮説（H1〜H3）を含み、📋設計・開発中。
    - `docs/verification.md`: 主張ごとの検証状態と証拠。状態を変えるときは、ここに再現できるコマンドか一次証拠を足す。
    - `docs/mcp-tools.md`、`docs/scenario-format.md`: MCP tool と JSON Scenario の外部契約。
    - `docs/roadmap.md`、`docs/go-port-compatibility-plan.md`: 計画と工程資料。権威は上の文書に置く。計画は製品の規範ではなく、それで上書きすると契約がずれるためです。
- **仕様が沈黙しているとき**: 判断は第 3 節の境界に従う。契約に関わる欠落は、該当箇所を引用して未解決として報告し、Owner の判断を受ける。契約に関わらない欠落は、根拠を書いて自分で決めて進める。過去のコード、テスト、Git の履歴は「何が行われたか」を示すだけで「何が決まったか」を示さないので、契約の欠落をそれで埋めない。
- **検証済みと未検証を分ける**: 文書、設計、実装に主張を書く前に、それが「このリポジトリ内の実測ログ、テスト結果、一次ソースを指させる**事実**」か「学習知識由来の**仮説**」かを分類する（`CLAUDE.md`）。仮説は仮説ラベルと検証方法を付けて書き、仕様の決定は事実の上にだけ置く。仮説の上に実装するときは「検証装置としての暫定実装」と明示する。外部ランタイムの挙動を推測で断言し、実測と真逆だった事故が実際に起きたためです。
- **文書の役割分担**:

    | 文書 | 置くもの |
    | --- | --- |
    | `AGENTS.md`（このファイル） | 常に要る規則: 判断の境界、製品不変条件、優先順位、構造と品質の必須ルール、Definition of Done、詳細の読み方 |
    | `docs/development/` | 作業ごとに読む詳細: 構造と配置の地図、実装規約、開発プロセス（第 7 節） |
    | `CLAUDE.md` | プロジェクトの核となる要求と、見失ってはいけない教訓。実装の詳細は書かない |
    | `README.md`、`docs/readme.ja.md` | 利用者向けの入口。✅実装+自動テスト済み / 🟡実機で部分検証済み / 📋設計・開発中 の 3 段階で検証境界を示す |
    | `docs/` | 設計と仕様の正本、検証マトリクス、利用者向けの詳細 |
    | `docs/testing.md` | ローカルゲートと CI の構成、個別のテストコマンド |
    | `.claude/rules/` | パス別の実測済みの罠（CMake、Windows interop、scripts）と、横断のレビュー観点（`review-checklist.md`） |

    内容が食い違ったら、要求は `CLAUDE.md`、プロトコルは `docs/device-core-spec.md`、検証状態は `docs/verification.md`、設計規則はこのファイルと `docs/development/` を優先し、食い違った側を同じ PR か別 Issue で直す。
- **作業記録**: 作業中の計画や工程記録を `.claude/` に置くときは、`.gitignore` が公開しない場所（`.claude/rules/` と `.claude/settings.json` 以外）に置き、追跡される文書とコードから参照しない（`CLAUDE.md`）。参照先が fresh clone に存在しないためです。
- **ライセンス**: MPL-2.0。ソースコード（`.go`、`.c`、`.h`、`.cpp`、`.sh`、`.mjs`）の先頭に MPL-2.0 のヘッダーと `SPDX-License-Identifier: MPL-2.0` を書く。第三者のコードと配布物の表示は `THIRD_PARTY_NOTICES.md` に書く。

## 2. 製品不変条件

次の不変条件は例外のない製品要件です。どの変更もこれを壊さない。壊す必要が出たら、それは契約の変更なので第 3 節に従い Owner の判断を受ける。

| 不変条件 | 内容 | 保証する機構 |
| --- | --- | --- |
| エンジン非依存 | 入力注入と観測は OpenXR の抽象化レベルで行う。Unity、Unreal などエンジン固有のプラグインや統合コードを必須の経路に置かない。Playwright がブラウザエンジンの内部を気にしないのと同じ位置に立つためです | OpenXR API レイヤー（`layer/`）と Runtime Adapter（`devicecore/` + ランタイム側シェル） |
| グラフィックス API の完全性 | 画面取得は OpenXR が定義する全グラフィックスバインディング（`XrGraphicsBindingD3D11KHR` / `D3D12KHR` / `VulkanKHR`）で動く。1 つでも欠けると「そのアプリは撮れない」不完全品になるので、どれも後回しにしない（`CLAUDE.md` のスコープの 2 軸） | `layer/src/capture_backends.h` の API ごとの backend |
| 入力経路の完全性 | 入力注入は全アクション種別（bool / float / vec2 / pose）、両手、頭部姿勢を扱う | `VirtualDeviceState`（`devicecore/`）と Go の Core の高水準操作 |
| 無言の壊れた結果を出さない | 未対応のフォーマット、MSAA、デバイスなしなどでは `{ok:false, error}` を返し、空白や全ゼロの画像を成功として返さない。受け取った値を無視して成功を返さない | 各 capture backend の戻り値の JSON |
| 観測対象のアプリを巻き込まない | レイヤーはアプリのプロセス内で動くので、C++ 例外を制御チャネルとフックの外へ出さない。例外の捕捉は不正メモリアクセスや GPU ドライバーの障害を防がないので、外部から来た値（ハンドル、サイズ、フォーマット、インデックス）は使う前に検査し、扱えないものはエラーで返す。ツールが観測対象を落とすと、観測そのものが成り立たないためです | `layer/src/control_channel.cpp` の `HandleRequest`（全体を try で囲む）と、各 backend の入力検査 |
| 操作と観測の分離 | 操作（デバイス状態）は Runtime Adapter の `127.0.0.1:52702`、観測（screenshot、recording、action discovery、診断）はレイヤーの `127.0.0.1:52700`。レイヤーの入力 override はテスト補助で、主経路ではない | `docs/architecture.md`、`devicecore/playspectra_control.c`、`layer/src/control_channel.cpp` |
| 完全スナップショット | `set_state` は接続中のデバイスの全可変フィールドを毎回含み、前回値を継承しない。writer は同時に 1 接続、observer は複数 | `devicecore/playspectra_proto.c`、`playspectra_control.c`（`docs/device-core-spec.md` §2.2、§5.3） |
| devicecore のランタイム中立 | `devicecore/` は Monado、SteamVR、OpenXR の型を知らない。ランタイム固有の値（ポート、名前、descriptor、ログ）は adapter が `playspectra_control_config` で注入する | `devicecore/README.md` |
| 配布単位の分離 | 実行時の 3 プロセス（Go の `playspectra`、Runtime Adapter を含むランタイム、レイヤーを読み込んだアプリ）は TCP の NDJSON 境界だけで通信する。Go 実行ファイルは C/C++ の成果物をリンクせず、cgo を使わない | `docs/architecture.md` の Distribution boundary、CI の cgo 検査 |
| 入口の同値 | CLI、JSON Scenario、MCP は同じ Core（`playspectra.Server`）を呼ぶ対等な入口で、業務ロジックを持たない | `cmd/playspectra`、`mcp/`、`playspectra/` |
| 主張と証拠 | 振る舞いを「対応済み」「検証済み」と書くのは、`docs/verification.md` に再現できる証拠があるときだけ。ユニットテストが通ったことを、ランタイムやエンジンへの対応の主張に変えない | `docs/verification.md`、`docs/testing.md` の Verification discipline |
| 深度は任意 | 既定の画面取得は利き目のカラー画像。深度マップは「出してもいい」nice-to-have で、必須仕様にしない。生の左右ステレオは `eye` パラメータで選べるように残す | `CLAUDE.md`、`Screenshot(ctx, eye, ...)` |

## 3. 判断の境界

作業を止めて Owner（ユーザー）に聞くのは、**契約が変わるとき**だけです。契約を保つ内部の判断は、AI が根拠とテストを残して自分で決めて進めます。全部を聞くと、局所的な判断のたびに作業が止まり、不要な Issue と PR が増えるためです。

| 区分 | 対象 | 扱い |
| --- | --- | --- |
| **Owner の判断** | 要求とスコープ（`CLAUDE.md`、何をコア必須にするか）、受入条件、外部契約（`:52702` のワイヤ、`:52700` のコマンド、CLI の引数と終了コード、MCP の tool 名・スキーマ・既定値・結果、Scenario と recording の形式）、製品不変条件（第 2 節）、安全性（bind 先、外部プロセスの起動、ダウンロード）、外部モジュールや新しいネイティブ依存の追加、リリースと公開 | 選択肢と推奨を示して判断を受ける。判断を待つ間も、それに依存しない部分は進める |
| **AI の自律判断** | 契約を保つ内部設計、設計パターンの選択、ファイルとパッケージの分割、命名、テスト補助や fake の追加、責務に沿った内部構造の変更、`docs/development/` の地図の範囲内での配置 | 決めて進め、選んだ理由を PR に書き、テストで振る舞いを固定する。地図に当てはまらない配置を選んだら、同じ PR で地図（`docs/development/structure.md`）を更新する |

- **基盤の扱い**: 作業の目的に密接な基盤（テスト補助、内部の helper、小さなインターフェース）は同じ PR に含める。独立していて影響が広い基盤の変更（複数のコンポーネントにまたがる移動、外部契約に触れる変更）だけを先行 PR に分ける。
- **未決事項があるとき**: 文書が決めていないことが契約に関わるなら、該当箇所を引用して未解決として報告する。契約に関わらないなら、根拠を書いて決めて進める。過去のコードや履歴は「何が行われたか」を示すだけで「何が決まったか」を示さないので、契約の欠落をそれで埋めない。
- **設計レビューの指摘**: 外部のレビューやエージェントの指摘は「コアループに必須」か「将来の拡張」かを分類し、後者をタスクの主軸に混ぜない（`CLAUDE.md`）。

## 4. 契約とトレードオフの優先順位

迷ったら上位を優先します。

1. **観測の正しさ**: 無言で誤った画像、状態、検証結果を返すより、明示的なエラーで失敗する。
2. **OpenXR 抽象化レベルの完全性**: 全グラフィックス API、全入力経路を揃える。エンジン固有のコードで穴を埋めない。
3. **外部契約の維持**: `:52702` のワイヤプロトコル、`:52700` のコマンド、CLI の引数と終了コード、MCP の tool 名、スキーマ、既定値、結果の形、JSON Scenario と recording の形式を変えない。変えるときは Owner の判断を受け（第 3 節）、仕様の文書を先か同じ PR で更新する。
4. **テスタビリティ**: GPU、ランタイム、ネットワークに触れない純粋な部分を切り出し、ホストのユニットテストで検証できるようにする。
5. **シンプルさ**: コアループに不要な汎用化はしない（YAGNI）。ただし知識の重複は排除する（DRY）。

## 5. 構造と品質の必須ルール

詳細は `docs/development/` にあります。ここにあるのは、どの作業でも守る骨格です。

- **配置**: コードは責務の合う場所に置く。アプリのプロセス内の観測は `layer/`、ランタイムのデバイス経路は `devicecore/`（中立）と adapter シェル（変換だけ）、ワイヤは `protocol/`、高水準操作と判定は `playspectra/`、入出力の変換は `cmd/playspectra/` と `mcp/`、live の検証は `verify/`。判定の手順と配置表は `structure.md`。
- **依存の向き**: Go の依存は入口から Core へ向け、`playspectra` は他の自前パッケージに依存しない。外部との通信は `Transport` / `CaptureTransport` を注入する。`go.mod` は外部モジュールを `require` しない。許可リストは `structure.md`。
- **分割**: 「別の理由で変わるものを別のファイルに」を基準にする。純粋な部分はユニットテストできる単位に切り出す。グラフィックス API とランタイムごとの処理はファイルごとに独立させ、比較して意味が同じと分かった部分だけを共通化する。
- **命名と重複**: 命名は各言語の標準の規約に従う。知識の重複（同じ規則が 2 か所で同じ理由で変わる）は排除し、形が似ているだけの偶発的な重複は共通化しない。
- **コメント**: ソースファイル（`.go`、`.c`、`.h`、`.cpp`、`.sh`、`.mjs`）の先頭に責務と境界を書き、本文には what ではなく why を書く。JSON などコメントを持てない形式は対象外。開発工程の経緯はコードに書かない。
- **テスト**: 追加、変更した振る舞いにテストを付ける。アサーションは「変化したか」ではなく「命令した値になったか」を見る。検証できない項目は明示的に SKIP にし、終了コードは FAIL だけで決める。
- **1 PR 1 目的**: 機能、不具合の修正、リファクタリング、依存の更新を混ぜない。移動と論理の変更は別の PR にする。

## 6. Definition of Done と重大度

### 6.1 Definition of Done

PR をマージ可能（Ready for review）にする条件です。

- CI の全ジョブが成功している（`.github/workflows/ci.yml`）。
- ローカルゲート `bash scripts/run_all_tests.sh` が成功している。SKIP があれば、どのスイートかを PR に書く。
- 追加、変更した振る舞いにテストがある。
- **この PR で追加、変更したコード**が、第 5 節と `docs/development/` の配置、依存、命名、分割、コメントに準拠し、意味の重複を持ち込んでいない。
- 外部契約を変えたなら、その仕様の文書が同じ PR で更新されている。
- live で確かめていない部分が、PR と文書に「未検証」と明記されている。

### 6.2 重大度

判定の対象は「この PR が持ち込んだもの」か「既存のもの」かで分けます。レビューの軸、重大度、CI の扱いはこの表で一本化します。

| 対象 | 例 | 扱い |
| --- | --- | --- |
| CI の失敗 | ビルド、テスト、`go vet`、cgo と Python の混入の検査 | マージしない。原因を直す |
| この PR が持ち込んだ不備 | 製品不変条件や依存の向きへの違反、責務の逸脱、不適切な命名、意味の重複、欠けたテストやコメント、未検証の主張 | マージ前にこの PR で直す |
| 既存のドリフト（この PR の範囲外） | 以前からある配置のずれ、命名、重複 | この PR を止めない。ドリフトを Issue に記録し、定期リファクタリングで直す（`process.md`）。ただし製品不変条件への既存の違反を見つけたら、別 PR で優先して直す |

## 7. 作業ごとに読む文書

`docs/development/` の 3 文書を、作業に応じて読みます。表の「必ず読む」を読まずに、その作業を始めない。

| 作業 | 必ず読む | 必要に応じて読む |
| --- | --- | --- |
| 新機能の設計 | `structure.md`（全体）、`process.md` §1（新機能の設計手順） | 関連する `docs/` の仕様 |
| 既存コードの変更、不具合の修正 | `structure.md` §4（配置の地図）、`implementation.md` の該当する言語の節 | `structure.md` §3（依存） |
| グラフィックス API、ランタイム、入力経路への対応 | `structure.md` §5（API とランタイムへの対応）、`implementation.md` §2（C / C++） | `.claude/rules/native-win-interop.md` |
| テストの追加、E2E、スクリプト | `implementation.md` §5（テスト）と §2（スクリプト） | `docs/testing.md`、`.claude/rules/setup-scripts.md` |
| CLI、Scenario、MCP の変更 | `implementation.md` §4（入口の設計） | `docs/mcp-tools.md`、`docs/scenario-format.md` |
| レビュー、定期リファクタリング | `process.md` §3（レビューとリファクタリング） | `.claude/rules/review-checklist.md` |
| PR、コミット、リリース、コマンド | `process.md` §2、§4、§5 | `.github/workflows/` |
| 文書や指示の作成 | `process.md` §6（指示と文書の書き方） | |

| 文書 | 内容 |
| --- | --- |
| `docs/development/structure.md` | アーキテクチャ、責務の表、依存の許可リスト、配置の地図と決定手順、API とランタイムへの対応 |
| `docs/development/implementation.md` | 場面ごとの設計（エラー、外部入力、決定論、安全性）、言語ごとの規約、コメント、DRY、入口の設計、テスト |
| `docs/development/process.md` | 新機能の設計手順、開発プロセス、レビューとリファクタリング、GitHub の運用、よく使うコマンド、指示と文書の書き方 |
| `.claude/rules/` | パス別の実測済みの罠（CMake、Windows interop、scripts）と、横断のレビュー観点 |
