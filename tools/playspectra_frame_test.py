#!/usr/bin/env python3
# PlaySpectra frame_synchronized conflict 判定 E2E テスト(spec §4)。
# 前提: action_probe(OpenXR ホスト)が :52702 で制御チャネル待受中。
# 検証: 新frame適用 / 同frame同内容→冪等 / 同frame異内容→conflict_error /
#       古frame→stale_frame。各段で get_state を読み、状態が期待どおりか確認する。
import socket, json, sys, time

HOST, PORT = "127.0.0.1", 52702

class Client:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=5)
        self.s.settimeout(0.5)
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
    def req(self, obj):
        self.s.sendall((json.dumps(obj) + "\n").encode())
        deadline = time.time() + 3
        while time.time() < deadline:
            line = self._readline()
            if line is None:
                continue
            if not line.strip():
                continue
            o = json.loads(line)
            if "event" in o:
                continue
            if o.get("request_id") == obj["request_id"]:
                return o
        return None

results = []
def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("PASS" if cond else "FAIL"), name, "-", detail)

def pose(z):
    return {"position": [0.0, 1.6, z], "orientation": [0, 0, 0, 1],
            "relation_flags": {"position_valid": True, "orientation_valid": True,
                               "position_tracked": True, "orientation_tracked": True}}

_n = [0]
def fs(frame, z, seq=None):
    _n[0] += 1
    st = {"clock": {"mode": "frame_synchronized", "logical_frame": frame},
          "hmd": {"connected": True, "head": pose(z)}}
    if seq is not None:
        st["sequence"] = seq
    return {"cmd": "set_state", "request_id": "f%d" % _n[0], "state": st}

def get_z(w, rid):
    g = w.req({"cmd": "get_state", "request_id": rid})
    try:
        return g["state"]["hmd"]["head"]["position"][2], g
    except Exception:
        return None, g

def main():
    w = Client()
    h = w.req({"cmd": "hello", "request_id": "h", "protocol_version": 1, "role": "writer"})
    check("writer hello", h and h.get("ok") and h.get("role_granted") == "writer", str(h))

    # 1. 新 frame=100, z=-1.0 -> applied True, logical_frame 100
    r = w.req(fs(100, -1.0))
    check("frame100 applied", r and r.get("ok") and r.get("applied") is True and r.get("logical_frame") == 100, str(r))
    z, _ = get_z(w, "g1")
    check("state z=-1.0 after frame100", z is not None and abs(z + 1.0) < 1e-3, "z=%s" % z)

    # 2. 同 frame=100, 同内容 z=-1.0 -> 冪等成功(applied True, idempotent True)
    r = w.req(fs(100, -1.0))
    check("frame100 same -> idempotent", r and r.get("ok") and r.get("applied") is True and r.get("idempotent") is True, str(r))

    # 3. 同 frame=100, 異内容 z=-7.7 -> conflict_error
    r = w.req(fs(100, -7.7))
    check("frame100 diff -> conflict_error",
          r and r.get("ok") is False and r.get("error_type") == "conflict_error"
          and r.get("error") == "frame_content_mismatch" and r.get("logical_frame") == 100, str(r))
    z, _ = get_z(w, "g2")
    check("conflict did NOT change state (still -1.0)", z is not None and abs(z + 1.0) < 1e-3, "z=%s" % z)

    # 4. 新 frame=101, z=-2.0 -> applied True
    r = w.req(fs(101, -2.0))
    check("frame101 applied", r and r.get("ok") and r.get("applied") is True and r.get("logical_frame") == 101, str(r))
    z, _ = get_z(w, "g3")
    check("state z=-2.0 after frame101", z is not None and abs(z + 2.0) < 1e-3, "z=%s" % z)

    # 5. 古 frame=100 (< last 101) -> stale_frame (applied False)
    r = w.req(fs(100, -1.0))
    check("old frame100 -> stale_frame",
          r and r.get("ok") and r.get("applied") is False and r.get("reason") == "stale_frame", str(r))
    z, _ = get_z(w, "g4")
    check("stale did NOT revert state (still -2.0)", z is not None and abs(z + 2.0) < 1e-3, "z=%s" % z)

    w.s.close()
    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== %d/%d PASS ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("EXC", repr(e)); sys.exit(2)
