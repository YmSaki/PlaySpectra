// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// VRDevApp verification client for the playspectra layer control channel (:52700).
// Always runs discovery: status (session/CA/framesObserved), actions dump (the app's registered
// action bindings -- so the movement stick's real source path is measured, not guessed), and a
// baseline screenshot. If MOVE_INPUT is set, it marks the hand active and drives a vec2 stick input
// (MOVE_X, MOVE_Y) repeatedly for MOVE_MS, then takes an "after" screenshot so movement can be
// judged from the captured info-window coordinates / N-E-W-S wall labels.
//
// Env: MOVE_INPUT (binding suffix e.g. "thumbstick"), MOVE_HAND (left|right, default left),
//      MOVE_X, MOVE_Y (default 0,1), MOVE_MS (default 2500), PLAYSPECTRA_PORT (default 52700).
import net from "node:net";

const PORT = Number(process.env.PLAYSPECTRA_PORT ?? "52700");
const HOST = "127.0.0.1";
const HAND = process.env.MOVE_HAND ?? "left";
const MOVE_INPUT = process.env.MOVE_INPUT ?? "";
const MX = Number(process.env.MOVE_X ?? "0");
const MY = Number(process.env.MOVE_Y ?? "1");
const MOVE_MS = Number(process.env.MOVE_MS ?? "2500");

// HMD (head) override mode: if HEAD_YAW_DEG is set, the layer rewrites xrLocateViews so the rendered
// viewpoint moves as if the HMD moved (no controller input). Position defaults to the app's standing
// eye height in STAGE space (y=1.7, measured from `view`).
const HEAD_YAW = process.env.HEAD_YAW_DEG;
const HX = Number(process.env.HEAD_X ?? "0");
const HY = Number(process.env.HEAD_Y ?? "1.7");
const HZ = Number(process.env.HEAD_Z ?? "0");
const HEAD_MS = Number(process.env.HEAD_MS ?? "2500");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function tryConnect() {
  return new Promise((resolve) => {
    const s = new net.Socket();
    s.setNoDelay(true);
    s.once("error", () => resolve(null));
    s.connect(PORT, HOST, () => resolve(s));
  });
}
async function connectWithRetry(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const s = await tryConnect();
    if (s) return s;
    await sleep(300);
  }
  throw new Error(`could not connect ${HOST}:${PORT}`);
}
function makeRpc(sock) {
  const pending = [];
  let buf = "";
  sock.on("data", (c) => {
    buf += c.toString("utf8");
    let nl;
    while ((nl = buf.indexOf("\n")) !== -1) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (line) { const p = pending.shift(); if (p) p(line); }
    }
  });
  return (o) => new Promise((res) => { pending.push(res); sock.write(JSON.stringify(o) + "\n"); });
}

async function main() {
  const sock = await connectWithRetry(20000);
  const rpc = makeRpc(sock);

  // The layer control channel opens at instance creation, but Godot creates the XR session and
  // starts submitting frames later. Poll status until the session is up and frames are flowing so
  // we don't screenshot an empty pre-session state.
  const WAIT_MS = Number(process.env.SESSION_WAIT_MS ?? "30000");
  const deadline = Date.now() + WAIT_MS;
  let st = {};
  while (Date.now() < deadline) {
    st = JSON.parse(await rpc({ cmd: "status" }));
    const frames = (st.capture && st.capture.framesObserved) || 0;
    if (st.session && frames > 0) break;
    process.stdout.write(`\r[vrdev] waiting: session=${st.session} frames=${frames}    `);
    await sleep(500);
  }
  console.log("\n[vrdev] status:", JSON.stringify(st));
  if (process.env.DUMP_ACTIONS) console.log("[vrdev] actions:", await rpc({ cmd: "actions" }));
  console.log("[vrdev] baseline view:", await rpc({ cmd: "view" }));
  console.log("[vrdev] baseline shot:", await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 5000 }));

  if (MOVE_INPUT) {
    console.log(`[vrdev] moving: hand=${HAND} input=${MOVE_INPUT} vec2(${MX},${MY}) for ${MOVE_MS}ms`);
    console.log("[vrdev] active:", await rpc({ cmd: "active", hand: HAND, active: true }));
    const deadline = Date.now() + MOVE_MS;
    let n = 0;
    while (Date.now() < deadline) {
      await rpc({ cmd: "active", hand: HAND, active: true });
      await rpc({ cmd: "input", hand: HAND, input: MOVE_INPUT, type: "vec2", x: MX, y: MY });
      n++;
      await sleep(100);
    }
    console.log(`[vrdev] sent ${n} stick injections`);
    console.log("[vrdev] status:", await rpc({ cmd: "status" }));
    console.log("[vrdev] after shot:", await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 5000 }));
  } else if (HEAD_YAW !== undefined) {
    const yaw = (Number(HEAD_YAW) * Math.PI) / 180;
    const qy = Math.sin(yaw / 2), qw = Math.cos(yaw / 2);
    console.log(`[vrdev] head override: yaw=${HEAD_YAW}deg pos(${HX},${HY},${HZ}) -> q(0,${qy.toFixed(3)},0,${qw.toFixed(3)})`);
    console.log("[vrdev] head set:", await rpc({ cmd: "head", x: HX, y: HY, z: HZ, qx: 0, qy, qz: 0, qw }));
    const deadline = Date.now() + HEAD_MS;
    while (Date.now() < deadline) await sleep(100);  // head is sticky in the layer; let it render
    console.log("[vrdev] status:", await rpc({ cmd: "status" }));
    console.log("[vrdev] view:", await rpc({ cmd: "view" }));
    console.log("[vrdev] after shot:", await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 5000 }));
  }
  sock.end();
}
main().catch((e) => { console.error("[vrdev] error:", e.message); process.exit(1); });
