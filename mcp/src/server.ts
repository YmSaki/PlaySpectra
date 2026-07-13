#!/usr/bin/env node
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import net from "node:net";
import { readFile } from "node:fs/promises";

// ------------------------------------------------------------------------------------------------
// Control-channel client: NDJSON over localhost TCP to the OpenXR API layer (control_channel.cpp).
// The layer's TCP server only exists once the VR app has created an XrInstance, so we connect
// lazily and treat "connection refused" as "no VR app running yet" rather than a hard error.
// ------------------------------------------------------------------------------------------------
const HOST = "127.0.0.1";
const PORT = Number(process.env.VR_AGENT_PORT ?? "52700");

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
async function send(obj: unknown): Promise<any> {
  try {
    return await client.request(obj);
  } catch (e) {
    return {
      ok: false,
      connected: false,
      error: `control channel unreachable on ${HOST}:${PORT} — is the VR app running with the vr_agent layer enabled? (${(e as Error).message})`,
    };
  }
}

function textResult(obj: unknown) {
  return { content: [{ type: "text" as const, text: JSON.stringify(obj, null, 2) }] };
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// Read a PNG at `path` into an MCP image content block (or null on failure).
async function pngImage(path: unknown) {
  if (typeof path !== "string") return null;
  try {
    const buf = await readFile(path);
    return { type: "image" as const, data: buf.toString("base64"), mimeType: "image/png" };
  } catch {
    return null;
  }
}

// Read a successful screenshot reply's color PNG into an MCP image content block (or null on failure).
async function shotImage(reply: any) {
  if (reply && reply.ok) return pngImage(reply.path);
  return null;
}

// Read a screenshot reply's depth PNG (16-bit grayscale) into an image block, if depth is present
// and available. Returns null when the app submitted no depth (reply.depth.available === false).
async function depthImage(reply: any) {
  if (reply && reply.depth && reply.depth.available) return pngImage(reply.depth.depthPath);
  return null;
}

// ------------------------------------------------------------------------------------------------
// Quaternion helpers for the ergonomic yaw/pitch/roll interface. The LAYER is the single source of
// truth for injected poses (sticky state); tools that need the current pose (vr_move, later
// vr_look_at) query it via pose_get/head_get rather than mirroring state, so they always act on
// what is really held.
// ------------------------------------------------------------------------------------------------
type Quat = { x: number; y: number; z: number; w: number };
const IDENTITY: Quat = { x: 0, y: 0, z: 0, w: 1 };

function normQuat(q: Quat): Quat {
  const n = Math.hypot(q.x, q.y, q.z, q.w);
  if (n < 1e-8) return { ...IDENTITY };
  return { x: q.x / n, y: q.y / n, z: q.z / n, w: q.w / n };
}
function quatAxis(ax: number, ay: number, az: number, deg: number): Quat {
  const h = (deg * Math.PI) / 180 / 2;
  const s = Math.sin(h);
  return { x: ax * s, y: ay * s, z: az * s, w: Math.cos(h) };
}
function quatMul(a: Quat, b: Quat): Quat {
  return {
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}
// yaw about +Y (turn), pitch about +X (look up/down), roll about +Z. Applied roll->pitch->yaw.
function eulerToQuat(yaw = 0, pitch = 0, roll = 0): Quat {
  return quatMul(quatMul(quatAxis(0, 1, 0, yaw), quatAxis(1, 0, 0, pitch)), quatAxis(0, 0, 1, roll));
}
// Resolve a (normalized) orientation from EITHER a raw quaternion OR yaw/pitch/roll (never both),
// else identity. Returns an error string if both families are supplied (silently dropping one is a
// footgun). Output is always unit-length so downstream math (vr_look_at) stays well-defined.
function orientationFrom(a: {
  qx?: number; qy?: number; qz?: number; qw?: number;
  yaw?: number; pitch?: number; roll?: number;
}): { ok: true; q: Quat } | { ok: false; error: string } {
  const hasQuat = a.qx !== undefined || a.qy !== undefined || a.qz !== undefined || a.qw !== undefined;
  const hasEuler = a.yaw !== undefined || a.pitch !== undefined || a.roll !== undefined;
  if (hasQuat && hasEuler)
    return { ok: false, error: "provide EITHER a quaternion (qx..qw) OR yaw/pitch/roll, not both" };
  if (hasQuat) return { ok: true, q: normQuat({ x: a.qx ?? 0, y: a.qy ?? 0, z: a.qz ?? 0, w: a.qw ?? 1 }) };
  if (hasEuler) return { ok: true, q: normQuat(eulerToQuat(a.yaw, a.pitch, a.roll)) };
  return { ok: true, q: { ...IDENTITY } };
}

// --- vector + look-rotation helpers (WU3: vr_point_at / vr_look_at) ---
type Vec3 = { x: number; y: number; z: number };
const vsub = (a: Vec3, b: Vec3): Vec3 => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
const vdot = (a: Vec3, b: Vec3): number => a.x * b.x + a.y * b.y + a.z * b.z;
const vcross = (a: Vec3, b: Vec3): Vec3 => ({
  x: a.y * b.z - a.z * b.y,
  y: a.z * b.x - a.x * b.z,
  z: a.x * b.y - a.y * b.x,
});
function vnorm(a: Vec3): Vec3 {
  const n = Math.hypot(a.x, a.y, a.z);
  return n < 1e-8 ? { x: 0, y: 0, z: -1 } : { x: a.x / n, y: a.y / n, z: a.z / n };
}

// Orientation whose FORWARD (OpenXR -Z) axis points from `from` to `target`, +Y roughly up.
// Returns null if from==target (no direction). Builds an orthonormal basis then converts to quat.
function lookQuat(from: Vec3, target: Vec3): Quat | null {
  const dir = vsub(target, from);
  if (Math.hypot(dir.x, dir.y, dir.z) < 1e-8) return null;
  const fwd = vnorm(dir);
  const zA = { x: -fwd.x, y: -fwd.y, z: -fwd.z }; // pose +Z is backward, so +Z = -forward
  let up: Vec3 = { x: 0, y: 1, z: 0 };
  if (Math.abs(vdot(fwd, up)) > 0.999) up = { x: 0, y: 0, z: -1 }; // looking near-vertical
  const xA = vnorm(vcross(up, zA));
  const yA = vcross(zA, xA);
  // Columns [xA yA zA] form the rotation matrix; convert to quaternion.
  const m00 = xA.x, m10 = xA.y, m20 = xA.z;
  const m01 = yA.x, m11 = yA.y, m21 = yA.z;
  const m02 = zA.x, m12 = zA.y, m22 = zA.z;
  const tr = m00 + m11 + m22;
  let q: Quat;
  if (tr > 0) {
    const s = 0.5 / Math.sqrt(tr + 1);
    q = { w: 0.25 / s, x: (m21 - m12) * s, y: (m02 - m20) * s, z: (m10 - m01) * s };
  } else if (m00 > m11 && m00 > m22) {
    const s = 2 * Math.sqrt(1 + m00 - m11 - m22);
    q = { w: (m21 - m12) / s, x: 0.25 * s, y: (m01 + m10) / s, z: (m02 + m20) / s };
  } else if (m11 > m22) {
    const s = 2 * Math.sqrt(1 + m11 - m00 - m22);
    q = { w: (m02 - m20) / s, x: (m01 + m10) / s, y: 0.25 * s, z: (m12 + m21) / s };
  } else {
    const s = 2 * Math.sqrt(1 + m22 - m00 - m11);
    q = { w: (m10 - m01) / s, x: (m02 + m20) / s, y: (m12 + m21) / s, z: 0.25 * s };
  }
  return normQuat(q);
}

const server = new McpServer({ name: "vr-mcp", version: "0.1.0" });

server.registerTool(
  "vr_status",
  {
    title: "VR session status",
    description:
      "Report the vr_agent layer's control-channel state: whether an OpenXR instance/session exists, " +
      "whether XR_EXT_conformance_automation is available (needed for input injection), the runtime " +
      "name, and how many haptic pulses the app has requested (an app-side reaction signal).",
    inputSchema: {},
  },
  async () => textResult(await send({ cmd: "status" })),
);

server.registerTool(
  "vr_input",
  {
    title: "Set a controller input",
    description:
      "Inject a controller input value into the VR app, as if a real controller were touched. " +
      "Applied on the app's next xrSyncActions via XR_EXT_conformance_automation. " +
      "`input` is the OpenXR input path suffix, e.g. 'squeeze/value', 'trigger/value', " +
      "'thumbstick' (vec2), 'a/click' (bool), 'b/click', 'thumbstick/click'.",
    inputSchema: {
      hand: z.enum(["left", "right"]).describe("Which controller"),
      input: z.string().describe("Input path suffix under /user/hand/<hand>/input/, e.g. 'squeeze/value'"),
      type: z.enum(["float", "bool", "vec2"]).describe("Value kind"),
      value: z.number().optional().describe("float value (0..1) or bool (0/1); required for float/bool"),
      x: z.number().optional().describe("vec2 X (-1..1)"),
      y: z.number().optional().describe("vec2 Y (-1..1)"),
    },
  },
  async ({ hand, input, type, value, x, y }) => {
    const cmd: Record<string, unknown> = { cmd: "input", hand, input, type };
    if (type === "vec2") {
      cmd.x = x ?? 0;
      cmd.y = y ?? 0;
    } else if (type === "bool") {
      cmd.value = value ? true : false;
    } else {
      cmd.value = value ?? 0;
    }
    return textResult(await send(cmd));
  },
);

server.registerTool(
  "vr_active",
  {
    title: "Mark a controller active/inactive",
    description:
      "Tell the runtime a controller is connected (active) or disconnected, via " +
      "XR_EXT_conformance_automation. Some runtimes need this before input on that hand registers. " +
      "Defaults the interaction profile to Oculus Touch (Meta simulator).",
    inputSchema: {
      hand: z.enum(["left", "right"]).describe("Which controller"),
      active: z.boolean().optional().describe("true = connected (default), false = disconnected"),
      profile: z
        .string()
        .optional()
        .describe("Interaction profile path, default /interaction_profiles/oculus/touch_controller"),
    },
  },
  async ({ hand, active, profile }) => {
    const cmd: Record<string, unknown> = { cmd: "active", hand, active: active ?? true };
    if (profile) cmd.profile = profile;
    return textResult(await send(cmd));
  },
);

server.registerTool(
  "vr_screenshot",
  {
    title: "Capture the VR view",
    description:
      "Capture the app's rendered view as a PNG image (read-only observation of what the headset " +
      "shows). Returns the actual pixels (image content) plus metadata (graphics API, dimensions, " +
      "format). `eye` selects left / right / dominant (default dominant = right); the layer copies " +
      "that eye's projection subimage at the next xrEndFrame. `eye:'both'` returns the left AND right " +
      "images — NOTE they are captured on SEPARATE frames (not a simultaneous stereo pair), so on a " +
      "moving scene they differ by motion, not just parallax. (Vulkan today; D3D11/D3D12 pending.) " +
      "`withDepth:true` also returns a 16-bit grayscale depth map (nearest=black, far=white) plus " +
      "depthMeta (nearZ/farZ/reversedZ/minView/maxView/encoding) IF the app submits depth via " +
      "XrCompositionLayerDepthInfoKHR; many apps do not, in which case depth.available is false.",
    inputSchema: {
      eye: z
        .enum(["left", "right", "dominant", "both"])
        .optional()
        .describe("Which eye (default dominant); 'both' returns left+right (separate frames)"),
      timeoutMs: z.number().optional().describe("Max ms to wait for the next frame (default 5000)"),
      withDepth: z
        .boolean()
        .optional()
        .describe("Also capture a depth map if the app submits one (default false)"),
    },
  },
  async ({ eye, timeoutMs, withDepth }) => {
    const e = eye ?? "dominant";
    const to = timeoutMs ?? 5000;
    const wd = withDepth ?? false;
    if (e === "both") {
      const l = await send({ cmd: "screenshot", eye: "left", timeoutMs: to, withDepth: wd });
      const r = await send({ cmd: "screenshot", eye: "right", timeoutMs: to, withDepth: wd });
      const content: any[] = [];
      const li = await shotImage(l);
      if (li) content.push(li);
      if (wd) { const ld = await depthImage(l); if (ld) content.push(ld); }
      const ri = await shotImage(r);
      if (ri) content.push(ri);
      if (wd) { const rd = await depthImage(r); if (rd) content.push(rd); }
      content.push({ type: "text" as const, text: JSON.stringify({ left: l, right: r }, null, 2) });
      return { content };
    }
    // The layer writes a PNG to disk and returns its path; the MCP server runs on the same host, so
    // read the file and hand the agent actual pixels (image content) plus metadata. With withDepth,
    // the depth PNG (when the app submitted one) is returned as a second image block.
    const reply = await send({ cmd: "screenshot", eye: e, timeoutMs: to, withDepth: wd });
    const img = await shotImage(reply);
    const content: any[] = [];
    if (img) content.push(img);
    if (wd) { const di = await depthImage(reply); if (di) content.push(di); }
    if (content.length === 0) return textResult(reply);
    content.push({ type: "text" as const, text: JSON.stringify(reply, null, 2) });
    return { content };
  },
);

server.registerTool(
  "vr_set_controller",
  {
    title: "Set a controller pose",
    description:
      "Place a controller at a pose (position + orientation) in the app's world space (LOCAL: " +
      "-Z forward, +Y up, metres). Held until vr_clear_controller / vr_reset. Orientation may be " +
      "given as yaw/pitch/roll degrees (ergonomic) or a raw quaternion; default identity.",
    inputSchema: {
      hand: z.enum(["left", "right"]),
      x: z.number().describe("metres, +X right"),
      y: z.number().describe("metres, +Y up"),
      z: z.number().describe("metres, -Z forward (in front)"),
      yaw: z.number().optional().describe("deg about +Y"),
      pitch: z.number().optional().describe("deg about +X"),
      roll: z.number().optional().describe("deg about +Z"),
      qx: z.number().optional(),
      qy: z.number().optional(),
      qz: z.number().optional(),
      qw: z.number().optional(),
    },
  },
  async ({ hand, x, y, z: zz, yaw, pitch, roll, qx, qy, qz, qw }) => {
    const o = orientationFrom({ qx, qy, qz, qw, yaw, pitch, roll });
    if (!o.ok) return textResult({ ok: false, error: o.error });
    const q = o.q;
    return textResult(
      await send({ cmd: "pose", hand, x, y, z: zz, qx: q.x, qy: q.y, qz: q.z, qw: q.w }),
    );
  },
);

server.registerTool(
  "vr_clear_controller",
  {
    title: "Clear a controller pose override",
    description: "Stop overriding a controller's pose; the runtime's own pose takes over again.",
    inputSchema: { hand: z.enum(["left", "right"]) },
  },
  async ({ hand }) => textResult(await send({ cmd: "pose_clear", hand })),
);

server.registerTool(
  "vr_set_hmd",
  {
    title: "Set the headset (viewpoint) pose",
    description:
      "Move the viewpoint: override the head pose in the app's world space (LOCAL: -Z forward, " +
      "+Y up, metres). Applied inside xrLocateViews (keeps the runtime's IPD + FOV). Orientation " +
      "as yaw/pitch/roll degrees or a raw quaternion; default identity. Held until vr_reset.",
    inputSchema: {
      x: z.number().optional().describe("metres, +X right (default 0)"),
      y: z.number().optional().describe("metres, +Y up (default 0)"),
      z: z.number().optional().describe("metres, -Z forward (default 0)"),
      yaw: z.number().optional().describe("deg about +Y (turn L/R)"),
      pitch: z.number().optional().describe("deg about +X (look up/down)"),
      roll: z.number().optional().describe("deg about +Z (tilt)"),
      qx: z.number().optional(),
      qy: z.number().optional(),
      qz: z.number().optional(),
      qw: z.number().optional(),
    },
  },
  async ({ x, y, z: zz, yaw, pitch, roll, qx, qy, qz, qw }) => {
    const o = orientationFrom({ qx, qy, qz, qw, yaw, pitch, roll });
    if (!o.ok) return textResult({ ok: false, error: o.error });
    const q = o.q;
    const px = x ?? 0, py = y ?? 0, pz = zz ?? 0;
    return textResult(
      await send({ cmd: "head", x: px, y: py, z: pz, qx: q.x, qy: q.y, qz: q.z, qw: q.w }),
    );
  },
);

server.registerTool(
  "vr_clear_hmd",
  {
    title: "Clear the head (viewpoint) override",
    description: "Stop overriding the viewpoint; the runtime's own head tracking drives the view again.",
    inputSchema: {},
  },
  async () => textResult(await send({ cmd: "head_clear" })),
);

server.registerTool(
  "vr_move",
  {
    title: "Move head/controller by a relative delta",
    description:
      "Translate the head or a controller by (dx,dy,dz) metres from its CURRENT injected pose " +
      "(queried from the layer, the single source of truth). Orientation is preserved. Errors if " +
      "that target has no active override yet — set one first with vr_set_hmd / vr_set_controller.",
    inputSchema: {
      target: z.enum(["head", "left", "right"]),
      dx: z.number().default(0),
      dy: z.number().default(0),
      dz: z.number().default(0),
    },
  },
  async ({ target, dx, dy, dz }) => {
    // Query the layer (single source of truth) for the current override, then nudge it. If nothing
    // is overridden yet, there's no meaningful base to move from -> error rather than warp to origin.
    const cur = await send(target === "head" ? { cmd: "head_get" } : { cmd: "pose_get", hand: target });
    if (!cur.ok) return textResult(cur);
    if (!cur.active)
      return textResult({
        ok: false,
        error: `no active ${target} override to move; set one first with ${
          target === "head" ? "vr_set_hmd" : "vr_set_controller"
        }`,
      });
    const nx = cur.x + dx, ny = cur.y + dy, nz = cur.z + dz;
    const cmd =
      target === "head"
        ? { cmd: "head", x: nx, y: ny, z: nz, qx: cur.qx, qy: cur.qy, qz: cur.qz, qw: cur.qw }
        : { cmd: "pose", hand: target, x: nx, y: ny, z: nz, qx: cur.qx, qy: cur.qy, qz: cur.qz, qw: cur.qw };
    return textResult(await send(cmd));
  },
);

server.registerTool(
  "vr_reset",
  {
    title: "Clear all overrides",
    description:
      "Drop every injected override — both controller poses and the head — so the runtime reverts " +
      "to its own poses. Does not affect button/analog inputs.",
    inputSchema: {},
  },
  async () => textResult(await send({ cmd: "reset" })),
);

server.registerTool(
  "vr_recenter",
  {
    title: "Freeze the viewpoint at the origin",
    description:
      "Pin the viewpoint to the world origin looking forward (identity head override). NOTE: this " +
      "FREEZES the view — runtime head tracking will no longer move it; call vr_clear_hmd to return " +
      "to runtime tracking. Leaves controllers as-is. (Equivalent to vr_set_hmd with no arguments.)",
    inputSchema: {},
  },
  async () => textResult(await send({ cmd: "head", x: 0, y: 0, z: 0, qx: 0, qy: 0, qz: 0, qw: 1 })),
);

// Resolve a "from" position for the aim tools: explicit fx/fy/fz wins; else the target's current
// injected override (queried from the layer); else an error (we can't aim from an unknown position).
async function resolveFrom(
  getCmd: Record<string, unknown>,
  f: { fx?: number; fy?: number; fz?: number },
  setterHint: string,
): Promise<{ ok: true; pos: Vec3 } | { ok: false; error: string }> {
  if (f.fx !== undefined || f.fy !== undefined || f.fz !== undefined) {
    return { ok: true, pos: { x: f.fx ?? 0, y: f.fy ?? 0, z: f.fz ?? 0 } };
  }
  const cur = await send(getCmd);
  if (!cur.ok) return { ok: false, error: cur.error ?? "control channel error" };
  if (!cur.active)
    return { ok: false, error: `no known position; pass fx/fy/fz or set one first with ${setterHint}` };
  return { ok: true, pos: { x: cur.x, y: cur.y, z: cur.z } };
}

server.registerTool(
  "vr_point_at",
  {
    title: "Aim a controller at a world point",
    description:
      "Orient a controller's GRIP pose so its forward (-Z) axis points at world point (tx,ty,tz), " +
      "keeping position. Uses the controller's current injected position unless fx/fy/fz given (LOCAL " +
      "metres); requires a prior vr_set_controller or explicit fx/fy/fz. NOTE: this points the GRIP " +
      "pose's -Z; an app that draws its pointer ray from the AIM pose will see the ray offset by the " +
      "fixed grip->aim rotation (aim-pose injection is a possible follow-up).",
    inputSchema: {
      hand: z.enum(["left", "right"]),
      tx: z.number(), ty: z.number(), tz: z.number(),
      fx: z.number().optional(), fy: z.number().optional(), fz: z.number().optional(),
    },
  },
  async ({ hand, tx, ty, tz, fx, fy, fz }) => {
    const from = await resolveFrom({ cmd: "pose_get", hand }, { fx, fy, fz }, "vr_set_controller");
    if (!from.ok) return textResult({ ok: false, error: from.error });
    const q = lookQuat(from.pos, { x: tx, y: ty, z: tz });
    if (!q) return textResult({ ok: false, error: "target coincides with controller position" });
    return textResult(
      await send({ cmd: "pose", hand, x: from.pos.x, y: from.pos.y, z: from.pos.z, qx: q.x, qy: q.y, qz: q.z, qw: q.w }),
    );
  },
);

server.registerTool(
  "vr_look_at",
  {
    title: "Aim the viewpoint at a world point",
    description:
      "Orient the head so the view looks at world point (tx,ty,tz), keeping the head position. Uses " +
      "the current head-override position unless fx/fy/fz are given (LOCAL metres). Freezes the view " +
      "(head tracking won't move it); vr_clear_hmd returns to runtime tracking.",
    inputSchema: {
      tx: z.number(), ty: z.number(), tz: z.number(),
      fx: z.number().optional(), fy: z.number().optional(), fz: z.number().optional(),
    },
  },
  async ({ tx, ty, tz, fx, fy, fz }) => {
    // Same single-source-of-truth / no-silent-warp contract as vr_point_at & vr_move: use the layer's
    // current head position, or explicit fx/fy/fz, else error (pass fx:0,fy:0,fz:0 to look from origin).
    const from = await resolveFrom({ cmd: "head_get" }, { fx, fy, fz }, "vr_set_hmd");
    if (!from.ok) return textResult({ ok: false, error: from.error });
    const q = lookQuat(from.pos, { x: tx, y: ty, z: tz });
    if (!q) return textResult({ ok: false, error: "target coincides with head position" });
    return textResult(
      await send({ cmd: "head", x: from.pos.x, y: from.pos.y, z: from.pos.z, qx: q.x, qy: q.y, qz: q.z, qw: q.w }),
    );
  },
);

server.registerTool(
  "vr_click",
  {
    title: "Press then release a controller input",
    description:
      "Click: drive an input to its pressed value, wait holdMs, then release to 0/false. `type` " +
      "'bool' for buttons (a/click, b/click, thumbstick/click), 'float' for trigger/grip values.",
    inputSchema: {
      hand: z.enum(["left", "right"]),
      input: z.string().describe("input path suffix, e.g. 'a/click', 'trigger/value'"),
      type: z.enum(["float", "bool"]).default("bool"),
      value: z.number().optional().describe("pressed value for float (0..1, default 1); bool presses true"),
      holdMs: z.number().optional().describe("hold before release (default 150)"),
    },
  },
  async ({ hand, input, type, value, holdMs }) => {
    const press =
      type === "bool"
        ? { cmd: "input", hand, input, type, value: true }
        : { cmd: "input", hand, input, type, value: value ?? 1 };
    const release = { cmd: "input", hand, input, type, value: type === "bool" ? false : 0 };
    const p = await send(press);
    if (p.ok === false) return textResult(p);
    await sleep(holdMs ?? 150);
    const r = await send(release);
    return textResult({ ok: r.ok !== false, press: p, release: r });
  },
);

server.registerTool(
  "vr_wait",
  {
    title: "Wait for N rendered frames",
    description:
      "Block until the app renders `frames` more frames (polls the layer's frame counter) or " +
      "timeoutMs elapses. Use to let an injected input/pose settle before a screenshot.",
    inputSchema: {
      frames: z.number().default(2),
      timeoutMs: z.number().optional().describe("max ms to wait (default 5000)"),
    },
  },
  async ({ frames, timeoutMs }) => {
    const to = timeoutMs ?? 5000;
    const t0 = Date.now();
    const s0 = await send({ cmd: "status" });
    if (s0.ok === false) return textResult(s0);
    const base = s0.capture?.framesObserved ?? 0;
    while (Date.now() - t0 < to) {
      const s = await send({ cmd: "status" });
      const cur = s.capture?.framesObserved ?? base;
      if (cur - base >= frames) return textResult({ ok: true, waitedFrames: cur - base, framesObserved: cur });
      await sleep(30);
    }
    return textResult({ ok: false, error: `timed out waiting for ${frames} frames` });
  },
);

server.registerTool(
  "vr_haptics",
  {
    title: "Recent haptic pulses",
    description:
      "Return the most recent haptic pulses the app requested (seq, hand, amplitude) — an app-reaction " +
      "signal (e.g. hello_xr buzzes on grab). Useful to confirm an injected input reached the app.",
    inputSchema: { limit: z.number().optional().describe("max entries, newest last (default 20)") },
  },
  async ({ limit }) => textResult(await send({ cmd: "haptics", limit: limit ?? 20 })),
);

server.registerTool(
  "vr_capture_sequence",
  {
    title: "Capture a burst of frames",
    description:
      "Take `count` screenshots spaced `intervalMs` apart and return them as images, for reviewing " +
      "motion over time (e.g. an animation or an injected movement). (GIF assembly is a follow-up; " +
      "this returns the individual frames.)",
    inputSchema: {
      count: z.number().default(4),
      intervalMs: z.number().default(200),
      eye: z.enum(["left", "right", "dominant"]).optional(),
    },
  },
  async ({ count, intervalMs, eye }) => {
    const e = eye ?? "dominant";
    const n = Math.min(Math.max(Math.floor(count), 1), 16);
    const content: any[] = [];
    const meta: any[] = [];
    for (let i = 0; i < n; i++) {
      const reply = await send({ cmd: "screenshot", eye: e, timeoutMs: 5000 });
      const img = await shotImage(reply);
      if (img) content.push(img);
      meta.push({ i, ok: reply.ok, path: reply.path, error: reply.error });
      if (i < n - 1) await sleep(intervalMs);
    }
    content.push({ type: "text" as const, text: JSON.stringify({ eye: e, frames: meta }, null, 2) });
    return { content };
  },
);

// TODO(WU5): vr_actions (action-set/action dump) + aim-pose override, xrCreateAction hand tracking.

const transport = new StdioServerTransport();
await server.connect(transport);
