#!/usr/bin/env node
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerObserveTools } from "./tools/observe.js";
import { registerInputTools } from "./tools/input.js";
import { registerPoseTools } from "./tools/pose.js";
import { registerHeadTools } from "./tools/head.js";
import { registerRecordingTools } from "./tools/recording.js";

const server = new McpServer({ name: "vr-mcp", version: "0.1.0" });

registerObserveTools(server);
registerInputTools(server);
registerPoseTools(server);
registerHeadTools(server);
registerRecordingTools(server);

const transport = new StdioServerTransport();
await server.connect(transport);
