// Control-channel client: NDJSON over localhost TCP to the OpenXR API layer (control_channel.cpp).
// The layer's TCP server only exists once the VR app has created an XrInstance, so we connect
// lazily and treat "connection refused" as "no VR app running yet" rather than a hard error.
import net from "node:net";
import { readFile } from "node:fs/promises";

export const HOST = "127.0.0.1";
export const PORT = Number(process.env.PLAYSPECTRA_PORT ?? "52700");

type Pending = { resolve: (v: any) => void; reject: (e: Error) => void };

class ControlClient {
  private socket: net.Socket | null = null;
  private connecting: Promise<void> | null = null;
  private buffer = "";
  private pending: Pending[] = [];

  private async connect(): Promise<void> {
    if (this.socket && !this.socket.destroyed) return;
    if (this.connecting) return this.connecting;
    this.connecting = new Promise<void>((resolve, reject) => {
      const sock = new net.Socket();
      sock.setNoDelay(true);
      const onError = (err: Error) => {
        this.connecting = null;
        this.socket = null;
        reject(err);
      };
      sock.once("error", onError);
      sock.connect(PORT, HOST, () => {
        sock.removeListener("error", onError);
        sock.on("error", () => this.teardown(new Error("socket error")));
        sock.on("close", () => this.teardown(new Error("connection closed")));
        sock.on("data", (chunk) => this.onData(chunk));
        this.socket = sock;
        this.connecting = null;
        resolve();
      });
    });
    return this.connecting;
  }

  private teardown(err: Error) {
    this.socket = null;
    this.buffer = "";
    const p = this.pending;
    this.pending = [];
    for (const req of p) req.reject(err);
  }

  private onData(chunk: Buffer) {
    this.buffer += chunk.toString("utf8");
    let nl: number;
    while ((nl = this.buffer.indexOf("\n")) !== -1) {
      const line = this.buffer.slice(0, nl).trim();
      this.buffer = this.buffer.slice(nl + 1);
      if (!line) continue;
      const req = this.pending.shift();
      if (!req) continue;
      try {
        req.resolve(JSON.parse(line));
      } catch (e) {
        req.reject(new Error(`bad reply from layer: ${line}`));
      }
    }
  }

  async request(obj: unknown): Promise<any> {
    await this.connect();
    const sock = this.socket;
    if (!sock) throw new Error("not connected");
    return new Promise((resolve, reject) => {
      const entry: Pending = { resolve, reject };
      // The layer serves ONE client at a time; if another client holds the connection, our reply
      // never comes. Bound the wait so a tool errors out instead of hanging the agent forever.
      const timer = setTimeout(() => {
        const idx = this.pending.indexOf(entry);
        if (idx !== -1) this.pending.splice(idx, 1);
        reject(new Error("control-channel request timed out (is another client connected?)"));
      }, 30000);
      const wrapped: Pending = {
        resolve: (v) => { clearTimeout(timer); resolve(v); },
        reject: (e) => { clearTimeout(timer); reject(e); },
      };
      this.pending.push(wrapped);
      sock.write(JSON.stringify(obj) + "\n", (err) => {
        if (err) {
          const idx = this.pending.indexOf(wrapped);
          if (idx !== -1) this.pending.splice(idx, 1);
          clearTimeout(timer);
          reject(err);
        }
      });
    });
  }
}

const client = new ControlClient();

// Send a command, mapping connection failures to a structured "not connected" result so the agent
// gets an actionable message instead of a raw exception.
export async function send(obj: unknown): Promise<any> {
  try {
    return await client.request(obj);
  } catch (e) {
    return {
      ok: false,
      connected: false,
      error: `control channel unreachable on ${HOST}:${PORT} — is the VR app running with the playspectra layer enabled? (${(e as Error).message})`,
    };
  }
}

export function textResult(obj: unknown) {
  return { content: [{ type: "text" as const, text: JSON.stringify(obj, null, 2) }] };
}

export const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// Read a PNG at `path` into an MCP image content block (or null on failure).
export async function pngImage(path: unknown) {
  if (typeof path !== "string") return null;
  try {
    const buf = await readFile(path);
    return { type: "image" as const, data: buf.toString("base64"), mimeType: "image/png" };
  } catch {
    return null;
  }
}

// Read a successful screenshot reply's color PNG into an MCP image content block (or null on failure).
export async function shotImage(reply: any) {
  if (reply && reply.ok) return pngImage(reply.path);
  return null;
}

// Read a screenshot reply's depth PNG (16-bit grayscale) into an image block, if depth is present
// and available. Returns null when the app submitted no depth (reply.depth.available === false).
export async function depthImage(reply: any) {
  if (reply && reply.depth && reply.depth.available) return pngImage(reply.depth.depthPath);
  return null;
}
