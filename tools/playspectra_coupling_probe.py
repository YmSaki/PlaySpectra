#!/usr/bin/env python3
"""Runtime-level operate -> real-app observation coupling probe (needs a LIVE stack: hello_xr on the
Windows-built Monado + the playspectra layer -- see scripts/run_hello_xr_monado.sh, which invokes this).

This is stronger than the layer's own head override. The layer's `head` command REWRITES xrLocateViews
inside the app's process, so a moved view there only proves the layer can rewrite. Here we instead:
  1. clear any layer-side override (cmd:head_clear on :52700) so we read the untouched runtime answer,
  2. drive the Monado VIRTUAL HMD at the runtime level via the operate channel (:52702, move_head),
  3. read the app's xrLocateViews result back through the observe channel (:52700, cmd:view).
A view that follows the runtime move proves runtime-level injection reaches a real app -- the actual
"does the operate path reach the app" question, not a layer rewrite.

Assertions (precise, not just "moved"): the located view's z tracks the commanded HMD z within
tolerance, while x/y stay put (a pure-z command must not drift the view sideways). Exit 0 iff all pass.

Usage: python tools/playspectra_coupling_probe.py [layer_port=52700] [operate_port=52702]
"""
import socket, json, sys, time, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from playspectra_server import ControlClient, Server

LAYER_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 52700
OP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 52702
TARGET_Z = -2.5
TOL = 0.3


def layer_rpc(sock, obj):
    sock.sendall((json.dumps(obj) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return json.loads(buf.split(b"\n", 1)[0])


def view_pose(sock):
    v = layer_rpc(sock, {"cmd": "view"})
    vs = v.get("views") or []
    return (vs[0].get("pose") if vs else None), v


def main():
    lyr = socket.create_connection(("127.0.0.1", LAYER_PORT), timeout=5)
    lyr.settimeout(5)
    # clear any layer-side override so we measure the RUNTIME pose, not a layer rewrite
    layer_rpc(lyr, {"cmd": "head_clear"})
    time.sleep(0.6)
    p0, v0 = view_pose(lyr)
    if p0 is None:
        print("FAIL: no baseline view from layer :%d" % LAYER_PORT, json.dumps(v0)); return 2

    # drive the Monado virtual HMD at the runtime level via the operate channel
    c = ControlClient("127.0.0.1", OP_PORT)
    srv = Server(c, log=lambda *a: None)
    srv.hello("coupling")
    srv.move_head({"position": [0.0, 1.6, TARGET_Z]}, 300)
    time.sleep(1.0)  # let a few xrLocateViews go by with the new runtime pose
    p1, v1 = view_pose(lyr)
    c.close(); lyr.close()
    if p1 is None:
        print("FAIL: no post-move view", json.dumps(v1)); return 2

    dz = p1["z"] - p0["z"]
    dx = abs(p1["x"] - p0["x"])
    dy = abs(p1["y"] - p0["y"])
    expected_dz = TARGET_Z - p0["z"]  # move_head sets absolute position; delta from baseline
    print("P0 = %s" % {k: round(p0[k], 3) for k in ("x", "y", "z")})
    print("P1 = %s" % {k: round(p1[k], 3) for k in ("x", "y", "z")})
    print("dz = %.3f (expected ~%.3f)  dx = %.3f  dy = %.3f" % (dz, expected_dz, dx, dy))

    results = []
    def check(name, cond, detail=""):
        results.append(bool(cond))
        print(("PASS" if cond else "FAIL"), name, ("- " + detail) if detail else "")

    check("runtime HMD move reaches the app's xrLocateViews (z tracks command)",
          abs(dz - expected_dz) < TOL, "dz=%.3f expected=%.3f" % (dz, expected_dz))
    check("pure-z command does not drift the view sideways", dx < TOL and dy < TOL,
          "dx=%.3f dy=%.3f" % (dx, dy))
    n = sum(results)
    print("=== runtime coupling %d/%d ===" % (n, len(results)))
    return 0 if n == len(results) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print("EXC", repr(e)); sys.exit(3)
