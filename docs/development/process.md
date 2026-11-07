# PlaySpectra の開発プロセス — 設計手順、PR、レビュー、運用、コマンド

この文書は `AGENTS.md` の第 3 節（判断の境界）と第 6 節（Definition of Done と重大度）を、作業の手順に落としたものです。判断の境界、Definition of Done、重大度の正本は `AGENTS.md` で、ここでは繰り返さずに参照します。

## 1. 新機能の設計手順

1. **関連する正本を読む。** `CLAUDE.md`、`docs/architecture.md`、該当する仕様（`docs/device-core-spec.md` など）を読み、仕様が決めていることと決めていないことを分ける。決めていないことのうち、契約に関わるもの（`AGENTS.md` 第 3 節の「Owner の判断」の行）だけを Owner に確認する。契約に関わらないものは、根拠を書いて自分で決める。確認を待つ間も、それに依存しない部分は進める。
2. **コアループに必須か、将来の拡張かを分類する。** OpenXR の構造体と関数で表される普遍的な話（全グラフィックス API、全入力経路）はコア必須。特定エンジンの内部の話はスコープ外。ユーザーが「〜してもいい」と許容表現で言ったものだけを nice-to-have にする（`CLAUDE.md`）。
3. **機能を関心事ごとに分解する。** 「アプリのプロセス内の観測」「ランタイムのデバイス経路」「ワイヤ」「高水準操作と判定」「入出力の変換」「検証の手順」に分ける。
4. **関心事ごとに配置を決める。** `structure.md` 第 4.2 節の手順で判定する。1 つの機能が Go、devicecore、レイヤーにまたがるのは通常のことです。
5. **外部契約への影響を確認する。** ワイヤ、レイヤーのコマンド、CLI、MCP、Scenario の形式を変えるなら Owner の判断を受け、仕様の文書（`structure.md` 第 3.4 節の所有者）を同じ PR か先の PR で更新する。
6. **依存を確認する。** 自前パッケージ間の依存は `structure.md` 第 3.2 節の許可リストを確認し、なければ第 3.1 節の基準で向きを合わせて同じ PR で表を更新する。外部モジュールやネイティブ依存の追加は Owner の判断です。
7. **新しいパッケージやコンポーネントは、既存のものに収まらないときに作る。** 条件は、`structure.md` 第 2.3、2.4 節のどの行にも該当しない、単独でテストできる、第 3.2 節に循環なく置ける、の 3 つ。満たせば同じ PR で表を更新して作ってよい。
8. **基盤の置き場所を決める。** 作業の目的に密接な基盤（テスト補助、内部の helper、小さなインターフェース）は同じ PR に含める。独立していて影響が広い基盤（複数のコンポーネントにまたがる移動、外部契約に触れる変更）だけを先行 PR に分ける。
9. **純粋な部分から実装し、検証の境界を書く。** 数学や変換とそのユニットテスト → 境界（ワイヤ、レイヤー） → Core → 入口と互換テスト → live の検証。live で確かめていない部分は 📋 または「未検証」と書く。

例として、「コントローラーの squeeze を一定時間握る高水準操作 `squeeze` を追加する」機能は次のように配置します（説明のための例で、計画ではありません）。CLI と MCP に操作を足すのは外部契約の追加なので、Owner の判断を受けてから進めます。

| 関心事 | 内容 | 配置 |
| --- | --- | --- |
| 仕様 | `/input/squeeze/value` は既に `VirtualDeviceState` の入力にある。ワイヤの変更は不要 | `docs/device-core-spec.md` §2.4 を確認するだけ |
| 高水準操作 | 値を補間して完全状態フレームを送り、時間が過ぎたら戻す | `playspectra/server.go` に `Server.Squeeze`。既存の `SetInput` で足りるなら新設しない |
| Scenario | ステップ名から `Server.Squeeze` への対応 | `Server.RunStep` |
| CLI | `playspectra cmd squeeze` の引数 | `cmd/playspectra/main.go` |
| MCP | tool の名前、スキーマ、既定値 | `mcp/server.go`、`docs/mcp-tools.md` |
| テスト | fake の `Transport` で送られたフレームの値を、命令した値と照合する | `playspectra/server_test.go`、`mcp/server_test.go` |
| 検証 | live の Monado で `get_state` が命令値になることを確かめるまでは未検証と書く | `docs/verification.md` |

## 2. 開発プロセス

1. 着手前に、関連する正本（`AGENTS.md` 第 1 節）を読む。新機能は第 1 節の手順で設計する。
2. **1 つの PR は 1 つの目的に限る。** 機能追加、仕様変更、不具合の修正、依存の更新、リファクタリングを混ぜない。目的のために必要な基盤やテスト補助は、目的の一部として同じ PR に含めてよい（第 1 節の 8）。
3. **移動（ファイルの移動、名前の変更）と論理の変更は別の PR にする。** 移動の PR は差分が大きく、論理の差分が埋もれるためです。名前を変えたら、旧い名前の `#include` と参照が残っていないかを grep で全数確認する。
4. **外部契約を保つ。** リファクタリングは外部契約（`AGENTS.md` 第 3 節）を変えずに行う。振る舞いを変える必要がある場合は、別の Issue と PR にする。
5. **範囲外の問題は Issue にする。** 作業中に見つけた、この PR の目的と関係のない既存の不具合、仕様の欠陥、ドリフトは、再現条件と影響を書いた Issue にし、作業中の PR に混ぜない。この PR が持ち込んだ不備は Issue に回さず、この PR で直す（`AGENTS.md` 第 6.2 節）。
6. **リリース作業の邪魔をしない。** リリースの準備中は、リリースのブランチや CI と同じファイルを触る PR を同時に出さない。
7. **マージ可能にする条件**は `AGENTS.md` 第 6.1 節の Definition of Done です。満たしたら Draft ではなく通常の PR（Ready for review）にする。Draft は作業途中のものだけに使う。

## 3. 定期リファクタリングとコードレビュー

### 3.1 方針

この節が扱うのは**既存のドリフト**（この PR の範囲外にある、以前からの配置、命名、重複のずれ）です。既存のドリフトは日々の PR を止めず、計画 Issue にまとめ、現在の位置から `structure.md` 第 4 節の地図が示す位置へ移動して直します。
この PR が持ち込んだ不備は、ここではなく `AGENTS.md` 第 6.2 節に従ってマージ前に直します。
地図が明確であれば、どのドリフトも移動（Move）、分割（Split）、抽出（Extract）、名前の変更（Rename）の小さなリファクタリングで直せます。そのため、地図の曖昧さを残さないことを最優先にします。

### 3.2 サイクルの手順

1. 計画 Issue を起票し、基準のコミット、環境、ゲートの結果を固定する。
2. コードベース全体を `structure.md` 第 2 節から第 4 節と照合し、ドリフトを計画 Issue に列挙する（形式は 3.3）。
3. ドリフトを「1 PR に 1 種類のリファクタリング」にまとめて直す。
4. 地図に欠落や矛盾が見つかったら、コードより先に地図（`structure.md`）を直す。
5. すべて解消したら計画 Issue をクローズする。

### 3.3 ドリフトの記述形式

| 項目 | 内容 |
| --- | --- |
| 現在の位置 | ファイルと関数、または型 |
| あるべき位置と形式 | `structure.md` 第 4 節の表に当てはめた結果 |
| 根拠 | `AGENTS.md` か `docs/development/` の節番号、仕様の文書と節 |
| 修正方法 | Move、Split、Extract、Rename のいずれか |
| 影響範囲 | 変更するファイルと、振る舞いを固定するテスト |

### 3.4 レビューの手順

重大度と、何をマージ前に直すかは `AGENTS.md` 第 6.2 節の表だけで決めます。レビューの軸はその表の判定に使う観点で、表と別の合否基準を持ちません。

1. **レビューの前に、対象、目的、判断軸、除外を書く。** 判断軸は `AGENTS.md` 第 2 節、第 5 節、第 6 節と、該当する `docs/development/` の節、`.claude/rules/review-checklist.md` のレンズから選ぶ。レビュー中は宣言した軸を変えない。軸を変えると、新しい基準の陰に失敗が隠れるためです。
2. **CI を確認する。** 失敗があれば、ほかの指摘より先に報告する。
3. **この PR が追加、変更した部分を、宣言した軸で見る。** 見つけた不備は、重さに関わらずマージ前の修正として報告する（命名、配置、意味の重複、コメント、欠けたテストを含む）。
4. **範囲外の既存のドリフトは、別に報告する。** この PR を止めず、Issue にする（第 3.1 節）。製品不変条件への既存の違反は、別 PR で優先して直すよう提案する。
5. **宣言した軸の外で重大な違反を見つけたら、** 軸外のブロッカーとして報告する。

- 直接の観察と推論を分ける。指摘にはファイルと行、またはコマンドの出力を証拠として付ける。レビュアーどうしで事実が食い違ったら、grep や実行の結果で裁く。
- 指摘には、`AGENTS.md` か `docs/development/` の節番号、または review-checklist のレンズを根拠として書く。根拠を示せない好みは指摘にしない。
- 文書の数値は、引用元の生データから機械的に数えて照合する。
- 外部のレビュー（設計レビューのエージェントなど）の指摘は、コアループに必須かどうかを分類してから取り込む（`CLAUDE.md`）。

## 4. GitHub の運用

- **ブランチ**: `master` が既定でリリース元。作業は `master` からトピックのブランチ（既存の例: `feat/`、`fix/`、`docs/`、`refactor/`、`chore/`）を切り、PR で `master` にマージする。`master` に直接 push しない。
- **コミットメッセージ**: Conventional Commits の形（`feat:`、`fix:`、`docs:`、`test:`、`refactor:`、`build:`、`ci:`、`chore:`）を使う。
- **PR**: 1 つの目的に限り、検証の証拠（実行したコマンドと結果、SKIP、未検証の範囲）を本文に書く。PR テンプレートは現在ない。Definition of Done を満たしたら通常の PR にし、Draft は作業途中のものだけに使う。
- **リリース**: `v*` のタグで `release.yml` が CI のゲートを通してから、Windows と Linux のパッケージ（Go 実行ファイル、Monado ランタイム、レイヤー）を作り、smoke test の後に GitHub Release に添付する。
- **GitHub Actions**（`ci.yml`、push to `master` と PR で動く）:
    - `go-tests`（ubuntu / windows）: `go test`、`go vet`、cgo なしのビルド、Python と cgo の混入の検査
    - `layer-tests`（windows）: `playspectra_test` のビルドと CTest（`loader_test` を除く）
    - `layer-build`（ubuntu / windows）: 配布用の `playspectra_layer` のビルド
    - `monado-build`（ubuntu / windows）: Monado と devicecore のビルド、stage-install、`monado-cli probe` の smoke
    - live の E2E は CI に含まれない

## 5. よく使うコマンド

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

## 6. 指示と文書の書き方

- 規則は「取る行動 → 理由 → 起こりうる状況と、それぞれの行動」の順で書く。短く書くと「X する。理由は Y。Z が起こりうるので、W の場合は V する」。
- 禁止は、同じ目的を果たす行動と、その理由と、場合ごとの行動で表す。例: 「推測を書かない」ではなく、「主張は実測か一次ソースを指せるときだけ事実として書く。推測が実測と真逆だったことがあるため。指せないときは仮説ラベルと検証方法を付ける」。
- 書き直した規則は、古い禁止が防いでいた状況で読み手が何をするかを問うて確かめる。
- 行動、理由、条件のどれも持たない文は削る。`AGENTS.md` のすべての文は毎回のセッションで読まれるためです。常に要る規則だけを `AGENTS.md` に置き、特定の作業でだけ要る規則は `docs/development/` に置いて、`AGENTS.md` 第 7 節の表から参照する。
- 利用者向けの文書は ✅ / 🟡 / 📋 の 3 段階で検証境界を示し、「できること」と「できる予定」を混ぜない。
- これらの規則は指示に適用する。製品についての事実の記述（機能の説明、検証の記録、仕様）は事実の記述のままにする。
