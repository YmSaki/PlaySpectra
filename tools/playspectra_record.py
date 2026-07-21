#!/usr/bin/env python3
"""PlaySpectra Recorder + Replay (record a session's state trajectory, replay it).

Completes the Playwright-style loop for PlaySpectra: operate (playspectra_server.py) / observe
(get_state) / RECORD / REPLAY. Architecture roadmap (playspectra-architecture.md §8): "Recorder+Replay".

- Recorder connects as an OBSERVER (spec §5.3, multiple observers allowed) and polls get_state at a
  fixed rate, appending timestamped full-state snapshots. It records whatever drives the adapter --
  the Server, another writer, or (later) a real app -- without holding the writer role.
- Replay connects as the WRITER and re-emits each recorded snapshot as a set_state, timed by the
  recorded t_ms, with Server-owned monotonic sequence. A get_state snapshot is schema-compatible with
  set_state input (head pose + controller grip/aim/inputs), so replay needs no translation, only a
  fresh sequence + clock envelope.

Recording file (JSON):  {"name","rate_hz","frames":[{"t_ms":<float>,"state":{...get_state...}}, ...]}

Usage:
  python playspectra_record.py record <out.json> --duration-ms 3000 [--rate 60]   # record (observer)
  python playspectra_record.py replay <in.json>                                    # replay (writer)
  python playspectra_record.py --verify                                            # self-checking record+replay
"""
import socket, json, sys, time, argparse, threading

# Reuse the control-channel client + Server from the sibling module.
from playspectra_server import ControlClient, Server


class Recorder:
    """Observer that samples get_state at a fixed rate into a trajectory."""
    def __init__(self, client, rate_hz=60.0):
        self.c = client
        self.dt = 1.0 / rate_hz
        self.rate_hz = rate_hz
        self.frames = []
        self._stop = threading.Event()
        self._n = 0

    def hello(self):
        r = self.c.request({"cmd": "hello", "request_id": "rec-hello",
                            "protocol_version": 1, "role": "observer"})
        if not (r and r.get("ok") and r.get("role_granted") == "observer"):
            raise RuntimeError("observer hello failed: %s" % r)
        return r

    def _sample(self, t0):
        self._n += 1
        g = self.c.request({"cmd": "get_state", "request_id": "rec-%d" % self._n}, timeout=1.0)
        if g and g.get("state"):
            self.frames.append({"t_ms": (time.time() - t0) * 1000.0, "state": g["state"]})

    def record_for(self, duration_ms, t0=None):
        """Blocking record for a fixed duration (foreground use)."""
        t0 = t0 if t0 is not None else time.time()
        end = t0 + duration_ms / 1000.0
        while time.time() < end and not self._stop.is_set():
            self._sample(t0)
            time.sleep(self.dt)
        return self.frames

    def run_thread(self, t0):
        """Record until stop() is called (background use, alongside a driver)."""
        while not self._stop.is_set():
            self._sample(t0)
            time.sleep(self.dt)

    def stop(self):
        self._stop.set()

    def recording(self, name="recording"):
        return {"name": name, "rate_hz": self.rate_hz, "frames": self.frames}


def save_recording(path, rec):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(rec, f)


def load_recording(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


class Replayer:
    """Writer that re-emits a recorded trajectory as set_state, timed by t_ms."""
    def __init__(self, client):
        self.c = client
        self.seq = 0

    def hello(self):
        r = self.c.request({"cmd": "hello", "request_id": "rep-hello",
                            "protocol_version": 1, "role": "writer"})
        if not (r and r.get("ok") and r.get("role_granted") == "writer"):
            raise RuntimeError("writer hello failed (writer already taken?): %s" % r)
        return r

    def play(self, rec, log=None):
        frames = rec.get("frames", [])
        if not frames:
            return 0
        # Base our sequence ABOVE the adapter's current: record-time sequences (carried in each frame's
        # state) are stale relative to the live adapter, and the adapter never rewinds (Q2). Query it
        # once and continue above (spec §4, Server-owned monotonic sequence).
        g = self.c.request({"cmd": "get_state", "request_id": "rep-seq"})
        try:
            self.seq = max(self.seq, int((g or {}).get("state", {}).get("sequence", 0)))
        except (TypeError, ValueError):
            pass
        start = time.time()
        t0 = frames[0]["t_ms"]
        for fr in frames:
            target = start + (fr["t_ms"] - t0) / 1000.0
            dtw = target - time.time()
            if dtw > 0:
                time.sleep(dtw)
            self.seq += 1
            st = dict(fr["state"])
            st.pop("sequence", None)  # drop the stale record-time sequence; we own a fresh one
            st["sequence"] = self.seq
            st["clock"] = {"mode": "realtime"}
            self.c.send_only({"cmd": "set_state", "request_id": "rep-%d" % self.seq, "state": st})
        if log:
            log("replayed %d frames over ~%.0f ms" % (len(frames), frames[-1]["t_ms"] - t0))
        return len(frames)


def _head_z_of(state):
    try:
        return state["hmd"]["head"]["position"][2]
    except Exception:
        return None


def verify_run(host, port, rate):
    """Record a driven trajectory via an observer, then replay it as the writer and confirm it reproduces."""
    results = []
    def check(name, cond, detail=""):
        results.append((name, bool(cond))); print(("PASS" if cond else "FAIL"), name, "-", detail)

    writer = ControlClient(host, port)
    observer = ControlClient(host, port)
    srv = Server(writer, rate_hz=rate)
    rec = Recorder(observer, rate_hz=rate)

    srv.hello("writer")
    rec.hello()

    # Record while the Server drives head z 0 -> -2.0 then a small look.
    t0 = time.time()
    th = threading.Thread(target=rec.run_thread, args=(t0,), daemon=True)
    th.start()
    srv.move_head({"position": [0.0, 1.6, -2.0]}, 400)
    srv.look(30.0, 200)
    time.sleep(0.1)
    rec.stop(); th.join(timeout=2.0)

    frames = rec.frames
    zs = [z for z in (_head_z_of(f["state"]) for f in frames) if z is not None]
    check("recorded frames > 0", len(frames) > 0, "frames=%d" % len(frames))
    check("trajectory captured (min head z <= -1.8)", zs and min(zs) <= -1.8, "min_z=%s" % (min(zs) if zs else None))
    check("trajectory starts near builder z~0", zs and abs(zs[0]) < 0.3, "z0=%s" % (zs[0] if zs else None))

    # Reset to builder state, then replay the recording as the writer.
    srv.reset()
    zr = _head_z_of((writer.request({"cmd": "get_state", "request_id": "vqr"}) or {}).get("state", {}))
    check("reset before replay -> z~0", zr is not None and abs(zr) < 1e-3, "z=%s" % zr)

    rep = Replayer(writer)  # reuse the writer connection (Server already holds writer role)
    rep.play({"frames": frames})
    writer.drain()
    zf = _head_z_of((writer.request({"cmd": "get_state", "request_id": "vqf"}) or {}).get("state", {}))
    check("replay reproduced final head z (~-2.0)", zf is not None and abs(zf + 2.0) < 0.15, "z=%s" % zf)

    writer.close(); observer.close()
    npass = sum(1 for _, ok in results if ok)
    print("\n=== %d/%d PASS ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1


def main():
    ap = argparse.ArgumentParser(description="PlaySpectra Recorder + Replay")
    ap.add_argument("mode", nargs="?", choices=["record", "replay"], help="record or replay")
    ap.add_argument("file", nargs="?", help="recording file (out for record, in for replay)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=52702)
    ap.add_argument("--rate", type=float, default=60.0)
    ap.add_argument("--duration-ms", type=int, default=3000, help="record duration")
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()

    if a.verify:
        return verify_run(a.host, a.port, a.rate)

    if a.mode == "record":
        if not a.file:
            ap.error("record needs an output file")
        c = ControlClient(a.host, a.port)
        rec = Recorder(c, rate_hz=a.rate)
        rec.hello()
        print("recording (observer) for %d ms at %g Hz ..." % (a.duration_ms, a.rate))
        rec.record_for(a.duration_ms)
        save_recording(a.file, rec.recording(name=a.file))
        print("saved %d frames to %s" % (len(rec.frames), a.file))
        c.close()
        return 0

    if a.mode == "replay":
        if not a.file:
            ap.error("replay needs an input file")
        rec = load_recording(a.file)
        c = ControlClient(a.host, a.port)
        rep = Replayer(c)
        rep.hello()
        print("replaying %s (%d frames) ..." % (a.file, len(rec.get("frames", []))))
        rep.play(rec, log=print)
        c.close()
        return 0

    ap.error("give a mode (record/replay) or --verify")
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("EXC", repr(e)); sys.exit(2)
