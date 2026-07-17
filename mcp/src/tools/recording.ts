import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { send, textResult } from "../client.js";

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
      const { execSync } = await import("node:child_process");
      const fps = Math.max(1, Math.round(72 / (reply.intervalFrames ?? 30)));
      const out = `${reply.dir}/recording.mp4`;
      execSync(
        `ffmpeg -y -framerate ${fps} -i "${reply.dir}/rec_%04d.png" -c:v libx264 -pix_fmt yuv420p "${out}"`,
        { timeout: 60000, stdio: "pipe" },
      );
      videoPath = out;
    } catch {
      // ffmpeg not available or encoding failed — graceful degradation
    }

    return textResult({
      ...reply,
      ...(videoPath ? { videoPath } : { videoNote: "ffmpeg not found; PNG sequence only" }),
    });
  },
);

}
