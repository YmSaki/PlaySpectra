// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { send, textResult, sleep, shotImage, depthImage } from "../client.js";

export function registerObserveTools(server: McpServer) {

server.registerTool(
  "vr_status",
  {
    title: "VR session status",
    description:
      "Report the playspectra layer's control-channel state: whether an OpenXR instance/session exists, " +
      "whether XR_EXT_conformance_automation is available (needed for input injection), the runtime " +
      "name, and how many haptic pulses the app has requested (an app-side reaction signal).",
    inputSchema: {},
  },
  async () => textResult(await send({ cmd: "status" })),
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
      "moving scene they differ by motion, not just parallax. " +
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
  "vr_view",
  {
    title: "Get the viewpoint pose + FOV (observe/act bridge)",
    description:
      "Return the per-eye view pose (position + orientation) and projection FOV captured at the app's " +
      "last xrLocateViews (after any vr_set_hmd head override). This is the bridge between the point " +
      "(px,py) seen in a vr_screenshot and a world coordinate you can act on with vr_set_hmd / " +
      "vr_set_controller / vr_look_at / vr_point_at: with the eye pose and FOV you project a world " +
      "point to a screen pixel and back, so observation and action close in the SAME coordinate " +
      "system. The view matrix is the inverse of the eye pose; the projection is built from the FOV " +
      "half-angles (radians; angleLeft/angleDown are typically negative); NDC maps to pixels by the " +
      "captured eye image's width/height (from vr_screenshot metadata). Fields: viewCount, space (the " +
      "reference space the poses are in, e.g. LOCAL), views:[{pose:{x,y,z,qx,qy,qz,qw}, " +
      "fov:{angleLeft,angleRight,angleUp,angleDown}}], note (the exact mapping recipe). Returns " +
      "{available:false} until the app has located views at least once.",
    inputSchema: {},
  },
  async () => textResult(await send({ cmd: "view" })),
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
  "vr_actions",
  {
    title: "Discover the app's actions",
    description:
      "List the VR app's registered action sets and actions — the semantic input vocabulary the app " +
      "defined (e.g. 'Grab', 'Teleport') — so you can drive inputs by NAME and see their bound " +
      "interaction-profile paths instead of GUESSING OpenXR paths for vr_input / vr_click. Returns " +
      "per action set {name, localizedName, attached} and per action {name, localizedName, type " +
      "(numeric + typeName like FLOAT_INPUT/BOOLEAN_INPUT/POSE_INPUT), boundPaths[] (interaction " +
      "profile + bound path, e.g. /user/hand/left/input/squeeze/value), subactionPaths[]}. This is a " +
      "STATIC registry captured as the app creates its actions; live action values / isActive are " +
      "not included (reading them needs the app's session thread). Empty until the app has created " +
      "its action sets and suggested bindings.",
    inputSchema: {},
  },
  async () => textResult(await send({ cmd: "actions" })),
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

}
