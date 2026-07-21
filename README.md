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

本環境で verifiable な主軸（操作 / 観察 / 記録 / 再生 / assert）は WSL2 上で end-to-end に成立している。検証環境は末尾の「検証環境」節を参照。

| 領域 | 状況 | 根拠（リポジトリ内） |
| --- | --- | --- |
| **Monado Adapter**: Virtual HMD + 左右コントローラー | ✅ Monado がデバイス列挙（head/left/right）・OpenXR アプリが pose 取得・`set_state` で pose/入力が遷移 | submodule `runtime/monado-playspectra` `drivers/playspectra/`、`tools/playspectra_{headless,action}_probe.c` |
| Monado 制御チャネル（NDJSON/TCP :52702） | ✅ feature-complete: `set_state`/`get_state`/haptics broadcast/複数 observer/writer 排他/`frame_synchronized`/`reset` | `tools/playspectra_{multiobs,frame,reset}_test.py`（E2E 11/11・10/10・20/20） |
| **PlaySpectra Server**: 高水準命令＋補間 | ✅ `move_head`/`look`/`walk_forward`/`strafe`/`trigger`/`press`/`set_input`/`move_controller`/`reset`/`get_state` | `tools/playspectra_server.py`（`--verify` 6/6、`controller_ops` 7/7） |
| **Recorder + Replay** | ✅ observer で軌跡サンプル → writer で `t_ms` どおり再生 | `tools/playspectra_record.py`（`--verify` 5/5） |
| **Scenario Runner + assert（状態）** | ✅ `run_scenario` + `assert`（get_state のパス比較、失敗で exit 1、negative control 実証） | `tools/scenarios/assert_demo.json` |
| **capture-assert（視覚回帰）** | ✅ 参照 screenshot の PNG hash を取り `changed`/`stable` を assert | `tools/scenarios/capture_assert_demo.json`（3/3、negative control FAIL rc=1） |
| **キャプチャ: Vulkan（Linux）** | ✅ hello_xr の描画から本番 Vulkan readback で PNG 生成（394枚） | Layer を Linux/Vulkan-only へ移植、`scripts/ps_layer_build.sh`＋`ps_capture_verify.sh`（scratchpad） |
| キャプチャ: D3D11 | 🟡 Windows 実測済み（`XR_EXT_conformance_automation` 経路） | 旧 layer 実測（journal M0） |
| キャプチャ: D3D12 | 🟡 MSVC 版 hello_xr で実測（Windows） | `scripts/setup_helloxr_msvc.sh` |
| **MCP サーバー（現行・Python）** | ✅ FastMCP が Server をラップ（operate→:52702 / capture→:52700）。実 MCP クライアントで 7/7 | `tools/playspectra_mcp.py`、`tools/playspectra_mcp_verify.py`（要 `pip install mcp`） |
| **end-to-end Playwright ループ** | ✅ 操作注入 → hello_xr 再描画 → capture PNG 変化 2/2 PASS | `scripts/e2e_playwright_loop.sh`、`tools/scenarios/big_view_change.json` |
| MCP サーバー（レガシー・TypeScript） | 🟡 改革前の設計（layer :52700 直結）。現行 Python 版に併存。今後の扱いは未決 | `mcp/src/` |
| VRDevApp（実 Godot アプリ・Windows） | 🟡 metasim/CA 経路で検証済み（session 確立・D3D12 キャプチャ・左スティック移動・視点回転） | `scripts/run_vrdevapp.sh`（未追跡）、memory `vrdevapp-test-target` |
| OpenVR アプリ | 🟡 OpenComposite（OpenVR→OpenXR 変換）経由で観察・姿勢注入（統合テスト 15 PASS / 1 SKIP）。openvr **v1.8.19** 世代でビルド | `scripts/integration_openvr_test.sh` |
| **SteamVR Adapter** | 📋 計画。VD1〜VD3 の実測知見あり、`driver/`（未追跡）に改革前スケルトン。新 Core への再接続と Windows 検証が未 | `.claude/steamvr-driver-plan.md` |
| Windows 実 GPU での完全 graphics session 検証 | 📋 本環境（WSL2）は非優先。実機・実 GPU が要る | — |
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

### A. Monado 経路（操作＋観察の主軸・本環境＝Linux/WSL2）

Monado のビルドには glslang / Vulkan SDK が要り、本プロジェクトは WSL2 Ubuntu 22.04 で検証している。GPU は不要（lavapipe / llvmpipe の CPU Vulkan で完走を実証済み）。

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
#    再現手順: scratchpad の ps_helloxr_run.sh 参照

# 5. Server / MCP からアプリを操作・観察
python3 tools/playspectra_server.py --verify        # 6/6
python3 tools/playspectra_mcp_verify.py             # 7/7（要 pip install mcp）
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

- 操作: `move_head` / `look` / `walk_forward` / `press` / `set_trigger` / `move_controller` / `set_input` / `reset`
- 観察: `get_state`（デバイス状態） / `screenshot`（画面を MCP Image で返す＝AI が画面を見る）
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
python3 tools/playspectra_server.py --verify    # 6/6
python3 tools/playspectra_record.py --verify    # 5/5
python3 tools/playspectra_mcp_verify.py         # MCP 7/7（要 pip install mcp）

# Layer（Instrumentation）
scripts/integration_test.sh [Vulkan|D3D11|D3D12]           # hello_xr + Meta XR Sim
PLAYSPECTRA_DISABLE_CA=1 scripts/integration_test.sh Vulkan # 非 CA フォールバック経路
VR_RUNTIME=monado scripts/integration_test.sh Vulkan       # ランタイム切替
scripts/integration_openvr_test.sh                         # OpenVR（OpenComposite + Monado）
./layer/build/playspectra_test.exe                         # ユニットテスト 78件
```

## 検証環境

- **Monado / Server / MCP / capture(Vulkan) の E2E は WSL2 Ubuntu 22.04 で検証**。GPU 不要（lavapipe の CPU Vulkan で完走を実証）。旧「キャプチャは実 GPU 必須」は end-to-end で否定済み。
- **D3D11 / D3D12 キャプチャは Windows で実測**（MSVC 版 hello_xr）。
- **Windows での完全 graphics session（VRDevApp 実アプリ）・SteamVR Adapter は実機環境が要る**ため本環境では非優先。該当は上表で 🟡 / 📋。
- 検証境界の規律（CLAUDE.md「検証済みと未検証を混ぜない」）に従い、本 README の各主張は上表の「根拠」列でリポジトリ内の実測ログ・テスト・一次ソースを指せるものだけを ✅ とし、実機依存・未検証は 🟡 / 📋 / 🔬 に分ける。

> OpenComposite は GPLv3。third_party/ に取得するのみで、本リポジトリには含めず再配布もしない。
