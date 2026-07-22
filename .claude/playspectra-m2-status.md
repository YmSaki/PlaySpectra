# M2 実装状況 — 最小 Monado Virtual HMD

作成: 2026-07-19 / 更新: 2026-07-20。`playspectra-architecture.md` §8 M2 / `playspectra-device-core-spec.md` に対応。
状態: **M2 の HMD＋Touch コントローラ縦切りが完全 E2E 検証済み**(WSL2 Ubuntu 22.04・in-process headless)。
仮想HMD＋左右Touchコントローラの列挙、OpenXRアプリでの pose/thumbstick/trigger/grip 取得、外部NDJSON
set_state による head/controller 更新まで実行検証済み。残り: 実アプリ(hello_xr/VRDevApp)・haptics 逆方向・
observer 複数接続・SteamVR Adapter。

## M2.1 検証結果 (2026-07-20・WSL2 Ubuntu 22.04.5 / branch feat/playspectra-driver-m2)

Windows では glslang/Vulkan SDK 不在で不可だったが、**既存の WSL2 Ubuntu に依存を導入して実ビルド・実行に成功**。
- **M2.1-a configure**: `cmake -S . -B build -G Ninja` 成功。`Enabled drivers: ... playspectra`、`DRIVER_PLAYSPECTRA: ON`。
- **M2.1-b compile/link**: フル `ninja` が `libdrv_playspectra.a` と `monado-service` をリンク(exit 0・警告なし)。
  → 📋 未コンパイルだった socket/thread/Monado glue が ✅ 実コンパイル・実リンク検証済み。
- **M2.1-c builder**: `Selected playspectra because it was certain it could create a head` / `Using builder playspectra`。
- **M2.1-d device**: `Got devices: 0: 'PlaySpectra HMD' (id=1)`。
- **M2.1-e 属性**: `head: PlaySpectra HMD (PlaySpectra HMD S/N), view count: 2` / `Result: XRT_SUCCESS`。
- **制御チャネル**: `PlaySpectra control channel listening on 127.0.0.1:52702`(M2.3 の土台が起動)。
- **実行時バグ発見→修正(commit 6f41045)**: Linux では blocking accept()/recv() が close() で確実に解除されず
  monado-cli probe が終了時に hang(20s)。→ select 200ms タイムアウトで stop フラグを検知して抜ける方式に修正。
  再ビルド後 **probe が 0 で即終了**することを確認(クリーン停止)。inspection では見えず実行で判明した好例。
- 手順: `wsl -d Ubuntu` → 依存 apt(下記) → `git clone -b feat/playspectra-driver-m2 <local> ~/monado-playspectra`
  → `cmake -B build -G Ninja` → `ninja` → `PLAYSPECTRA_ENABLE=1 monado-cli probe`。
  ※Ubuntu の cmake-data がファイル欠落していて `apt-get install --reinstall cmake-data` が必要だった(環境固有)。

## M2.2 / M2.3 検証結果 (2026-07-20・WSL2 in-process + XR_MND_headless)

コンポジタ/Vulkan 窓を避けるため **in-process(`XRT_FEATURE_SERVICE=OFF`) + `XR_MND_headless` +
`XRT_COMPOSITOR_NULL=1`** で headless セッションを張り、`tools/playspectra_headless_probe.c`
(`XR_KHR_convert_timespec_time` で有効な XrTime を作り `xrLocateSpace(VIEW in STAGE)`)で pose を読む。

- **M2.2 固定 pose 取得** ✅: OpenXR アプリが `lr=0 flags=0xf`(valid+tracked 全ビット)・
  `pos=(0.000, 1.600, 0.000)`(builder の head_center y=1.6)・`quat=(0,0,0,1)` を読取。
  経路: PlaySpectra driver → relation_history → Monado oxr → OpenXR loader → app が疎通。
- **M2.3 set_state E2E** ✅: 実行中に 127.0.0.1:52702 へ NDJSON 送信 → hello 応答
  `{role_granted:writer, descriptor{...}, ok:true}`、set_state 応答 `{applied:true, sequence:1, ok:true}`。
  アプリが読む pose が **(0,1.6,0)×24 → (0,1.6,-1.5)×31 に遷移**。= 外部client→NDJSON set_state→
  proto parse→driver set_pose→relation_history→app の全経路が疎通。**ドライバ変更なし**で達成。
- 途中の詰まり(事実として記録):
  (a) `XR_MND_headless` だけでは Monado が direct-Wayland コンポジタを起動して失敗(drm-lease 無し) →
      `XRT_COMPOSITOR_NULL=1`(target_instance.c の `use_null`)で null コンポジタ選択。
  (b) null でも frame timing が出ず `predictedDisplayTime=0` → frame ループ依存を捨て
      `XR_KHR_convert_timespec_time` で XrTime 生成する方式に変更(pose 読取に frame 不要)。
  (c) WSL が呼び出し間で自動シャットダウンし `/tmp`(tmpfs)が消える → compile+run を1スクリプトに統合・`$HOME` 出力。
- 再現: `tools/playspectra_headless_probe.c` を `gcc -lopenxr_loader` でビルドし、上記 env で実行。
  set_state は `python3` で 52702 へ hello+set_state を送る(scratchpad の wsl_m2_3.sh 参照)。

## 実アプリ試験 (2026-07-21)

「可能な限り多くの方法で試験」の方針で実アプリを試した結果:
- **カスタム OpenXR クライアント2種**(標準ローダ+API 使用・headless): ✅ 全経路疎通済み
  (`tools/playspectra_{headless,action}_probe.c`。M2.2/M2.3/コントローラ入力)。これらが「任意の
  OpenXR アプリで動く」ことの実証になっている(XR_MND_headless で Vulkan を回避)。
- **hello_xr(標準サンプル・第三者アプリ)**: WSL でビルド成功、**PlaySpectra Monado の XR instance 生成には
  成功**(`Monado(XRT) ... RuntimeVersion=25.1.0 '1e5133360'`=本ビルド)。だが**自前の Vulkan graphics 生成
  (lavapipe ソフトウェア Vulkan + MESA device_select)でスタックし xrCreateSession に到達せず** → PlaySpectra の
  device/入力経路までは行けなかった。原因は **WSL のソフトウェア Vulkan 環境**(GPU パススルーの dzn ドライバ無し・
  lavapipe が instance/device 生成で hang)であり **PlaySpectra 側ではない**。実 GPU か GPU-accel Vulkan(dzn)が
  ある環境なら hello_xr は session まで進み PlaySpectra を叩けるはず(instance 接続は既に成功しているため)。
- **VRDevApp.exe**: **Windows/D3D12 専用**で WSL Linux では実行不可。Windows 版 PlaySpectra Monado
  (glslang/Vulkan SDK が要る)か、Windows の OpenXR ランタイム経由が必要。未実施。
- 結論: PlaySpectra の入力アダプタ自体は OpenXR 標準経路で完全検証済み。実 GPU アプリ(hello_xr レンダリング・
  VRDevApp)の試験は「実 GPU が使える Vulkan 環境」の確保が前提条件(次の課題)。

## (参考) 旧・Windows での検証境界(なぜ WSL に移したか)

- Monado の CMake configure は MSVC 2022 まで到達するが **`glslangValidator` 不足**で停止
  (`cmake/SPIR-V.cmake:8`)。フルビルドには glslang(+ Vulkan SDK 等)が必要。
  → PoC が prebuilt CI artifact を使ったのと同じ理由。**コンパイル/実機検証はビルド環境依存**。
- 追加した CMake 編集は configure を**同一地点(glslang)で停止**させる = 早期解析部
  (MonadoSetOptions.cmake の option リスト)に構文エラーを混入していないことを確認済み。
- driver 固有 CMake(drivers/CMakeLists・targets/common・root の option/AVAILABLE_DRIVERS)は
  glslang 停止より後に解析されるため、この環境では inspection 検証のみ。
- 使用した Monado API の実在は grep で確認済み: `os_thread_helper_{init,start,is_running,lock,
  unlock,stop_and_wait,destroy}` / `u_json.h`(→ `<cjson/cJSON.h>` を export) / `cJSON_*` /
  `U_LOG_IFL_I|E(level,...)` / `m_relation_history_*` / `u_device_*` / `xrt_device` 構造。
- **NDJSON パース層(playspectra_proto)は実コンパイル・実行検証済み(inspection でなく)**: 同梱 cJSON +
  ローカル gcc(`-Wall -Wextra` 警告なし)で `playspectra_proto_test.c` をビルド・実行 → **20/20 PASS**。
  検証: 完全 set_state パース / sequence 有無 / hmd 無し・connected=false → head 非適用 / position 欠落・
  配列長不正 → validation_error / relation_flags 既定 true・明示 false / 非 object state /
  hello protocol_version 一致・不一致。socket/thread/Monado glue のみ未コンパイル。
  再現: `gcc -I src/external/cjson -I src/xrt/drivers/playspectra
  src/xrt/drivers/playspectra/playspectra_proto{_test,}.c src/external/cjson/cjson/cJSON.c -o t -lm && ./t`

## 追加/変更したファイル(submodule 内・branch feat/playspectra-driver-m2 にコミット済み: 325fe40 proto / 1e51333 wire / 6f41045 fix+M2.1検証)

新規 driver:
- `src/xrt/drivers/playspectra/playspectra_interface.h` — create/set_pose/control API 宣言
- `src/xrt/drivers/playspectra/playspectra_hmd.c` — 仮想 HMD(sample_hmd.c 準拠、relation_history
  で pose 保持、control channel を寿命内包)
- `src/xrt/drivers/playspectra/playspectra_proto.{h,c}` — **純粋な NDJSON パース(cJSON のみ・Monado 非依存)**。
  set_state.state → 平文の head pose/flags/sequence。テスタビリティのため control から分離。
- `src/xrt/drivers/playspectra/playspectra_control.c` — NDJSON 制御チャネル(spec §5。socket は
  remote/r_hub.c 同型のクロスプラットフォーム、os_thread_helper、cJSON。パースは proto に委譲し、
  stale 判定と Monado への pose 適用のみ担当)
- `src/xrt/drivers/playspectra/playspectra_proto_test.c` — proto の単体テスト(スタンドアロン実行可)

builder + 配線:
- `src/xrt/targets/common/target_builder_playspectra.c` — head-only builder(PLAYSPECTRA_ENABLE で有効)
- `src/xrt/targets/common/target_builder_interface.h` — T_BUILDER_PLAYSPECTRA + 宣言
- `src/xrt/targets/common/target_lists.c` — builder 配列に高優先で追加
- `src/xrt/targets/common/CMakeLists.txt` — builder source + drv_playspectra link
- `src/xrt/drivers/CMakeLists.txt` — drv_playspectra(WIN32 は ws2_32)
- `CMakeLists.txt` — option XRT_BUILD_DRIVER_PLAYSPECTRA + AVAILABLE_DRIVERS "PLAYSPECTRA" + status
- `cmake/MonadoSetOptions.cmake` — option リストに追加

## 有効化と制御

- ビルド時: `XRT_BUILD_DRIVER_PLAYSPECTRA=ON`(既定 ON)。
- 実行時: `PLAYSPECTRA_ENABLE=1` で builder を有効化(未設定なら列挙されない)。
- 制御チャネル: TCP 127.0.0.1、既定ポート **52702**(env `PLAYSPECTRA_MONADO_PORT`)。
  layer=52700 / SteamVR driver=52701 に続く Monado Adapter=52702。

## 段階別 検証計画(Monado ビルド環境で実施)

前提: glslang + 必要 SDK を入れて `taskfile.yml build:monado`(cmake -S runtime/monado-playspectra
-B build/monado; cmake --build)が通ること。または PoC と同じ prebuilt 方式を使うなら、この driver を
含めて再ビルドした artifact が要る(prebuilt にはこの新 driver は入っていない)。

- **M2.1 列挙**: `PLAYSPECTRA_ENABLE=1` + `monado-service`/`monado-cli info` 等で
  "PlaySpectra HMD" がデバイスとして出る。機械判定: デバイス一覧に playspectra が現れる。
- **M2.2 固定 pose 取得**: 素の `hello_xr -g Vulkan` を PlaySpectra ランタイムで起動し、
  session RUNNING + view が供給される(center = STAGE y=1.6 の頭部)。
- **M2.3 set_state 更新**: 127.0.0.1:52702 に NDJSON で
  `{"cmd":"hello","request_id":"h","protocol_version":1,"role":"writer"}` →
  `{"cmd":"set_state","request_id":"s","state":{"sequence":1,"clock":{"mode":"realtime"},
  "hmd":{"connected":true,"head":{"position":[0,1.6,-1],"orientation":[0,0,0,1],
  "relation_flags":{"position_valid":true,"orientation_valid":true,"position_tracked":true,
  "orientation_tracked":true}}}}}` を送り、hello_xr のビューが動く(head pose 反映)。
- **M2.4 hello_xr headless**: hello_xr を headless で起動し継続動作(framesObserved>0 相当で機械判定。
  stdin EOF 対策は既存 feeder パイプ [[hello-xr-getchar-stdin-eof]] を流用)。
- **M2.5 VRDevApp E2E**: VRDevApp を PlaySpectra ランタイムで起動し、set_state で pose を変えて
  画像が変わることを before/after で確認(コントローラ移動は後続マイルストーン)。

## ビルド環境の確立 (2026-07-19 調査 / 2026-07-20 WSL2 Ubuntu で実施済み・M2.1 成功)

方針: Windows へ直接 Vulkan SDK を入れる前に、再現性の高い **WSL2/Linux** を第一候補にする
(PlaySpectra M2 は headless/CI 利用が目的 → Linux 再現ビルド経路の確立自体が製品要件に沿う)。

**利用可能な環境(事実・read-only 確認済み)**:
- WSL2 に **Ubuntu 22.04.5 LTS**(version 2) が存在(現在 Stopped)。他に rancher-desktop。
- Ubuntu 内: **有** cmake / gcc / g++ / pkg-config / python3 / git。
  **無** ninja / glslangValidator / vulkan・cjson・eigen・SDL2 ヘッダ。HOME=/home/ymsaki 書込可。

**依存の一次ソース**: `runtime/monado-playspectra/.gitlab-ci.yml` の `FDO_DISTRIBUTION_PACKAGES`
(Ubuntu 行)。`task monado:deps` で表示。M2.1 の最初のブロッカーは
`ninja-build glslang-tools libvulkan-dev libeigen3-dev`。

**実行予定コマンド(⚠ システム変更 = apt install は承認前に停止)**:
```bash
# WSL Ubuntu に入る
wsl -d Ubuntu
# [⚠ システム変更・要承認] ビルド依存を導入(A: CI 相当のフルセットで再現性優先)
sudo apt-get update && sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config glslang-tools libvulkan-dev libeigen3-dev \
  libcjson-dev libx11-dev libx11-xcb-dev libxcb-randr0-dev libxrandr-dev libxxf86vm-dev \
  libgl1-mesa-dev libegl1-mesa-dev libglvnd-dev libwayland-dev wayland-protocols \
  libudev-dev libusb-1.0-0-dev libsdl2-dev libbsd-dev
# WSL ネイティブ fs へローカルブランチを clone(高速ビルド。commit は未 push なのでローカルパスから)
git clone -b feat/playspectra-driver-m2 \
  /mnt/c/programing/Arcadia-Metaverses-dev/CluadeDevelopment-VR-App/runtime/monado-playspectra \
  ~/monado-playspectra
cd ~/monado-playspectra
# configure(PlaySpectra driver ON)。不足依存が出たら CI フルセットから1つずつ足す(事実として記録)
cmake -S . -B build -G Ninja -DXRT_BUILD_DRIVER_PLAYSPECTRA=ON
# build
cmake --build build --parallel
```

**M2.1 の到達判定(この段階では TCP/set_state/controller/VRDevApp は不要)**:
- M2.1-a configure 成功 / M2.1-b `drv_playspectra` + runtime の compile・link 成功
- M2.1-c 起動時に builder(`PLAYSPECTRA_ENABLE=1`)が呼ばれる
- M2.1-d Virtual HMD が xrt_device として1台生成 / M2.1-e name・serial・device type・初期 pose をログ確認
  (例: `PLAYSPECTRA_ENABLE=1 XRT_LOG=debug` で monado-service/monado-cli info)

## コントローラ対応の進捗と後続

- ✅ **proto 拡張済み(commit 2c60686)**: `playspectra_proto` が left/right(grip/aim + semantic-path
  inputs)をパース。`playspectra_proto_test` 29/29・drv_playspectra は Monado で再コンパイル確認済み。
  共通 `playspectra_pose` を head/grip/aim で共用。control.c は head.pose.* に追従。
- ✅ **共有状態リファクタ済み(commit b57dddc・Stage 1)**: `playspectra_state`(refcount+mutex)を新設。
  制御チャネルが書き、各デバイスが読む。制御スレッドは xrt_device を触らないので破棄順の寿命ハザードを排除。
  builder が state/hmd/control を ref 配線、最初の device 破棄で control を1度だけ停止(take_control)。
  HMD は relation_history を廃し state から head を読む。**M2.3 回帰 一致で検証済み**。
- 🟡 **決定(2026-07-20)**: コントローラは **Touch 相当のフルセット**。push は当面しない(ローカル継続)。
- ✅ **Stage 2 実装済み(commit ce8c70f)**: `playspectra_controller.c`(XRT_DEVICE_TOUCH_CONTROLLER 左右)、
  共有 state に per-hand `playspectra_ctrl`(grip/aim relation + trigger/squeeze/thumbstick + primary/secondary
  ボタン等)、builder が left/right 生成(estimate.certain.left/right)、control.c が parsed.left/right の
  semantic-path を共有 state へマップ(x/a→primary, y/b→secondary)。
  **検証(WSL2 in-process)**: フル ninja link・monado-cli probe が **3デバイス列挙**
  (head/left/right ロール割当)・**HMD M2.3 回帰一致**・controller set_state(grip/aim+thumbstick+trigger+button)
  が applied:true でクラッシュなし。
  ✅ **入力 E2E 検証済み(2026-07-20・tools/playspectra_action_probe.c)**: OpenXR action set を
  `/interaction_profiles/oculus/touch_controller` にバインドし、frame loop で session を **FOCUSED まで進めて**
  (IDLE→READY→SYNCHRONIZED→VISIBLE→FOCUSED。null compositor で frame pump は正常動作)、
  `xrGetActionStateVector2f/Float`＋`xrLocateSpace(grip)` で読取。**set_state 前 thumb(0,0)/trig(0)/grip(-0.2,1.3,-0.5)
  → 後 thumb(0,1.0)/trig(0.75)/grip(-0.3,1.2,-0.6)**（送った値が反映・action active=1）。
  = 外部 set_state → proto → apply_ctrl(semantic-path) → controller device → Monado action → app の全経路疎通。
  ※以前 head client で frame loop が動かないと見えたのは locate 手順の問題で、frame pump 自体は機能する
    (convert_timespec は必須でなく代替手段だった)。
- 📋 **(参考) Stage 2 設計メモ**:
  - **device**: `XRT_DEVICE_TOUCH_CONTROLLER` を left/right で生成(simulated_controller.c 準拠)。
    `inputs[]` に Touch 入力名を並べ、`update_inputs` が共有 state から値(vec1/vec2/boolean)を書き、
    `get_tracked_pose` が grip/aim(`XRT_INPUT_TOUCH_GRIP_POSE`/`AIM_POSE`)を共有 state から返す。
    出力: `XRT_OUTPUT_NAME_TOUCH_HAPTIC`。profile は `/interaction_profiles/oculus/touch_controller` へネイティブ束縛。
  - **左 Touch 入力**: X/Y click+touch, MENU_CLICK, SQUEEZE_VALUE, TRIGGER value+touch+proximity,
    THUMBSTICK +click+touch, THUMBREST_TOUCH, THUMB_PROXIMITY, GRIP_POSE, AIM_POSE。
    **右**: A/B click+touch, SYSTEM_CLICK, ほか同じ(trigger/thumbstick/squeeze/grip/aim)。
  - **共有 state 追加**: 各手 {connected, grip relation, aim relation, trigger, squeeze, thumbstick vec2,
    ボタン(x/y or a/b)+touch, menu/system, thumbstick click+touch, thumbrest touch}。mutex 保護・getter/setter。
  - **control.c**: `parsed.left/right`(semantic path)を共有 state の各フィールドへマップ(/input/trigger/value→trigger 等)。
  - **builder**: left/right を生成し tbrh->left/right + xsysd に追加、estimate.certain.left/right=true。
  - **検証**: headless client を拡張し action(XR_ACTION) で thumbstick/trigger を読む、または grip/aim space を
    locate。set_state で値が反映されることを E2E 確認。
  - value union: `struct xrt_vec1 vec1 / xrt_vec2 vec2 / bool boolean`(xrt_defines.h:1537)。
  3. **builder を left/right 生成に拡張**(estimate.certain.left/right)。
  4. **control.c で left/right を共有状態へ適用**(parsed.left/right は既に解析済み)。
  5. **headless client 拡張**で aim/grip space + action(thumbstick/trigger)を読み、set_state で検証。
- fov/解像度/refresh は sample 相当の暫定値。実値は Descriptor へ実測反映(spec §7)。
- ✅ **haptics 逆方向 実装済み(commit 77df429)**: controller の set_output → 共有 state の haptic リング →
  制御チャネルが `{"event":"haptics",hand,frequency,amplitude,duration_ns}` を接続中 writer へ送出。
  wait_readable を timeout 返し(1/2/0/-1)に変更し serve ループで drain。
  **E2E 検証**: action probe が FOCUSED で xrApplyHapticFeedback → observer が haptics イベント受信。
- ✅ **observer 複数接続 実装済み・完全E2E検証(commit b6da9f1c / submodule)**: 制御チャネルを単一接続
  逐次 accept から **単一スレッド select() 多重化**へ書き換え(spec §5.3/§5.4)。
  - hello.role で writer(同時1接続排他・2人目は `writer_taken`) / observer(複数可) を割当。
  - set_state は writer のみ(observer/hello前は `not_writer`)。
  - **get_state 新規実装**: 現在の VirtualDeviceState 全体(head + left/right grip/aim + semantic-path
    inputs)を返す。observer が状態を読めるように(§5.3 で observer の主機能)。
  - haptics は接続中の**全**ハンドシェイク済み接続へ broadcast(リングから1度 pop→全 conn へ fan-out)。
  - status は writer_connected + observer 数を報告。
  - **E2E(WSL2 in-process, action_probe ホスト + python マルチクライアント)= 11/11 PASS**:
    obs×2 hello / 2人目writer→writer_taken / observer set_state→not_writer / writer set_state applied /
    observer get_state が writer の set_state を反映(head z 0→-2.5) / status writer_connected+observers=2 /
    **haptics broadcast が obs1/obs2/writer 全員へ**(driver ログ "broadcast haptics to 3 conns")。
  - **haptics タイミングの知見(実測)**: broadcast はリングから pop して即破棄するので、one-shot haptic は
    observer の hello 完了前に fire すると取りこぼす(初回 E2E で 0 受信の原因はこれ。set_output 自体は
    XRT_LOG=debug の DIAG で到達確認済み)。検証ツール(action_probe)を **周期発火**(15 read ごと)に改善し
    決定論化(parent commit 43f46bb2e)。機構は当初から正しく、one-shot のタイミングだけが不安定だった。
- ✅ **reset コマンド 実装済み・E2E検証(commit 49010dbdf submodule / 7a6f5c89a parent harness)**:
  spec §5.4 reset(writer限定)を実装。**質問1(初期化セマンティクス)は spec 精読で機械的に解決**:
  §5.3 は reset を「writer切断時の"保持"の反対の"初期化"」と定義しており、「全device切断」は初期化でなく
  shutdown で語義矛盾(ドライバは非接続状態で起動しないため戻る先が無い)→ **builder起動時状態への復元**のみが整合。
  - state: `capture_initial()` が builder の初期 pose 書込直後(制御ch start 前=client 未接続で上書き不能)に
    head+両手をスナップショット。`reset()` がそれをコピーで戻す。**仮定最小**(既定値ハードコードでなく実起動状態を復元)。
    haptic キュー(アプリ→デバイスの in-flight)は触れない。
  - builder: 両コントローラ生成後・control_start 前に `playspectra_state_capture_initial(state)`。
  - control: `handle_reset()`(writer限定・非writerは `not_writer`)が reset+frame重複判定クリア(has_frame/
    last_frame/frame_sig)。frame記録は Adapter ローカル(§4)なので新writerが logical_frame を振り直しても
    直前frameと stale/conflict にならない。**sequence は Server所有の単調増加(§4)なので Adapter からは巻き戻さない**。
  - **E2E(WSL2 in-process, :52702)= 20/20**(tools/playspectra_reset_test.py): baseline=builder既定
    (head z=0/左grip x=-0.2/右grip x=0.2/入力0)→ set_state で head+左手移動 → writer reset で全復元 →
    observer も復元状態を観測 → observer/非writer reset→not_writer → reset後の古frame5(<200)が stale でなく適用。
  - **回帰**: frame_synchronized 10/10・observer-multi 11/11。インクリメンタルビルド clean・.so 再リンク。
- ✅ **submodule gitlink 登録済(2026-07-21・親commit 711f5faaf)**: ユーザー判断で登録。従来は .gitmodules +
    親.git/config だけ半設定で gitlink/​.gitmodules 未コミット(親から runtime/ は ?? untracked)。→ 親 HEAD に
    **mode 160000 gitlink @ 49010dbdf** + .gitmodules(path+url) をコミット・push。`git submodule status` 認識・
    49010dbdf は submodule remote origin/feat/playspectra-driver-m2 上にあり新規 clone の `submodule update --init`
    で実取得可能を機械検証。(注: .gitmodules に branch 行は無し=gitlink のピン commit が真実源。`--remote` 追従が
    要るなら後で branch=feat/playspectra-driver-m2 を足す。)
- **質問リスト(要ユーザー判断)**:
  - **(残)次の主軸の方向性**: north star のもう半分「画面観察(利き目カラー画像取得)」が Monado アダプタに未実装。
    north star 優先順位としては (a) 画面キャプチャが核(SteamVR=2つ目 runtime の breadth、Server=観察対象が先に要る
    上位層でどちらも後)。**ただし本環境での実行可能性が分かれる(2026-07-21 整理)**:
    - **(a) Monado 画面キャプチャ**: 実装フックは事実確定(null_compositor layer_commit で readback)。
      **★重大更新(2026-07-21・実測): null compositor は WSL2 lavapipe(llvmpipe LLVM15,CPU) で Vulkan デバイス生成に成功**。
      既存 action_probe ホストを XRT_COMPOSITOR_LOG=debug で起動したログで確認: select_physical_device→"Selected GPU:0
      llvmpipe"、必要デバイス拡張(dedicated_allocation/external_fence/external_memory/**external_memory_fd**/
      external_semaphore/get_memory_requirements2)が全て supported、hang せず完走。→ 旧「WSL2 では graphics 不可(hello_xr
      hang)」は**コンポジタ Vulkan に関しては誤り**。キャプチャゲートは想定より開いている。**残る唯一の未検証=アプリ側が
      graphics binding+swapchain+描画で session を張れるか**(hello_xr の hang はアプリ固有=MESA device_select 等の可能性)。
      **★★決定的ブレークスルー(2026-07-21・hello_xr Vulkan2 実測): 本環境で完全な graphics session 確立に成功**。
      `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`(lavapipe 単体強制)で hello_xr -g Vulkan2 を
      PlaySpectra Monado に対し実行 → llvmpipe で GRAPHICS queue 付き VkDevice 生成成功・xrCreateVulkanInstanceKHR/
      xrGetVulkanGraphicsDeviceKHR 成功・`System Properties: Name=Monado: PlaySpectra HMD`(我々の driver)・
      **view0/1 の color+depth swapchain 生成(320x240)**・セッション **IDLE→READY→SYNCHRONIZED→VISIBLE→FOCUSED**・
      **rc=0 クリーン終了**(hang でもクラッシュでもない)。前回 hang は複数 ICD の device_select 由来で lavapipe 単体強制で解消。
      → **「キャプチャは Windows/実GPU 必須」は end-to-end 動作で決定的に否定**。**キャプチャは本 WSL2 環境で実装も検証も可能**。
      再現: scratchpad/ps_helloxr_run.sh。
      **★★★重大なアーキテクチャ是正(2026-07-21・実装前の俯瞰で発見)**: キャプチャを null_compositor に実装しかけたが、
      **PlaySpectra OpenXR Layer に既に完全なキャプチャ実装が存在**する。`layer/src/capture.{cpp,h}`・`capture_common.cpp`・
      `hooks_capture.cpp`・**`capture_{vulkan,d3d11,d3d12}.cpp`(D3D11/D3D12/Vulkan 全対応=north-star 必須)**・`pixel_convert.cpp`。
      capture.h より: xrEndFrame で利き目 projection subimage を PNG 化・制御チャネルから `CaptureRequestScreenshot(eye,timeout,
      withDepth)`・recording モードあり。D3D11 は Windows で実測検証済み(native-win-interop.md の keyed mutex 処理)。
      → **null compositor 実装は冗長・アーキ違反(Layer=Instrumentation 軸)・Monado 専用の三重の誤りだった。中止。**
      **capture 軸の実際の作業=「再実装」でなく「既存 Layer キャプチャ(特に Vulkan)の E2E 検証/完成」**。
      **feasibility の要点**: Layer は現在 `capture_{vulkan,d3d11,d3d12}.cpp` を無条件コンパイル+d3d11/d3d12 リンク=**Windows 専用ビルド**。
      hello_xr で WSL2 の Vulkan graphics session が動くことは証明済みなので、**Layer を D3D バックエンド WIN32 条件化した
      Linux/Vulkan-only ビルドへポート → hello_xr にレイヤをロード → Vulkan キャプチャ PNG を検証**、が本環境で verifiable な機械的作業。
      **Linux 移植の深さ実測(2026-07-21)=浅くない**: (i) `control_channel.cpp` は winsock 専用・preprocessor ガード 0
      (WSAStartup/SOCKET/closesocket 全面)→POSIX ソケット移植要。(ii) `capture.cpp` は windows.h/d3d11.h/d3d12.h 無条件 include
      +D3D dispatch inline+Vulkan エントリを LoadLibraryA で読む(Linux は dlopen)→全 D3D 参照 #ifdef _WIN32 化要。
      (iii) dxgi_formats.h・CMake 条件化・Linux manifest(.so)。
      **★誤前提の是正(2026-07-21・capture_vulkan.cpp 精読)**: 旧「capture_vulkan は win32 external-memory 前提の疑い=Linux は
      別コード経路」は**誤り**。capture_vulkan の readback は**完全に標準 Vulkan**(vkCmdCopyImageToBuffer・host-visible staging・
      vkMapMemory・image barrier・MSAA resolve・fence)で **win32 external-memory も HANDLE も不使用**(同一プロセス/同一デバイスの
      VkImage を標準コピー)。唯一の Windows 依存は `LoadLibraryA("vulkan-1.dll")`+GetProcAddress の動的ロード ~10行シムだけ
      (Linux は dlopen("libvulkan.so.1")+dlsym)。→ **Linux 検証は本番と同じ readback ロジックを検証する**(別経路でない)。
      **★★★capture 本環境検証 達成(2026-07-21・親commit dadfb4e19 push済)**: Layer を Linux(Vulkan-only)へ移植し、
      **hello_xr の描画フレームから 394 枚の正当な PNG(320x240=swapchain 一致・PNG署名OK)を本番と同じ Vulkan readback で生成**。
      ＝「capture は Windows/実GPU 必須」を end-to-end で決定的に否定。north-star のもう半分(観察)が本環境で verifiable に成立。
      移植(全て #ifdef _WIN32 ガード・Windows パスは verbatim 保存=Windows ビルド不変): CMake(d3d/winsock を WIN32 条件化・POSIX は
      Threads+dl)、control_channel.cpp(winsock→POSIX shim)、capture.cpp(D3D dispatch/include/swapchain-image ガード・localtime_s→
      localtime_r・_mkdir→mkdir・**PLAYSPECTRA_CAPTURE_TEST=<N> で control_channel 無しに auto-record する headless 検証フック追加**)、
      capture_vulkan.cpp(LoadLibraryA→dlopen)、layer_entry.cpp(__declspec→visibility)。Linux ビルドは g++11 で clean。
      再現: scratchpad/ps_layer_build.sh + ps_capture_verify.sh。
      **PlaySpectra は本環境で両輪成立: 操作(Server/Monado)+観察(Layer capture)=Playwright コアループ完成**。
    - **★★★end-to-end 統合検証 達成(2026-07-21・親commit 53edc57aa push済)**: 操作→観察の全ループを WSL2 で実証。
      Server が big_view_change シナリオ(head 150°回転+移動・reset無しで保持)を Monado(:52702)へ注入 → hello_xr が新視点で
      再レンダー → Layer capture の PNG が**変化**(baseline 573B/hash c3ba9140 ↔ post 別hash 3a815fab・30frame中4種内容)。
      **2/2 PASS**: capture が状態追従(≥2 distinct)・post≠baseline。=capture が固定/空バッファでなく実レンダリング状態を反映すること、
      および操作+観察が end-to-end で合成することを実証。**「VR版Playwright」コアループ(入力注入→アプリ再描画→変化を capture)が
      本環境で完全 verifiable**。再現: scripts/e2e_playwright_loop.sh + tools/scenarios/big_view_change.json。
    - **on-demand screenshot プロトコル Linux 検証✅(2026-07-21)**: env auto-record(テストフック)でなく**本番 capture API**を検証。
      hello_xr+layer で layer 制御ch :52700(POSIX 化)へ `status`/`screenshot` 送信 → **6/6 PASS**(status ok+capture api=Vulkan /
      screenshot ok+path 返却 / PNG 320x240 署名OK)。control_channel.cpp の winsock→POSIX 移植が end-to-end で機能。再現: scratchpad/ps_screenshot_verify.sh。
    - **シナリオ assert = テストフレームワーク化✅(2026-07-21・親commit d38369937 push済)**: Server に `assert` コマンド追加。
      get_state のフィールドをキー/添字パス(slash 入りinput キー・配列添字対応)で読み op(near/eq/ne/gt/lt/true/false)判定、
      run_scenario が集計サマリを返し main() は失敗時 exit 1。=operate+observe+**assert** の Playwright モデルを Runtime adapter 上で実現。
      **検証**: assert_demo 5/5 PASS rc=0(baseline z~0/move→z=-1.5/yaw90°→quat.y=0.707/trigger 0.8保持/reset→z~0)+
      **negative control(z=+9 期待・実-1)が FAIL rc=1**(不一致で確実に失敗=false-pass でない)。tools/scenarios/assert_demo.json。
      **残候補**: capture 差分を assert ステップ化(:52700 と結合した capture-assert) / D3D は Windows 検証済み / MCP・CLI 操作IF /
      recording manifest 活用 / Server のコマンド拡充(controller grip pose 直接移動 等)。
    - **★MCP サーバは既存(2026-07-21 メタ認知で発見・再実装回避)**: `mcp/`(tracked・TS・name "playspectra"・@modelcontextprotocol SDK・
      dist/ ビルド済)。tools=head/input/pose/observe/recording、`client.ts` は **layer :52700 に接続**(PLAYSPECTRA_PORT 既定 52700)。
      = MCP 軸は「新規実装」でなく「**既存 MCP を新アーキと整合**」。
      **方針確定(2026-07-21・ユーザー「同一 MCP で両方扱えないのか」)**: **一つの MCP で両方**扱う(2つの MCP に分けない)。
      注: layer :52700 は現状 inject+observe+capture を1チャネルで全部持つ(control_channel.cpp Handle_*)ので、既存 MCP は既に一つで全部扱えている。
      整合方針=**(B) 一つの MCP が2バックエンドへ fan-out**: 注入/状態観測 → Monado :52702(driver・本セッションで注入→再描画→capture 変化を
      end-to-end 検証済み)、画面 capture → layer :52700(screenshot/recording・6/6 検証済)。これで [[layer-observe-driver-inject]] とも整合。
      **これはブロッキング質問でなく実装詳細**(過剰問題化を是正)。
    - **★MCP サーバ(Python・Server ラップ)実装+E2E検証✅(2026-07-21・branch feat/mcp-server・push済)**: ユーザー axis (a)
      「Server を MCP で公開しAIが operate/observe/assert」を実現。既存 TS MCP(mcp/ は layer :52700 直結=旧設計)を拡張せず、
      **`tools/playspectra_mcp.py`(FastMCP)が playspectra_server.Server をラップ**(operate/observe/assert/capture ロジック再利用・
      (B)方針 operate→:52702/capture→:52700)。tools: move_head/look/walk_forward/press/set_trigger/reset(operate)、get_state(observe)、
      **screenshot→MCP Image(AI が画面を見る)**、run_scenario(operate+assert)。Server は初回ツール呼で lazy 接続。
      **検証(実 MCP クライアント stdio・hello_xr+Monado+layer)= 7/7**: 9ツール列挙・move_head→z=-1.5・look90°→quat.y=0.707・
      get_state 反映・screenshot が image 返却・run_scenario assert 通過・reset→z~0。`tools/playspectra_mcp_verify.py`。要 `pip install mcp`(mcp-1.28.1)。
      = AI エージェントが VR アプリを operate+observe(状態+画面)+assert できる「エージェント向け VR Playwright」成立。
      **branch**: feat/mcp-server(refactor/playspectra-rename-m05 off)。PR 化するなら →refactor branch(MCP 差分のみ)か PR#7 マージ後→master。
    - **capture-assert = 視覚回帰テスト✅(2026-07-21・親commit 6d985c2d7)**: シナリオが device 状態だけでなく**画面**を assert 可能に。
    - **PR 作成済(2026-07-21・ユーザー依頼)**: 親 #7(YmSaki/VR-MCP refactor/playspectra-rename-m05→master・12コミット)、
      submodule #1(YmSaki/monado-playspectra feat/playspectra-driver-m2→main・11コミット)。相互リンク・検証マトリクス記載。
      **注(branch 戦略)**: cron 指示「以降は必要なら別branch」。capture-assert(6d985c2d7)は PR #7 branch へ直接コミット済
      (cohesive+検証済+PR未マージで許容)だが、**次の新機能(MCP 整合)からは別ブランチに切って PR #7 を安定化**させる。
    - **capture-assert = 視覚回帰テスト✅(2026-07-21・親commit 6d985c2d7)**: シナリオが device 状態だけでなく**画面**を assert 可能に。
      (B)方針どおり operate→:52702 / capture→:52700 を一つの Server が横断。`capture`(参照 screenshot を layer :52700 から取り PNG hash 保存)+
      `assert_capture`(op changed|stable で参照比較)。ControlClient.req_line が :52700 の request_id 無しプロトコルを処理、--capture-port で接続
      (0=無効なので state-only シナリオは不変)。**検証**: capture_assert_demo 3/3 rc=0(参照撮影→no-op で stable→look150°+move で changed
      c3ba9140→3a815fab、hash は E2E 一致)+negative control(無操作で changed 期待)FAIL rc=1。tools/scenarios/capture_assert_demo.json。
      =operate+observe(状態+**画面**)+assert の Playwright テストFW が本環境で完備。
    - **(d) operate 面拡張✅(2026-07-21・branch feat/mcp-server commit a1634cfc2)**: head は動かせたが手 pose を直接動かせない
      ギャップを解消。`move_controller(hand, position[, orientation])`(grip+aim pose を補間移動・hello_xr は controller を cube 描画
      するので capture にも反映)+`set_input(hand, path, value)`(任意宣言 input を float/bool でセット・held)。Server dispatch+MCP tools 両方。
      **検証**: controller_ops シナリオ 7/7(baseline 右grip x=0.2→move_controller で grip x=0.7/y=1.1・aim x=0.7→set_input 左 squeeze=0.9・
      x/touch=true→reset で復元)。tools/scenarios/controller_ops.json。
    - **★ユーザー全4軸 対応完了(2026-07-21)**: (a)MCP✅(feat/mcp-server) (b)capture-assert✅(6d985c2d7) (c)Windows実アプリ=env-blocked記録
      (d)operate拡充✅(a1634cfc2)。本環境で verifiable な主軸は完了。残=TS MCP の扱い(質問)・Windows実アプリ(env)・PR 管理(#7/#1/feat/mcp-server)。
      **Q1: capture は既存(未実装でない)。SteamVR=Windows 要で記録・後回し。次の実作業軸=(c) Server/Scenario Runner(本環境で verifiable)。**
    - **(b) SteamVR Adapter**: Windows/SteamVR 必須(本環境不可)。
    - **(c) 上位 Server / Scenario Runner** ✅ **最小実装+E2E検証済み(2026-07-21・親commit 259465e35)**: Runtime 非依存・
      本環境で実装も検証も可能(既存の制御チャネル :52702 上のオーケストレーション・graphics 不要)。`tools/playspectra_server.py`=
      Server が権威的 VirtualDeviceState モデルを保持し、高水準コマンドを**補間しながら完全スナップショット set_state 列**へ
      (sequence/補間は Server 所有・spec §4/§6)。コマンド: hello/move_head(pos lerp+quat slerp)/look(world+Y yaw slerp)/
      walk_forward・strafe(スティック ramp+release)/trigger/press(click+touch hold+release)/wait/reset。apply_ctrl が未指定
      入力を 0 化するため毎フレーム全入力を送る(Q3 寛容のまま正しい)。get_state で自 seed・reset で再 seed。
      **検証**: `--verify` 6/6(seed z~0/move_head z=-1.0/look 90°→quat.y=0.707/walk_forward スティック release/press ボタン
      release/reset→z~0、各 get_state 実測)+シナリオ `tools/scenarios/walk_and_look.json` 全11ステップ clean。
      設計判断(シナリオ形式・コマンド語彙・補間)は指示どおり質問せず妥当な既定で確定・文書化。
    - **Recorder + Replay** ✅ **実装+E2E検証済み(2026-07-21・親commit d84c1386f)**: `tools/playspectra_record.py`。
      Recorder=observer で get_state を一定レートでサンプルしタイムスタンプ付き軌跡へ。Replay=writer で各スナップショットを
      t_ms どおり set_state 再送(get_state スキーマは set_state 入力と互換なので無変換)。**検証 --verify 5/5**(observer が
      head z 0→-2 軌跡を記録→reset z~0→replay で最終 z=-2 再現)+CLI record/replay clean。
      **バグ修正(同commit)**: writer の sequence を adapter の現在値の上から発番(hello 時 get_state.sequence 参照)。
      adapter は sequence を巻き戻さない(Q2)ため、低い seq で始めた writer/replayer は stale 却下される問題を Server/Replayer 両方で修正。
      → **PlaySpectra の Playwright コアループ(操作 Server/observe get_state/record/replay)が本環境で verifiable に成立**
      (画面 capture は Layer 既存・Linux 検証保留を除く)。**次候補: コマンド拡充/MCP・CLI 操作IF/シナリオ assert(検証ステップ)**。
    - **権威的根拠(architecture §8)**: 「以降(**順不同・M2 後に再優先度付け**): SteamVR Adapter / Layer instrumentation
      移行 / Scenario Runner / Recorder+Replay」。=次軸選択は**ユーザーが M2 後の再優先度付けとして明示的に留保した決定**。
      待機は受動でなくこのロードマップに従うもの。3軸とも機械的着手不可(env/未仕様/再優先付け)を一次ソースで確認済み。
    - **画面キャプチャの実現性=事実確定(2026-07-21・null_compositor.c 精読)**: 旧記載「実GPU要(未検証仮説)」を是正。
      **事実**: null compositor は `comp_swapchain_shared_init` で**実 Vulkan swapchain を生成**しアプリは実イメージへ描画。
      `null_compositor_layer_commit` は提出レイヤ(`layer_accum`=swapchain/image 参照保持)を受けるが**意図的に readback せず破棄**、
      コメントが「ここが独自コンポジタがフレームを取り出す(remote clients 向け)正規フック」と明示。→ **キャプチャに物理
      ディスプレイ/HMD 不要**、headless の layer_commit 時点で実イメージ在。実装フック=null を fork し layer_commit で
      vkCmdCopyImage→host-visible→map、または OpenXR layer で swapchain intercept。
      **残る唯一の未知**: 動作する graphics セッション確立(アプリが D3D/Vulkan/GL binding で session+swapchain+描画)。
      WSL2 は lavapipe(soft Vulkan)で以前 hello_xr が device 生成 hang。最小自作 graphics クライアントが lavapipe で
      session を張れるかは未検証。「実GPU categorically 必須」ではない。north star の D3D11/12/Vulkan 全対応は
      Windows/実GPU で成立、readback は graphics-API 別。
  - **(spec gap 観察)writer 引き継ぎ時のカウンタ挙動**: §5.3 は「新しい writer が hello で引き継ぎ可能」だが、
    新writer が sequence/logical_frame を低い値から始めると apply_realtime/apply_frame_synchronized の stale ガードに
    弾かれる(reset を挟めば frame記録はクリアされるが sequence は残る)。reset とは別の writer-handoff の未規定点。
    現状は保守的に「Adapter はカウンタを勝手に巻き戻さない」で実装。要ユーザー方針。
  - **(新・spec strictness 2026-07-21)入力の完全性強制 未実装**: §2.2「connected device の全可変フィールドを
    毎回含む・省略=validation_error」/§2.4「Descriptor 宣言 path をすべて含む・欠落=validation_error」だが、
    現状 parse_inputs は寛容(送られた path だけ取込・欠落非検証)、add_descriptor は HMD のみ宣言(§3 のコントローラ
    input path リスト未宣言)。**独断実装しない理由**: (1) §3 コントローラ Descriptor 設計判断が要る、(2) 強制すると
    既存検証済みハーネス(reset_test=左trigger のみ / frame_test=controller 無し等の部分入力)が全滅=Server メッセージ
    構築契約の変更で波及、(3) spec 自身が寛容な初期実装を明示許容(§2.4「初期実装が内部で固定 struct を使うのは可」・
    §3「形のみ・実値は M2 実測」)。reset(§5.3 が一意決定・追加のみ非破壊)と違い一意に決まらず破壊的・設計判断込み
    → 質問リスト送りが正しい仕分け。いつ・どこまで厳格化するか要ユーザー方針。
- ✅ **frame_synchronized conflict 判定 実装済み・E2E検証(commit ba5055a2 submodule / b1e7e268 parent harness)**:
  spec §4 の frame_synchronized モードを realtime latest-wins と並置。
  - proto: `clock.mode`("realtime"既定 | "frame_synchronized") + `clock.logical_frame` をパース。
    frame_synchronized で logical_frame 欠落は validation_error。**proto単体 33/33**(clock 4件追加)。
  - control: handle_set_state を clock mode で分岐。realtime=apply_realtime(従来の sequence latest-wins)、
    frame_synchronized=apply_frame_synchronized:
    新frame→適用+内容署名記録 / 同frame同内容→冪等成功(applied:true,idempotent:true) /
    同frame異内容→conflict_error "frame_content_mismatch" / 古frame→applied:false reason stale_frame。
  - 内容署名=device状態(head+両手 grip/aim/inputs)のみの FNV-1a(envelope の sequence/clock 除外)。
    inputs は順序非依存(XOR)で順序違いの同一内容を conflict にしない。
  - **E2E(WSL2 in-process, writer over :52702)= 10/10**: frame100適用→同内容冪等→異内容conflict(状態不変)
    →frame101適用→古frame100 stale(状態非revert)。各段 get_state で状態確認。
  - **回帰**: realtime の observer-multi E2E は 11/11 のまま。full build クリーン。
  - ハーネス: tools/playspectra_frame_test.py(parent 追加)。

## 検証境界の現在地(2026-07-22 更新)

> 2026-07-20 時点の旧記述は本ファイル上部(2026-07-21 節)と Windows 検証で更新済み。現在地:

- ✅ 実行検証(WSL2): M2.1 列挙 / M2.2 pose取得 / M2.3 set_state E2E / proto(HMD+controller)単体29/29 /
  controller device/builder/apply＋入力E2E / haptics 逆方向 / 複数 observer 11/11 / reset 20/20 /
  frame_synchronized 10/10 / hello_xr(Vulkan2・lavapipe)session確立 / Layer capture(Linux移植)394 PNG。
- ✅ 実行検証(Windows・実GPU 2026-07-22): 統一スタック(service/CLI/wait_for)9/9 / record-replay 5/5 /
  frame 10/10 / reset 20/20 / 実アプリ hello_xr×capture D3D11/D3D12/Vulkan 各20/20 / MCP 14/14 /
  operate 到達(coupling)2/2。
- 🟡 未検証: VRDevApp(Windows 実機) / 表示 compositor 経路 / SteamVR Adapter。
- submodule branch `feat/playspectra-driver-m2`(push済・親 gitlink 登録済 49010dbdf)。
