import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { send, textResult, sleep } from "../client.js";
import { orientationFrom, lookQuat } from "../math.js";
import { resolveFrom } from "./pose.js";

export function registerHeadTools(server: McpServer) {

server.registerTool(
  "vr_set_hmd",
  {
    title: "Set the headset (viewpoint) pose",
    description:
      "Move the viewpoint: override the head pose in the app's world space (LOCAL: -Z forward, " +
      "+Y up, metres). NOTE: the LOCAL origin differs per runtime (Meta XR Simulator puts the " +
      "HEAD at y≈0; SteamVR puts the floor at y=0) — call vr_view first to learn the current " +
      "eye height before picking y. Applied inside xrLocateViews (keeps the runtime's IPD + FOV). Orientation " +
      "as yaw/pitch/roll degrees or a raw quaternion; default identity. Held until vr_reset. " +
      "Optional durationMs glides the viewpoint smoothly from its previous injected pose to the " +
      "target over that time (a comfortable move, not a teleport); the tool returns after the glide.",
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
      durationMs: z.number().optional().describe("glide time in ms (default 0 = snap)"),
    },
  },
  async ({ x, y, z: zz, yaw, pitch, roll, qx, qy, qz, qw, durationMs }) => {
    const o = orientationFrom({ qx, qy, qz, qw, yaw, pitch, roll });
    if (!o.ok) return textResult({ ok: false, error: o.error });
    const q = o.q;
    const px = x ?? 0, py = y ?? 0, pz = zz ?? 0;
    const reply = await send({
      cmd: "head", x: px, y: py, z: pz, qx: q.x, qy: q.y, qz: q.z, qw: q.w,
      ...(durationMs && durationMs > 0 ? { durationMs } : {}),
    });
    if (reply?.ok && durationMs && durationMs > 0) await sleep(durationMs);
    return textResult(reply);
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

}
