#!/usr/bin/env python3
# PlaySpectra 制御チャネル observer 複数接続 E2E テスト。
# 前提: action_probe(OpenXR ホスト)が起動していて 127.0.0.1:52702 で制御チャネルが待受中。
# 検証項目:
#   1. observer×2 が同時接続でき hello(role=observer) が role_granted=observer で ok
#   2. writer が hello(role=writer) で ok、2人目の writer は writer_taken 拒否
#   3. observer からの set_state は not_writer 拒否(writer 排他)
#   4. writer の set_state が applied、observer の get_state に反映(共有 state 読取)
#   5. status が writer_connected=true / observers=2 を報告
#   6. haptics イベントが接続中の全 observer + writer へ broadcast される
import socket, json, sys, time

HOST, PORT = "127.0.0.1", 52702

class Client:
    def __init__(self, name):
        self.name = name
        self.s = socket.create_connection((HOST, PORT), timeout=5)
        self.s.settimeout(0.3)
        self.rbuf = b""
        self.events = []   # {"event": ...} 行
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
    def _pump(self, want_request_id=None, deadline=None):
        # 行を読み、event はためて、request_id 一致の応答を返す。
        while True:
            if deadline and time.time() > deadline:
                return None
            line = self._readline()
            if line is None:
                if want_request_id is None:
                    return None
                continue
            if not line.strip():
                continue
            obj = json.loads(line)
            if "event" in obj:
                self.events.append(obj)
                continue
            if want_request_id is None or obj.get("request_id") == want_request_id:
                return obj
    def req(self, obj):
        self.s.sendall((json.dumps(obj) + "\n").encode("utf-8"))
        return self._pump(want_request_id=obj["request_id"], deadline=time.time() + 3)
    def drain_events(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self._pump(want_request_id=None, deadline=deadline)
    def close(self):
        try: self.s.close()
        except Exception: pass

results = []
def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("PASS" if cond else "FAIL"), name, "-", detail)

HELLO_OBS = lambda rid: {"cmd": "hello", "request_id": rid, "protocol_version": 1, "role": "observer"}
HELLO_WRT = lambda rid: {"cmd": "hello", "request_id": rid, "protocol_version": 1, "role": "writer"}

def pose(z):
    return {"position": [0.0, 1.6, z], "orientation": [0, 0, 0, 1],
            "relation_flags": {"position_valid": True, "orientation_valid": True,
                               "position_tracked": True, "orientation_tracked": True}}

def main():
    # 1) observer×2 を先に接続(haptic を取りこぼさないため FOCUSED 前に繋ぐ)
    obs1 = Client("obs1"); obs2 = Client("obs2")
    r1 = obs1.req(HELLO_OBS("o1")); r2 = obs2.req(HELLO_OBS("o2"))
    check("obs1 hello ok", r1 and r1.get("ok") and r1.get("role_granted") == "observer", str(r1))
    check("obs2 hello ok", r2 and r2.get("ok") and r2.get("role_granted") == "observer", str(r2))

    # 2) writer 接続 + 2人目 writer は writer_taken
    wrt = Client("wrt")
    rw = wrt.req(HELLO_WRT("w1"))
    check("writer hello ok", rw and rw.get("ok") and rw.get("role_granted") == "writer", str(rw))
    wrt2 = Client("wrt2")
    rw2 = wrt2.req(HELLO_WRT("w2"))
    check("2nd writer -> writer_taken", rw2 and rw2.get("ok") is False and rw2.get("error") == "writer_taken", str(rw2))

    # 3) observer の set_state は not_writer
    ss_obs = {"cmd": "set_state", "request_id": "os1",
              "state": {"sequence": 1, "clock": {"mode": "realtime"},
                        "hmd": {"connected": True, "head": pose(-1.0)}}}
    ro = obs1.req(ss_obs)
    check("observer set_state -> not_writer", ro and ro.get("ok") is False and ro.get("error") == "not_writer", str(ro))

    # 4) writer set_state -> observer get_state に反映
    g_before = obs1.req({"cmd": "get_state", "request_id": "g0"})
    ss = {"cmd": "set_state", "request_id": "s1",
          "state": {"sequence": 5, "clock": {"mode": "realtime"},
                    "hmd": {"connected": True, "head": pose(-2.5)}}}
    rs = wrt.req(ss)
    check("writer set_state applied", rs and rs.get("ok") and rs.get("applied") is True, str(rs))
    g_after = obs1.req({"cmd": "get_state", "request_id": "g1"})
    z_after = None
    try:
        z_after = g_after["state"]["hmd"]["head"]["position"][2]
    except Exception:
        pass
    check("observer get_state reflects writer", z_after is not None and abs(z_after - (-2.5)) < 1e-3,
          "z_after=%s (before=%s)" % (z_after, (g_before or {}).get("state", {}).get("hmd", {}).get("head", {}).get("position")))

    # 5) status
    st = obs2.req({"cmd": "status", "request_id": "q1"})
    check("status writer_connected+observers", st and st.get("writer_connected") is True and st.get("observers") == 2, str(st))

    # 6) haptics broadcast: action_probe が FOCUSED で xrApplyHapticFeedback を1回適用する。
    #    接続中の obs1/obs2/writer が {"event":"haptics"} を受信するはず。最大 12s 待つ。
    print("... waiting up to 12s for haptics broadcast from host app ...")
    deadline = time.time() + 12
    while time.time() < deadline:
        for c in (obs1, obs2, wrt):
            c.drain_events(0.2)
        if any(any(e.get("event") == "haptics" for e in c.events) for c in (obs1,)) and \
           any(any(e.get("event") == "haptics" for e in c.events) for c in (obs2,)):
            break
    h1 = [e for e in obs1.events if e.get("event") == "haptics"]
    h2 = [e for e in obs2.events if e.get("event") == "haptics"]
    hw = [e for e in wrt.events if e.get("event") == "haptics"]
    check("haptics broadcast to obs1", len(h1) >= 1, str(h1[:1]))
    check("haptics broadcast to obs2", len(h2) >= 1, str(h2[:1]))
    check("haptics broadcast to writer", len(hw) >= 1, str(hw[:1]))

    for c in (obs1, obs2, wrt, wrt2):
        c.close()

    npass = sum(1 for _, ok, _ in results if ok)
    ntot = len(results)
    print("\n=== %d/%d PASS ===" % (npass, ntot))
    return 0 if npass == ntot else 1

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("EXC", repr(e))
        sys.exit(2)
