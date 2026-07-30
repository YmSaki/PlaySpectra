# PlaySpectra

[![CI](https://github.com/YmSaki/PlaySpectra/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/YmSaki/PlaySpectra/actions/workflows/ci.yml)

> **Playwright for XR applications.** PlaySpectra drives an OpenXR app with a virtual HMD and controllers, observes the rendered result, and turns the interaction into repeatable tests.

PlaySpectra can inject HMD/controller poses and input, capture screenshots and recordings, replay device-state trajectories, and assert on device state or rendered output. The same PlaySpectra Server can be reached through a CLI, JSON scenarios, or MCP. The primary target is headless testing without a physical HMD.

The current end-to-end evidence covers a native OpenXR sample (hello_xr) and Godot 4.7 (VRAppDummyGame). “Engine-independent” describes the OpenXR-level design; it does not mean that every engine has been verified. Unity and Unreal are not yet verified.

## What PlaySpectra can do

- Inject virtual HMD and left/right controller poses and inputs.
- Capture screenshots and recordings from an OpenXR application.
- Run reproducible JSON scenarios with state and visual assertions.
- Wait for device state to reach a condition before asserting it.
- Record and replay device-state trajectories.
- Expose the same operations to an AI agent through MCP.
- Test OpenXR applications in a headless Monado setup, without a physical headset.

## Minimal example

With a PlaySpectra Monado adapter listening on 127.0.0.1:52702, run the checked-in scenario:

~~~bash
python3 tools/playspectra_server.py tools/scenarios/assert_demo.json
~~~

assert_demo.json moves the head, turns it, presses a trigger, checks the resulting state, and resets the virtual devices. A successful run exits with code 0 and reports the assertion summary.

The CLI exposes the same operation vocabulary one command at a time:

~~~bash
python3 tools/playspectra_server.py --cmd move_head --args '{"to":{"position":[0,1.6,-1]},"duration_ms":400}'
python3 tools/playspectra_server.py --cmd get_state
~~~

When the application also has the PlaySpectra OpenXR layer loaded on 127.0.0.1:52700, the visual-regression example is:

~~~bash
python3 tools/playspectra_server.py tools/scenarios/capture_assert_demo.json --capture-port 52700
~~~

## Architecture overview

~~~text
 CLI / JSON Scenario Runner / MCP
                 |
                 v
        PlaySpectra Server
   high-level commands -> state frames
                 |
                 v
        Virtual Device Core
                 |
       Runtime Adapter(s)
          |             |
       Monado      SteamVR (planned)
          |             |
    OpenXR app   OpenVR/OpenXR app

 OpenXR Instrumentation Layer (separate axis)
        screenshot / recording / diagnostics
                 |
           rendered frames
~~~

In the current implementation, operation and state observation use the Monado adapter on :52702; screenshot and recording use the OpenXR layer on :52700. The layer is instrumentation, not a second runtime backend. The rationale for runtime-specific adapters, the absence of a common driver ABI, and the separation of instrumentation is documented in [Architecture](docs/architecture.md).

## Current support

These labels describe the current evidence boundary, not just what the interfaces were designed to support.

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Windows | **Verified** | Monado headless E2E on a real GPU; D3D11, D3D12, and Vulkan capture paths are covered. |
| WSL2 / Linux headless | **Verified** | Ubuntu 22.04 path with Monado and software Vulkan (lavapipe/llvmpipe). |
| Monado | **Verified** | Virtual HMD, left/right controllers, control channel, and native OpenXR app path. |
| SteamVR Adapter | **Planned** | The repository contains an earlier driver skeleton; reconnection to the current core and Windows verification remain. |
| OpenVR via OpenComposite | **Partially verified** | The OpenVR-to-OpenXR integration path has recorded passes and a skip; this is not SteamVR Adapter completion. |
| Native OpenXR application | **Verified** | hello_xr is the primary cross-engine smoke target. |
| Godot 4.7 | **Verified** | VRAppDummyGame in a separate repository; the executable is supplied through PLAYSPECTRA_VRAPP_EXE or the default sibling path. |
| Unity | **Not yet verified** | No Unity application E2E result is claimed. |
| Unreal Engine | **Not yet verified** | No Unreal application E2E result is claimed. |
| D3D11 | **Verified** | Windows capture backend. |
| D3D12 | **Verified** | Windows capture backend. |
| Vulkan | **Verified** | Windows and Linux/WSL2 capture paths. |
| MCP | **Verified** | Current Python/FastMCP server against the live Monado path. |
| Scenario / assert | **Verified** | JSON state assertions and capture-assert are implemented. |
| Recording / replay | **Verified** | Device-state trajectory recording and replay on the Monado control channel; this is not video replay. |

The physical-HMD display compositor path and deterministic frame timing are not yet verified. See the [full verification matrix](docs/verification.md) for test counts, dates, probes, graphics-API results, and negative controls.

## Quick Start by platform

Choose the environment in which the XR runtime and application will actually run. Windows native and Linux are different build families; WSL2 uses the Linux family even though the host OS is Windows.

| Environment | Runtime and app | Graphics path | Guide |
| --- | --- | --- | --- |
| Windows native | Windows Monado service + Windows OpenXR app | D3D11, D3D12, Vulkan | [Windows setup](docs/getting-started-windows.md) |
| Windows WSL2 | Linux Monado + Linux OpenXR app inside WSL2 | Vulkan, usually lavapipe | [Linux/WSL2 setup](docs/getting-started-linux.md) |
| Ubuntu Linux | Linux Monado + Linux OpenXR app | Vulkan, software or hardware ICD | [Linux/WSL2 setup](docs/getting-started-linux.md) |

All paths require the Monado submodule. Do not reuse build directories, CMake caches, or node_modules between Windows native and WSL2/Ubuntu; they contain platform-specific paths or binaries.

<details>
<summary>Windows native — Monado service + Windows OpenXR app</summary>

Prerequisites are Windows, Visual Studio 2022 with the C++ workload, CMake, Git Bash, Python 3, and a Vulkan SDK with glslang. Clone with `--recurse-submodules`, build the Windows Monado targets and the instrumentation layer, then run `bash scripts/setup_helloxr_msvc.sh` followed by `bash scripts/run_hello_xr_monado.sh D3D11`.

The harness starts the runtime and sample app in the required order, injects virtual device input, captures frames, and checks assertions. Success ends with `INTEGRATION_RC=0`. Use `D3D12`, `Vulkan`, or `all` for the other graphics paths.

See the complete [Windows setup](docs/getting-started-windows.md), including the Visual Studio/vcpkg paths and GPU-specific notes.

</details>

<details>
<summary>Windows WSL2 — Linux binaries inside WSL2</summary>

WSL2 uses the Linux build procedure. Install Git, Python 3, CMake, Ninja, Go Task, and the Ubuntu/WSL2 build prerequisites; clone with `--recurse-submodules`; run `task bootstrap:linux`; build the layer; then run `scripts/e2e_playwright_loop.sh` with the Monado build, `hello_xr`, and layer paths.

The default path uses Linux software Vulkan (`lavapipe`) and does not use the Windows Monado service or Windows `hello_xr`. Success is reported by `PASS` lines for captured output and the post-injection frame difference.

See the complete [Linux/WSL2 setup](docs/getting-started-linux.md).

</details>

<details>
<summary>Ubuntu Linux — native Linux</summary>

Ubuntu uses the same Linux procedure as WSL2: clone with `--recurse-submodules`, run `task bootstrap:linux`, build the layer with Ninja, and run the headless E2E loop. It uses Vulkan through the selected software or hardware ICD; set `VK_ICD_FILENAMES` when the default ICD is not the intended one.

See the complete [Linux/WSL2 setup](docs/getting-started-linux.md) for Ubuntu prerequisites, Monado build details, and headless constraints.

</details>

## Usage

### CLI / Server

tools/playspectra_server.py is the shared Server and one-command CLI. It sends interpolated full device states to the adapter and can read state back.

~~~bash
python3 tools/playspectra_server.py --cmd look --args '{"yaw_deg":90,"duration_ms":400}'
python3 tools/playspectra_server.py --cmd wait_for --args '{"get":["hmd","head","position",2],"op":"near","value":-1}'
~~~

See [CLI and Server details](tools/README.md).

### JSON Scenario Runner

Scenarios are {"name": ..., "steps": [...]} files. The checked-in examples cover movement, controller input, state assertions, visual assertions, and waiting.

~~~bash
python3 tools/playspectra_server.py tools/scenarios/walk_and_look.json
python3 tools/playspectra_record.py --verify
~~~

See the [scenario format](docs/scenario-format.md) and [sample scenarios](tools/scenarios/).

### MCP

MCP is one operation interface over the same Server; it is not a separate product path. The current implementation is the Python server in tools/playspectra_mcp.py and uses stdio:

~~~bash
python3 -m venv .venv-mcp
.venv-mcp/bin/python -m pip install -r tools/requirements.txt
.venv-mcp/bin/python tools/playspectra_mcp.py
~~~

On Windows Git Bash, use .venv-mcp/Scripts/python.exe instead. The server exposes movement/input, state observation, auto-wait, screenshot, reset, and run_scenario tools. See [MCP tools](docs/mcp-tools.md) for the exact list and the live verification command.

## Development and testing

The representative local gate is:

~~~bash
bash scripts/run_all_tests.sh
~~~

It runs the MCP, layer, Python-tool, and Monado-protocol unit suites. A missing toolchain is reported as a named SKIP; ALL GREEN and GREEN WITH SKIPS are intentionally distinct. Live Monado/app E2E is separate because it needs a runtime and application process. GitHub Actions runs the environment-independent MCP suite on Ubuntu and layer suite on Windows; the workflow is [.github/workflows/ci.yml](.github/workflows/ci.yml).

See [Testing](docs/testing.md) for individual commands, suite counts, CI boundaries, and skip conditions.

## Project status and roadmap

Current:

- Monado virtual-device backend: working and verified on Windows headless and Linux/WSL2 headless paths.
- Python PlaySpectra Server, JSON runner, recorder/replayer, and MCP server: current interfaces.
- OpenXR instrumentation layer: screenshot, recording, action discovery, diagnostics, and test-only overrides.
- Native hello_xr and Godot 4.7 VRAppDummyGame: verified application targets.

Planned or not yet verified:

- SteamVR Adapter: reconnect the earlier skeleton to the current Virtual Device Core and verify it on Windows.
- Unity and Unreal: application E2E verification is still outstanding.
- Physical-HMD display compositor and deterministic frame timing: not yet verified.
- Legacy TypeScript MCP in mcp/: scheduled for retirement; it is not the recommended MCP interface.

The [roadmap](docs/roadmap.md) keeps these boundaries separate from current features.

## Documentation index

- [Architecture](docs/architecture.md)
- [Linux / WSL2 setup](docs/getting-started-linux.md)
- [Windows setup](docs/getting-started-windows.md)
- [Scenario format](docs/scenario-format.md)
- [MCP tools](docs/mcp-tools.md)
- [Verification matrix](docs/verification.md)
- [Testing](docs/testing.md)
- [Roadmap](docs/roadmap.md)
