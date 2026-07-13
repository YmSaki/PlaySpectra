// Head / viewpoint override demo client. Sets a sticky head pose via the `head` command (layer
// overrides xrLocateViews + xrLocateSpace(VIEW)) so the rendered viewpoint moves, then captures.
//
// Env: HEAD_YAW_DEG (yaw about +Y, default 0), HEAD_X/HEAD_Y/HEAD_Z (position, default 0),
//      HOLD_MS (default 2000). Usage: node scripts/head_client.mjs [port]
import net from "node:net";

const PORT = Number(process.argv[2] ?? process.env.VR_AGENT_PORT ?? "52700");
const HOST = "127.0.0.1";
const YAW = (Number(process.env.HEAD_YAW_DEG ?? "0") * Math.PI) / 180;
const HX = Number(process.env.HEAD_X ?? "0");
const HY = Number(process.env.HEAD_Y ?? "0");
const HZ = Number(process.env.HEAD_Z ?? "0");
const HOLD_MS = Number(process.env.HOLD_MS ?? "2000");

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
  // Yaw about +Y: q = (0, sin(θ/2), 0, cos(θ/2)).
  const qy = Math.sin(YAW / 2), qw = Math.cos(YAW / 2);
  console.log(`[head] connecting; yaw=${(YAW * 180) / Math.PI}deg pos(${HX},${HY},${HZ}) -> q(0,${qy.toFixed(3)},0,${qw.toFixed(3)})`);
  const sock = await connectWithRetry(20000);
  const rpc = makeRpc(sock);
  console.log("[head] set:", await rpc({ cmd: "head", x: HX, y: HY, z: HZ, qx: 0, qy, qz: 0, qw }));

  const deadline = Date.now() + HOLD_MS;
  while (Date.now() < deadline) await sleep(100);  // head is sticky in the layer; just let it render

  console.log("[head] status:", await rpc({ cmd: "status" }));
  console.log("[head] shot dominant:", await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 5000 }));
  sock.end();
}
main().catch((e) => { console.error("[head] error:", e.message); process.exit(1); });
