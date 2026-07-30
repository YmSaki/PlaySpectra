# PlaySpectra

[![CI](https://github.com/YmSaki/PlaySpectra/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/YmSaki/PlaySpectra/actions/workflows/ci.yml)

[English](README.md) | [日本語](docs/readme.ja.md)

> **Playwright for XR applications.** PlaySpectra drives an OpenXR app with a virtual HMD and controllers, observes the rendered result, and turns the interaction into repeatable tests.

PlaySpectra can inject HMD/controller poses and input, capture screenshots and recordings, replay device-state trajectories, and assert on device state or rendered output. The same PlaySpectra Server can be reached through a CLI, JSON scenarios, or MCP. The primary target is headless testing without a physical HMD.

The current end-to-end evidence covers a native OpenXR application and a Godot 4.7 application. “Engine-independent” describes the OpenXR-level design; it does not mean that every engine has been verified. Unity and Unreal are not yet verified.

## What PlaySpectra can do

- Inject virtual HMD and left/right controller poses and inputs.
- Capture screenshots and recordings from an OpenXR application.
- Run reproducible JSON scenarios with state and visual assertions.
- Wait for device state to reach a condition before asserting it.
- Record and replay device-state trajectories.
- Expose the same operations to an AI agent through MCP.
- Test OpenXR applications in a headless Monado setup, without a physical headset.

## Minimal example

Run the following steps from a second terminal after starting the Monado adapter and an OpenXR application with the [Windows](docs/getting-started-windows.md) or [Linux / WSL2](docs/getting-started-linux.md) guide:

1. Start the PlaySpectra Monado adapter. Its operation channel normally listens on `127.0.0.1:52702`.
2. From the repository root, run the checked-in scenario:

~~~bash
python3 tools/playspectra_server.py tools/scenarios/assert_demo.json
~~~

3. Confirm the assertion summary. A successful run exits with code 0. The scenario moves the head, turns it, presses a trigger, checks the resulting state, and resets the virtual devices.

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

PlaySpectra separates the device-operation path from the in-process application-observation path.

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

- CLI, JSON scenarios, and MCP call the same PlaySpectra Server.
- The Server converts high-level commands into virtual HMD/controller state frames.
- A Runtime Adapter translates that state into a runtime's native device path. Monado is the current working backend.
- The OpenXR layer observes the application process; it is instrumentation, not a second runtime backend.

The Monado operation channel is `127.0.0.1:52702`. The layer capture channel is `127.0.0.1:52700`. The rationale for runtime-specific adapters, the absence of a common driver ABI, and the separation of instrumentation is documented in [Architecture](docs/architecture.md).

## Current support

These labels describe the current evidence boundary, not just what the interfaces were designed to support. If an item has not been measured in an application or runtime path, it is not presented as verified.

### Environments and runtimes

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Windows native | **Verified** | Monado headless E2E on a real GPU. |
| Windows WSL2 | **Verified** | Linux Monado and software Vulkan inside WSL2. |
| Ubuntu Linux headless | **Verified** | Ubuntu 22.04 headless path. |
| Monado virtual-device backend | **Verified** | Virtual HMD, controllers, control channel, and native OpenXR path. |
| SteamVR Adapter | **Planned** | Reconnection to the current core and Windows verification remain. |
| OpenVR via OpenComposite | **Partially verified** | OpenVR-to-OpenXR conversion path only; this is not SteamVR Adapter completion. |

### Application targets

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Native OpenXR application | **Verified** | Native OpenXR application path. |
| Godot 4.7 application | **Verified** | Separate verification application and its OpenXR path. |
| Unity application | **Not yet verified** | No Unity application E2E result is claimed. |
| Unreal Engine application | **Not yet verified** | No Unreal application E2E result is claimed. |

### Graphics APIs

| API | Status | Scope or boundary |
| --- | --- | --- |
| D3D11 | **Verified** | Windows capture backend. |
| D3D12 | **Verified** | Windows capture backend. |
| Vulkan | **Verified** | Windows and Linux/WSL2 capture paths. |

### Interfaces and test features

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Python MCP server | **Verified** | Current Python/FastMCP server against the live Monado path. MCP is one operation interface. |
| Scenario / state assert | **Verified** | JSON state assertions. |
| Screenshot / capture assert | **Verified** | OpenXR-layer capture assertions. |
| Recording / replay | **Verified** | Device-state trajectory recording and replay; this is not video replay. |
| Physical-HMD display compositor | **Not yet verified** | Headless null-compositor evidence does not cover physical display presentation. |
| Deterministic application timing | **Not yet verified** | GPU scheduling, physics, async loading, and dropped-frame behavior need separate evidence. |

See the [full verification matrix](docs/verification.md) for test counts, dates, probes, graphics-API results, and negative controls.

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

**Environment**

- OS: Windows native.
- Runtime: Windows Monado service.
- Application: Windows OpenXR application.
- Graphics: D3D11, D3D12, or Vulkan.
- Prerequisites: Visual Studio 2022 with the C++ workload, CMake, Git Bash, Python 3, and a Vulkan SDK with glslang.

**Steps**

1. Clone the repository with `--recurse-submodules`.
2. Follow the [Windows setup](docs/getting-started-windows.md) steps to build the Windows Monado targets and instrumentation layer.
3. Run `bash scripts/setup_helloxr_msvc.sh` to prepare the Windows OpenXR sample application.
4. Run `bash scripts/run_hello_xr_monado.sh D3D11`. The harness starts the runtime and application in the required order, injects virtual device input, captures frames, and checks assertions.
5. Confirm `INTEGRATION_RC=0`. Use `D3D12`, `Vulkan`, or `all` for the other graphics paths.

See the complete [Windows setup](docs/getting-started-windows.md), including the Visual Studio/vcpkg paths and GPU-specific notes.

</details>

<details>
<summary>Windows WSL2 — Linux binaries inside WSL2</summary>

**Environment**

- OS: Linux inside Windows WSL2.
- Runtime: Linux Monado inside WSL2.
- Application: Linux OpenXR application inside WSL2.
- Graphics: Vulkan, usually software Vulkan (`lavapipe`).
- Prerequisites: Git, Python 3, CMake, Ninja, Go Task, and the Ubuntu/WSL2 build dependencies.

**Steps**

1. Clone the repository with `--recurse-submodules` inside WSL2.
2. Run `task bootstrap:linux` to install Linux dependencies, initialize the submodule, and build Monado.
3. Build the layer with the commands in [Linux / WSL2 setup](docs/getting-started-linux.md).
4. Run `scripts/e2e_playwright_loop.sh` with the Monado build, `hello_xr`, and layer paths.
5. Confirm `PASS` lines for distinct captured contents and the post-injection frame difference.

This path does not use the Windows Monado service or Windows OpenXR application. It uses Linux binaries inside WSL2.

See the complete [Linux/WSL2 setup](docs/getting-started-linux.md).

</details>

<details>
<summary>Ubuntu Linux — native Linux</summary>

**Environment**

- OS: native Ubuntu Linux.
- Runtime: native Linux Monado.
- Application: Linux OpenXR application.
- Graphics: Vulkan through a software or hardware ICD.
- Prerequisites: Git, Python 3, CMake, Ninja, Go Task, and the Ubuntu build dependencies.

**Steps**

1. Clone the repository with `--recurse-submodules`.
2. Run `task bootstrap:linux` to install dependencies, initialize the submodule, and build Monado.
3. Build the layer with Ninja as described in [Linux / WSL2 setup](docs/getting-started-linux.md).
4. Run the headless E2E loop with the Monado build, OpenXR application, and layer paths.
5. Confirm the `PASS` lines for captured output and the post-injection frame difference.

Set `VK_ICD_FILENAMES` when the default Vulkan ICD is not the intended one. See the complete [Linux / WSL2 setup](docs/getting-started-linux.md) for Ubuntu prerequisites, Monado build details, and headless constraints.

</details>

## Usage

### CLI / Server

`tools/playspectra_server.py` is both the shared Server and a one-command CLI. It turns high-level HMD/controller operations into device-state frames and can read the resulting state back.

~~~bash
python3 tools/playspectra_server.py --cmd look --args '{"yaw_deg":90,"duration_ms":400}'
python3 tools/playspectra_server.py --cmd wait_for --args '{"get":["hmd","head","position",2],"op":"near","value":-1}'
~~~

See [CLI and Server details](tools/README.md).

### JSON Scenario Runner

A JSON Scenario is an ordered sequence of operations and assertions. Its minimum structure is:

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

The checked-in examples cover movement, controller input, state assertions, visual assertions, and waiting. A failing assertion makes the runner exit non-zero.

~~~bash
python3 tools/playspectra_server.py tools/scenarios/walk_and_look.json
python3 tools/playspectra_record.py --verify
~~~

See the [scenario format](docs/scenario-format.md) and [sample scenarios](tools/scenarios/).

### MCP

MCP is one operation interface over the same Server; it is not a separate product path. The current implementation is the Python server in `tools/playspectra_mcp.py` and uses stdio.

Set it up in a dedicated environment:

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
