#!/usr/bin/env python3
"""PlaySpectra MCP server — exposes the Server (operate / observe / assert / capture) as MCP tools.

This is the "VR Playwright" for AI agents: an agent can drive a VR app and observe it — move the
head/controllers, press buttons, read the device state, and SEE the rendered eye image. It wraps
playspectra_server.Server, per the settled operation-IF split: operate + state-observe via the Monado
adapter control channel (:52702), screen capture via the layer channel (:52700). MCP is one operation
interface (alongside the CLI/JSON Scenario Runner) over the same Server.

Run (stdio):  python3 tools/playspectra_mcp.py
Requires: `pip install mcp`, and a VR app running with the PlaySpectra Monado adapter (:52702). The
layer channel (:52700) is optional — only the `screenshot` tool needs it. The Server connects lazily
on the first tool call, so the MCP server can start before the VR app does.
"""
import os
import sys
import json

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mcp.server.fastmcp import FastMCP, Image
from playspectra_server import ControlClient, Server

HOST = "127.0.0.1"
MONADO_PORT = int(os.environ.get("PLAYSPECTRA_MONADO_PORT", "52702"))
LAYER_PORT = int(os.environ.get("PLAYSPECTRA_PORT", "52700"))

mcp = FastMCP("playspectra")
_srv = None


def srv() -> Server:
    """Lazily connect the Server (writer to :52702, optional capture to :52700) on first use."""
    global _srv
    if _srv is None:
        c = ControlClient(HOST, MONADO_PORT)
        cap = None
        try:
            cap = ControlClient(HOST, LAYER_PORT)
        except OSError:
            cap = None  # no layer -> screenshot tool will report it, operate/observe still work
        s = Server(c, rate_hz=60.0, log=lambda *a: None, capture=cap)
        s.hello("writer")
        _srv = s
    return _srv


def _state() -> dict:
    g = srv().c.request({"cmd": "get_state", "request_id": "mcp-gs"})
    return (g or {}).get("state", {})


@mcp.tool()
def move_head(x: float, y: float, z: float, duration_ms: int = 400) -> str:
    """Move the HMD viewpoint to a STAGE-space position in metres (x=right, y=up, z=-forward).
    Interpolated. Returns the resulting device state as JSON."""
    srv().move_head({"position": [x, y, z]}, duration_ms)
    return json.dumps(_state())


@mcp.tool()
def look(yaw_deg: float, duration_ms: int = 400) -> str:
    """Turn the head by yaw_deg about world up (+Y); positive = left. Returns the device state."""
    srv().look(yaw_deg, duration_ms)
    return json.dumps(_state())


@mcp.tool()
def walk_forward(speed: float = 1.0, duration_ms: int = 1000, hand: str = "left") -> str:
    """Hold the thumbstick forward (speed in [-1,1]) for duration, then release. Returns the state."""
    srv().walk_forward(speed, duration_ms, hand)
    return json.dumps(_state())


@mcp.tool()
def press(hand: str = "right", button: str = "a", ms: int = 120) -> str:
    """Press and release a controller button (right: a/b, left: x/y). Returns the device state."""
    srv().press(hand, button, ms)
    return json.dumps(_state())


@mcp.tool()
def set_trigger(hand: str = "right", value: float = 1.0, duration_ms: int = 200) -> str:
    """Hold a controller trigger at value in [0,1] for duration. Returns the device state."""
    srv().set_trigger(hand, value, duration_ms)
    return json.dumps(_state())


@mcp.tool()
def move_controller(hand: str, x: float, y: float, z: float, duration_ms: int = 400) -> str:
    """Move a controller (hand = "left" | "right") grip+aim to a STAGE-space position in metres.
    Interpolated. Returns the resulting device state."""
    srv().move_controller(hand, {"position": [x, y, z]}, duration_ms)
    return json.dumps(_state())


@mcp.tool()
def set_input(hand: str, path: str, value: float, duration_ms: int = 0) -> str:
    """Set an arbitrary controller input path (e.g. '/input/squeeze/value', '/input/thumbstick/x') on
    hand = "left"|"right". Use 1/0 for bool paths (/click, /touch). Returns the device state."""
    srv().set_input(hand, path, value, duration_ms)
    return json.dumps(_state())


@mcp.tool()
def reset() -> str:
    """Reset the virtual devices to the builder-initial state. Returns the device state."""
    srv().reset()
    return json.dumps(_state())


@mcp.tool()
def get_state() -> str:
    """Read the current virtual device state (HMD head pose + left/right controller grip/aim/inputs)."""
    return json.dumps(_state())


@mcp.tool()
def screenshot(eye: str = "left"):
    """Capture and return the rendered eye image (PNG) the VR app is currently showing.
    eye = "left" | "right" | "dominant". Needs the PlaySpectra layer (:52700) loaded in the app."""
    s = srv()._screenshot(eye)
    if not s.get("ok"):
        return "screenshot failed: " + str(s.get("error", "unknown"))
    return Image(path=s["path"])


@mcp.tool()
def wait_for(path_json: str, op: str = "near", value: float = 0.0, tol: float = 1e-2,
             timeout_ms: int = 5000) -> str:
    """Auto-wait (Playwright-style) until a device-state field satisfies a condition, then return it.
    Poll get_state until the field at path_json satisfies (op, value) or timeout_ms elapses -- use this
    instead of a fixed sleep before reading state. path_json is a JSON array walking the state tree,
    e.g. '["hmd","head","position",2]' for head z, or '["right","inputs","/input/trigger/value"]'.
    op: near|eq|ne|gt|lt|true|false (true/false ignore value). Returns {"met": bool, "state": {...}}."""
    try:
        path = json.loads(path_json)
    except (ValueError, TypeError):
        path = path_json  # allow a bare single key
    met = srv().wait_for(path, op, value, tol, timeout_ms)
    return json.dumps({"met": bool(met), "state": _state()})


@mcp.tool()
def run_scenario(scenario_json: str) -> str:
    """Run a PlaySpectra JSON scenario (operate + assert + capture-assert steps) and return the
    assertion summary {asserts, passed, failed, ok, failures}. The scenario is a self-checking test."""
    srv().assertions.clear()
    summary = srv().run_scenario(json.loads(scenario_json))
    return json.dumps(summary)


if __name__ == "__main__":
    mcp.run()
