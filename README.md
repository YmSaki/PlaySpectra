# PlaySpectra

[![CI](https://github.com/YmSaki/PlaySpectra/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/YmSaki/PlaySpectra/actions/workflows/ci.yml)

[English](README.md) | [日本語](docs/readme.ja.md)

> **A virtual headset and controllers that AI agents can use.**

PlaySpectra is an XR operation adapter that connects AI agents to VR, AR, and MR applications.

With PlaySpectra-MCP, an AI agent can enter an XR application without a physical headset, operate the application, and observe its state and rendered world.

When a procedure needs to be fixed and repeated, PlaySpectra can be used through PlaySpectra-CLI or JSON Scenarios. The same operation path can therefore be used for headless automated testing.

The current end-to-end evidence covers a native OpenXR application and a Godot 4.7 application. VR/AR/MR is the target domain; an AR/MR-specific application path is not yet verified.

## What PlaySpectra enables

| Use case | What PlaySpectra enables |
| --- | --- |
| AI-agent XR operation and observation | An AI agent operates an XR application and observes its state and rendered output. |
| XR application development | Check whether head and controller input reaches an application without wearing a headset. |
| Reproducible interaction | Repeat the same head movement, walking, and controller actions. |
| Headless operation | Run an XR application from a server, CI job, WSL2 environment, or Linux machine. |
| Automated testing | Assert that an application's state and rendered output match expectations after fixed actions. |

## Minimal example

The PlaySpectra Server/CLI connects to an OpenXR application that is already running on a Monado Runtime Adapter; it does not start the runtime or application by itself. Choose the environment in which you want to try it, complete that platform's build steps, and then use one of the existing bring-up commands below. Those bring-up scripts start the runtime and sample application for you.

These commands are intentionally shown here as well as in the platform guides: they are the shortest path from a completed build to a running sample application.

| Environment | Start the verified sample run | What the command starts and checks |
| --- | --- | --- |
| Windows native | `scripts/run_scenario_e2e_monado.sh D3D11` | Windows Monado service, `hello_xr`, the capture layer, and `capture_assert_demo.json`; exits non-zero on failure. |
| Windows WSL2 | `MONADO_BUILD="$PWD/build/monado" HELLOXR="$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" LAYER_SO="$PWD/layer/build/playspectra_layer.so" bash scripts/e2e_playwright_loop.sh` | Linux Monado and `hello_xr` inside WSL2; injects a pose and checks that the captured frame changes. |
| Ubuntu Linux | `MONADO_BUILD="$PWD/build/monado" HELLOXR="$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" LAYER_SO="$PWD/layer/build/playspectra_layer.so" bash scripts/e2e_playwright_loop.sh` | Native Linux Monado and `hello_xr`; injects a pose and checks that the captured frame changes. |

The Windows command must be run from Git Bash after the [Windows setup](docs/getting-started-windows.md). The WSL2 and Ubuntu command must be run from the repository root after the [Linux/WSL2 setup](docs/getting-started-linux.md). Those commands start the runtime and sample application in the required order, so you do not need to start `monado-service` or `hello_xr` separately for this first run.

If you want to run the JSON Scenario directly, run one of the following blocks in a terminal. Each block starts the runtime and sample application, waits for the control channel, runs the Scenario, and then stops the application.

**Windows native (Git Bash)**

~~~bash
set -e
source scripts/lib_monado_stack.sh
mstack_env D3D11
mstack_up D3D11 120
trap mstack_down EXIT
python tools/playspectra_server.py tools/scenarios/assert_demo.json
~~~

**WSL2 or Ubuntu (bash)**

~~~bash
set -e
export PLAYSPECTRA_ENABLE=1 XRT_COMPOSITOR_NULL=1
export XR_RUNTIME_JSON="$PWD/build/monado/openxr_monado-dev.json"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}"
sleep 120 | "$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" -g Vulkan2 &
APP_PID=$!
trap 'kill "$APP_PID" 2>/dev/null || true; wait "$APP_PID" 2>/dev/null || true' EXIT
python - <<'PY'
import socket, sys, time
for _ in range(60):
    with socket.socket() as sock:
        sock.settimeout(0.3)
        if sock.connect_ex(("127.0.0.1", 52702)) == 0:
            sys.exit(0)
    time.sleep(0.3)
raise SystemExit("PlaySpectra control channel :52702 did not become ready")
PY
python tools/playspectra_server.py tools/scenarios/assert_demo.json
~~~

The Scenario moves the head, turns it, presses a right trigger, asserts the resulting state, and resets the virtual devices. When all assertions pass, the runner exits with status 0.

To issue individual operations through PlaySpectra-CLI, run:

~~~bash
python tools/playspectra_server.py --cmd move_head --args '{"to":{"position":[0,1.6,-1]},"duration_ms":400}'
python tools/playspectra_server.py --cmd get_state
~~~

## Current support

The tables below use one classification axis per table. Status is shown for each item instead of grouping different kinds of items by status.

Legend: ✅ verified, ⚠️ partially verified, 🔍 not yet verified, 🚧 planned or not implemented. 🔍 means that the current operation path or target exists but the specific condition has not yet been checked.

### Execution environments

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Windows native | ✅ | Monado headless E2E on a real GPU. |
| Windows WSL2 | ✅ | Linux Monado and software Vulkan inside WSL2. |
| Ubuntu Linux | ✅ | Ubuntu 22.04 headless path. |

### Runtime adapters

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Monado Adapter | ✅ | Virtual HMD, controllers, control channel, and native OpenXR path. |
| OpenVR via OpenComposite | ⚠️ | OpenVR-to-OpenXR conversion path only. |
| SteamVR Adapter | 🚧 | Adapter for the current core is not implemented. |

### Application targets

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Native OpenXR application | ✅ | Native OpenXR application path. |
| Godot 4.7 application | ✅ | Separate verification application and its OpenXR path. |
| Unity application | 🔍 | Unity application E2E is not yet verified. |
| Unreal Engine application | 🔍 | Unreal application E2E is not yet verified. |
| AR/MR-specific application | 🔍 | AR/MR application E2E is not yet verified. |

### Graphics and capture

| Area | Status | Scope or boundary |
| --- | --- | --- |
| D3D11 | ✅ | Windows capture backend. |
| D3D12 | ✅ | Windows capture backend. |
| Vulkan | ✅ | Windows and Linux/WSL2 capture paths. |
| Physical-HMD display compositor | 🔍 | Headless null-compositor evidence does not cover physical display presentation. |

### Operation interfaces

| Area | Status | Scope or boundary |
| --- | --- | --- |
| PlaySpectra-MCP | ✅ | AI-agent operation and observation through MCP. |
| PlaySpectra-CLI | ✅ | Individual operation and state observation. |
| JSON Scenario | ✅ | Repeatable operation procedures. |

### Verification and recording

| Area | Status | Scope or boundary |
| --- | --- | --- |
| Device-state assertion | ✅ | State assertions from Server and scenarios. |
| Screenshot / capture assertion | ✅ | Rendered-output assertions through the OpenXR layer. |
| Recording / replay | ✅ | Device-state trajectories; this is not video replay. |
| Deterministic application timing | 🔍 | GPU scheduling, physics, async loading, and dropped frames are not yet verified. |

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
- Prerequisites: Visual Studio 2022 with MSVC and the C++ workload, CMake, Git Bash, Python 3, and a Vulkan SDK with glslang.

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
- Prerequisites: Git, Python 3, CMake, Ninja, Go Task, `build-essential` (including GCC/G++), and the Ubuntu/WSL2 build dependencies.

**Steps**

1. Clone the repository with `--recurse-submodules` inside WSL2.
2. Run `task bootstrap:linux` to install Linux dependencies, initialize the submodule, and build Monado.
3. Build the layer with the commands in [Linux / WSL2 setup](docs/getting-started-linux.md).
4. Run `scripts/e2e_playwright_loop.sh` with the Monado build, `hello_xr`, and layer paths.
5. Confirm `PASS` lines for distinct captured contents and the post-injection frame difference.

This path does not use the Windows Monado service or Windows OpenXR application. It uses Linux binaries inside WSL2.

On WSL2, use the GCC/G++ installed inside the Linux distribution. Do not substitute a Windows MSYS2 compiler for the Linux build.

See the complete [Linux/WSL2 setup](docs/getting-started-linux.md).

</details>

<details>
<summary>Ubuntu Linux — native Linux</summary>

**Environment**

- OS: native Ubuntu Linux.
- Runtime: native Linux Monado.
- Application: Linux OpenXR application.
- Graphics: Vulkan through a software or hardware ICD.
- Prerequisites: Git, Python 3, CMake, Ninja, Go Task, `build-essential` (including GCC/G++), and the Ubuntu build dependencies.

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

`tools/playspectra_server.py` is both the shared Server and a one-command CLI. It executes commands such as `move_head`, `look`, `press`, and `get_state`, and can read the resulting device state back.

~~~bash
python tools/playspectra_server.py --cmd look --args '{"yaw_deg":90,"duration_ms":400}'
python tools/playspectra_server.py --cmd wait_for --args '{"get":["hmd","head","position",2],"op":"near","value":-1}'
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
python tools/playspectra_server.py tools/scenarios/walk_and_look.json
python tools/playspectra_record.py --verify
~~~

See the [scenario format](docs/scenario-format.md) and [sample scenarios](tools/scenarios/).

### MCP

PlaySpectra-MCP is the interface that lets an AI agent operate and observe an XR application. It uses the same operation model as PlaySpectra-CLI and JSON Scenarios. The current server uses stdio and is implemented in `tools/playspectra_mcp.py`.

Set it up in a dedicated environment:

~~~bash
python -m venv .venv-mcp
.venv-mcp/bin/python -m pip install -r tools/requirements.txt
.venv-mcp/bin/python tools/playspectra_mcp.py
~~~

On Windows Git Bash, use .venv-mcp/Scripts/python.exe instead. The server exposes movement/input, state observation, auto-wait, screenshot, reset, and run_scenario tools. See [MCP tools](docs/mcp-tools.md) for the exact list and the live verification command.

## Architecture overview

The user-facing model is an AI agent operating and observing an XR application through a virtual headset and controllers.

Internally, the operation path is separated from the in-process observation path:

~~~mermaid
flowchart TD
  I[AI agent / CLI / JSON Scenario] --> M[PlaySpectra-MCP / Server]
  M --> A[Common operation model]
  A --> V[Virtual HMD / controllers]
  V --> R[Runtime Adapter]
  R --> X[OpenXR application]
  L[OpenXR Instrumentation Layer] --> X
  L --> O[State / screenshot / recording observation]
~~~

- The common operation model represents actions such as head movement, gaze rotation, and controller input.
- A Runtime Adapter delivers those actions through a runtime's device path. Monado is the current working adapter.
- The OpenXR layer observes the application process; it is not a replacement for the Runtime Adapter.
- The same operation model supports AI operation, CLI/JSON replay, and automated assertions.

The Monado operation channel is `127.0.0.1:52702`. The layer capture channel is `127.0.0.1:52700`. Detailed design rationale is documented in [Architecture](docs/architecture.md).

## Development and testing

The representative local gate is:

~~~bash
bash scripts/run_all_tests.sh
~~~

It runs the MCP, layer, Python-tool, and Monado-protocol unit suites. A missing toolchain is reported as a named SKIP; ALL GREEN and GREEN WITH SKIPS are intentionally distinct. Live Monado/app E2E is separate because it needs a runtime and application process. GitHub Actions runs the environment-independent MCP suite on Ubuntu and layer suite on Windows; the workflow is [.github/workflows/ci.yml](.github/workflows/ci.yml).

See [Testing](docs/testing.md) for individual commands, suite counts, CI boundaries, and skip conditions.

## Project status and roadmap

The [support tables above](#current-support) are the authoritative status summary.

Roadmap items are separate from current capabilities:

- SteamVR Adapter: implement the adapter for the current Virtual Device Core and verify it on Windows.
- Unity, Unreal, and AR/MR-specific applications: add application E2E evidence.
- Physical-HMD display compositor and deterministic application timing: collect the missing evidence.
- Legacy TypeScript MCP in `mcp/`: retire it after the current MCP interface has fully replaced it.

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
