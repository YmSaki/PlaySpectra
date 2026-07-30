#!/usr/bin/env python3
"""Driver for VRAppDummyGame, the real-engine verification target (Godot 4.7, OpenXR, D3D12).

Why this exists: hello_xr can be driven but cannot *report*. To know whether an injected input reached
a real engine app we had to read its xrLocateViews back out through our own capture layer, which only
ever proves our own plumbing. VRAppDummyGame instead self-reports over a documented contract:

  - every event is one line  `[VRTEST] {json}`  on stdout, each carrying a `t` (event type),
  - a stdin request channel answers `{"req":"pose"}` with a `pose_snapshot` of the scene-graph
    positions of XRCamera3D / XRController3D_Left / XRController3D_Right plus the XROrigin3D.

So the observation path belongs to the app: a passing assertion cannot be explained by anything on the
PlaySpectra side rewriting what we read. See the app's own README for the full event table.

The app is NOT part of this repository (separate git repo, engine-side lifecycle). Point
PLAYSPECTRA_VRAPP_EXE at its console wrapper, or rely on the default sibling-directory location.
Absent app => callers must SKIP explicitly, never silently pass.

Coordinates: the app reports GLOBAL positions, while PlaySpectra commands are in STAGE space. They
differ by the XROrigin3D transform, which MOVES when locomotion input is injected (thumbstick). Always
convert with the origin read from the same snapshot -- see stage_to_global().
"""
import json
import os
import subprocess
import threading
import time

DEFAULT_EXE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "VRAppDummyGame", "build", "vrapp.console.exe")


def default_exe():
    """The console wrapper, not vrapp.exe: the latter is a GUI-subsystem binary with no stdout, and the
    [VRTEST] log is the entire machine value here."""
    return os.environ.get("PLAYSPECTRA_VRAPP_EXE") or DEFAULT_EXE


def app_revision(exe):
    """The app co-evolves with PlaySpectra (its stdin request channel exists for us), so a green run
    must be attributable to a specific app revision. Best-effort: '' when git or the repo is absent."""
    d = os.path.dirname(os.path.dirname(os.path.abspath(exe)))
    try:
        out = subprocess.run(["git", "-C", d, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        rev = out.stdout.strip()
        dirty = subprocess.run(["git", "-C", d, "status", "--porcelain"],
                               capture_output=True, text=True, timeout=10).stdout
        n = len([x for x in dirty.splitlines() if x.strip()])
        return "%s%s" % (rev, (" +%d dirty" % n) if n else "") if rev else ""
    except Exception:  # noqa: BLE001 - revision reporting must never fail a run
        return ""


def stage_to_global(stage_xyz, origin):
    """STAGE -> the GLOBAL frame the app reports. Only translation is applied: assertions that inject
    yaw must compare against the app's own rotation fields rather than using this helper."""
    op = origin["pos"]
    return [stage_xyz[i] + op[i] for i in range(3)]


def global_to_stage(global_xyz, origin):
    op = origin["pos"]
    return [global_xyz[i] - op[i] for i in range(3)]


class VrApp:
    """Owns the app process because the [VRTEST] contract needs both its stdout and its stdin."""

    def __init__(self, exe=None, env=None, log_path=None):
        self.exe = exe or default_exe()
        self.env = dict(env or os.environ)
        self.log_path = log_path
        self.proc = None
        self._events = []
        self._raw = []
        self._lock = threading.Lock()
        self._req_id = 0

    # -- lifecycle --
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    def start(self):
        self.proc = subprocess.Popen(
            [self.exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1, encoding="utf-8", errors="replace", env=self.env)
        threading.Thread(target=self._read_loop, daemon=True).start()

    def stop(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.close()
        except Exception:  # noqa: BLE001
            pass
        self.proc.terminate()
        # The console wrapper spawns the real GUI binary; terminating the wrapper alone leaves it live.
        for name in ("vrapp.exe", "vrapp.console.exe"):
            subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)
        if self.log_path:
            try:
                with open(self.log_path, "w", encoding="utf-8") as fh:
                    fh.write("\n".join(self._raw))
            except OSError:
                pass

    def _read_loop(self):
        for line in self.proc.stdout:
            line = line.rstrip("\r\n")
            self._raw.append(line)
            if not line.startswith("[VRTEST] "):
                continue
            try:
                ev = json.loads(line[len("[VRTEST] "):])
            except Exception:  # noqa: BLE001 - a malformed line is data, not a crash
                continue
            with self._lock:
                self._events.append(ev)

    # -- event access --
    def count(self):
        """Event count now; pass it as `since` so a later assertion cannot be satisfied by an event
        that predates its own stimulus."""
        with self._lock:
            return len(self._events)

    def wait_for(self, pred, timeout=10.0, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                snap = self._events[since:]
            for ev in snap:
                if pred(ev):
                    return ev
            if self.proc.poll() is not None:
                return None  # app died; no point waiting out the timeout
            time.sleep(0.05)
        return None

    def non_vrtest_lines(self, limit=25):
        return [x for x in self._raw if not x.startswith("[VRTEST] ")][:limit]

    # -- stdin request channel --
    def request(self, req, want_t, timeout=8.0, **params):
        """Send one request and return its matching response (matched on the echoed id)."""
        self._req_id += 1
        obj = {"req": req, "id": self._req_id}
        obj.update(params)
        since = self.count()
        try:
            self.proc.stdin.write(json.dumps(obj) + "\n")
            self.proc.stdin.flush()
        except Exception:  # noqa: BLE001
            return None
        return self.wait_for(lambda e: e.get("t") == want_t and e.get("id") == obj["id"],
                             timeout, since)

    def pose(self, timeout=8.0):
        """Snapshot of head/left/right (GLOBAL) plus origin. Not throttled -- it is a response."""
        return self.request("pose", "pose_snapshot", timeout)

    # -- common waits --
    def wait_xr_init(self, timeout=45.0):
        return self.wait_for(lambda e: e.get("t") == "xr_init", timeout)

    def wait_controller_active(self, hand, timeout=20.0):
        return self.wait_for(lambda e: (e.get("t") == "controller_state" and e.get("hand") == hand
                                        and e.get("active") is True), timeout)


# -- event predicates (shared by the assertion suite) --
def testbed(event, **kw):
    def pred(e):
        if e.get("t") != "testbed" or e.get("event") != event:
            return False
        return all(e.get(k) == v for k, v in kw.items())
    return pred


def hover_start(target, source):
    return testbed("hover_start", target=target, source=source)


def axis(hand, name):
    return lambda e: e.get("t") == "axis" and e.get("hand") == hand and e.get("name") == name


def button(hand, state):
    return lambda e: e.get("t") == "button" and e.get("hand") == hand and e.get("state") == state
