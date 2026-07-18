import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { send, textResult, sleep } from "../client.js";
import { orientationFrom, lookQuat, type Vec3 } from "../math.js";

// Resolve a "from" position for the aim tools: explicit fx/fy/fz wins; else the target's current
// injected override (queried from the layer); else an error (we can't aim from an unknown position).
export async function resolveFrom(
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

export function registerPoseTools(server: McpServer) {

server.registerTool(
  "vr_set_controller",
  {
    title: "Set a controller pose",
    description:
      "Place a controller at a pose (position + orientation) in the app's world space (LOCAL: " +
      "-Z forward, +Y up, metres). NOTE: the LOCAL origin differs per runtime (Meta XR Simulator " +
      "puts the HEAD at y≈0; SteamVR puts the floor at y=0) — call vr_view first and compute " +
      "coordinates relative to the view pose, or the object may land out of view. " +
      "Held until vr_clear_controller / vr_reset. Orientation may be " +
      "given as yaw/pitch/roll degrees (ergonomic) or a raw quaternion; default identity. " +
      "Optional durationMs glides the controller linearly from its previous injected pose to the " +
      "target over that time (like a real hand moving); the tool returns after the glide completes.",
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
      durationMs: z.number().optional().describe("glide time in ms (default 0 = snap)"),
    },
  },
  async ({ hand, x, y, z: zz, yaw, pitch, roll, qx, qy, qz, qw, durationMs }) => {
    const o = orientationFrom({ qx, qy, qz, qw, yaw, pitch, roll });
    if (!o.ok) return textResult({ ok: false, error: o.error });
    const q = o.q;
    const reply = await send({
      cmd: "pose", hand, x, y, z: zz, qx: q.x, qy: q.y, qz: q.z, qw: q.w,
      ...(durationMs && durationMs > 0 ? { durationMs } : {}),
    });
    // Playwright-like: an animated action resolves when the motion is done, so the caller's next
    // observe (screenshot/view) sees the settled pose. The layer glides on its own; we just wait.
    if (reply?.ok && durationMs && durationMs > 0) await sleep(durationMs);
    return textResult(reply);
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
  "vr_move",
  {
    title: "Move head/controller by a relative delta",
    description:
      "Translate the head or a controller by (dx,dy,dz) metres from its CURRENT injected pose " +
      "(queried from the layer, the single source of truth). Orientation is preserved. Errors if " +
      "that target has no active override yet — set one first with vr_set_hmd / vr_set_controller. " +
      "Optional durationMs glides instead of snapping (the tool returns after the glide).",
    inputSchema: {
      target: z.enum(["head", "left", "right"]),
      dx: z.number().default(0),
      dy: z.number().default(0),
      dz: z.number().default(0),
      durationMs: z.number().optional().describe("glide time in ms (default 0 = snap)"),
    },
  },
  async ({ target, dx, dy, dz, durationMs }) => {
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
    const dur = durationMs && durationMs > 0 ? { durationMs } : {};
    const cmd =
      target === "head"
        ? { cmd: "head", x: nx, y: ny, z: nz, qx: cur.qx, qy: cur.qy, qz: cur.qz, qw: cur.qw, ...dur }
        : { cmd: "pose", hand: target, x: nx, y: ny, z: nz, qx: cur.qx, qy: cur.qy, qz: cur.qz, qw: cur.qw, ...dur };
    const reply = await send(cmd);
    if (reply?.ok && durationMs && durationMs > 0) await sleep(durationMs);
    return textResult(reply);
  },
);

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

}
