#!/usr/bin/env python3
"""Deterministic test for assert_capture's retry path (timeout_ms), added alongside wait_for. Runs
in-environment with NO layer / GPU: mocks BOTH the adapter control channel (:52702, via MockAdapter)
and the layer capture channel (:52700, MockCapture) over real localhost sockets, then drives the real
Server.capture_ref / assert_capture. The capture mock returns a file path per `screenshot` request;
the test swaps the file's bytes to simulate 'changed' vs 'stable' frames (Server hashes the bytes).

Run:  python tools/playspectra_capture_assert_test.py
"""
import os, sys, json, socket, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from playspectra_server import ControlClient, Server
from playspectra_waitfor_test import MockAdapter


class MockCapture:
    """Stand-in for the layer capture channel (:52700). Answers `screenshot` with the CURRENT png path;
    the test controls that path's bytes. One-line request / one-line reply (req_line protocol)."""
    def __init__(self, png_path):
        self.png_path = png_path
        self.running = True
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

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
                if req.get("cmd") == "screenshot":
                    reply = {"ok": True, "path": self.png_path, "eye": req.get("eye", "left")}
                else:
                    reply = {"ok": True}
                try:
                    conn.sendall((json.dumps(reply) + "\n").encode())
                except OSError:
                    return


def main():
    import tempfile
    d = tempfile.mkdtemp(prefix="ps_cap_")
    png = os.path.join(d, "shot.png")

    def write_frame(tag):
        # bytes need not be a valid PNG: the Server hashes the file content, it does not parse it.
        with open(png, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + tag)

    write_frame(b"AAAA")  # baseline frame

    adapter = MockAdapter()
    cap = MockCapture(png)
    c = ControlClient("127.0.0.1", adapter.port)
    capc = ControlClient("127.0.0.1", cap.port)
    srv = Server(c, rate_hz=60.0, log=lambda *a: None, capture=capc)
    srv.hello("writer")

    results = []
    def check(name, cond, detail=""):
        results.append(bool(cond))
        print(("PASS" if cond else "FAIL"), name, ("- " + detail) if detail else "")

    # reference the baseline frame
    check("capture_ref stores a reference", srv.capture_ref("base") is True)

    # stable: same bytes -> single-shot passes
    check("assert_capture stable (same frame) PASS", srv.assert_capture("base", "stable") is True)

    # changed: swap bytes -> single-shot passes
    write_frame(b"BBBB")
    check("assert_capture changed (after swap) PASS", srv.assert_capture("base", "changed") is True)

    # single-shot 'changed' with NO change -> FAIL immediately (0ms, backward-compat single shot)
    srv.capture_ref("base2")  # re-baseline to current bytes
    t0 = time.monotonic()
    r = srv.assert_capture("base2", "changed", timeout_ms=0)
    el = (time.monotonic() - t0) * 1000
    check("single-shot changed with no change -> FAIL fast", r is False and el < 200, "rc=%s el=%dms" % (r, el))

    # retry: 'changed' with timeout, frame swaps after a delay -> retry catches it
    threading.Timer(0.3, lambda: write_frame(b"CCCC")).start()
    t0 = time.monotonic()
    r = srv.assert_capture("base2", "changed", timeout_ms=2000, poll_ms=50)
    el = (time.monotonic() - t0) * 1000
    check("retrying assert_capture catches a delayed change", r is True, "el=%dms" % el)
    check("retry actually waited (>=200ms)", el >= 200, "el=%dms" % el)

    # retry timeout: 'changed' but frame never changes -> times out to FAIL
    srv.capture_ref("base3")
    t0 = time.monotonic()
    r = srv.assert_capture("base3", "changed", timeout_ms=350, poll_ms=50)
    el = (time.monotonic() - t0) * 1000
    check("assert_capture retry times out to FAIL (no hang)", r is False and 250 <= el <= 1500, "rc=%s el=%dms" % (r, el))

    c.close(); capc.close(); adapter.running = False; cap.running = False
    n = sum(results)
    print("\n=== %d/%d PASS ===" % (n, len(results)))
    return 0 if n == len(results) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # noqa: BLE001
        print("EXC", repr(e))
        sys.exit(2)
