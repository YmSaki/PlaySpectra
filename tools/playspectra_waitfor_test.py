#!/usr/bin/env python3
"""Deterministic, self-contained test for the Server's Playwright-style auto-wait (wait_for +
retrying asserts). Runs entirely in-environment with NO Monado / graphics / real GPU: it starts a
mock Runtime-Adapter control server that speaks the exact :52702 NDJSON protocol (hello / get_state /
set_state / reset) over a real localhost socket, then drives the real Server (real ControlClient
framing, real _poll_until timing) against it. Only the adapter is mocked; the get_state path that
wait_for polls is already E2E-verified against Monado, so this verifies the new polling/retry/timeout
logic layered on top.

Run:  python tools/playspectra_waitfor_test.py
"""
import os, sys, json, socket, threading, time, copy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from playspectra_server import ControlClient, Server


def _ctrl(x):
    return {"connected": True,
            "grip": {"position": [x, 1.3, -0.5], "orientation": [0, 0, 0, 1]},
            "aim": {"position": [x, 1.3, -0.5], "orientation": [0, 0, 0, 1]},
            "inputs": {"/input/trigger/value": 0.0,
                       "/input/thumbstick/x": 0.0, "/input/thumbstick/y": 0.0}}


class MockAdapter:
    """A minimal, thread-safe stand-in for the Monado adapter control channel (:52702). The test
    mutates head z directly (set_head_z) to simulate an async state change the Server must poll for;
    the socket is only ever touched by the Server, so there is no cross-thread socket race."""
    def __init__(self):
        self.lock = threading.Lock()
        self.state = {"sequence": 0, "clock": {"mode": "realtime"},
                      "hmd": {"connected": True,
                              "head": {"position": [0.0, 1.6, 0.0], "orientation": [0, 0, 0, 1]}},
                      "left": _ctrl(-0.2), "right": _ctrl(0.2)}
        self.running = True
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

    def set_head_z(self, z):
        with self.lock:
            self.state["hmd"]["head"]["position"][2] = z

    def _serve(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                break
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn):
        conn.settimeout(0.5)
        buf = b""
        while self.running:
            try:
                data = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    req = json.loads(line)
                except ValueError:
                    continue
                reply = self._handle(req)
                try:
                    conn.sendall((json.dumps(reply) + "\n").encode())
                except OSError:
                    return

    def _handle(self, req):
        cmd = req.get("cmd")
        rid = req.get("request_id")
        if cmd == "hello":
            return {"request_id": rid, "ok": True, "role_granted": req.get("role", "writer"),
                    "descriptor": {}}
        if cmd == "get_state":
            with self.lock:
                return {"request_id": rid, "ok": True, "state": copy.deepcopy(self.state)}
        if cmd == "set_state":
            with self.lock:
                st = req.get("state", {})
                h = st.get("hmd", {}).get("head")
                if h:
                    self.state["hmd"]["head"] = copy.deepcopy(h)
                seq = st.get("sequence", self.state["sequence"])
                self.state["sequence"] = seq
                return {"request_id": rid, "ok": True, "applied": True, "sequence": seq}
        if cmd == "reset":
            with self.lock:
                self.state["hmd"]["head"]["position"] = [0.0, 1.6, 0.0]
                return {"request_id": rid, "ok": True}
        return {"request_id": rid, "ok": False, "error": "unknown cmd"}


HZ = ["hmd", "head", "position", 2]  # head z path


def main():
    m = MockAdapter()
    c = ControlClient("127.0.0.1", m.port)
    srv = Server(c, rate_hz=60.0, log=lambda *a: None)
    srv.hello("writer")

    results = []
    def check(name, cond, detail=""):
        results.append(bool(cond))
        print(("PASS" if cond else "FAIL"), name, ("- " + detail) if detail else "")

    # (a) single-shot assert (timeout_ms=0) is unchanged: passes when true, fails immediately when false
    m.set_head_z(0.0)
    check("single-shot assert PASS when condition holds",
          srv.assert_state(HZ, "near", 0.0, tol=1e-6, timeout_ms=0) is True)
    t0 = time.monotonic()
    r = srv.assert_state(HZ, "near", -2.0, tol=1e-6, timeout_ms=0)
    el = (time.monotonic() - t0) * 1000
    check("single-shot assert FAIL immediately (no wait) when false", r is False and el < 100,
          "returned=%s elapsed=%dms" % (r, el))

    # (b) wait_for catches a delayed change without any fixed sleep
    m.set_head_z(0.0)
    threading.Timer(0.3, lambda: m.set_head_z(-2.0)).start()
    t0 = time.monotonic()
    met = srv.wait_for(HZ, "near", -2.0, tol=1e-6, timeout_ms=3000, poll_ms=25)
    el = (time.monotonic() - t0) * 1000
    check("wait_for catches a delayed change", met is True, "elapsed=%dms" % el)
    check("wait_for actually polled/waited (>=200ms)", el >= 200, "elapsed=%dms" % el)

    # (c) impossible condition times out (returns False), honouring the deadline, without hanging
    m.set_head_z(0.0)
    t0 = time.monotonic()
    to = srv.wait_for(HZ, "near", 999.0, timeout_ms=350, poll_ms=25)
    el = (time.monotonic() - t0) * 1000
    check("wait_for times out on impossible condition (False)", to is False, "returned=%s" % to)
    check("timeout deadline honoured (250-1500ms)", 250 <= el <= 1500, "elapsed=%dms" % el)

    # (d) retrying assert (timeout_ms>0) waits out a delayed change; single-shot at t0 fails
    m.set_head_z(0.0)
    check("single-shot assert fails at t0 before the change",
          srv.assert_state(HZ, "near", -3.0, tol=1e-6, timeout_ms=0) is False)
    threading.Timer(0.3, lambda: m.set_head_z(-3.0)).start()
    check("retrying assert (timeout_ms>0) passes after the change",
          srv.assert_state(HZ, "near", -3.0, tol=1e-6, timeout_ms=3000, poll_ms=25) is True)

    # (e) scenario dispatch: a wait_for step then an assert step, both pass on a delayed change
    m.set_head_z(0.0)
    threading.Timer(0.25, lambda: m.set_head_z(-1.5)).start()
    srv.assertions.clear()
    summary = srv.run_scenario({"name": "waitdemo", "steps": [
        {"cmd": "wait_for", "get": HZ, "op": "near", "value": -1.5, "tol": 1e-6,
         "timeout_ms": 3000, "poll_ms": 25},
        {"cmd": "assert", "get": HZ, "op": "near", "value": -1.5, "tol": 1e-6},
    ]})
    check("scenario wait_for + assert both pass", summary["ok"] and summary["asserts"] == 2, str(summary))

    c.close()
    m.running = False
    npass = sum(results)
    print("\n=== %d/%d PASS ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print("EXC", repr(e))
        sys.exit(2)
