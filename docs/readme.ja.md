# PlaySpectra

[English](../README.md) | **日本語**

> **XRアプリのためのPlaywright。**

PlaySpectraは、OpenXRアプリへ仮想HMDとコントローラーの入力を注入し、デバイス状態と描画結果を検証する自動テスト基盤です。

PlaySpectraは、実機HMDを使わないヘッドレステストを主な実行形態とします。

このページはREADMEの日本語訳です。
正典は[英語版README](../README.md)です。

OS別の完全な構築手順、Scenario形式、MCPツール一覧、検証結果はリンク先の詳細文書を参照してください。

## PlaySpectraの動作

PlaySpectra Serverは、CLI、JSON Scenario、MCPから受け取った操作を、仮想HMDとコントローラーの状態へ変換します。

MonadoのRuntime Adapterは、その状態をOpenXRアプリが読むデバイス経路へ渡します。

OpenXR Instrumentation Layerは、アプリ内の描画結果をキャプチャし、スクリーンショット、録画、描画結果のassertを提供します。

この構成によって、入力注入、状態確認、描画確認を同じテスト手順で実行できます。

## セットアップ後の最小操作例

先に、下の「Quick Start：実行環境を選ぶ」で実行環境を選び、MonadoとOpenXRアプリを起動してください。

Quick Startの手順を完了して、MonadoのPlaySpectra adapterとOpenXRアプリを起動してください。
その後、次のコマンドをPlaySpectra Serverとは別のターミナルで実行してください。

1. リポジトリのルートディレクトリへ移動してください。
2. 同梱のassertシナリオを実行してください。操作用のcontrol channelは通常`127.0.0.1:52702`です。

~~~bash
python3 tools/playspectra_server.py tools/scenarios/assert_demo.json
~~~

3. assertionの結果を確認してください。成功時は終了コード0になります。

CLIで個別操作する場合は、次のコマンドを順番に実行してください。

~~~bash
python3 tools/playspectra_server.py --cmd move_head --args '{"to":{"position":[0,1.6,-1]},"duration_ms":400}'
python3 tools/playspectra_server.py --cmd get_state
~~~

## 現在確認できている範囲

以下の表は、実行結果を確認できた範囲を示します。
詳細なテスト件数、実測環境、graphics API別の結果、negative controlは[Verification matrix](verification.md)を参照してください。

### 実行環境とRuntime

| 対象 | 状態 | 確認範囲 |
| --- | --- | --- |
| Windows native | **Verified** | Windows Monadoのheadless E2Eを実GPUで確認 |
| Windows WSL2 | **Verified** | WSL2内のLinux MonadoとソフトウェアVulkanで確認 |
| Ubuntu Linux | **Verified** | Ubuntu 22.04のheadless経路を確認 |
| Monado virtual-device backend | **Verified** | 仮想HMD、左右コントローラー、control channel |
| SteamVR Adapter | **Planned** | 現行Virtual Device Coreへの接続とWindows検証が未完了 |
| OpenVR via OpenComposite | **Partially verified** | OpenVRからOpenXRへの変換経路に限定した確認 |

### アプリケーション

| 対象 | 状態 | 境界 |
| --- | --- | --- |
| Native OpenXR application | **Verified** | OpenXRの動作確認用サンプルアプリで確認 |
| Godot 4.7 application | **Verified** | 別リポジトリの検証用Godotアプリで確認 |
| Unity application | **Not yet verified** | UnityアプリのE2E結果は未取得 |
| Unreal Engine application | **Not yet verified** | UnrealアプリのE2E結果は未取得 |

### Graphics API

| API | 状態 | 境界 |
| --- | --- | --- |
| D3D11 | **Verified** | Windows capture backend |
| D3D12 | **Verified** | Windows capture backend |
| Vulkan | **Verified** | WindowsおよびLinux/WSL2のcapture経路 |

### 操作インターフェースとテスト機能

| 機能 | 状態 | 境界 |
| --- | --- | --- |
| MCP Server | **Verified** | MCPは操作インターフェースの一つ |
| JSON Scenario / state assert | **Verified** | Scenario実行とデバイス状態assert |
| Screenshot / capture assert | **Verified** | OpenXR layer経由の描画結果assert |
| Recording / replay | **Verified** | 動画ではなくデバイス状態の軌跡 |
| 実機HMDの表示compositor | **Not yet verified** | headless null compositorの確認範囲外 |
| アプリ動作全体のdeterministic timing | **Not yet verified** | GPU、物理、非同期処理を含む決定性は未確認 |

## Quick Start：実行環境を選ぶ

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
- CMake、Git Bash、Python 3
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
- Git、Python 3、CMake、Ninja、Go Task
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

`tools/playspectra_server.py`は、`move_head`、`look`、`press`、`get_state`などの操作命令を実行し、デバイス状態を読み取るServer兼CLIです。

~~~bash
python3 tools/playspectra_server.py --cmd look --args '{"yaw_deg":90,"duration_ms":400}'
python3 tools/playspectra_server.py --cmd get_state
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
python3 tools/playspectra_server.py tools/scenarios/walk_and_look.json
~~~

完全な形式は[Scenario format](scenario-format.md)、同梱例は[`tools/scenarios/`](../tools/scenarios/)を参照してください。

### MCP

MCPは、同じPlaySpectra ServerをAIエージェントから操作するためのインターフェースです。現行実装は`tools/playspectra_mcp.py`のPythonサーバーで、stdioを使います。

次の順番で実行してください。

1. `python3 -m venv .venv-mcp`を実行して専用環境を作成してください。
2. `.venv-mcp/bin/python -m pip install -r tools/requirements.txt`を実行してください。
3. `.venv-mcp/bin/python tools/playspectra_mcp.py`を実行してください。Windows Git Bashでは`.venv-mcp/Scripts/python.exe`を使用してください。

ツール一覧は[MCP tools](mcp-tools.md)を参照してください。

## Architecture overview

PlaySpectraは、操作経路とアプリ内観測経路を分けています。

~~~mermaid
flowchart TD
  I[CLI / JSON Scenario / MCP] --> S[PlaySpectra Server]
  S --> C[Virtual Device Core]
  C --> A[Runtime Adapter]
  A --> M[Monado]
  M --> O[OpenXR application]
  L[OpenXR Instrumentation Layer] --> O
  L --> V[Screenshot / recording / diagnostics]
~~~

- CLI、JSON Scenario、MCPは、同じPlaySpectra Serverを呼び出します。
- Serverは、頭部移動、視線回転、コントローラー入力などの操作をデバイス状態へ変換します。
- Runtime AdapterはRuntimeごとの入力経路を担当します。現在の動作backendはMonadoです。
- OpenXR Instrumentation Layerはアプリ内の描画結果を観測します。Runtime Adapterの代替ではありません。

詳細な設計理由は[Architecture](architecture.md)、完全な検証根拠は[Verification matrix](verification.md)、代表テストは[Testing](testing.md)を参照してください。

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
