import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { send, textResult, sleep } from "../client.js";

export function registerInputTools(server: McpServer) {

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

}
