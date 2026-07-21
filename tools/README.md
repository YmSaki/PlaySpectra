# PlaySpectra tools — operate / observe / assert an XR app

These are the runtime-agnostic operation interfaces that sit on top of a **Runtime Adapter**'s NDJSON
control channel. In the PlaySpectra architecture they are the *PlaySpectra Server* plus its
operation interfaces (CLI / JSON Scenario Runner / MCP). They drive the app the way Playwright drives
a browser: inject the same input a controller/HMD would, observe the device state and the rendered
eye image, and assert on both.

## Channels (the operate / capture split)

- **Operate + state-observe → the Runtime Adapter, `127.0.0.1:52702`** (the Monado adapter's control
  channel; `set_state` / `get_state` / `reset`, writer-exclusive). This is the "real" input path — the
  app reads the injected poses/inputs exactly as it would real hardware.
- **Screen capture → the OpenXR layer, `127.0.0.1:52700`** (on-demand `screenshot` / recording). Capture
  must live in the layer because it reads the app's own swapchain images, in-process.

One `Server` connects to both; `--capture-port 52700` enables the capture channel.

## Files

| File | What |
|---|---|
| `playspectra_server.py` | The **Server / Scenario Runner** (operate + observe + assert + capture-assert). CLI. |
| `playspectra_record.py` | **Recorder + Replay** — observer records a state trajectory; writer replays it. |
| `playspectra_mcp.py` | **MCP server** — exposes the Server as MCP tools so an AI agent can drive/observe (needs `pip install mcp`). |
| `playspectra_mcp_verify.py` | Drives the MCP server with a real MCP client (end-to-end check). |
| `scenarios/*.json` | Sample scenarios (walk_and_look, assert_demo, capture_assert_demo, controller_ops, big_view_change). |
| `requirements.txt` | Python deps (only the MCP pieces need `mcp`; the rest are stdlib-only). |

## Prerequisites

A VR app running against a Runtime Adapter that exposes the control channel on `:52702`. In the
in-environment (WSL2) setup that is the Monado fork (`runtime/monado-playspectra`) with the
`playspectra` driver enabled, and — for capture — the PlaySpectra OpenXR layer loaded into the app
(`XR_ENABLE_API_LAYERS`). `scripts/e2e_playwright_loop.sh` is a full bring-up + verification of the
whole loop against `hello_xr`.

## Quick start

```bash
# Run a JSON scenario (operate + assert) against the adapter:
python3 tools/playspectra_server.py tools/scenarios/assert_demo.json

# Add visual-regression (screen) asserts via the layer capture channel:
python3 tools/playspectra_server.py tools/scenarios/capture_assert_demo.json --capture-port 52700

# Built-in self-checking runs (exit non-zero on failure):
python3 tools/playspectra_server.py --verify          # Server operate/observe
python3 tools/playspectra_record.py  --verify         # record + replay

# Expose the Server to an AI agent over MCP (stdio):
python3 tools/playspectra_mcp.py
```

## Scenario format

A scenario is `{"name", "steps": [ {"cmd": ...}, ... ]}`. A scenario with `assert` / `assert_capture`
steps is a self-checking test: the runner exits non-zero if any assert fails.

Operate steps:

| cmd | args | effect |
|---|---|---|
| `hello` | `role` (`writer`\|`observer`) | handshake; seeds the model from the adapter |
| `move_head` | `to:{position,orientation?}`, `duration_ms` | move the HMD viewpoint (interpolated) |
| `look` | `yaw_deg`, `duration_ms` | turn the head about world up (+Y) |
| `move_controller` | `hand`, `to:{position,orientation?}`, `duration_ms` | move a controller's grip+aim pose |
| `walk_forward` / `strafe` | `speed`, `duration_ms`, `hand` | hold a thumbstick axis, then release |
| `trigger` | `hand`, `value`, `duration_ms` | hold a trigger value |
| `press` | `hand`, `button` (a/b or x/y), `ms` | press + release a click button |
| `set_input` | `hand`, `path`, `value`, `duration_ms?` | set any declared input (float or bool) |
| `wait` | `ms` | pause |
| `reset` | — | restore the builder-initial device state |

Observe + assert steps:

| cmd | args | effect |
|---|---|---|
| `assert` | `get` (key/index path), `op` (`near`/`eq`/`ne`/`gt`/`lt`/`true`/`false`), `value`, `tol`, `name` | assert a field of the live `get_state` (e.g. `["hmd","head","position",2]`) |
| `capture` | `name`, `eye` | take a reference screenshot (layer channel) and store its hash |
| `assert_capture` | `ref`, `op` (`changed`/`stable`), `eye`, `name` | screenshot now and compare to a stored reference |

## MCP tools

`playspectra_mcp.py` exposes: `move_head`, `look`, `walk_forward`, `press`, `set_trigger`,
`move_controller`, `set_input`, `reset` (operate); `get_state` (observe); `screenshot` → an MCP image
block (the agent *sees* the rendered eye); `run_scenario` (operate + assert). The Server connects
lazily on the first tool call, so the MCP server may start before the VR app.

## Conventions

- Coordinates: STAGE space, right-handed, +Y up / +X right / −Z forward, metres. Quaternions `[x,y,z,w]`.
- `set_state` is a full snapshot; the Server holds the authoritative device model and emits the
  complete controller input set every frame, and owns a monotonic `sequence` above the adapter's
  current value (the adapter never rewinds it).
