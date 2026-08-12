# MCP tools

The current MCP interface is the stdio server built into the cgo-free Go `playspectra` executable. It uses the same Core as the CLI and JSON runner.

## Start the server

Start the MCP frontend directly:

~~~bash
playspectra mcp
~~~

The compiled executable has no language-runtime or package dependency. The server connects lazily on the first tool call.

The server uses:

- 127.0.0.1:52702, or PLAYSPECTRA_MONADO_PORT, for operation and state observation.
- 127.0.0.1:52700, or PLAYSPECTRA_PORT, for screenshot capture.
- The screenshot tool requires the application to have the PlaySpectra layer loaded. The other tools only need the operation channel.

## Tools

| Tool | Parameters | Result |
| --- | --- | --- |
| move_head | x, y, z, optional duration_ms=400 | Move the HMD viewpoint in STAGE metres and return state JSON. |
| look | yaw_deg, optional duration_ms=400 | Turn the head and return state JSON. |
| walk_forward | optional speed=1.0, duration_ms=1000, hand=left | Hold the hand thumbstick forward, release it, and return state JSON. |
| strafe | optional speed=1.0, duration_ms=1000, hand=left | Hold the hand thumbstick sideways, release it, and return state JSON. |
| press | optional hand=right, button=a, ms=120 | Press and release a button. |
| set_trigger | optional hand=right, value=1.0, duration_ms=200 | Hold a trigger value for a duration. |
| move_controller | hand, x, y, z, optional duration_ms=400 | Move the selected controller grip and aim pose. |
| set_input | hand, path, value, optional duration_ms=0 | Set an arbitrary declared controller input path. |
| reset | none | Restore builder-initial virtual-device state. |
| get_state | none | Return HMD/controller state as JSON text. |
| screenshot | optional eye=left | Return an MCP image from the rendered eye image. Requires the layer. |
| wait_for | path_json, optional op=near, value=0.0, tol=0.01, timeout_ms=5000 | Poll state until the condition is met and return met plus state. |
| run_scenario | scenario_json | Run a serialized JSON scenario and return the assertion summary. |

The current implementation exposes 13 tools. Their names, schemas, defaults, descriptions, and result shapes are compatibility-tested.

## Live verification

Against the Windows Monado stack:

~~~bash
go build -o build/playspectra.exe ./cmd/playspectra
PLAYSPECTRA_BIN="$PWD/build/playspectra.exe" scripts/run_mcp_verify_monado.sh D3D11
~~~

The verifier starts the MCP server over stdio, lists the tools, exercises operation and state observation, requests a screenshot, and runs an inline scenario. It needs a running stack; it is not a unit test.

