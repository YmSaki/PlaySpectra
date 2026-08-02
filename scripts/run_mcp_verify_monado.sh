#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Full-chain check of the PlaySpectra MCP server (the AI-agent-facing interface) against the LIVE
# Windows-built Monado stack. Stands up monado-service (:52702) + hello_xr with the capture layer
# (:52700) via lib_monado_stack.sh, then runs playspectra verify mcp, which spawns the MCP
# server over stdio and drives every operate/observe/screenshot/scenario tool. Proves an AI agent can
# operate AND observe (screenshots included) a real VR app on Windows via MCP.
#
# Usage: [PLAYSPECTRA_BIN=<path>] scripts/run_mcp_verify_monado.sh [gfx=D3D11] [secs=60]
set -u
source "$(dirname "${BASH_SOURCE[0]}")/lib_monado_stack.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_playspectra.sh"
GFX="${1:-D3D11}"; SECS="${2:-60}"

mstack_env "$GFX"

RC=99
if mstack_up "$GFX" "$SECS"; then
  sleep 1
  echo "=== playspectra verify mcp (MCP full-chain against live Windows Monado) ==="
  ps_run verify mcp; RC=$?
else
  echo "layer never came up"; grep -iE "error|unsupported|module" "$MSTACK_APPLOG" 2>/dev/null | tail
fi
mstack_down
echo "MCP_VERIFY_RC=$RC"
exit $RC
