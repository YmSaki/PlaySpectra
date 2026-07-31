# Go 移植のコード互換性計画

Issue #10 は移植する範囲を定義するために使い、移植の完了判定には使わない。移植対象は
Server／MCP／Recorder の3本だけではなく、**このリポジトリが所有する全Pythonコード**である。
完了度は、各Python実装から観測できる振る舞いとの互換性で判定する。

比較元は、Go 移植開始時点の `feat/reimplementation-go` の基点
`65d33725bb010c5139d46dd7fc2189ffd184b089` にある全Pythonコードとする。Python 側に修正を
加える場合は、参照基点と characterization test を同じ変更で更新する。移植中に追加した
`tools/playspectra_go_parity_test.py` も、最終的にはGo test／fixtureへ置換して削除する。

## Pythonコード全量inventory

基点commitでトップレベルリポジトリが所有する `.py` は次の14本である。移植作業用の
`playspectra_go_parity_test.py` を含めると、現在のworking treeには15本ある。

| Python source | 担当する振る舞い | Go移植先 | 現在 |
| --- | --- | --- | --- |
| `tools/playspectra_server.py` | NDJSON client、Core、state、操作、Scenario、assert、capture、CLI、self-verify | `protocol/`、`playspectra/`、`cmd/playspectra` | 部分移植 |
| `tools/playspectra_record.py` | Recorder、Replayer、file I/O、CLI、live verify | `playspectra/recording.go`、`playspectra record/replay/verify` | 部分移植 |
| `tools/playspectra_mcp.py` | FastMCP server、13 tools、lazy connection | `mcp/`、`playspectra mcp` | 部分移植 |
| `tools/playspectra_math_test.py` | math characterization 17 cases | Go table-driven unit tests | 一部のみ移植 |
| `tools/playspectra_waitfor_test.py` | mock adapter、wait_for/assert retry/timeout E2E | Go fake adapter integration tests | 未移植 |
| `tools/playspectra_capture_assert_test.py` | mock capture、stable/changed/retry/timeout E2E | Go capture integration tests | 未移植 |
| `tools/playspectra_frame_test.py` | live `frame_synchronized` apply/idempotent/conflict/stale probe | `playspectra verify frame` またはGo E2E binary | 未移植 |
| `tools/playspectra_multiobs_test.py` | observer複数、writer排他、status、haptics broadcast probe | `playspectra verify multiobs` | 未移植 |
| `tools/playspectra_reset_test.py` | live reset、role拒否、builder state、frame history reset probe | `playspectra verify reset` | 未移植 |
| `tools/playspectra_coupling_probe.py` | runtime HMD操作からapp `xrLocateViews`までのcoupling probe | `playspectra verify coupling` | 未移植 |
| `tools/playspectra_png_stats.py` | PNG decode/unfilter、色統計、non-degenerate判定、CLI | Go image package、`playspectra image-stats` | 未移植 |
| `tools/playspectra_vrapp.py` | VRApp process、`[VRTEST]` parser、request/event wait、座標変換 | Go VRApp driver package | 未移植 |
| `tools/playspectra_vrapp_test.py` | Godot real-app 24-check E2E、capture/pacing/interaction | `playspectra verify vrapp` | 未移植 |
| `tools/playspectra_mcp_verify.py` | 実MCP clientでserverをspawnし13 toolsを検証 | Go MCP client E2E／`playspectra verify mcp` | 未移植 |
| `tools/playspectra_go_parity_test.py` | 移植中のPython/Go differential smoke | Go golden/differential testへ置換後削除 | 移行専用 |

`.py` 以外にも、リポジトリ所有script内にPython heredocまたはPython実行依存がある。これらも
最終的なPython除去の対象とする。

| Source | Pythonの用途 | 置換方針 |
| --- | --- | --- |
| `scripts/setup_hellovr.sh` | vcxproj変換、artifact配備 | Go setup helperまたはshellの決定的処理 |
| `scripts/setup_monado.sh` | ZIPの選択展開 | Go setup helper |
| `scripts/setup_helloxr_msvc.sh` | hello_xr source patch | Go patch helper、またはrepository patch file適用 |
| `scripts/setup_opencomposite.sh` | PE/x64検証 | Go PE parser/helper |
| `scripts/e2e_playwright_loop.sh` | port wait、capture hash集計・判定、Python Server起動 | Go CLI／verify command |
| `scripts/run_*`、`scripts/run_all_tests.sh` | 上記 `.py` の起動 | 対応Go command／Go testへ切替 |
| README／docsのPython実行例 | 利用者向け手順 | Go binaryの例へ切替 |
| `tools/requirements.txt` | FastMCP client/server dependency | Go移植完了後に削除 |

`runtime/monado-playspectra` は別Gitリポジトリを指すsubmoduleであり、その中には上流Monadoの
build generator等のPythonが28本ある。トップレベルリポジトリはそれらのファイルを所有せず、
C++ Runtime Adapterの外部ソース境界なので、この計画のGo control-plane移植には含めない。
トップレベルからsubmodule内のPythonを新たに呼び出す依存は増やさない。最終条件で
「submoduleを展開したtreeにもPythonが1本も存在しない」ことまで要求する場合は、Monado forkの
build systemを含む別移植になるため、control-plane移植とは分けて管理する。

## 目標とする配布単位

目標は物理ファイルを厳密に2個にすることではなく、利用者が導入する実装系を次の2配布単位に
分けることである。

1. **C++ native bundle**
   - Monado Runtime Adapter
   - 仮想HMD／左右コントローラー
   - OpenXR Instrumentation Layer
   - D3D11／D3D12／Vulkan capture
   - Runtime／service executable、DLL／SO、OpenXR manifest、runtime JSONなど、loaderやOSが
     必要とする複数のnative artifactを含んでよい
2. **単一のGo executable (`playspectra`)**
   - CLI／MCP／JSON Scenario
   - 高水準操作と補間
   - `VirtualDeviceState`管理
   - state／capture assertionと`wait_for`
   - Recorder／Replayer
   - session／process management、doctor

Go executableはC++を直接linkせず、cgoも使わない。両配布単位の境界は現在のNDJSON/TCP
（operate `:52702`、capture `:52700`）を維持する。したがってC++側のABIやDLL構成が変わっても、
Go binaryを再linkする必要はない。

Python test／probeの移植先が必ず本番subcommandである必要はない。利用者が使う機能やlive
diagnosticは既存の`playspectra`内のsubcommandにし、開発時だけ必要なfixture／unit testは
`*_test.go`へ移す。Python除去のために第3の配布binaryを増やさない。

このIssueでは、配布境界を保った実装とローカルbuild可能性までを扱う。次は**後続Issue**の範囲とする。

- Windows／Linux release artifactの生成・収集
- C++ native bundleのplatform別packaging
- checksum、署名、version metadata
- GitHub Actions等からのartifact upload／Release公開
- 公開された配布物をPython／Node.jsなしのclean環境へ導入するrelease smoke

互換性を継続確認する通常のunit／integration test CIは本Issueの範囲に残す。後続へ送るのは
「配布するためのCI」であり、testを自動実行するCIではない。

## 互換性の判定方法

正常な入力について、以下を比較する。

- 受理される command／scenario step／MCP tool と引数
- インターフェースごとの既定値
- NDJSON request の順序、command、role、state、および request/response の対応
- 補間中に送信される全 `set_state` frame と最終 `VirtualDeviceState`
- `sequence` の開始値、単調増加、reset／replay 後の継続
- 戻り値、Scenario summary、CLI の stdout／stderr、終了コード
- MCP `tools/list` schema、tool result の content type、JSON-RPC error
- 記録ファイルの意味内容と replay で送信される frame

JSON object の key 順と空白、wall-clock timestamp、一時ファイルの絶対 path はそのまま比較しない。
浮動小数点は演算結果だけに明示した tolerance を使う。`sequence`、既定値、frame 数、JSON の
missing／`null`、値の型、終了コードは非決定値として除外しない。`request_id` の文字列そのものは
自由とするが、一意性、response との対応、frame との順序は比較する。

より厳密な型検証、無効入力の拒否、精度向上、安全上の上限、Go 専用機能は許容する。ただし、
Python が受理する正常入力の結果を変えないことを compatibility test で確認し、差分を
「意図的改善／拡張」として個別に固定する。正常入力の結果が変わる差分を、単に「改善」として
除外しない。

## 現在地

2026-07-31 時点では、control-plane本体の機能骨格は一通り存在するが、全Pythonコード移植としては
**未完了**、本体のコード互換性は **部分確認** である。

| 対象 | 実装 | 現在の互換性エビデンス | 判定 |
| --- | --- | --- | --- |
| NDJSON/TCP client | あり | request ID 対応と event skip の Go unit test、fake TCP integration | 部分確認 |
| `VirtualDeviceState` | あり | default/full snapshot とnon-default adapter seedの Go unit test | 部分確認 |
| hello/get_state/set_state/reset | あり | `get-state` の Python/Go semantic comparison、reset後sequence継続test | 部分確認 |
| 高水準操作と補間 | あり | Go unit test。1 scenario の duration=0 操作を最終状態だけ比較 | 部分確認 |
| JSON Scenario Runner | あり | 1 scenario の summary と最終状態を比較 | 部分確認 |
| assert／wait_for | あり | `near` の assert と wait_for を各1ケース比較 | 部分確認 |
| capture assertion | あり | Go/Python の個別 test はあるが相互比較なし | 未確認 |
| CLI | あり | default/flag mapping、fake adapter経由のJSON・exit・全frame test | 部分確認 |
| MCP frontend | 13 tools 実装 | FastMCP 1.29 schema characterization、13 tool call、error/lazy reuse test | 部分確認 |
| Recorder／Replayer | あり | role、empty/round-trip、timing、fresh sequence、全replay frameのGo test | 部分確認 |
| doctor／session | Go 独自であり | Go unit test と command 実装 | 意図的拡張 |
| protocol live probes | なし | Python frame/multiobs/reset probesだけ | 未移植 |
| runtime coupling probe | なし | Python live probeだけ | 未移植 |
| PNG解析 | なし | Python implementationだけ | 未移植 |
| VRApp driver／24-check E2E | なし | Python implementationだけ | 未移植 |
| MCP実client verifier | なし | Python implementationだけ | 未移植 |
| setup/E2E内のPython heredoc | 残存 | 5 scriptsで使用 | 未移植 |
| Windows／Linux binary | build 定義あり | この監査では release artifact の相互確認なし | 未確認 |

現在の自動チェックは次の状態で通る。

- `go test ./...`: 4 packages、112 tests/subtests
- `go vet ./...`
- `python tools/playspectra_go_parity_test.py`: 1 scenario の summary／最終状態と
  `get-state` CLI JSON

既存 parity test は root の `sequence`、`protocol_version`、`clock` を比較から外し、途中の
`set_state` frame、既定値、MCP、Recorder／Replayer を見ていない。この pass だけでは移植完了と
判定しない。

## コード監査で判明している差分

以下は Issue 文との差ではなく、Python と Go のコードを比較して見つかった項目である。各項目は
実行可能な compatibility test で再現してから、互換修正または意図的改善として確定する。

### 正常系で解消が必要な候補

- ~~Go の `Hello`／`Reset` が state 本体をseedできず、adapterのnon-default stateを失う。~~
  characterization test追加後、`Model.Seed`がreply envelopeとstate本体の両方を受理するよう修正済み。
- ~~Go のresetがadapterのreset後sequenceへ`Seq`を巻き戻す。~~ reset前のwriter-owned sequenceを
  維持し、次frameが単調増加するtestとともに修正済み。
- Scenario/Core の `move_head`／`look` default は Python、Go とも 500 ms だが、Go の
  `playspectra cmd` は 400 ms を使う。Python 旧 CLI は Scenario default の 500 ms を使う。
  なお MCP の default は両方 400 ms であり、インターフェース別に試験する必要がある。
- ~~Go CLIの`move_head`／`look`が400 ms、`--hand`が一律leftだった。~~ interface別defaultの
  characterizationを追加し、CLIはPython CLIと同じ500 ms、操作別left/rightへ修正済み。
- ~~Python の frame 数はties-to-even、Goはhalf-away-from-zeroで`.5` frame境界が異なる。~~
  `math.RoundToEven`と2.5 frameのcharacterization testで修正済み。
- Python の state snapshot は `protocol_version` を含めず、Go は含める。adapter と利用者に
  対する正規の state envelope を確認し、互換修正か明示的な protocol 精密化かを決める。
- request ID の命名と、CLI／Scenario の進捗・エラー文字列は異なる。ID は相関性を、表示は
  stdout／stderr と機械可読 JSON の契約を優先して判定する。

### 意図的改善として確認する候補

- Python の `hand(name)` は `left` 以外を right として扱う。Go は left/right 以外を拒否する。
- Python の `set_input` は値をそのまま保存する。Go は path に応じて number／boolean を検証し、
  axis/value を範囲内にする。正常範囲の数値と bool path の出力型を明示する必要がある。
- Go protocol client は 1 MiB 上限と context timeout を持つ。Python の timeout／EOF 時の
  `None` より明示的な error を返す。
- Go Recorder は不正な `get_state` を error にし、JSON を indent + trailing newline で保存する。
  記録内容は semantic JSON で比較し、破損入力の厳密化は別 test にする。
- Go MCP の `set_input.value` schema は number と boolean を許可する。Python annotation は
  float だが、Core の bool input を型どおり表せる拡張として互換入力を壊さないか確認する。
- `set_state` scenario step、`status`、`set_trigger` alias、doctor、session は Go の追加機能であり、
  Python 互換達成数には含めない。

## TDD の進め方

失敗する test 群を先に積み上げない。下記をチェックリストの1項目、または同じ原因を持つ小さな
table として繰り返し、各作業単位を必ず GREEN で終える。

1. Python の現在の振る舞いを characterization fixture で記録する。
2. 同じ入力を Go に与える compatibility test を1項目追加し、差分を確認する。
3. 正常系互換または承認済みの精密化になる最小実装を行う。
4. 新規 test、既存 Go test、Python/Go differential test をすべて GREEN に戻す。
5. GREEN のまま重複を整理し、次の項目へ進む。

CI やレビューに渡す状態では既知の失敗 test を残さない。まだ実装しないケースは、この文書の未完
checkbox と fixture case inventory に置き、実行 suite に無条件の失敗として追加しない。Pythonの
test harnessをGoへ移すときは、単にファイルを削除せず、Python版の全checkをGo版へ対応付け、
同じfixtureでGREENになってから呼び出し元を切り替える。

## 実装チェックリスト

### 0. Differential harness

- [x] fake NDJSON adapter で Python と Go を別 process 実行できる
- [x] 1 scenario の summary／最終状態を比較できる
- [x] `get-state` CLI の semantic JSON と exit code を比較できる
- [ ] adapter が受信した全 request を保存し、command 列と全 `set_state` frame を比較する
- [ ] adapter の初期 state、reply、event、error、遅延を case ごとに注入できる
- [ ] Python の `time.sleep` と Go sleeper を制御し、wall-clock を待たず frame 列を比較する
- [ ] JSON、float、timestamp、path の normalization rule を helper に一元化する
- [ ] 差分表示に最初の不一致 path、frame index、Python 値、Go 値を出す
- [ ] case を Core／Scenario／CLI／MCP／record-replay ごとの table から実行する

### 1. Protocol と state ownership

- [x] non-default adapter state から hello 後の Model が同じ値に seed される
- [ ] hello の role、protocol version、seed 用 get_state、失敗条件が一致する
- [ ] get_state が state の値と型を失わず返す
- [ ] default state の全 field、input path、numeric/bool type を比較する
- [x] connected／disconnected controller の snapshotを比較し、disconnectedは`connected:false`だけを出す
- [ ] `set_state` の完全 snapshot、sequence、clock、validation を試験する
- [x] reset 後も writer sequence が巻き戻らず、次 frame が stale reject されない
- [x] async event、異なる request ID、空行、分割 packet、複数 reply を試験する
- [x] timeout、EOF、不正 JSON、oversize の互換領域と Go の厳密化領域を分ける

### 2. Math、高水準操作、補間

- [x] `lerp3`、`quat_mul`、`quat_yaw`、`quat_norm`、`slerp` のPython 17 caseをGoへ1対1移植する
- [x] 0 ms、1 frame、複数 frame、`.5` frame 境界の frame 数を比較する
- [x] `move_head`の全frame position、sequence、full snapshotを固定する
- [x] `move_head` の省略引数、position、default duration を比較する
- [ ] `move_head` のorientationと全slerp frameを比較する
- [ ] `look` の正負・0・大角度と default duration を比較する
- [ ] `walk_forward` の clamp、hold frames、release frame、default hand/duration を比較する
- [ ] `strafe` の clamp、hold frames、release frame、default hand/duration を比較する
- [ ] `trigger` の clamp、hold state、default hand/value/duration を比較する
- [ ] `move_controller` の grip/aim 同期、orientation、default hand/duration を比較する
- [ ] `set_input` の全宣言 path、instant/hold、値型、正常範囲を比較する
- [ ] `press` の touch/click press frame、release frame、default hand/button/ms を比較する
- [ ] `wait` の0／正／負 duration の扱いを比較する
- [ ] 無効 hand、button、path、値型は Go の厳密化 test として固定する

### 3. Scenario、assert、capture

- [ ] Python command inventory と各 default を table 化する
- [ ] hello 省略時／明示時、空 steps、連続 scenario の接続・assertion 状態を比較する
- [ ] 各 operation step の戻り値、送信 frame、最終 state を比較する
- [ ] path 解決の object key、slash 入力 key、array index、missing、型違いを比較する
- [ ] `near`、`eq`、`ne`、`gt`、`lt`、`true`、`false` を型境界込みで比較する
- [ ] `assert` の single-shot、retry、名前、summary failure 名を比較する
- [ ] `wait_for` の即時成功、poll 後成功、timeout、poll interval を比較する
- [ ] assertion 0件／全成功／一部失敗の summary と runner exit code を比較する
- [ ] unknown command、不正 step、不正 scenario JSON の error と exit code を比較する
- [ ] screenshot success、channel 不在、not-ok、path 不在、read error を比較する
- [ ] capture reference、stable、changed、missing ref、retry/timeout、summary を比較する

### 4. CLI

- [ ] Python 旧 CLI の入力を新 subcommand に対応付ける互換 matrix を作る
- [x] 全 `cmd` operation のflag mappingとdefaultをtable testで比較する
- [x] `cmd get-state` の `cmd`、`result:null`、`state`、stdout、exit 0 をfake adapterで比較する
- [ ] assert／wait_for／assert_capture の pass=0、false=1、実行 error=2 を比較する
- [ ] Scenario success／assert failure／decode error／接続 error の stdout、stderr、exit を比較する
- [ ] `--demo`、`--verify`、位置引数 scenario を維持するか、新 CLI への対応を文書化する
- [x] kebab-case と snake_case operation alias を試験する
- [ ] stdout は機械可読 JSON のみ、progress/error は stderr という契約を全 command で試験する

### 5. MCP

- [x] Python FastMCP 1.29の`tools/list`をcharacterizeする
- [x] 13 tool の名前、required、type、default、input/output titleを意味比較する
- [ ] 13 tool のdescription全文をfixtureで固定する
- [x] initialize、initialized notification、ping、unknown method、malformed request を試験する
- [x] lazy adapter connection と capture channel 不在時の動作を比較する
- [x] 12 text/result tool の正常 resultと`structuredContent`を比較する
- [x] screenshot の image content、MIME type、base64 data を比較する
- [x] 全 tool の default 呼び出しと明示引数呼び出しを比較する
- [x] Core error、invalid argument、unknown tool の MCP error 表現を比較する
- [ ] adapter transport errorのMCP error表現を比較する
- [x] 複数 request の server reuse、state 継続、sequence を比較する

### 6. Recorder／Replayer

- [x] observer/writer hello の role と失敗条件を比較する
- [x] sample request、欠損 state、frame order、`t_ms` の単調性を比較する
- [ ] 0 duration、短時間、複数 sample の停止条件を比較する
- [x] recording の name、rate_hz、frames と load/save round-trip を semantic JSON で比較する
- [x] 空 recording の replay result を比較する
- [x] replay 前 get_state、fresh sequence、clock、全 set_state frame を比較する
- [x] replay timing は最初の frame を基準にし、順序と相対 interval tolerance を比較する
- [x] replay 後の最終 state と sequence 単調増加を比較する
- [x] replay frameの`protocol_version:1`追加をprotocol精密化として固定する

### 7. Go binary境界と後続Issueへのhandoff

- [ ] doctor と session は Python 互換 suite から分離して Go 固有 test を維持する
- [ ] Go 固有 alias／validation／limit が正常系 compatibility を壊さない
- [ ] `playspectra`が単一main packageからbuildでき、Go側にcgo importがないことを検査する
- [ ] Go executableがC++ libraryを直接linkせず、NDJSON/TCPだけで接続することを検査する
- [ ] 利用者向け機能を`playspectra`のsubcommandに集約し、第3の配布binaryを必要としない
- [ ] C++ native bundleに必要なartifact種別をhandoff資料へ列挙する
- [ ] release build／packaging／upload／clean-install smokeを後続Issueの未完項目として残す

### 8. Python test／probe／utilityの移植

- [x] `playspectra_math_test.py` の17 caseをGo tableへ1対1対応付ける
- [x] wait_for mock adapterの全checkをGo integration testへ移す
- [x] capture mockの全checkをGo integration testへ移す
- [ ] frame_synchronizedの10 checkをGo live probeへ移す
- [ ] multi-observer/writer排他/status/hapticsの全checkをGo live probeへ移す
- [ ] resetの20 checkをGo live probeへ移す
- [ ] coupling probeの2 checkとexit codeをGoへ移す
- [ ] PNG parserの全color type/filter処理をgolden PNG fixtureでcharacterizeする
- [ ] `png_stats` resultの全field、rounding、unsupported/errorをGoへ移す
- [ ] VRAppのprocess lifecycle、line parser、request ID、event waitをfake processでGo testする
- [ ] STAGE↔GLOBAL変換とevent predicateをGo table testへ移す
- [ ] VRApp E2Eのstartup、pose/input、capture、pacing、interaction全checkをGoへ移す
- [ ] MCP verifierをGo MCP clientで移し、13 toolsとscreenshotをlive stackで検証する
- [ ] Server 9-checkとRecorder 5-checkのself-verifyをGo verify commandへ移す
- [ ] Python版とGo版のcheck名対応表を作り、欠落checkをCIで検出する

### 9. Embedded Pythonと起動経路の除去

- [ ] `setup_hellovr.sh` の2 heredocをGo/shellへ置換する
- [ ] `setup_monado.sh` のZIP展開heredocをGoへ置換する
- [ ] `setup_helloxr_msvc.sh` の3 source patch heredocをGo/patch fileへ置換する
- [ ] `setup_opencomposite.sh` のPE検証heredocをGoへ置換する
- [ ] `e2e_playwright_loop.sh` のport wait、Server起動、hash判定をGoへ置換する
- [ ] `run_scenario_e2e_monado.sh` をGo Scenario Runnerへ切り替える
- [ ] `run_hello_xr_monado.sh` をGo coupling probeへ切り替える
- [ ] `run_vrapp_monado.sh` をGo VRApp E2Eへ切り替える
- [ ] `run_mcp_verify_monado.sh` をGo MCP verifierへ切り替える
- [ ] `run_all_tests.sh` とCIからPython suite起動を除き、対応Go suiteを必須化する
- [ ] 対応Go commandがGREENになった時点でREADME／docs／scenario説明の利用例を切り替える
- [ ] READMEには利用方法と安定した構成だけを置き、進捗、未完check、Issue handoff、実行履歴を書かない
- [ ] 移植中だけ使うPython/Go parity fixtureを固定goldenへ変換する
- [ ] 対応Go testがGREENになったPython sourceから順に削除する
- [ ] `tools/requirements.txt` とPython venv手順を削除する
- [ ] `git ls-files '*.py'` が0件であることをCIで検査する
- [ ] owned scriptsにPython shebang、`python`実行、Python heredocがないことをCIで検査する

## 最初の実装順

最初は広い test suite を追加せず、次の順に1項目ずつ GREEN にする。

1. Coreのnon-default初期state seed、sequence、default、補間frameをGREENにする
2. Server/math/wait_for/captureのPython checkをGo unit/integration testへ移す
3. Scenario／assertion／CLIの全正常入力をGREENにする
4. MCP schema、13 tool call、MCP verifierをGoへ移す
5. Recorder／Replayerとself-verifyをGoへ移す
6. frame／multiobs／reset／coupling live probeをGoへ移す
7. PNG解析をGoへ移し、golden PNGでGREENにする
8. VRApp driverと24-check E2EをGoへ移す
9. setup/E2E script内のPythonをGo helperへ移す
10. 呼び出し元とdocsをGoへ切り替え、対応済みPythonを削除する

## 完了条件

- 上記の Python 互換対象 checkbox がすべて自動 test で GREEN
- Python の正常入力に対する未説明の出力差分がない
- 意図的改善／拡張は互換 test と専用 test の両方で境界が固定されている
- 全送信 frame、sequence、summary、CLI exit、MCP schema/result、record/replay が比較対象になっている
- Python test／probeが持つ全checkにGo側の対応checkがある
- PNG解析、VRApp driver/E2E、live protocol probe、setup helperがGoで利用できる
- 通常のWindows／Linux test CIがGREENで、release artifactの生成・公開は行わない
- 後続の配布Issueが「C++ native bundle + Go executable」の2配布単位を構築できるhandoff情報がある
- トップレベルGit管理下の `.py` が0本で、owned scriptにPython実行依存がない
