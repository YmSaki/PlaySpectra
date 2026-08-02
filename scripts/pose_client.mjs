// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// "前ならえ" (arms-forward) pose demo client for the playspectra control channel.
// Marks both controllers active, sets a sticky grip pose per hand (arms extended forward in the
// layer's LOCAL space), holds it while re-affirming `active`, then captures screenshots.
//
// Positions are parameterised via env so the LOCAL-origin height can be dialled in without editing:
//   POSE_X (half shoulder width, default 0.2), POSE_Y (height rel. LOCAL origin, default -0.2),
//   POSE_Z (forward, negative = in front, default -0.5), HOLD_MS (default 2000).
//
// Usage: node scripts/pose_client.mjs [port]
import net from "node:net";

const PORT = Number(process.argv[2] ?? process.env.PLAYSPECTRA_PORT ?? "52700");
const HOST = "127.0.0.1";
const X = Number(process.env.POSE_X ?? "0.2");
const Y = Number(process.env.POSE_Y ?? "-0.2");
const Z = Number(process.env.POSE_Z ?? "-0.5");
const HOLD_MS = Number(process.env.HOLD_MS ?? "2000");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function tryConnect() {
  return new Promise((resolve) => {
    const sock = new net.Socket();
    sock.setNoDelay(true);
    sock.once("error", () => resolve(null));
    sock.connect(PORT, HOST, () => resolve(sock));
  });
}
async function connectWithRetry(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const sock = await tryConnect();
    if (sock) return sock;
    await sleep(300);
  }
  throw new Error(`could not connect to ${HOST}:${PORT} within ${timeoutMs}ms`);
}
function makeRpc(sock) {
  const pending = [];
  let buffer = "";
  sock.on("data", (chunk) => {
    buffer += chunk.toString("utf8");
    let nl;
    while ((nl = buffer.indexOf("\n")) !== -1) {
      const line = buffer.slice(0, nl).trim();
      buffer = buffer.slice(nl + 1);
      if (!line) continue;
      const p = pending.shift();
      if (p) p(line);
    }
  });
  return (obj) =>
    new Promise((resolve) => {
      pending.push(resolve);
      sock.write(JSON.stringify(obj) + "\n");
    });
}

async function main() {
  console.log(`[pose] connecting ${HOST}:${PORT} ... pose L(${-X},${Y},${Z}) R(${X},${Y},${Z})`);
  const sock = await connectWithRetry(20000);
  const rpc = makeRpc(sock);
  console.log("[pose] status:", await rpc({ cmd: "status" }));

  // 前ならえ: both grips extended forward (-Z), shoulder height (Y), shoulder-width apart (±X).
  // Identity orientation (qw=1) -> cubes axis-aligned so position reads clearly.
  console.log("[pose] active L:", await rpc({ cmd: "active", hand: "left", active: true }));
  console.log("[pose] active R:", await rpc({ cmd: "active", hand: "right", active: true }));
  console.log("[pose] set L:", await rpc({ cmd: "pose", hand: "left", x: -X, y: Y, z: Z }));
  console.log("[pose] set R:", await rpc({ cmd: "pose", hand: "right", x: X, y: Y, z: Z }));

  // Hold: pose is sticky in the layer (re-applied each sync); we re-affirm `active` so the runtime
  // keeps the controllers connected, and (when GRAB=1) drive squeeze=1.0 so hello_xr enlarges the
  // hand cubes -- making the two controllers unmistakable vs the fixed reference-space marker cubes.
  const GRAB = Number(process.env.GRAB ?? "0");
  const deadline = Date.now() + HOLD_MS;
  while (Date.now() < deadline) {
    await rpc({ cmd: "active", hand: "left", active: true });
    await rpc({ cmd: "active", hand: "right", active: true });
    if (GRAB > 0) {
      await rpc({ cmd: "input", hand: "left", input: "squeeze/value", type: "float", value: GRAB });
      await rpc({ cmd: "input", hand: "right", input: "squeeze/value", type: "float", value: GRAB });
    }
    await sleep(100);
  }

  console.log("[pose] status:", await rpc({ cmd: "status" }));
  console.log("[pose] shot dominant:", await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 5000 }));
  console.log("[pose] shot left:", await rpc({ cmd: "screenshot", eye: "left", timeoutMs: 5000 }));
  sock.end();
}
main().catch((e) => {
  console.error("[pose] error:", e.message);
  process.exit(1);
});
