#!/usr/bin/env node
// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerObserveTools } from "./tools/observe.js";
import { registerInputTools } from "./tools/input.js";
import { registerPoseTools } from "./tools/pose.js";
import { registerHeadTools } from "./tools/head.js";
import { registerRecordingTools } from "./tools/recording.js";

const server = new McpServer({ name: "playspectra", version: "0.1.0" });

registerObserveTools(server);
registerInputTools(server);
registerPoseTools(server);
registerHeadTools(server);
registerRecordingTools(server);

const transport = new StdioServerTransport();
await server.connect(transport);
