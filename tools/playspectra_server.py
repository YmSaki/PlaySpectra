#!/usr/bin/env python3
"""PlaySpectra Server / Scenario Runner (minimal, in-environment verifiable).

Architecture (playspectra-architecture.md): 操作IF -> PlaySpectra Server(高水準命令の解釈・補間)
-> Virtual Device Core -> Runtime Adapter. This module is the Server plus a JSON Scenario Runner
operation interface. It interprets high-level commands (walk_forward, look, move_head, press, ...)
into VirtualDeviceState *full-snapshot* set_state sequences and streams them, with Server-owned
interpolation and Server-owned monotonic sequence (spec §4/§6), to a Runtime Adapter's NDJSON/TCP
control channel (Monado adapter = 127.0.0.1:52702).

Key contract points honoured:
- set_state is a complete snapshot (spec §2.2): the Server holds the authoritative device model and
  emits the *complete* controller input set every frame, because the adapter zeroes any input path
  that a frame omits (apply_ctrl U_ZERO). Partial frames would silently clear inputs.
- The Server owns `sequence` (spec §4, realtime latest-wins) and increments it per emitted frame.
- Interpolation (position lerp, orientation slerp, stick ramp) lives in the Server, not the Driver
  (spec §6: "補間列の生成も Server 側").

Usage:
  python playspectra_server.py <scenario.json> [--host H] [--port P] [--rate HZ] [--verify]
  python playspectra_server.py --demo [--verify]      # run a built-in demo scenario

`--verify` reads the state back (get_state) at checkpoints and asserts the scripted effect, so the
run is self-checking against a live adapter (CLAUDE.md: 完了判定は機械値で).
"""
import socket, json, sys, time, math, argparse, hashlib, os

# ---- control channel (NDJSON over TCP), same framing as the E2E harnesses ----

class ControlClient:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.settimeout(1.0)
        self.rbuf = b""

    def _readline(self):
        while b"\n" not in self.rbuf:
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.rbuf += chunk
        line, self.rbuf = self.rbuf.split(b"\n", 1)
        return line.decode("utf-8")

    def request(self, obj, timeout=3.0):
        """Send a request and return the matching response (skips async events)."""
        self.s.sendall((json.dumps(obj) + "\n").encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline()
            if line is None or not line.strip():
                continue
            o = json.loads(line)
            if "event" in o:
                continue
            if o.get("request_id") == obj.get("request_id"):
                return o
        return None

    def req_line(self, obj, timeout=10.0):
        """Send a request and return the next reply line parsed. For the layer capture channel
        (:52700), whose protocol is one-line-request / one-line-reply with no request_id echo."""
        self.s.sendall((json.dumps(obj) + "\n").encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline()
            if line is None or not line.strip():
                continue
            return json.loads(line)
        return None

    def send_only(self, obj):
        """Fire-and-forget (used for high-rate interpolation frames)."""
        self.s.sendall((json.dumps(obj) + "\n").encode())

    def drain(self):
        """Discard any buffered responses/events without blocking."""
        self.s.setblocking(False)
        try:
            while True:
                chunk = self.s.recv(4096)
                if not chunk:
                    break
        except (BlockingIOError, socket.error):
            pass
        finally:
            self.s.settimeout(1.0)
        self.rbuf = b""

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# ---- small vector / quaternion helpers (xyzw quaternions, spec §1) ----

def lerp3(a, b, t):
    return [a[i] + (b[i] - a[i]) * t for i in range(3)]

def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]

def quat_yaw(yaw_rad):
    """Rotation about +Y (world up) by yaw_rad, as [x,y,z,w]."""
    h = yaw_rad * 0.5
    return [0.0, math.sin(h), 0.0, math.cos(h)]

def quat_norm(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return [c / n for c in q]

def slerp(q0, q1, t):
    q0 = quat_norm(q0); q1 = quat_norm(q1)
    dot = sum(q0[i] * q1[i] for i in range(4))
    if dot < 0.0:
        q1 = [-c for c in q1]; dot = -dot
    if dot > 0.9995:  # nearly colinear -> linear + normalize
        return quat_norm([q0[i] + (q1[i] - q0[i]) * t for i in range(4)])
    theta0 = math.acos(max(-1.0, min(1.0, dot)))
    theta = theta0 * t
    s0 = math.sin(theta0 - theta) / math.sin(theta0)
    s1 = math.sin(theta) / math.sin(theta0)
    return [q0[i] * s0 + q1[i] * s1 for i in range(4)]


# ---- authoritative device-state model (what the Server owns and streams) ----

_FLAGS = {"position_valid": True, "orientation_valid": True,
          "position_tracked": True, "orientation_tracked": True}

def _pose(pos, quat):
    return {"position": list(pos), "orientation": list(quat), "relation_flags": dict(_FLAGS)}

# Full input path sets the Server always emits (spec §2.4 / driver ctrl_to_json symmetry):
# left uses x/y + menu, right uses a/b + system.
_LEFT_INPUTS = ["/input/trigger/value", "/input/trigger/touch", "/input/squeeze/value",
                "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
                "/input/thumbstick/touch", "/input/thumbrest/touch",
                "/button/x/click", "/button/x/touch", "/button/y/click", "/button/y/touch",
                "/input/menu/click"]
_RIGHT_INPUTS = ["/input/trigger/value", "/input/trigger/touch", "/input/squeeze/value",
                 "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
                 "/input/thumbstick/touch", "/input/thumbrest/touch",
                 "/button/a/click", "/button/a/touch", "/button/b/click", "/button/b/touch",
                 "/input/system/click"]

def _default_inputs(paths):
    return {p: (0.0 if p.endswith(("/value", "/x", "/y")) else False) for p in paths}

class Controller:
    def __init__(self, paths, grip_pos, grip_quat):
        self.connected = True
        self.grip_pos = list(grip_pos); self.grip_quat = list(grip_quat)
        self.aim_pos = list(grip_pos);  self.aim_quat = list(grip_quat)
        self.paths = paths
        self.inputs = _default_inputs(paths)

    def to_json(self):
        if not self.connected:
            return {"connected": False}
        return {"connected": True,
                "grip": _pose(self.grip_pos, self.grip_quat),
                "aim": _pose(self.aim_pos, self.aim_quat),
                "inputs": dict(self.inputs)}

class DeviceModel:
    """Server-owned full VirtualDeviceState. Mutated by commands, serialized every frame."""
    def __init__(self):
        self.head_pos = [0.0, 1.6, 0.0]
        self.head_quat = [0.0, 0.0, 0.0, 1.0]
        self.left = Controller(_LEFT_INPUTS, [-0.2, 1.3, -0.5], [0, 0, 0, 1])
        self.right = Controller(_RIGHT_INPUTS, [0.2, 1.3, -0.5], [0, 0, 0, 1])

    def hand(self, name):
        return self.left if name == "left" else self.right

    def state_json(self, sequence):
        return {"sequence": sequence,
                "clock": {"mode": "realtime"},
                "hmd": {"connected": True, "head": _pose(self.head_pos, self.head_quat)},
                "left": self.left.to_json(),
                "right": self.right.to_json()}

    def init_from(self, get_state_reply):
        """Seed the model from the adapter's current state so we start consistent (builder defaults)."""
        st = (get_state_reply or {}).get("state", {})
        h = st.get("hmd", {}).get("head")
        if h:
            self.head_pos = list(h["position"]); self.head_quat = list(h["orientation"])
        for name in ("left", "right"):
            c = st.get(name)
            ctrl = self.hand(name)
            if c and c.get("connected"):
                if "grip" in c:
                    ctrl.grip_pos = list(c["grip"]["position"]); ctrl.grip_quat = list(c["grip"]["orientation"])
                if "aim" in c:
                    ctrl.aim_pos = list(c["aim"]["position"]); ctrl.aim_quat = list(c["aim"]["orientation"])
                for p, v in (c.get("inputs") or {}).items():
                    if p in ctrl.inputs:
                        ctrl.inputs[p] = v


# ---- the Server: high-level commands -> interpolated set_state stream ----

class Server:
    def __init__(self, client, rate_hz=60.0, log=print, capture=None):
        self.c = client
        self.model = DeviceModel()
        self.seq = 0
        self.dt = 1.0 / rate_hz
        self.log = log
        self.assertions = []  # (name, ok, actual) per assert step -> scenario becomes a test
        # Optional capture channel (layer :52700, per the operate->:52702 / capture->:52700 split).
        # A ControlClient to the layer, or None if the scenario does no capture-asserts.
        self.capture = capture
        self.captures = {}  # name -> {"hash":..., "path":...} for visual-regression asserts

    # -- transport --
    def hello(self, role="writer"):
        r = self.c.request({"cmd": "hello", "request_id": "srv-hello",
                            "protocol_version": 1, "role": role})
        if not (r and r.get("ok")):
            raise RuntimeError("hello failed: %s" % r)
        # seed model from the adapter's current (builder) state
        g = self.c.request({"cmd": "get_state", "request_id": "srv-seed"})
        self.model.init_from(g)
        # Own a monotonic sequence ABOVE the adapter's current (spec §4): the adapter never rewinds
        # (Q2), so a writer that starts low would be stale-rejected. Continue above what is there.
        try:
            self.seq = int((g or {}).get("state", {}).get("sequence", 0))
        except (TypeError, ValueError):
            self.seq = 0
        return r

    def _emit(self):
        self.seq += 1
        self.c.send_only({"cmd": "set_state", "request_id": "srv-s%d" % self.seq,
                          "state": self.model.state_json(self.seq)})

    def _stream(self, duration_ms, apply_fn):
        """Call apply_fn(t in [0,1]) each frame for duration_ms, emitting a snapshot per frame."""
        n = max(1, int(round((duration_ms / 1000.0) / self.dt)))
        for i in range(1, n + 1):
            apply_fn(i / n)
            self._emit()
            time.sleep(self.dt)

    # -- high-level commands (spec §6 examples) --
    def move_head(self, to, duration_ms=500):
        p0 = list(self.model.head_pos); q0 = list(self.model.head_quat)
        p1 = list(to.get("position", p0)); q1 = quat_norm(list(to.get("orientation", q0)))
        self._stream(duration_ms, lambda t: (
            self.model.__setattr__("head_pos", lerp3(p0, p1, t)),
            self.model.__setattr__("head_quat", slerp(q0, q1, t))))

    def look(self, yaw_deg=0.0, duration_ms=500):
        """Turn the head by yaw_deg about world +Y (walk_forward's companion, look_at -> quat列)."""
        q0 = list(self.model.head_quat)
        q1 = quat_norm(quat_mul(quat_yaw(math.radians(yaw_deg)), q0))
        self._stream(duration_ms, lambda t: self.model.__setattr__("head_quat", slerp(q0, q1, t)))

    def walk_forward(self, speed=1.0, duration_ms=1000, hand="left"):
        """Hold the thumbstick forward (y=+speed) for duration, then release to neutral."""
        speed = max(-1.0, min(1.0, speed))
        ctrl = self.model.hand(hand)
        self._stream(duration_ms, lambda t: ctrl.inputs.__setitem__("/input/thumbstick/y", speed))
        ctrl.inputs["/input/thumbstick/y"] = 0.0
        self._emit()

    def strafe(self, speed=1.0, duration_ms=1000, hand="left"):
        speed = max(-1.0, min(1.0, speed))
        ctrl = self.model.hand(hand)
        self._stream(duration_ms, lambda t: ctrl.inputs.__setitem__("/input/thumbstick/x", speed))
        ctrl.inputs["/input/thumbstick/x"] = 0.0
        self._emit()

    def set_trigger(self, hand="right", value=1.0, duration_ms=200):
        value = max(0.0, min(1.0, value))
        ctrl = self.model.hand(hand)
        self._stream(duration_ms, lambda t: ctrl.inputs.__setitem__("/input/trigger/value", value))

    def press(self, hand="right", button="a", ms=120):
        """Press+hold a click button for ms then release (also sets its touch while held)."""
        ctrl = self.model.hand(hand)
        click = "/button/%s/click" % button
        touch = "/button/%s/touch" % button
        if click not in ctrl.inputs:
            raise ValueError("unknown button %r for %s hand" % (button, hand))
        ctrl.inputs[click] = True; ctrl.inputs[touch] = True
        self._emit(); time.sleep(ms / 1000.0)
        ctrl.inputs[click] = False; ctrl.inputs[touch] = False
        self._emit()

    def wait(self, ms=100):
        time.sleep(ms / 1000.0)

    def reset(self):
        r = self.c.request({"cmd": "reset", "request_id": "srv-reset"})
        # re-seed from the restored (builder) state so the model stays authoritative
        g = self.c.request({"cmd": "get_state", "request_id": "srv-reseed"})
        self.model.init_from(g)
        return r

    # -- scenario dispatch --
    # -- observe + assert (turns a scenario into a test: operate -> observe -> assert) --
    @staticmethod
    def _resolve(node, path):
        """Walk a get_state tree by a list of keys/indices. Handles slash-bearing input keys and array
        indices uniformly (e.g. ["hmd","head","position",2] or ["left","inputs","/input/trigger/value"])."""
        cur = node
        for k in path:
            if isinstance(cur, list):
                if not isinstance(k, int) or k < 0 or k >= len(cur):
                    return None
                cur = cur[k]
            elif isinstance(cur, dict):
                cur = cur.get(k)
            else:
                return None
        return cur

    @staticmethod
    def _cmp(actual, op, value, tol):
        try:
            if op == "near":  return actual is not None and abs(float(actual) - float(value)) <= tol
            if op == "eq":    return actual == value
            if op == "ne":    return actual != value
            if op == "gt":    return actual is not None and float(actual) > float(value)
            if op == "lt":    return actual is not None and float(actual) < float(value)
            if op == "true":  return actual is True
            if op == "false": return actual is False
        except (TypeError, ValueError):
            return False
        return False

    def assert_state(self, get, op="near", value=None, tol=1e-3, name=None):
        """Read the live state (get_state) and check a field. Records the result; never raises."""
        g = self.c.request({"cmd": "get_state", "request_id": "srv-assert%d" % (len(self.assertions) + 1)})
        actual = self._resolve((g or {}).get("state", {}), get if isinstance(get, list) else [get])
        ok = self._cmp(actual, op, value, tol)
        label = name or ("%s %s %s" % (get, op, value))
        self.assertions.append((label, bool(ok), actual))
        self.log("  assert: %s -> %s (actual=%s)" % (label, "PASS" if ok else "FAIL", actual))
        return ok

    # -- capture assertions (visual regression): observe the SCREEN via the layer channel (:52700) --
    def _screenshot(self, eye="left", timeout_ms=8000):
        """Request an on-demand screenshot over the layer capture channel and hash the resulting PNG."""
        if self.capture is None:
            return {"ok": False, "error": "no capture channel (need --capture-port + a layer-loaded app)"}
        try:
            r = self.capture.req_line({"cmd": "screenshot", "eye": eye, "timeoutMs": timeout_ms},
                                      timeout=timeout_ms / 1000.0 + 3)
        except Exception as e:  # noqa: BLE001 -- a capture failure must not crash the scenario
            return {"ok": False, "error": "capture request failed: %r" % e}
        if not (r and r.get("ok")):
            return {"ok": False, "error": "screenshot not ok: %s" % r}
        path = r.get("path", "")
        if not path or not os.path.exists(path):
            return {"ok": False, "error": "screenshot path missing: %s" % path}
        try:
            h = hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]
        except OSError as e:
            return {"ok": False, "error": "read PNG failed: %r" % e}
        return {"ok": True, "hash": h, "path": path}

    def capture_ref(self, name, eye="left"):
        """Take a reference screenshot and store it under `name` for a later assert_capture."""
        s = self._screenshot(eye)
        ok = s.get("ok", False)
        if ok:
            self.captures[name] = {"hash": s["hash"], "path": s["path"]}
        self.assertions.append(("capture[%s]" % name, bool(ok), s.get("hash") or s.get("error")))
        self.log("  capture %s -> %s (%s)" % (name, "ok" if ok else "FAIL", s.get("hash") or s.get("error")))
        return ok

    def assert_capture(self, ref, op="changed", eye="left", name=None):
        """Screenshot now and compare to a stored reference. op: 'changed' (visual regression detects a
        difference after an operation) or 'stable' (no change expected)."""
        base = self.captures.get(ref)
        s = self._screenshot(eye)
        label = name or ("capture %s vs ref[%s]" % (op, ref))
        if not s.get("ok") or base is None:
            detail = s.get("error") or ("no reference %r" % ref)
            self.assertions.append((label, False, detail))
            self.log("  assert_capture: %s -> FAIL (%s)" % (label, detail))
            return False
        same = s["hash"] == base["hash"]
        ok = (op == "stable" and same) or (op == "changed" and not same)
        self.assertions.append((label, bool(ok), "now=%s ref=%s" % (s["hash"], base["hash"])))
        self.log("  assert_capture: %s -> %s (now=%s ref=%s)" % (label, "PASS" if ok else "FAIL", s["hash"], base["hash"]))
        return ok

    def run_step(self, step):
        cmd = step.get("cmd")
        args = {k: v for k, v in step.items() if k != "cmd"}
        fn = {
            "hello": lambda role="writer", **_: self.hello(role),
            "assert": lambda get=None, op="near", value=None, tol=1e-3, name=None, **_:
                self.assert_state(get or [], op, value, tol, name),
            "capture": lambda name="ref", eye="left", **_: self.capture_ref(name, eye),
            "assert_capture": lambda ref="ref", op="changed", eye="left", name=None, **_:
                self.assert_capture(ref, op, eye, name),
            "move_head": lambda to=None, duration_ms=500, **_: self.move_head(to or {}, duration_ms),
            "look": lambda yaw_deg=0.0, duration_ms=500, **_: self.look(yaw_deg, duration_ms),
            "walk_forward": lambda speed=1.0, duration_ms=1000, hand="left", **_: self.walk_forward(speed, duration_ms, hand),
            "strafe": lambda speed=1.0, duration_ms=1000, hand="left", **_: self.strafe(speed, duration_ms, hand),
            "trigger": lambda hand="right", value=1.0, duration_ms=200, **_: self.set_trigger(hand, value, duration_ms),
            "press": lambda hand="right", button="a", ms=120, **_: self.press(hand, button, ms),
            "wait": lambda ms=100, **_: self.wait(ms),
            "reset": lambda **_: self.reset(),
        }.get(cmd)
        if fn is None:
            raise ValueError("unknown scenario cmd: %r" % cmd)
        self.log("  step: %s %s" % (cmd, args if args else ""))
        return fn(**args)

    def assertion_summary(self):
        total = len(self.assertions)
        npass = sum(1 for _, ok, _ in self.assertions if ok)
        return {"asserts": total, "passed": npass, "failed": total - npass,
                "ok": total == 0 or npass == total,
                "failures": [n for n, ok, _ in self.assertions if not ok]}

    def run_scenario(self, scenario):
        steps = scenario.get("steps", [])
        if not steps or steps[0].get("cmd") != "hello":
            self.hello("writer")  # ensure a writer session even if the scenario omits it
        for step in steps:
            self.run_step(step)
        return self.assertion_summary()


DEMO = {
    "name": "demo",
    "steps": [
        {"cmd": "hello", "role": "writer"},
        {"cmd": "move_head", "to": {"position": [0.0, 1.6, -1.0]}, "duration_ms": 400},
        {"cmd": "walk_forward", "speed": 1.0, "duration_ms": 500},
        {"cmd": "look", "yaw_deg": 90.0, "duration_ms": 400},
        {"cmd": "trigger", "hand": "right", "value": 0.8, "duration_ms": 200},
        {"cmd": "press", "hand": "right", "button": "a", "ms": 120},
        {"cmd": "reset"},
    ],
}


def _read_head_z(client):
    g = client.request({"cmd": "get_state", "request_id": "vq"})
    try:
        return g["state"]["hmd"]["head"]["position"][2]
    except Exception:
        return None


def verify_run(host, port, rate):
    """Self-checking run: assert the scripted effects show up via get_state."""
    c = ControlClient(host, port)
    srv = Server(c, rate_hz=rate)
    results = []
    def check(name, cond, detail=""):
        results.append((name, bool(cond))); print(("PASS" if cond else "FAIL"), name, "-", detail)
    srv.hello("writer")
    base_z = _read_head_z(c)
    check("seeded at builder head z~0", base_z is not None and abs(base_z) < 1e-3, "z=%s" % base_z)
    srv.move_head({"position": [0.0, 1.6, -1.0]}, 300)
    z1 = _read_head_z(c)
    check("move_head -> z~-1.0", z1 is not None and abs(z1 + 1.0) < 1e-2, "z=%s" % z1)
    srv.look(90.0, 300)
    g = c.request({"cmd": "get_state", "request_id": "vq2"})
    qy = g["state"]["hmd"]["head"]["orientation"][1]
    check("look 90deg -> quat.y ~ sin(45)=0.707", abs(qy - 0.7071) < 5e-2, "qy=%s" % qy)
    srv.walk_forward(1.0, 200)
    g = c.request({"cmd": "get_state", "request_id": "vq3"})
    ty = g["state"]["left"]["inputs"]["/input/thumbstick/y"]
    check("walk_forward releases stick to 0 at end", abs(ty) < 1e-3, "ty=%s" % ty)
    srv.press("right", "a", 80)
    g = c.request({"cmd": "get_state", "request_id": "vq4"})
    ac = g["state"]["right"]["inputs"]["/button/a/click"]
    check("press releases button (a/click false at end)", ac is False, "a=%s" % ac)
    srv.reset()
    zr = _read_head_z(c)
    check("reset -> head back to z~0", zr is not None and abs(zr) < 1e-3, "z=%s" % zr)
    c.close()
    npass = sum(1 for _, ok in results if ok)
    print("\n=== %d/%d PASS ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


def main():
    ap = argparse.ArgumentParser(description="PlaySpectra Server / Scenario Runner")
    ap.add_argument("scenario", nargs="?", help="scenario JSON file")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=52702)
    ap.add_argument("--rate", type=float, default=60.0, help="interpolation frames per second")
    ap.add_argument("--capture-port", type=int, default=0,
                    help="layer capture channel (:52700) for capture/assert_capture steps; 0 = disabled")
    ap.add_argument("--demo", action="store_true", help="run the built-in demo scenario")
    ap.add_argument("--verify", action="store_true", help="self-checking run (asserts effects via get_state)")
    a = ap.parse_args()

    if a.verify:
        return verify_run(a.host, a.port, a.rate)

    if a.demo:
        scenario = DEMO
    elif a.scenario:
        with open(a.scenario, "r", encoding="utf-8") as f:
            scenario = json.load(f)
    else:
        ap.error("give a scenario file, or --demo, or --verify")
        return 2

    c = ControlClient(a.host, a.port)
    cap = None
    if a.capture_port:
        try:
            cap = ControlClient(a.host, a.capture_port)
        except OSError as e:
            print("warning: capture channel :%d unreachable (%s); capture-asserts will fail" % (a.capture_port, e))
    srv = Server(c, rate_hz=a.rate, capture=cap)
    print("running scenario %r against %s:%d%s" % (
        scenario.get("name", "?"), a.host, a.port,
        (" (capture :%d)" % a.capture_port) if cap else ""))
    summary = srv.run_scenario(scenario)
    c.close()
    if cap:
        cap.close()
    if summary["asserts"]:
        print("scenario done. asserts: %d/%d passed%s" % (
            summary["passed"], summary["asserts"],
            "" if summary["ok"] else " -- FAILED: " + ", ".join(summary["failures"])))
        return 0 if summary["ok"] else 1
    print("scenario done.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("EXC", repr(e)); sys.exit(2)
