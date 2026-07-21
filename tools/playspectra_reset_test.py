#!/usr/bin/env python3
# PlaySpectra reset コマンド E2E テスト(spec §5.3/§5.4)。
# 前提: action_probe(OpenXR ホスト)が :52702 で制御チャネル待受中。
# 検証: writer が set_state で動かした head/controller を reset が builder 起動時値へ戻す /
#       非 writer の reset は not_writer で拒否 / reset は frame_synchronized の重複判定も
#       クリアするので直後の古い logical_frame が stale にならず適用される。
# builder 既定値: head=(0,1.6,0), 左 grip=(-0.2,1.3,-0.5), 右 grip=(0.2,1.3,-0.5), 入力すべて0。
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

def pose(pos):
    return {"position": pos, "orientation": [0, 0, 0, 1],
            "relation_flags": {"position_valid": True, "orientation_valid": True,
                               "position_tracked": True, "orientation_tracked": True}}

def get_state(w, rid):
    return w.req({"cmd": "get_state", "request_id": rid})

def head_z(st):
    try:
        return st["state"]["hmd"]["head"]["position"][2]
    except Exception:
        return None

def grip_x(st, hand):
    try:
        return st["state"][hand]["grip"]["position"][0]
    except Exception:
        return None

def trigger(st, hand):
    try:
        return st["state"][hand]["inputs"]["/input/trigger/value"]
    except Exception:
        return None

def near(a, b, eps=1e-3):
    return a is not None and abs(a - b) < eps

def main():
    w = Client()
    h = w.req({"cmd": "hello", "request_id": "h", "protocol_version": 1, "role": "writer"})
    check("writer hello", h and h.get("ok") and h.get("role_granted") == "writer", str(h))

    # 0. baseline: builder 起動時値であることを確認(reset の復元先)
    st = get_state(w, "g0")
    check("baseline head z=0.0", near(head_z(st), 0.0), "z=%s" % head_z(st))
    check("baseline left grip x=-0.2", near(grip_x(st, "left"), -0.2), "x=%s" % grip_x(st, "left"))
    check("baseline right grip x=0.2", near(grip_x(st, "right"), 0.2), "x=%s" % grip_x(st, "right"))
    check("baseline left trigger=0", near(trigger(st, "left"), 0.0), "t=%s" % trigger(st, "left"))

    # 1. realtime set_state で head と左 controller を動かす
    r = w.req({"cmd": "set_state", "request_id": "s1", "state": {
        "hmd": {"connected": True, "head": pose([0.0, 1.6, -2.5])},
        "left": {"connected": True, "grip": pose([-1.0, 1.3, -0.5]),
                 "aim": pose([-1.0, 1.3, -0.5]),
                 "inputs": {"/input/trigger/value": 0.9}}}})
    check("set_state applied", r and r.get("ok") and r.get("applied") is True, str(r))
    st = get_state(w, "g1")
    check("moved head z=-2.5", near(head_z(st), -2.5), "z=%s" % head_z(st))
    check("moved left grip x=-1.0", near(grip_x(st, "left"), -1.0), "x=%s" % grip_x(st, "left"))
    check("moved left trigger=0.9", near(trigger(st, "left"), 0.9, 1e-2), "t=%s" % trigger(st, "left"))

    # 2. frame_synchronized frame=200 を適用(reset が frame 記録を消すことの前段)
    r = w.req({"cmd": "set_state", "request_id": "s2", "state": {
        "clock": {"mode": "frame_synchronized", "logical_frame": 200},
        "hmd": {"connected": True, "head": pose([0.0, 1.6, -3.3])}}})
    check("frame200 applied", r and r.get("ok") and r.get("applied") is True and r.get("logical_frame") == 200, str(r))

    # 3. 非 writer(observer)の reset は拒否される
    obs = Client()
    ho = obs.req({"cmd": "hello", "request_id": "ho", "protocol_version": 1, "role": "observer"})
    check("observer hello", ho and ho.get("ok") and ho.get("role_granted") == "observer", str(ho))
    ro = obs.req({"cmd": "reset", "request_id": "ro1"})
    check("observer reset -> not_writer",
          ro and ro.get("ok") is False and ro.get("error_type") == "protocol_error"
          and ro.get("error") == "not_writer", str(ro))

    # 4. writer reset -> ok:true
    r = w.req({"cmd": "reset", "request_id": "r1"})
    check("writer reset ok", r and r.get("ok") is True, str(r))

    # 5. reset 後の状態が builder 起動時値へ戻っている
    st = get_state(w, "g2")
    check("reset head -> z=0.0", near(head_z(st), 0.0), "z=%s" % head_z(st))
    check("reset left grip -> x=-0.2", near(grip_x(st, "left"), -0.2), "x=%s" % grip_x(st, "left"))
    check("reset right grip -> x=0.2", near(grip_x(st, "right"), 0.2), "x=%s" % grip_x(st, "right"))
    check("reset left trigger -> 0", near(trigger(st, "left"), 0.0), "t=%s" % trigger(st, "left"))

    # 6. observer からも reset 後の初期状態が観測できる
    sto = get_state(obs, "go1")
    check("observer sees reset head z=0.0", near(head_z(sto), 0.0), "z=%s" % head_z(sto))

    # 7. reset は frame 記録もクリア: 古い logical_frame=5 (< 200) が stale にならず適用される
    r = w.req({"cmd": "set_state", "request_id": "s3", "state": {
        "clock": {"mode": "frame_synchronized", "logical_frame": 5},
        "hmd": {"connected": True, "head": pose([0.0, 1.6, -0.7])}}})
    check("post-reset old frame5 applied (not stale)",
          r and r.get("ok") and r.get("applied") is True and r.get("logical_frame") == 5, str(r))
    st = get_state(w, "g3")
    check("post-reset frame5 moved head z=-0.7", near(head_z(st), -0.7), "z=%s" % head_z(st))

    obs.s.close()
    w.s.close()
    npass = sum(1 for _, ok, _ in results if ok)
    print("\n=== %d/%d PASS ===" % (npass, len(results)))
    return 0 if npass == len(results) else 1

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("EXC", repr(e)); sys.exit(2)
