# PlaySpectra

> **VRアプリ版の Playwright。** XRアプリを *操作*（入力注入）・*観察*（画面キャプチャ）・*記録*・*再生*・*検証（assert）* するための自動化基盤。

Playwright がブラウザに合成入力を注入して DOM / スクリーンショットを観察するのと同じことを、VRアプリに対して行う。VRコントローラーと HMD を「触ったのと同じ入力」を注入し、レンダリング結果を画像として取得し、シナリオとして記録・再生し、画面や状態を assert する。

MCP は製品本体ではなく、**複数ある操作インターフェースの1つ**（他に CLI / JSON シナリオ実行）。**エンジン非依存**（OpenXR 抽象に介入するため Unity / Unreal / Godot / ネイティブ OpenXR アプリを問わない）。実機 HMD 不要のヘッドレス動作を志向する。

> リポジトリ名 (`VR-MCP`) は歴史的経緯で据え置き。製品名は PlaySpectra（旧 VR-MCP から 2026-07-19 に再定義）。設計の正典は [`.claude/playspectra-architecture.md`](.claude/playspectra-architecture.md)。

---

## アーキテクチャ

```text
  ┌─ 操作インターフェース ─────────────────────────────┐
  │   MCP  /  CLI  /  JSON Scenario Runner             │
  └───────────────────────┬───────────────────────────┘
                          ▼
                 PlaySpectra Server            高水準命令 (walk_forward / look_at …) を
        (高水準命令 → デバイス状態へ解釈・補間)   デバイス状態の列へ解釈・補間する
                          ▼
              Virtual Device Core              Runtime 非依存の VirtualDeviceState
          (protocol_version / sequence / HMD / L / R)
                          ▼
        ┌──────── Device Backend (Runtime Adapter) ────────┐
        │   Monado Adapter        SteamVR Adapter          │
        │   (OpenXR / headless)   (SteamVR 実ゲーム・計画)  │
        └───────────┬───────────────────┬──────────────────┘
                    ▼                    ▼
                 Monado              SteamVR Runtime
                    ▼                    ▼
             OpenXR アプリ        OpenVR / OpenXR アプリ

  ── Instrumentation（別軸・Device Backend ではない）──
     OpenXR API Layer:  screenshot / recording / action discovery /
                        diagnostics /（自動テスト用の override のみ）
```

**設計の骨子**（詳細は正典 §2）:

- **共通汎用 Driver ABI は存在しない**。OpenXR が標準化するのは「アプリ ⇔ Runtime」の間だけで、その下の「Runtime ⇔ Driver」は Runtime ごとに固有。よって単一 DLL が全ランタイムに刺さる構造にはならず、**「共通デバイスモデル（Virtual Device Core）＋ Runtime 別 Adapter」** が骨格になる。
- **PlaySpectra Server** が高水準命令の解釈・補間を担い、Driver/Adapter は「状態配信装置」に保つ（`walk_forward` を Driver に持たせない）。
- **OpenXR Layer は Instrumentation（観測軸）** であり、Monado/SteamVR と同種の Device Backend としては扱わない。本番の入力経路は Adapter 側で、Layer の入力 override は自動テスト補助に限定する。

## 実装・検証状況

凡例: ✅ 本環境で実装・E2E 検証済み / 🟡 実機・別環境で部分検証済み / 📋 設計・開発中 / 🔬 仮説（検証法付き）

本環境で verifiable な主軸（操作 / 観察 / 記録 / 再生 / assert）は WSL2 上で end-to-end に成立し、さらに **Windows・実GPU でも、Windows ビルドの Monado 上で実アプリ（hello_xr）相手に E2E 成立**している（下表・末尾行）。検証環境は末尾の「検証環境」節を参照。

| 領域 | 状況 | 根拠（リポジトリ内） |
| --- | --- | --- |
| **Monado Adapter**: Virtual HMD + 左右コントローラー | ✅ Monado がデバイス列挙（head/left/right）・OpenXR アプリが pose 取得・`set_state` で pose/入力が遷移 | submodule `runtime/monado-playspectra` `drivers/playspectra/`、`tools/playspectra_{headless,action}_probe.c` |
| Monado 制御チャネル（NDJSON/TCP :52702） | ✅ feature-complete: `set_state`/`get_state`/haptics broadcast/複数 observer/writer 排他/`frame_synchronized`/`reset`。**live Windows でも frame 10/10・reset 20/20・multiobs 中核 8/8 実測**（haptics broadcast の3件は host app 前提＝`integration_hello_xr` の grab→buzz 0→22 で別途実証） | `tools/playspectra_{multiobs,frame,reset}_test.py`（E2E 11/11・10/10・20/20、Windows 実測 2026-07-22） |
| **PlaySpectra Server**: 高水準命令＋補間 | ✅ `move_head`/`look`/`walk_forward`/`strafe`/`trigger`/`press`/`set_input`/`move_controller`/`reset`/`get_state`。**live Windows で operate 面を全アクション・両手 完全実測**: `--verify` 9/9（HMD+auto-wait）+ `controller_ops` 7/7 + `operate_completeness` 7/7（両手の trigger/move_controller/set_input + strafe） | `tools/playspectra_server.py`、`tools/scenarios/{controller_ops,operate_completeness}.json`（Windows 2026-07-22） |
| **Recorder + Replay** | ✅ observer で軌跡サンプル → writer で `t_ms` どおり再生。**live Windows Monado で 5/5**（42フレーム記録・軌跡 z→-2・reset 後 replay が head z を再現） | `tools/playspectra_record.py`（`--verify --port 52702`、2026-07-22） |
| **Scenario Runner + assert（状態）** | ✅ `run_scenario` + `assert`（get_state のパス比較、失敗で exit 1、negative control 実証） | `tools/scenarios/assert_demo.json` |
| **capture-assert（視覚回帰）** | ✅ 参照 screenshot の PNG hash を取り `changed`/`stable` を assert | `tools/scenarios/capture_assert_demo.json`（3/3、negative control FAIL rc=1） |
| **キャプチャ: Vulkan** | ✅ Windows・実GPU で full E2E 20/20（fmt=43・深度パス present）＋ Linux で本番 Vulkan readback 394 PNG | `scripts/integration_test.sh Vulkan`（metasim, 2026-07-22）／ Linux 移植＝`layer/`（`CMakeLists.txt` の `if(WIN32)` で D3D/winsock を条件化した Vulkan-only ビルド・POSIX socket 化、committed） |
| **キャプチャ: D3D11** | ✅ Windows・実GPU で full E2E 20/20（screenshot 1680x1760 fmt=29・非退化・録画） | `scripts/integration_test.sh D3D11`（metasim + MSVC hello_xr、2026-07-22） |
| **キャプチャ: D3D12** | ✅ Windows・実GPU で full E2E 20/20（395フレーム・fmt=29・非退化・録画） | `scripts/integration_test.sh D3D12`（metasim + MSVC hello_xr、2026-07-22） |
| **MCP サーバー（現行・Python）** | ✅ FastMCP が Server をラップ（operate→:52702 / capture→:52700）。**live Windows Monado + hello_xr 相手に実 MCP クライアントで 14/14**（全13ツールを実測: HMD操作/両コントローラー move_controller・set_input・set_trigger/walk_forward・strafe/press/wait_for/screenshot 実画像/run_scenario/reset。arg マッピングも確認） | `tools/playspectra_mcp.py`、`scripts/run_mcp_verify_monado.sh`（要 `pip install mcp`・**venv 推奨**、2026-07-22） |
| **end-to-end Playwright ループ**（操作シナリオ→再描画→視覚回帰） | ✅ **Windows・実GPU**: server.py が `capture_assert_demo` シナリオを実行 operate(:52702)→観測(:52700)→視覚 assert（no-op stable / head 移動で changed）3/3。＋ WSL2 で 2/2 | `scripts/run_scenario_e2e_monado.sh`（Windows）、`scripts/e2e_playwright_loop.sh`（WSL2）、`tools/scenarios/{capture_assert_demo,big_view_change}.json` |
| **実アプリ E2E: hello_xr × Windows Monado × capture** | ✅ Windows・実GPU headless（null compositor）: hello_xr が Windows ビルドの Monado(:52702) に **client↔service IPC 接続** → **D3D11 / D3D12 / Vulkan の全3API** で実描画 → capture レイヤー(:52700) が非退化観測。各 20/20 | `scripts/run_hello_xr_monado.sh [D3D11\|D3D12\|Vulkan\|all]`（各 20/20＋coupling 2/2、`all` で3API一括回帰、2026-07-22） |
| **operate 到達（runtime レベル）** | ✅ layer override ではなく **:52702 で Monado 仮想 HMD を駆動 → 実アプリの xrLocateViews が追従**（override クリア状態で dz=−2.5 を 1:1 反映、x/y 不変）。「注入のアプリ到達」を runtime 経路で実証。2/2 | `tools/playspectra_coupling_probe.py`（`run_hello_xr_monado.sh` のゲート、2026-07-22） |
| MCP サーバー（レガシー・TypeScript） | 🟡 改革前の設計（layer :52700 直結）。現行 Python 版に併存。今後の扱いは未決 | `mcp/src/` |
| VRDevApp（実 Godot アプリ・Windows） | 🟡 metasim/CA 経路で検証済み（session 確立・D3D12 キャプチャ・左スティック移動・視点回転） | `scripts/run_vrdevapp.sh`（未追跡）、memory `vrdevapp-test-target` |
| OpenVR アプリ | 🟡 OpenComposite（OpenVR→OpenXR 変換）経由で観察・姿勢注入（統合テスト 15 PASS / 1 SKIP）。openvr **v1.8.19** 世代でビルド | `scripts/integration_openvr_test.sh` |
| **SteamVR Adapter** | 📋 計画。VD1〜VD3 の実測知見あり、`driver/`（未追跡）に改革前スケルトン。新 Core への再接続と Windows 検証が未 | `.claude/steamvr-driver-plan.md` |
| Windows Monado の**メイン（表示）compositor** session | 📋 上の実アプリ E2E は null compositor（headless）で実証済み。実 HMD へ提示する表示 compositor 経路は未検証 | `run_hello_xr_monado.sh` は `XRT_COMPOSITOR_NULL=1` |
| Frame-synchronized Mode の「決定性」 | 🔬 delta time / GPU scheduling / physics 等が揺れるため未検証。実測してから "Deterministic" へ昇格 | 正典 §4 |

> **グラフィックスAPIの完全性（D3D11 / D3D12 / Vulkan）はコア必須要件**（「VR版Playwrightが全キーを打てる」ため）。いずれのバックエンドも未対応フォーマットでは「無言で壊れた画像」を返さず、必ず明示的にエラーを返す。深度マップのみ nice-to-have（ユーザーが許容表現で明示）。

## 構成

```text
tools/                    PlaySpectra 本体ツール群（Python）
  playspectra_server.py     Server: 高水準命令の解釈・補間・シナリオ・assert・capture-assert
  playspectra_mcp.py        現行 MCP サーバー（FastMCP、Server をラップ）
  playspectra_record.py     Recorder + Replay
  playspectra_*_test.py     制御チャネル E2E（multiobs / frame / reset）
  playspectra_*_probe.c     Monado デバイス列挙 / action 到達の検証プローブ
  scenarios/*.json          シナリオ（walk_and_look / assert_demo / capture_assert_demo / …）
runtime/
  monado-playspectra/       Monado fork（submodule）。drivers/playspectra が Monado Adapter
                            = Virtual HMD + 左右コントローラー + 制御チャネル :52702
layer/                    OpenXR API Layer（Instrumentation 軸・C++/CMake）
  src/                      フック・制御チャネル(:52700)・状態ストア・キャプチャ
                            （capture_{vulkan,d3d11,d3d12}.cpp。D3D は WIN32 条件、Vulkan は Linux 可）
  tests/                    ユニットテスト（gtest、78件）
mcp/                      レガシー TypeScript MCP（layer :52700 直結・改革前）
driver/                   改革前 SteamVR 仮想ドライバースケルトン（未追跡・SteamVR Adapter の素材）
scripts/                  セットアップ・E2E ハーネス（e2e_playwright_loop.sh 等）
third_party/             外部ランタイム / SDK 取得先（非コミット）
```

## Getting Started

PlaySpectra は2つの検証経路がある。

### A. Monado 経路（操作＋観察の主軸）

**WSL2 Ubuntu 22.04（下記）と Windows・実GPU（末尾に別記）の両方で検証済み**。Monado のビルドには glslang / Vulkan SDK が要る。WSL2 は GPU 不要（lavapipe / llvmpipe の CPU Vulkan で完走を実証済み）。

```bash
# 1. submodule（Monado fork）を取得
git submodule update --init runtime/monado-playspectra

# 2. Monado（PlaySpectra ドライバー入り）をビルド
#    Enabled drivers に playspectra が入ることを configure ログで確認
cmake -S runtime/monado-playspectra -B runtime/monado-playspectra/build -G Ninja
cmake --build runtime/monado-playspectra/build

# 3. PlaySpectra ドライバーを有効化して Monado を起動（制御チャネル :52702）
PLAYSPECTRA_ENABLE=1 runtime/monado-playspectra/build/src/xrt/targets/service/monado-service

# 4. OpenXR アプリを headless で起動（例: hello_xr -g Vulkan2）
#    lavapipe を単体強制すると複数 ICD の device_select 由来 hang を回避できる
#    このフル bring-up は committed の scripts/e2e_playwright_loop.sh（lavapipe 単体強制込み）が自動化

# 5. Server / MCP からアプリを操作・観察
python3 tools/playspectra_server.py --verify        # 9/9
python3 tools/playspectra_mcp_verify.py             # 14/14（要 pip install mcp）
```

#### Windows・実GPU（本セッションで Monado 経路の全層を E2E 検証）

WSL2 と同じ Monado 経路が Windows でも動く（capture/operate/record-replay/MCP 全層。詳細は検証境界表＋memo）。ビルドは **VS2022 同梱 vcpkg ツールチェーン**で `--target monado-service cli openxr_monado`（`/utf-8` は submodule に導入済み。手順の詳細は各 `scripts/run_*_monado.sh` のヘッダ prereqs）。フル E2E は1コマンド:

```bash
scripts/run_hello_xr_monado.sh all       # 実アプリ×Windows Monado×capture: D3D11/D3D12/Vulkan 各20/20 + coupling 2/2
scripts/run_scenario_e2e_monado.sh       # VR-Playwright ループ: 操作シナリオ→再描画→視覚回帰 3/3
PY=<venv-python> scripts/run_mcp_verify_monado.sh   # MCP 全13ツール 14/14（mcp は venv 隔離必須）
```

### B. Layer 経路（Instrumentation＝観測・録画）

Layer は対象アプリのプロセスに OpenXR ローダ経由で注入され、制御チャネル :52700 で screenshot / recording / action discovery を提供する。D3D バックエンドは Windows 専用、Vulkan は Linux でもビルド・検証可能。

```bash
# Windows（D3D11/D3D12 + Vulkan、MinGW-w64 + CMake ≥ 3.21）
cd layer
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build           # POST_BUILD で manifest/ に DLL が同期される

# ユニットテスト（任意・既定 OFF）
cmake -S . -B build -G "MinGW Makefiles" -DPLAYSPECTRA_BUILD_TESTS=ON
cmake --build build --target playspectra_test && ctest --test-dir build --output-on-failure

# 環境変数（Meta XR Sim ランタイム + レイヤー有効化）
source scripts/env.sh
```

主要な環境変数:

| 変数 | 既定 | 説明 |
| --- | --- | --- |
| `PLAYSPECTRA_ENABLE` | — | Monado ドライバー側: `1` で PlaySpectra Virtual HMD/コントローラーを有効化 |
| `XR_RUNTIME_JSON` | — | 使用する OpenXR ランタイム（Meta XR Simulator の JSON 等） |
| `XR_API_LAYER_PATH` / `XR_ENABLE_API_LAYERS` | — | Layer manifest 探索ディレクトリ / `XR_APILAYER_playspectra` |
| `PLAYSPECTRA_PORT` | `52700` | Layer 制御チャネルの TCP ポート |
| `PLAYSPECTRA_CAPTURE_DIR` | `%TEMP%` | スクリーンショット / 録画の出力先 |
| `PLAYSPECTRA_DISABLE_CA` | — | `1` で `XR_EXT_conformance_automation` の自動有効化を無効化し非 CA フォールバック経路で動作（診断用） |

## 使い方

### Server / MCP（高水準命令）

現行 MCP（`tools/playspectra_mcp.py`）はエージェント向けに、Server が持つ高水準命令を公開する:

- 操作: `move_head` / `look` / `walk_forward` / `strafe` / `press` / `set_trigger` / `move_controller` / `set_input` / `reset`
- 観察: `get_state`（デバイス状態） / `wait_for`（状態が条件を満たすまで自動待機＝Playwright 流 auto-wait） / `screenshot`（画面を MCP Image で返す＝AI が画面を見る）
- 実行: `run_scenario`（JSON シナリオを assert 込みで実行）

Server は操作を `set_state`（完全スナップショット）の列へ補間して Monado Adapter（:52702）へ送り、capture は Layer（:52700）から取得する（`operate→:52702 / capture→:52700` の横断）。

### シナリオ（記録・再生・assert）

```bash
# 状態 assert 付きシナリオ
python3 tools/playspectra_server.py run_scenario tools/scenarios/assert_demo.json

# 視覚回帰（capture-assert）: --capture-port で layer:52700 に接続
python3 tools/playspectra_server.py run_scenario tools/scenarios/capture_assert_demo.json --capture-port 52700

# 記録 → reset → 再生
python3 tools/playspectra_record.py --verify

# end-to-end Playwright ループ（操作 → 再描画 → capture 変化）
scripts/e2e_playwright_loop.sh
```

## テスト

```bash
# 制御チャネル E2E（Monado Adapter）
python3 tools/playspectra_multiobs_test.py      # 複数 observer 11/11
python3 tools/playspectra_frame_test.py         # frame_synchronized 10/10
python3 tools/playspectra_reset_test.py         # reset 20/20

# Server / MCP / Recorder
python3 tools/playspectra_server.py --verify    # 9/9
python3 tools/playspectra_record.py --verify    # 5/5
python3 tools/playspectra_mcp_verify.py         # MCP 14/14（要 pip install mcp）

# Layer（Instrumentation）
scripts/integration_test.sh [Vulkan|D3D11|D3D12]           # hello_xr + Meta XR Sim
PLAYSPECTRA_DISABLE_CA=1 scripts/integration_test.sh Vulkan # 非 CA フォールバック経路
VR_RUNTIME=monado scripts/integration_test.sh Vulkan       # ランタイム切替
scripts/integration_openvr_test.sh                         # OpenVR（OpenComposite + Monado）
./layer/build/playspectra_test.exe                         # ユニットテスト 78件
```

## 検証環境

- **Monado / Server / MCP / capture(Vulkan) の E2E は WSL2 Ubuntu 22.04 で検証**。GPU 不要（lavapipe の CPU Vulkan で完走を実証）。旧「キャプチャは実 GPU 必須」は end-to-end で否定済み。
- **D3D11 / D3D12 / Vulkan キャプチャは Windows・実 GPU で full E2E 検証済み**（各 20/20 = 60/60、2026-07-22、metasim CA 経路 + DoS 修正済み Layer DLL）。WSL2 が原理的に触れない D3D を含めコア必須マトリクスを Windows で完結。同ラン内で Layer CA 経路の head/controller override・durationMs グライド・haptic sync round-trip も緑。
- **Windows での完全 graphics session（VRDevApp 実アプリ）・SteamVR Adapter は実機環境が要る**ため本環境では非優先。該当は上表で 🟡 / 📋。
- 検証境界の規律（CLAUDE.md「検証済みと未検証を混ぜない」）に従い、本 README の各主張は上表の「根拠」列でリポジトリ内の実測ログ・テスト・一次ソースを指せるものだけを ✅ とし、実機依存・未検証は 🟡 / 📋 / 🔬 に分ける。

> OpenComposite は GPLv3。third_party/ に取得するのみで、本リポジトリには含めず再配布もしない。
