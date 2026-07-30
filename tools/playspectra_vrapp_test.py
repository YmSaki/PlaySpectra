#!/usr/bin/env python3
"""End-to-end: the PlaySpectra core loop against a REAL ENGINE app (VRAppDummyGame, Godot 4.7 / D3D12).

Until now the only real OpenXR app in the regression set was hello_xr -- an SDK sample that renders and
nothing else. It can be driven but not interrogated, so "the injection reached the app" had to be read
back through our own capture layer. This suite closes that gap: VRAppDummyGame reports what it received
over its own [VRTEST] contract, so every verdict below is the APP's word, not ours.

What it proves, in the order it proves it:
  startup      the engine's OpenXR stack comes up on our Monado build and binds our virtual devices
  pose+inputs  head/controller poses land at the COMMANDED coordinates, and every input KIND
               (float / vec2 / bool / pose) reaches the engine's action system, both hands
  capture      operate -> re-render -> observe: changing ONLY an input value repaints the screen
  interaction  the app's own game logic runs: press a button, grab and throw a cube, drive a lever

Coordinates: the app reports GLOBAL positions; PlaySpectra commands are STAGE. They differ by the
XROrigin3D transform, which MOVES when thumbstick locomotion is injected -- so every positional
assertion converts using the origin from the same snapshot rather than a constant.

Usage: python tools/playspectra_vrapp_test.py [operate_port=52702] [capture_port=52700]
  PLAYSPECTRA_VRAPP_EXE overrides the app location. Absent app/service => explicit SKIP, never a
  silent pass. Capture assertions SKIP (counted, printed) when no layer channel is present.
Exit code is decided by FAILs only; SKIPs never make it non-zero.
"""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from playspectra_png_stats import is_non_degenerate, png_stats  # noqa: E402
from playspectra_server import ControlClient, Server  # noqa: E402
from playspectra_vrapp import (VrApp, app_revision, axis, button, default_exe,  # noqa: E402
                               hover_start, stage_to_global, testbed)

OP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("PLAYSPECTRA_MONADO_PORT", 52702))
CAP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else int(os.environ.get("PLAYSPECTRA_PORT", 52700))

POS_TOL = 0.05   # measured error was 0.000; keep the gate tight enough to catch a real regression
VAL_TOL = 0.05

# Scene facts from VRAppDummyGame/vrapp.tscn (GLOBAL coords). The interactables are static bodies except
# GrabCube, a RigidBody that has fallen to the ground (ground top y=0, cube half-extent 0.075) by the
# time we reach for it.
INSPECTOR_POS = [0.0, 1.6, 4.534]   # Label3D showing live input state; faces -Z, so we must turn around
PUSH_BUTTON = [3.0, 1.05, 3.0]      # PressZone centre (0.3m box)
GRAB_CUBE_RESTING = [-3.0, 0.075, 3.0]
LEVER_HANDLE = [-3.0, 1.28, -3.0]


class Suite:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = []

    def check(self, name, cond, detail=""):
        ok = bool(cond)
        self.passed += ok
        self.failed += (not ok)
        print("%s %s%s" % ("PASS" if ok else "FAIL", name, ("  - " + str(detail)) if detail else ""),
              flush=True)
        return ok

    def skip(self, name, why):
        self.skipped.append(name)
        print("SKIP %s  - %s" % (name, why), flush=True)

    def report(self):
        total = self.passed + self.failed
        line = "=== VRAppDummyGame E2E %d/%d passed" % (self.passed, total)
        if self.skipped:
            line += ", %d skipped (%s)" % (len(self.skipped), ", ".join(self.skipped))
        print(line + " ===", flush=True)
        return 1 if self.failed else 0


def port_open(port, timeout=1.0):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=timeout).close()
        return True
    except OSError:
        return False


def wait_port(port, timeout=45.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_open(port):
            return True
        time.sleep(0.4)
    return False


def phase_startup(app, s):
    init = app.wait_xr_init()
    if not s.check("[startup] the engine initialised OpenXR on our Monado build",
                   init and init.get("ok"), init):
        for line in app.non_vrtest_lines(12):
            print("      | %s" % line)
        return False
    for hand in ("left", "right"):
        act = app.wait_controller_active(hand)
        s.check("[startup] %s controller became active, profile resolved" % hand,
                act and act.get("profile"), (act or {}).get("profile"))
    return True


def phase_pose_and_inputs(app, srv, s):
    """Positional assertions compare against the COMMANDED coordinate (origin-corrected), not merely
    'it moved' -- a loose gate here would not notice injection drifting."""
    target = [0.0, 1.6, -2.5]
    srv.move_head({"position": target}, 300)
    time.sleep(1.0)
    p = app.pose()
    if not s.check("[pose] the app answers a pose request", bool(p), p):
        return
    want = stage_to_global(target, p["origin"])
    got = p["head"]["pos"]
    err = max(abs(got[i] - want[i]) for i in range(3))
    s.check("[pose] head lands at the commanded position", err < POS_TOL,
            "err=%.4f got=%s want=%s" % (err, [round(v, 3) for v in got], [round(v, 3) for v in want]))

    since = app.count()
    srv.set_input("left", "/input/trigger/value", 0.7)
    ev = app.wait_for(axis("left", "trigger"), 8, since)
    s.check("[input float] left trigger value reaches the engine's action system",
            ev and abs(ev["value"] - 0.7) < VAL_TOL, ev)

    since = app.count()
    srv.set_input("right", "/input/squeeze/value", 0.8)
    ev = app.wait_for(axis("right", "grip"), 8, since)
    s.check("[input float] right grip value reaches it too (both hands)",
            ev and abs(ev["value"] - 0.8) < VAL_TOL, ev)

    since = app.count()
    srv.set_input("left", "/input/thumbstick/x", 0.5)
    srv.set_input("left", "/input/thumbstick/y", -0.4)
    ev = app.wait_for(lambda e: (e.get("t") == "vec2" and e.get("hand") == "left"
                                 and abs(e.get("y", 0) + 0.4) < VAL_TOL), 8, since)
    s.check("[input vec2] left thumbstick reaches it", bool(ev), ev)
    srv.set_input("left", "/input/thumbstick/x", 0.0)
    srv.set_input("left", "/input/thumbstick/y", 0.0)

    since = app.count()
    srv.press("right", "a", ms=200)
    s.check("[input bool] right A press reaches it", bool(app.wait_for(button("right", "pressed"), 8, since)))
    s.check("[input bool] and its release", bool(app.wait_for(button("right", "released"), 8, since)))

    p0 = app.pose()
    ctarget = [-0.3, 1.2, -0.6]
    srv.move_controller("left", {"position": ctarget}, 300)
    time.sleep(0.8)
    p1 = app.pose()
    if p0 and p1:
        want = stage_to_global(ctarget, p1["origin"])
        got = p1["left"]["pos"]
        err = max(abs(got[i] - want[i]) for i in range(3))
        s.check("[input pose] left controller lands at the commanded position", err < POS_TOL,
                "err=%.4f got=%s want=%s" % (err, [round(v, 3) for v in got],
                                             [round(v, 3) for v in want]))


def shoot(srv):
    shot = srv._screenshot(eye="dominant")  # noqa: SLF001 - the helper scenario capture-asserts use
    stats = {}
    if shot.get("ok") and os.path.exists(shot.get("path", "")):
        stats = png_stats(shot["path"])
    return shot, stats


def phase_capture(app, srv, s):
    """The load-bearing assertion is the input one: change ONLY input values, camera untouched, and
    require the picture to change. input_inspector.gd renders the live input state, so a changed frame
    means injection reached RENDERING -- not just the app's event handlers. That requires the panel to
    be on screen: it sits at +Z facing -Z while an OpenXR camera looks down -Z, i.e. behind us at start.
    """
    srv.move_head({"position": [0.0, 1.6, INSPECTOR_POS[2] - 1.5]}, 400)
    srv.look(180, 400)
    time.sleep(1.2)

    s0, st0 = shoot(srv)
    if not s.check("[capture] screenshot of a real engine app succeeds", s0.get("ok"), s0.get("error")):
        return
    s.check("[capture] the frame is a rendered scene, not a flat fill", is_non_degenerate(st0),
            "%dx%d distinct=%s dominant=%s" % (st0.get("w", 0), st0.get("h", 0),
                                               st0.get("distinctColors"), st0.get("dominantFraction")))

    srv.set_input("left", "/input/trigger/value", 0.85)
    srv.set_input("right", "/input/squeeze/value", 0.65)
    time.sleep(1.2)
    s1, _ = shoot(srv)
    s.check("[capture] an injected INPUT alone (camera unmoved) repaints the screen",
            s1.get("ok") and s1["hash"] != s0["hash"], "%s -> %s" % (s0.get("hash"), s1.get("hash")))

    # Negative control: without it, the assertion above could be satisfied by any drifting frame.
    time.sleep(1.0)
    s2, _ = shoot(srv)
    s3, _ = shoot(srv)
    s.check("[capture] negative control: two idle frames are identical",
            s2.get("ok") and s2["hash"] == s3["hash"], "%s vs %s" % (s2.get("hash"), s3.get("hash")))

    srv.move_head({"position": [0.0, 1.6, INSPECTOR_POS[2] - 3.0]}, 400)
    time.sleep(1.0)
    s4, _ = shoot(srv)
    s.check("[capture] moving the virtual HMD changes the rendered frame",
            s4.get("ok") and s4["hash"] != s3["hash"], "%s -> %s" % (s3.get("hash"), s4.get("hash")))


def phase_pacing(app, srv, s, cap):
    """Does the runtime pace frames at the rate the device declares?

    The null compositor derives its frame interval from the xdev, which feeds u_pc_fake's pacing period
    (and xrGetDisplayRefreshRate). Upstream shipped a fixed 20 FPS stub there, which silently
    contradicted PlaySpectra's own descriptor (refresh_hz 90) -- the stack asserted two different rates
    about the same device. Measuring the frames the layer actually observes tests the behaviour rather
    than trusting either declared number.
    """
    try:
        st0 = cap.req_line({"cmd": "status"}, timeout=5)
        f0 = ((st0 or {}).get("capture") or {}).get("framesObserved")
        t0 = time.time()
        time.sleep(3.0)
        st1 = cap.req_line({"cmd": "status"}, timeout=5)
        f1 = ((st1 or {}).get("capture") or {}).get("framesObserved")
    except Exception as e:  # noqa: BLE001
        s.skip("[pacing] observed frame rate", "status unavailable: %r" % e)
        return
    if f0 is None or f1 is None:
        s.skip("[pacing] observed frame rate", "framesObserved missing from layer status")
        return
    fps = (f1 - f0) / max(1e-6, time.time() - t0)
    # The device declares 90 Hz (playspectra_hmd.c: nominal_frame_interval_ns = 1/90 s). The gate sits
    # well above the 20 FPS stub and well below 90 so it detects the regression without turning GPU
    # scheduling jitter into a failure.
    s.check("[pacing] frames arrive at the device's declared rate, not the 20 FPS stub", fps > 40.0,
            "measured %.1f fps over 3s (%d frames)" % (fps, f1 - f0))


def phase_interactions(app, srv, s):
    """Every verdict here is a `testbed` event the app emits from its own game logic, so a pass means
    the engine's physics/interaction code ran. hover_start is the positioning oracle: it fires when the
    controller's 8cm GrabZone actually overlaps the target, separating "we aimed wrong" from "the
    interaction did not fire" -- two failures that otherwise look identical.
    """
    p = app.pose()
    if not p:
        s.skip("[interaction] all", "no pose snapshot to resolve STAGE coordinates against")
        return
    origin = p["origin"]

    def reach(hand, global_pos, ms=500):
        srv.move_controller(hand, {"position": [global_pos[i] - origin["pos"][i] for i in range(3)]}, ms)

    since = app.count()
    reach("right", PUSH_BUTTON)
    s.check("[interaction] right hand reaches the push button",
            bool(app.wait_for(hover_start("PushButton", "right"), 10, since)))
    since = app.count()
    srv.set_input("right", "/input/trigger/value", 1.0)
    s.check("[interaction] the trigger toggles it (the app's button logic ran)",
            bool(app.wait_for(testbed("button_toggled"), 10, since)))
    srv.set_input("right", "/input/trigger/value", 0.0)

    since = app.count()
    reach("left", GRAB_CUBE_RESTING)
    s.check("[interaction] left hand reaches the fallen cube",
            bool(app.wait_for(hover_start("GrabCube", "left"), 10, since)))
    since = app.count()
    srv.set_input("left", "/input/squeeze/value", 0.9)
    grabbed = app.wait_for(testbed("cube_grabbed", hand="left"), 10, since)
    s.check("[interaction] grip grabs it", bool(grabbed))
    if grabbed:
        # Two app behaviours constrain this: the cube is thrown with the mean velocity of the last 6
        # physics frames (so releasing from a standstill correctly yields zero), but vr_controller.gd
        # clears _grabbed_object on body_exited WITHOUT calling release() (so out-running the cube
        # strands it grabbed and no cube_released ever fires). Hence: a short unhurried lift, released
        # while still moving.
        since = app.count()
        srv.move_controller("left", {"position": [GRAB_CUBE_RESTING[i] - origin["pos"][i]
                                                  + [0.05, 0.225, -0.05][i] for i in range(3)]}, 700)
        srv.set_input("left", "/input/squeeze/value", 0.0)
        rel = app.wait_for(testbed("cube_released"), 10, since)
        s.check("[interaction] releasing throws it with the carried velocity",
                rel and any(abs(v) > 0.01 for v in (rel.get("velocity") or [0, 0, 0])), rel)

    since = app.count()
    reach("right", LEVER_HANDLE)
    s.check("[interaction] right hand reaches the lever handle",
            bool(app.wait_for(hover_start("Lever", "right"), 10, since)))
    since = app.count()
    srv.set_input("right", "/input/squeeze/value", 0.9)
    if s.check("[interaction] grip grabs the lever",
               bool(app.wait_for(testbed("lever_grabbed"), 10, since))):
        since = app.count()
        srv.move_controller("right", {"position": [LEVER_HANDLE[i] - origin["pos"][i]
                                                   + [0.25, 0, 0][i] for i in range(3)]}, 600)
        ang = app.wait_for(lambda e: (e.get("t") == "testbed" and e.get("event") == "lever"
                                      and abs(e.get("angle", 0)) > 1.0), 10, since)
        s.check("[interaction] moving the hand drives the lever angle", bool(ang), ang)
        srv.set_input("right", "/input/squeeze/value", 0.0)


def main():
    exe = default_exe()
    s = Suite()
    if not os.path.isfile(exe):
        print("SKIP: VRAppDummyGame not built: %s" % exe)
        print("      export it with Godot 4.7 (--export-release \"Windows Desktop\" build/vrapp.exe)"
              " or set PLAYSPECTRA_VRAPP_EXE")
        return 0
    if not port_open(OP_PORT):
        print("SKIP: no operate channel on :%d (start monado-service with PLAYSPECTRA_ENABLE=1)" % OP_PORT)
        return 0
    print("app      : %s" % exe)
    print("app rev  : %s" % (app_revision(exe) or "(unknown)"))
    print("operate  : :%d   capture: :%d" % (OP_PORT, CAP_PORT))

    log = os.path.join(os.environ.get("TEMP", "."), "playspectra_vrapp_e2e.log")
    with VrApp(exe, log_path=log) as app:
        if not phase_startup(app, s):
            print("app log: %s" % log)
            return s.report()
        c = ControlClient("127.0.0.1", OP_PORT)
        cap = None
        if wait_port(CAP_PORT, timeout=20):
            try:
                cap = ControlClient("127.0.0.1", CAP_PORT)
            except OSError as e:
                print("capture channel unreachable: %s" % e)
        srv = Server(c, capture=cap, log=lambda *a: None)
        srv.hello("vrapp-e2e")
        time.sleep(1.0)

        phase_pose_and_inputs(app, srv, s)
        if cap is None:
            s.skip("[capture] all", "no layer control channel on :%d (run with the layer enabled)" % CAP_PORT)
            s.skip("[pacing] observed frame rate", "needs the layer's framesObserved counter")
        else:
            phase_capture(app, srv, s)
            phase_pacing(app, srv, s, cap)
        phase_interactions(app, srv, s)

        c.close()
        if cap:
            cap.close()
    print("app log: %s" % log)
    return s.report()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print("EXC", repr(e))
        sys.exit(3)
