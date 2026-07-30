#!/usr/bin/env python3
"""End-to-end check of the PlaySpectra MCP server: spawn tools/playspectra_mcp.py over stdio with a
real MCP client and exercise operate / observe / screenshot / scenario tools.

Prerequisite: a VR app running with the PlaySpectra Monado adapter (:52702) and, for the screenshot
tool, the layer (:52700) -- e.g. hello_xr -g Vulkan2 with the layer loaded (see
scripts/e2e_playwright_loop.sh for the bring-up). Then: `python3 tools/playspectra_mcp_verify.py`.
Requires `pip install mcp`.
"""
import asyncio, json, os, sys
from mcp.client.stdio import stdio_client, StdioServerParameters
from mcp.client.session import ClientSession

MCP_PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                              "playspectra_mcp.py")

def _text(res):
    for c in res.content:
        if getattr(c, "type", None) == "text":
            return c.text
    return None

def _types(res):
    return [getattr(c, "type", None) for c in res.content]

async def main():
    results = []
    def check(n, cond, d=""):
        results.append((n, bool(cond))); print(("PASS" if cond else "FAIL"), n, "-", d)

    # spawn the server under the SAME interpreter running this verify (not a bare "python3", which on
    # Windows resolves to a different install that may lack `mcp` -- ModuleNotFoundError at spawn).
    async with stdio_client(StdioServerParameters(command=sys.executable, args=[MCP_PATH])) as (r, w):
        async with ClientSession(r, w) as session:
            await session.initialize()
            names = [t.name for t in (await session.list_tools()).tools]
            need = {"move_head", "look", "walk_forward", "strafe", "press", "set_trigger", "move_controller",
                    "set_input", "get_state", "screenshot", "reset", "wait_for", "run_scenario"}
            check("tools listed", need.issubset(set(names)), str(sorted(names)))

            st = json.loads(_text(await session.call_tool("move_head", {"x": 0.0, "y": 1.6, "z": -1.5})))
            check("move_head -> z=-1.5", abs(st["hmd"]["head"]["position"][2] + 1.5) < 0.05,
                  "z=%s" % st["hmd"]["head"]["position"][2])

            st = json.loads(_text(await session.call_tool("look", {"yaw_deg": 90.0})))
            check("look 90deg -> quat.y~0.707", abs(st["hmd"]["head"]["orientation"][1] - 0.7071) < 0.05,
                  "qy=%s" % st["hmd"]["head"]["orientation"][1])

            st = json.loads(_text(await session.call_tool("get_state", {})))
            check("get_state reflects state", abs(st["hmd"]["head"]["position"][2] + 1.5) < 0.05,
                  "z=%s" % st["hmd"]["head"]["position"][2])

            res = await session.call_tool("screenshot", {"eye": "left"})
            check("screenshot returns an image", "image" in _types(res), "content=%s" % _types(res))

            scen = json.dumps({"name": "mcp_inline", "steps": [
                {"cmd": "move_head", "to": {"position": [0.0, 1.6, -0.5]}, "duration_ms": 200},
                {"cmd": "assert", "get": ["hmd", "head", "position", 2], "op": "near", "value": -0.5, "tol": 0.05},
            ]})
            summ = json.loads(_text(await session.call_tool("run_scenario", {"scenario_json": scen})))
            check("run_scenario assert passes", summ.get("ok") and summ.get("passed") == 1, str(summ))

            # --- exercise the remaining tools so an arg-mapping bug in the agent-facing wrapper (like
            # the scenario `trigger` vs `set_trigger` name mismatch found this session) can't hide. ---
            await session.call_tool("move_controller", {"hand": "right", "x": 0.7, "y": 1.1, "z": -0.3})
            st = json.loads(_text(await session.call_tool("get_state", {})))
            check("move_controller -> right grip (x/y/z args -> position)",
                  abs(st["right"]["grip"]["position"][0] - 0.7) < 0.05 and abs(st["right"]["grip"]["position"][1] - 1.1) < 0.05,
                  "grip=%s" % st["right"]["grip"]["position"])

            await session.call_tool("set_input", {"hand": "left", "path": "/input/squeeze/value", "value": 0.9})
            st = json.loads(_text(await session.call_tool("get_state", {})))
            check("set_input -> left squeeze 0.9", abs(st["left"]["inputs"]["/input/squeeze/value"] - 0.9) < 0.05,
                  "v=%s" % st["left"]["inputs"]["/input/squeeze/value"])

            await session.call_tool("set_trigger", {"hand": "right", "value": 0.8})
            st = json.loads(_text(await session.call_tool("get_state", {})))
            check("set_trigger -> right trigger 0.8", abs(st["right"]["inputs"]["/input/trigger/value"] - 0.8) < 0.05,
                  "v=%s" % st["right"]["inputs"]["/input/trigger/value"])

            wf = json.loads(_text(await session.call_tool("wait_for", {
                "path_json": json.dumps(["right", "inputs", "/input/trigger/value"]),
                "op": "near", "value": 0.8, "tol": 0.05, "timeout_ms": 1000})))
            check("wait_for on already-true condition -> met", wf.get("met") is True, str(wf.get("met")))

            rw = _text(await session.call_tool("walk_forward", {"speed": 1.0, "duration_ms": 150}))
            check("walk_forward returns state (no error)", isinstance(rw, str) and len(rw) > 0, "len=%s" % (len(rw) if rw else None))
            rs = _text(await session.call_tool("strafe", {"speed": 1.0, "duration_ms": 150}))
            check("strafe returns state (no error)", isinstance(rs, str) and len(rs) > 0, "len=%s" % (len(rs) if rs else None))
            rp = _text(await session.call_tool("press", {"hand": "right", "button": "a", "ms": 100}))
            check("press returns state (no error)", isinstance(rp, str) and len(rp) > 0, "len=%s" % (len(rp) if rp else None))

            st = json.loads(_text(await session.call_tool("reset", {})))
            check("reset -> z~0", abs(st["hmd"]["head"]["position"][2]) < 0.01,
                  "z=%s" % st["hmd"]["head"]["position"][2])

    npass = sum(1 for _, ok in results if ok)
    print("\n=== %d/%d PASS ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
