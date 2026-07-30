# PlaySpectra（日本語版）

> **XRアプリのためのPlaywright。** PlaySpectraは、仮想HMDとコントローラーでOpenXRアプリを操作し、描画結果を観察して、再現可能なテストにします。

このページは日本語で読むための概要入口です。英語版の[トップレベルREADME](../README.md)を正典とし、詳細な構築手順・検証根拠・インターフェース仕様はリンク先の文書で管理します。

## PlaySpectraでできること

- 仮想HMDと左右コントローラーの姿勢・入力を注入する
- OpenXRアプリのスクリーンショットと録画を取得する
- JSONシナリオを再現可能な形で実行する
- デバイス状態と描画結果をassertする
- デバイス状態の軌跡を記録・再生する
- CLI、JSON Scenario、MCPから同じServerを操作する
- 実機HMDなしのヘッドレス環境でOpenXRアプリを検証する

MCPは製品本体ではなく、複数ある操作インターフェースの一つです。

## 最小実行例

MonadoのPlaySpectra adapterが`127.0.0.1:52702`で起動している状態で、同梱シナリオを実行します。

~~~bash
python3 tools/playspectra_server.py tools/scenarios/assert_demo.json
~~~

成功時はassertionの結果を表示して終了コード0になります。CLIで個別操作する場合は次のように実行します。

~~~bash
python3 tools/playspectra_server.py --cmd move_head --args '{"to":{"position":[0,1.6,-1]},"duration_ms":400}'
python3 tools/playspectra_server.py --cmd get_state
~~~

## 対応状況

ここでの状態は、設計上の対応可能性ではなく、現在確認できている証拠の範囲です。

| 項目 | 状態 |
| --- | --- |
| Windows native | 検証済み |
| Windows WSL2 / Ubuntu Linux headless | 検証済み |
| Monado virtual device backend | 検証済み |
| Native OpenXR application | 検証済み（hello_xr） |
| Godot 4.7 | 検証済み（VRAppDummyGame） |
| D3D11 / D3D12 / Vulkan | 検証済みの経路あり |
| MCP / Scenario / assert | 検証済み |
| Recording / replay | 検証済み（デバイス状態の軌跡） |
| SteamVR Adapter | Planned |
| Unity / Unreal | 未検証 |
| 実機HMDの表示compositor | 未検証 |

テスト件数、実測環境、graphics API別の根拠、negative controlは[Verification matrix](verification.md)を参照してください。

## Quick Start：実行環境を選ぶ

XR runtimeとアプリを実際に動かす環境に合わせて選択します。Windows nativeとLinuxは別のビルド経路です。WSL2はホストOSがWindowsでも、WSL内のLinuxバイナリを使います。

| 環境 | 実行経路 | 手順 |
| --- | --- | --- |
| Windows native | Windows Monado service + Windows OpenXR app | [Windows setup](getting-started-windows.md) |
| Windows WSL2 | WSL内のLinux Monado + Linux OpenXR app | [Linux / WSL2 setup](getting-started-linux.md) |
| Ubuntu Linux | native Linux Monado + Linux OpenXR app | [Linux / WSL2 setup](getting-started-linux.md) |

いずれもclone時にMonado submoduleを取得します。

<details>
<summary>Windows nativeの導入概要</summary>

Visual Studio 2022のC++ workload、CMake、Git Bash、Python 3、Vulkan SDKを用意します。Windows用Monado、OpenXR instrumentation layer、hello_xrをビルドし、次のスクリプトでruntimeとアプリを起動します。

~~~bash
bash scripts/setup_helloxr_msvc.sh
bash scripts/run_hello_xr_monado.sh D3D11
~~~

成功時は`INTEGRATION_RC=0`になります。完全な手順は[Windows setup](getting-started-windows.md)を参照してください。

</details>

<details>
<summary>Windows WSL2 / Ubuntu Linuxの導入概要</summary>

Git、Python 3、CMake、Ninja、Go Taskなどを用意し、Linux用のMonadoとlayerをビルドします。基本経路は次のとおりです。

~~~bash
git clone --recurse-submodules https://github.com/YmSaki/PlaySpectra.git
cd PlaySpectra
task bootstrap:linux
cmake -S layer -B layer/build -G Ninja
cmake --build layer/build --parallel
~~~

その後、`scripts/e2e_playwright_loop.sh`をMonado build、hello_xr、layerのパスとともに実行します。標準経路はソフトウェアVulkan（lavapipe）で、実機HMDは不要です。完全な手順は[Linux / WSL2 setup](getting-started-linux.md)を参照してください。

</details>

Windows nativeとWSL2/Ubuntuのbuild directory、CMake cache、`node_modules`は共有しないでください。プラットフォーム固有のパスやバイナリを含むためです。

## 操作インターフェース

### CLI / Server

`tools/playspectra_server.py`が高水準命令をデバイス状態へ変換するServer兼CLIです。詳細は[CLI and Server details](../tools/README.md)を参照してください。

### JSON Scenario

シナリオは`{"name": ..., "steps": [...]}`形式です。例：

~~~bash
python3 tools/playspectra_server.py tools/scenarios/walk_and_look.json
~~~

形式の詳細は[Scenario format](scenario-format.md)、同梱例は[`tools/scenarios/`](../tools/scenarios/)を参照してください。

### MCP

現行MCP実装は`tools/playspectra_mcp.py`のPythonサーバーです。stdioで起動し、Serverと同じ操作をAIエージェントへ公開します。

~~~bash
python3 -m venv .venv-mcp
.venv-mcp/bin/python -m pip install -r tools/requirements.txt
.venv-mcp/bin/python tools/playspectra_mcp.py
~~~

Windows Git Bashでは`.venv-mcp/Scripts/python.exe`を使用します。ツール一覧は[MCP tools](mcp-tools.md)を参照してください。

## アーキテクチャと検証根拠

CLI / JSON / MCP → PlaySpectra Server → Virtual Device Core → Runtime Adapter → Monado → OpenXR applicationという構成です。スクリーンショット・録画を担当するOpenXR Instrumentation Layerはruntime backendとは別軸です。

設計上の理由は[Architecture](architecture.md)、完全な検証根拠は[Verification matrix](verification.md)、代表テストは[Testing](testing.md)を参照してください。

## ドキュメント

- [English README（正典）](../README.md)
- [Windows setup](getting-started-windows.md)
- [Linux / WSL2 setup](getting-started-linux.md)
- [Architecture](architecture.md)
- [Scenario format](scenario-format.md)
- [MCP tools](mcp-tools.md)
- [Verification matrix](verification.md)
- [Testing](testing.md)
- [Roadmap](roadmap.md)
