// Standalone walking-skeleton test client: speaks the raw NDJSON control protocol to the
// vr_agent layer (no MCP layer involved), to validate control channel + conformance-automation
// input injection. Connects with retry, marks the right controller active, drives squeeze/value
// to 1.0 for a few seconds, and prints the layer's status (including the haptic count, which
// rises when hello_xr reacts to the injected grab).
//
// Usage: node scripts/inject_client.mjs [durationMs] [port]
import net from "node:net";

const DURATION_MS = Number(process.argv[2] ?? "6000");
const PORT = Number(process.argv[3] ?? process.env.VR_AGENT_PORT ?? "52700");
const HOST = "127.0.0.1";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function connectWithRetry(timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const sock = await tryConnect();
    if (sock) return sock;
    await sleep(300);
  }
  throw new Error(`could not connect to ${HOST}:${PORT} within ${timeoutMs}ms`);
}

function tryConnect() {
  return new Promise((resolve) => {
    const sock = new net.Socket();
    sock.setNoDelay(true);
    sock.once("error", () => resolve(null));
    sock.connect(PORT, HOST, () => resolve(sock));
  });
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
  console.log(`[client] connecting to ${HOST}:${PORT} ...`);
  const sock = await connectWithRetry(20000);
  console.log("[client] connected");
  const rpc = makeRpc(sock);

  console.log("[client] status:", await rpc({ cmd: "status" }));
  console.log("[client] active right:", await rpc({ cmd: "active", hand: "right", active: true }));

  const deadline = Date.now() + DURATION_MS;
  let n = 0;
  while (Date.now() < deadline) {
    await rpc({ cmd: "input", hand: "right", input: "squeeze/value", type: "float", value: 1.0 });
    n++;
    await sleep(150);
  }
  console.log(`[client] sent ${n} grab injections`);
  console.log("[client] final status:", await rpc({ cmd: "status" }));
  console.log("[client] screenshot(dominant):", await rpc({ cmd: "screenshot", eye: "dominant", timeoutMs: 5000 }));
  console.log("[client] screenshot(left):", await rpc({ cmd: "screenshot", eye: "left", timeoutMs: 5000 }));
  sock.end();
}

main().catch((e) => {
  console.error("[client] error:", e.message);
  process.exit(1);
});
