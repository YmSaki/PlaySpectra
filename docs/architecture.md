# Architecture

PlaySpectra has two independent concerns:

1. **Operate and observe device state**: send virtual HMD/controller state to a Runtime Adapter and read that state back.
2. **Instrument the application**: observe rendered frames and OpenXR actions from an API layer loaded into the application process.

Keeping these concerns separate lets the runtime backend provide the input path while the layer provides in-process observation.

## System shape

~~~text
CLI / JSON Scenario Runner / MCP
              |
              v
     PlaySpectra Server
  high-level commands -> state frames
              |
              v
     Virtual Device Core
      VirtualDeviceState
              |
       Runtime Adapter(s)
          |             |
       Monado      SteamVR (planned)
          |             |
     OpenXR app   OpenVR/OpenXR app

OpenXR Instrumentation Layer
 screenshot / recording / action discovery / diagnostics
~~~

The current operation interface is the cgo-free Go `playspectra` executable. Its shared Core owns high-level commands such as move_head, look, walk_forward, strafe, controller input, reset, and scenario assertions, and converts them into interpolated full-state frames. CLI, JSON Scenario, MCP, record/replay, and process management are subcommands over that Core.

The current Monado adapter receives those frames through the NDJSON/TCP control channel at 127.0.0.1:52702. The application reads the virtual HMD and controller devices through the normal runtime path.

The OpenXR layer is a separate instrumentation path. Its control channel is 127.0.0.1:52700 and provides screenshot, recording, action discovery, diagnostics, and test-only override facilities. It reads the application's rendered swapchain images in process.

## Why there is no common driver ABI

OpenXR standardizes the application-to-runtime boundary. The runtime-to-device-driver boundary is runtime-specific. A single driver DLL cannot therefore be expected to plug into Monado, SteamVR, and every other runtime through one universal ABI.

PlaySpectra uses a common Virtual Device Core plus a Runtime Adapter for each runtime. The core carries the runtime-neutral device state; an adapter translates that state into the native device path of its runtime. The Monado adapter is the current working backend. The SteamVR adapter remains planned.

The core is implemented once, as runtime-neutral C in [`devicecore/`](../devicecore/): the NDJSON protocol, the shared `VirtualDeviceState`, and the TCP control channel. Each adapter compiles it into its runtime's process — the Monado fork pulls it into `drv_playspectra` — because runtimes query device state through synchronous in-process callbacks and accept no out-of-process devices. The adapter shell that remains runtime-side only converts the core's state into its runtime's native types (`xrt_space_relation` today, `DriverPose_t` for SteamVR later); the control channel a client connects to is owned by the core, not by the shell.

## Responsibility boundaries

- Operation interfaces are peers: CLI, JSON scenarios, and MCP all call the same Server.
- The Server interprets high-level commands and owns interpolation.
- The Virtual Device Core represents HMD and controller state without runtime-specific types.
- A Runtime Adapter exposes that state through a runtime's normal device path.
- The OpenXR layer observes the app and supports test instrumentation; it is not a replacement for the runtime backend.

The OpenXR layer's input override is a test aid. The primary input path for the current Monado E2E is the adapter control channel.

## Distribution boundary

At runtime there are three processes, and the distribution units follow them rather than a single
"native bundle":

1. The cgo-free `playspectra` executable: CLI, MCP server, JSON Scenario Runner, high-level
   operations/interpolation, state management, assertions, record/replay, doctor, and
   session/process management.
2. The runtime process hosting the device core plus its adapter shell — today `monado-service`
   with the virtual HMD/controllers compiled in (multiple executables, DLLs/shared objects,
   loader manifests, and runtime JSON files as the platform requires).
3. The target application's process, which the OpenXR instrumentation layer (with D3D11/D3D12/
   Vulkan capture) joins via the OpenXR loader. The layer never links against any runtime, and it
   lives wherever the observed application runs — not necessarily beside the runtime bundle.

The Go executable does not link the C/C++ artifacts. The units communicate only over the existing
NDJSON/TCP operate (`:52702`) and capture (`:52700`) boundaries. The compiled control plane has no
language-runtime dependency, while the native artifacts remain
independent of Go and cgo.

## Execution modes

The adapter supports realtime operation and a frame-synchronized protocol path. Frame synchronization is useful for replay and test control, but the project does not currently call the complete application behavior deterministic: display timing, GPU scheduling, physics, async loading, and dropped frames still require separate evidence.

## Contributor references

- Internal architecture record: [.claude/playspectra-architecture.md](../.claude/playspectra-architecture.md)
- Device state and protocol: [.claude/playspectra-device-core-spec.md](../.claude/playspectra-device-core-spec.md)
- SteamVR adapter plan: [.claude/steamvr-driver-plan.md](../.claude/steamvr-driver-plan.md)

