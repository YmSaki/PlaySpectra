# PlaySpectra

[English](../README.md) | **日本語**

> **AIエージェントが使える仮想HMD・仮想コントローラー。**

PlaySpectraは、AIエージェントをVR・AR・MRアプリへ接続するためのXR操作アダプターです。

PlaySpectra-MCPを使用すると、AIエージェントは実機HMDなしにXRアプリの中に入り、アプリを操作し、アプリの世界を観測できます。

手順を固定化する場合は、PlaySpectra-CLIやJSON Scenarioを経由してPlaySpectraを利用できます。同じ操作を繰り返せるため、ヘッドレスな自動テストにも使用できます。

このページはREADMEの日本語訳です。
正典は[英語版README](../README.md)です。

OS別の完全な構築手順、Scenario形式、MCPツール一覧、検証結果はリンク先の詳細文書を参照してください。

## PlaySpectraで実現できること

| 利用目的 | PlaySpectraでできること |
| --- | --- |
| AIエージェントのXR操作・観測 | AIエージェントがXRアプリを操作し、アプリの状態や画面を観測する |
| XRアプリの開発 | 実機HMDなしで、頭や手の入力がアプリに届くか確認する |
| 操作の再現 | 同じ視線移動、歩行、コントローラー操作を何度でも再現する |
| ヘッドレス運用 | サーバー、CI、WSL2、Linux環境でXRアプリを動かす |
| 自動テスト | 固定した操作の後に、状態や描画結果が期待どおりか確認する |

## 最小実行例

PlaySpectra Server/CLIは、Monado Runtime Adapter上ですでに起動しているOpenXRアプリへ接続します。Server/CLI自身がRuntimeやアプリを起動するわけではありません。試したい実行環境を選び、各プラットフォームのbuild手順を完了してから、次の既存bring-upコマンドを実行してください。これらのbring-up scriptがRuntimeとサンプルアプリを起動します。

これらのコマンドはプラットフォーム別ガイドにも掲載していますが、build完了後にサンプルアプリを起動して動作確認する最短経路なので、ここにも掲載しています。

| 実行環境 | 確認済みサンプル実行 | 起動・確認するもの |
| --- | --- | --- |
| Windows native | `scripts/run_scenario_e2e_monado.sh D3D11` | Windows Monado service、`hello_xr`、capture layerを起動し、`capture_assert_demo.json`を実行します。失敗時はnon-zeroで終了します。 |
| Windows WSL2 | `MONADO_BUILD="$PWD/build/monado" HELLOXR="$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" LAYER_SO="$PWD/layer/build/playspectra_layer.so" bash scripts/e2e_playwright_loop.sh` | WSL2内のLinux Monadoと`hello_xr`を起動し、姿勢入力後にcapture画像が変化することを確認します。 |
| Ubuntu Linux | `MONADO_BUILD="$PWD/build/monado" HELLOXR="$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" LAYER_SO="$PWD/layer/build/playspectra_layer.so" bash scripts/e2e_playwright_loop.sh` | native Linux Monadoと`hello_xr`を起動し、姿勢入力後にcapture画像が変化することを確認します。 |

Windowsのコマンドは、[Windows setup](getting-started-windows.md)完了後にGit Bashから実行してください。WSL2とUbuntuのコマンドは、[Linux / WSL2 setup](getting-started-linux.md)完了後にリポジトリルートから実行してください。これらのコマンドはRuntimeとサンプルアプリを必要な順番で起動するため、最初の動作確認では`monado-service`や`hello_xr`を別々に起動する必要はありません。

JSON Scenarioを直接実行したい場合は、次のいずれかのブロックをターミナルで一連に実行してください。各ブロックはRuntimeとサンプルアプリを起動し、control channelの準備を待ち、Scenarioを実行してからアプリを停止します。

**Windows native（Git Bash）**

~~~bash
set -e
source scripts/lib_monado_stack.sh
source scripts/lib_playspectra.sh
mstack_env D3D11
mstack_up D3D11 120
trap mstack_down EXIT
ps_run run tools/scenarios/assert_demo.json
~~~

**WSL2またはUbuntu（bash）**

~~~bash
set -e
export PLAYSPECTRA_ENABLE=1 XRT_COMPOSITOR_NULL=1
export XR_RUNTIME_JSON="$PWD/build/monado/openxr_monado-dev.json"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}"
sleep 120 | "$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" -g Vulkan2 &
APP_PID=$!
trap 'kill "$APP_PID" 2>/dev/null || true; wait "$APP_PID" 2>/dev/null || true' EXIT
go build -o build/playspectra ./cmd/playspectra
./build/playspectra internal wait-tcp --address 127.0.0.1:52702 --timeout 18s
./build/playspectra run tools/scenarios/assert_demo.json
~~~

このScenarioは、頭の移動、視線の回転、右手トリガーの入力、状態assert、仮想デバイスのresetを順番に実行します。すべてのassertが成功すると、runnerは終了コード0で終了します。

CLIで個別操作する場合は、次を実行してください。

~~~bash
playspectra cmd move-head --x 0 --y 1.6 --z -1 --duration-ms 400
playspectra cmd get-state
~~~

## 対応状況

状態は、対象の種類ごとに分けて記載します。
各表の状態欄は、✅確認済み、⚠️部分確認、🔍未検証、🚧未実装・予定を表します。🔍は現行の操作経路や対象設計はあるものの、対象条件での確認がまだない状態です。

### 実行環境

| 対象 | 状態 | 確認範囲 |
| --- | --- | --- |
| Windows native | ✅ | 実GPUを使ったMonado headless E2E |
| Windows WSL2 | ✅ | WSL2内のLinux Monadoとsoftware Vulkan |
| Ubuntu Linux | ✅ | Ubuntu 22.04のheadless経路 |

### Runtime / Adapter

| 対象 | 状態 | 確認範囲 |
| --- | --- | --- |
| Monado Adapter | ✅ | 仮想HMD、左右コントローラー、control channel |
| OpenVR via OpenComposite | ⚠️ | OpenVRからOpenXRへの変換経路に限定 |
| SteamVR Adapter | 🚧 | 現行Coreへ接続するAdapterは未実装 |

### アプリケーション

| 対象 | 状態 | 確認範囲 |
| --- | --- | --- |
| Native OpenXR application | ✅ | OpenXRアプリへの入力到達と描画 |
| Godot 4.7 application | ✅ | 別リポジトリのGodot検証アプリ |
| Unity application | 🔍 | UnityアプリのE2Eは未検証 |
| Unreal Engine application | 🔍 | UnrealアプリのE2Eは未検証 |
| AR/MRアプリ固有のE2E | 🔍 | AR/MRアプリでのE2Eは未検証 |

### Graphics / Capture

| 対象 | 状態 | 確認範囲 |
| --- | --- | --- |
| D3D11 | ✅ | Windows capture backend |
| D3D12 | ✅ | Windows capture backend |
| Vulkan | ✅ | WindowsおよびLinux/WSL2のcapture経路 |
| 実機HMDの表示compositor | 🔍 | headless null compositor以外は未検証 |

### 操作インターフェース

| 対象 | 状態 | 確認範囲 |
| --- | --- | --- |
| PlaySpectra-MCP | ✅ | AIエージェントからMonado経路を操作・観測 |
| PlaySpectra-CLI | ✅ | Serverの操作命令と状態取得 |
| JSON Scenario | ✅ | 操作手順の再現 |

### 検証・記録機能

| 対象 | 状態 | 確認範囲 |
| --- | --- | --- |
| デバイス状態のassert | ✅ | ScenarioとServerの状態assert |
| Screenshot / capture assert | ✅ | OpenXR layer経由の描画結果assert |
| Recording / replay | ✅ | デバイス状態の軌跡。動画の再生ではない |
| アプリ全体のdeterministic timing | 🔍 | GPU、物理、非同期処理を含む決定性は未検証 |

詳細なテスト件数、実測環境、graphics API別の結果、negative controlは[Verification matrix](verification.md)を参照してください。

## Quick Start

実行環境を選んでください。

XR runtimeとアプリを実際に動かす環境を選んでください。Windows nativeとLinuxは別のビルド経路です。Windows上のWSL2は、Windows用バイナリではなくWSL内のLinuxバイナリを実行します。

| 環境 | Runtime | アプリ | 主なGraphics API | 詳細手順 |
| --- | --- | --- | --- | --- |
| Windows native | Windows Monado service | Windows OpenXR app | D3D11 / D3D12 / Vulkan | [Windows setup](getting-started-windows.md) |
| Windows WSL2 | WSL内のLinux Monado | Linux OpenXR app | Vulkan（既定はlavapipe） | [Linux / WSL2 setup](getting-started-linux.md) |
| Ubuntu Linux | native Linux Monado | Linux OpenXR app | Vulkan（software / hardware ICD） | [Linux / WSL2 setup](getting-started-linux.md) |

### Windows native

前提ソフトウェアを用意してください。

- Windows
- Visual Studio 2022（MSVCとC++ workload）
- CMake、Git Bash、Go 1.22+、native Monadoのソースビルド用Python 3
- glslangを含むVulkan SDK

次の順番で実行してください。

1. `git clone --recurse-submodules https://github.com/YmSaki/PlaySpectra.git`を実行してcloneしてください。
2. `cd PlaySpectra`を実行して、リポジトリのルートへ移動してください。
3. [Windows setup](getting-started-windows.md)のMonadoとlayerのビルド手順を実行してください。
4. `bash scripts/setup_helloxr_msvc.sh`を実行して、Windows用のOpenXRサンプルアプリを準備してください。
5. `bash scripts/run_hello_xr_monado.sh D3D11`を実行してください。このスクリプトがMonado service、OpenXRサンプルアプリ、layer、操作・capture処理を順番に起動します。
6. `INTEGRATION_RC=0`を確認してください。D3D12、Vulkan、または`all`を指定すると別のGraphics APIも実行できます。

### Windows WSL2 / Ubuntu Linux

WSL2とUbuntuでは同じLinux手順を実行してください。前提ソフトウェアを用意してください。

- Ubuntu 22.04またはWSL2
- Git、Go 1.22+、native Monadoのソースビルド用Python 3、CMake、Ninja、Go Task
- `build-essential`（GCC/G++を含む）とMonadoのUbuntu依存パッケージ
- ソフトウェアVulkanを使う場合はlavapipe。別のVulkan ICDを使う場合はそのドライバー

WSL2ではWindows側のMSYS2ツールチェーンを使わず、WSL内のUbuntuにインストールしたGCC/G++を使ってください。

次の順番で実行してください。

1. `git clone --recurse-submodules https://github.com/YmSaki/PlaySpectra.git`を実行してcloneしてください。
2. `cd PlaySpectra`を実行して、リポジトリのルートへ移動してください。
3. `task bootstrap:linux`を実行して、Linux依存パッケージ、Monado submodule、Monado buildを準備してください。
4. `cmake -S layer -B layer/build -G Ninja`を実行してください。
5. `cmake --build layer/build --parallel`を実行してください。
6. `MONADO_BUILD="$PWD/build/monado" HELLOXR="$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" LAYER_SO="$PWD/layer/build/playspectra_layer.so" bash scripts/e2e_playwright_loop.sh`を実行してください。
7. capture内容の差分とpost-injection frameについて`PASS`が表示されることを確認してください。

Windows nativeとWSL2/Ubuntuのbuild directory、CMake cache、`node_modules`は共有しないでください。プラットフォーム固有のパスやバイナリを含むためです。詳細な依存パッケージと制約は[Linux / WSL2 setup](getting-started-linux.md)を参照してください。

## 操作インターフェース

### CLI / Server

`playspectra`実行ファイルには、共通control-plane CoreとCLIが含まれます。`move_head`、`look`、`press`、`get_state`などの操作命令を実行し、デバイス状態を読み取れます。

~~~bash
playspectra cmd look --yaw-deg 90 --duration-ms 400
playspectra cmd get-state
~~~

詳細は[CLI and Server details](../tools/README.md)を参照してください。

### JSON Scenario Runner

JSON Scenarioは、操作とassertを順番に実行するファイルです。最小の構造は次のとおりです。

~~~json
{
  "name": "assert_demo",
  "steps": [
    {"cmd": "hello", "role": "writer"},
    {"cmd": "move_head", "to": {"position": [0.0, 1.6, -1.5]}, "duration_ms": 300},
    {"cmd": "assert", "get": ["hmd", "head", "position", 2], "op": "near", "value": -1.5, "tol": 0.02},
    {"cmd": "reset"}
  ]
}
~~~

次のコマンドで実行してください。assertが失敗するとrunnerはnon-zeroで終了します。

~~~bash
playspectra run tools/scenarios/walk_and_look.json
~~~

完全な形式は[Scenario format](scenario-format.md)、同梱例は[`tools/scenarios/`](../tools/scenarios/)を参照してください。

### MCP

PlaySpectra-MCPは、AIエージェントがXRアプリを操作・観測するためのインターフェースです。PlaySpectra-CLIやJSON Scenarioと同じ操作モデルを使用し、同じ`playspectra`実行ファイルからstdio serverとして起動します。

~~~bash
playspectra mcp
~~~

コンパイル済み実行ファイルの利用に、言語runtimeや追加packageの導入は不要です。

ツール一覧は[MCP tools](mcp-tools.md)を参照してください。

## Architecture overview

利用者から見ると、AIエージェントが仮想HMDとコントローラーを使ってXRアプリを操作・観測します。

内部では、操作経路とアプリ内観測経路を分けています。

~~~mermaid
flowchart TD
  I[AI agent / CLI / JSON Scenario] --> M[PlaySpectra-MCP / Server]
  M --> A[共通操作モデル]
  A --> V[仮想HMD / コントローラー]
  V --> R[Runtime Adapter]
  R --> O[OpenXR application]
  L[OpenXR Instrumentation Layer] --> O
  L --> Q[状態 / screenshot / recording]
~~~

- 共通操作モデルは、頭部移動、視線回転、コントローラー入力などを表します。
- Runtime Adapterは、その操作をRuntimeのデバイス経路へ渡します。現在の動作backendはMonadoです。
- OpenXR Instrumentation Layerはアプリ内の描画結果を観測します。Runtime Adapterの代替ではありません。
- 同じ操作モデルを、AI操作、CLI/JSONの再現、assertに使用できます。

仮想HMDとコントローラーの実体は、ランタイム中立のdevice core（[`devicecore/`](../devicecore/)）として1回だけ実装され、各Runtime AdapterがRuntimeのプロセスへコンパイルして取り込みます。Adapter自体は薄い型変換の殻です。

詳細な設計理由は[Architecture](architecture.md)、完全な検証根拠は[Verification matrix](verification.md)、代表テストは[Testing](testing.md)を参照してください。

## Project status and roadmap

上の[対応状況](#対応状況)が、現在の実装・検証状態の正典です。

今後の予定は、現在の機能とは分けて管理します。

- SteamVR Adapter：共有device core（`devicecore/`）へ接続するAdapter殻を実装し、Windowsで検証する
- Unity、Unreal、AR/MR固有アプリ：アプリE2Eの検証結果を追加する
- 実機HMDの表示compositor、アプリ全体のdeterministic timing：不足している検証結果を追加する
- legacy TypeScript MCP：現行MCPへ移行後に廃止する

## ライセンス

PlaySpectraのライセンスは[Mozilla Public License 2.0](../LICENSE)です。第三者コンポーネントは[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)に記載しています。

`runtime/monado-playspectra` submoduleはMonadoのforkで、このリポジトリのMPL-2.0は適用されません。upstream Monadoのライセンス構成をそのまま引き継いでおり、PlaySpectraドライバーのソースを含めて大部分はBSL-1.0ですが、それ以外のライセンスのファイルもあります。submoduleの`LICENSES/`を参照してください。

## ドキュメント

- [英語版README（正典）](../README.md)
- [Windows setup](getting-started-windows.md)
- [Linux / WSL2 setup](getting-started-linux.md)
- [Architecture](architecture.md)
- [Scenario format](scenario-format.md)
- [MCP tools](mcp-tools.md)
- [Verification matrix](verification.md)
- [Testing](testing.md)
- [Roadmap](roadmap.md)
