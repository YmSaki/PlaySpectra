// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import {
  registerAppTool,
  registerAppResource,
  RESOURCE_MIME_TYPE,
} from "@modelcontextprotocol/ext-apps/server";
import { send, textResult, pngImage } from "../client.js";

// --- MCP App widget (recording viewer) ---
// vr_view_recording carries _meta.ui so hosts with the apps surface (Claude Desktop) render an
// interactive frame-strip + video player; hosts without it (Claude Code CLI) ignore _meta.ui and
// fall back to the same result's inline images + text automatically.
const WIDGET_URI = "ui://vragent/recording-viewer.html";

// The widget iframe's CSP blocks CDN script fetches, so the ext-apps browser bundle must be inlined
// into the HTML at load time (build-mcp-app pattern: rewrite the trailing `export{...}` into a
// `globalThis.ExtApps={...}` assignment so it runs as a plain inline module).
function loadWidgetHtml(): string {
  const req = createRequire(import.meta.url);
  const bundle = readFileSync(req.resolve("@modelcontextprotocol/ext-apps/app-with-deps"), "utf8")
    .replace(/export\{([^}]+)\};?\s*$/, (_, body: string) =>
      "globalThis.ExtApps={" +
      body
        .split(",")
        .map((p) => {
          const [local, exported] = p.split(" as ").map((s) => s.trim());
          return `${exported ?? local}:${local}`;
        })
        .join(",") +
      "};",
    );
  // ../../widgets resolves from both dist/tools/ (node) and src/tools/ (tsx) — same depth.
  const html = readFileSync(new URL("../../widgets/recording-viewer.html", import.meta.url), "utf8");
  return html.replace("/*__EXT_APPS_BUNDLE__*/", () => bundle);
}

// Evenly sample up to `max` paths (always keeping first and last) so a recording can be eyeballed
// in-chat without dumping every frame. MCP clients render inline images; video playback is not part
// of the MCP content model, so sampled frames + the mp4 path is the portable answer.
function sampleEvenly<T>(items: T[], max: number): T[] {
  if (items.length <= max) return items;
  const out: T[] = [];
  for (let i = 0; i < max; i++) {
    out.push(items[Math.round((i * (items.length - 1)) / (max - 1))]);
  }
  return out;
}

// Load sampled frames of a recording dir as MCP image blocks. Frame list comes from manifest.json
// (explicit paths — robust even if numbering ever gets sparse), falling back to rec_*.png scan.
async function frameImages(dir: string, maxFrames: number) {
  const { readFile, readdir } = await import("node:fs/promises");
  let paths: string[] = [];
  try {
    const manifest = JSON.parse(await readFile(`${dir}/manifest.json`, "utf8"));
    paths = (manifest.frames ?? []).map((f: any) => f.path).filter(Boolean);
  } catch {
    try {
      paths = (await readdir(dir))
        .filter((f) => /^rec_\d+\.png$/.test(f))
        .sort()
        .map((f) => `${dir}/${f}`);
    } catch {
      return { images: [], total: 0 };
    }
  }
  const sampled = sampleEvenly(paths, Math.max(1, maxFrames));
  const images = (await Promise.all(sampled.map(pngImage))).filter(
    (i): i is NonNullable<Awaited<ReturnType<typeof pngImage>>> => i !== null,
  );
  return { images, total: paths.length };
}

export function registerRecordingTools(server: McpServer) {

server.registerTool(
  "vr_start_recording",
  {
    title: "Start recording the VR view",
    description:
      "Start capturing frames periodically as PNG files. The layer captures one frame every " +
      "`intervalFrames` frames (default 30 ≈ 2.4 fps at 72 Hz). Recording runs independently of " +
      "vr_screenshot (both can be used simultaneously). Call vr_stop_recording to stop and get " +
      "the manifest + optional video.",
    inputSchema: {
      intervalFrames: z
        .number()
        .optional()
        .describe("Capture every N frames (default 30)"),
      eye: z
        .enum(["left", "right", "dominant"])
        .optional()
        .describe("Which eye to record (default dominant)"),
    },
  },
  async ({ intervalFrames, eye }) => {
    const reply = await send({
      cmd: "start_recording",
      intervalFrames: intervalFrames ?? 30,
      eye: eye ?? "dominant",
    });
    return textResult(reply);
  },
);

server.registerTool(
  "vr_stop_recording",
  {
    title: "Stop recording and get the result",
    description:
      "Stop the recording started by vr_start_recording. Returns the manifest (frame list, " +
      "timestamps, paths). If ffmpeg is available on PATH, also encodes the PNG sequence into " +
      "an mp4 video and returns the video path. Without ffmpeg, only the PNG directory and " +
      "manifest are returned (no error).",
    inputSchema: {},
  },
  async () => {
    const reply = await send({ cmd: "stop_recording" });
    if (!reply.ok || !reply.dir) return textResult(reply);

    let videoPath: string | null = null;
    try {
      const { execFile } = await import("node:child_process");
      const fps = Math.max(1, Math.round(72 / (reply.intervalFrames ?? 30)));
      const out = `${reply.dir}/recording.mp4`;
      await new Promise<void>((resolve, reject) => {
        execFile(
          "ffmpeg",
          ["-y", "-framerate", String(fps), "-i", `${reply.dir}/rec_%04d.png`,
           "-c:v", "libx264", "-pix_fmt", "yuv420p", out],
          { timeout: 60000 },
          (err) => (err ? reject(err) : resolve()),
        );
      });
      videoPath = out;
    } catch {
      // ffmpeg not available or encoding failed — graceful degradation
    }

    // Attach a few sampled frames inline so the result is reviewable in-chat without opening files.
    const { images } = await frameImages(reply.dir, 3);
    return {
      content: [
        ...images,
        ...textResult({
          ...reply,
          ...(videoPath ? { videoPath } : { videoNote: "ffmpeg not found; PNG sequence only" }),
        }).content,
      ],
    };
  },
);

registerAppTool(
  server,
  "vr_view_recording",
  {
    description:
      "Open an interactive viewer for a finished recording (by its directory path, as returned " +
      "from vr_stop_recording): sampled frames as a clickable film strip, plus in-place mp4 " +
      "playback when the host supports widgets. On hosts without widget support the same result " +
      "degrades to inline images + text. First and last frames are always included.",
    annotations: { title: "View VR recording", readOnlyHint: true },
    inputSchema: {
      dir: z.string().describe("Recording directory (the `dir` from vr_start/stop_recording)"),
      maxFrames: z.number().optional().describe("Max frames to return (default 4)"),
    },
    _meta: { ui: { resourceUri: WIDGET_URI } },
  },
  async ({ dir, maxFrames }) => {
    const { images, total } = await frameImages(dir, maxFrames ?? 4);
    if (images.length === 0) {
      return textResult({ ok: false, error: `no readable frames in ${dir} (manifest.json or rec_*.png)` });
    }
    let videoPath: string | undefined;
    try {
      const { stat } = await import("node:fs/promises");
      const p = `${dir}/recording.mp4`;
      if ((await stat(p)).isFile()) videoPath = p;
    } catch {}
    return {
      content: [
        ...images,
        ...textResult({ ok: true, dir, totalFrames: total, shown: images.length, ...(videoPath ? { videoPath } : {}) })
          .content,
      ],
    };
  },
);

// Widget-only media fetch (hidden from Claude's tool list): the iframe cannot touch the filesystem
// or network, so the viewer pulls the mp4 through this and plays it from a Blob URL. Path access is
// restricted to recording artifacts by basename to keep this from being a generic file reader.
registerAppTool(
  server,
  "vragent_get_media",
  {
    description: "Internal: fetch a recording media file (base64) for the recording-viewer widget.",
    annotations: { title: "Get recording media", readOnlyHint: true },
    inputSchema: { path: z.string() },
    _meta: { ui: { visibility: ["app"] } },
  },
  async ({ path }) => {
    const base = path.replace(/\\/g, "/").split("/").pop() ?? "";
    if (!/^(rec_\d+\.png|recording\.mp4)$/.test(base)) {
      return textResult({ ok: false, error: "only recording artifacts (rec_*.png, recording.mp4) are served" });
    }
    try {
      const { readFile } = await import("node:fs/promises");
      const buf = await readFile(path);
      return textResult({
        ok: true,
        mime: base.endsWith(".mp4") ? "video/mp4" : "image/png",
        base64: buf.toString("base64"),
      });
    } catch (e) {
      return textResult({ ok: false, error: (e as Error).message });
    }
  },
);

registerAppResource(server, "Recording Viewer", WIDGET_URI, {}, async () => ({
  contents: [{ uri: WIDGET_URI, mimeType: RESOURCE_MIME_TYPE, text: loadWidgetHtml() }],
}));

}
