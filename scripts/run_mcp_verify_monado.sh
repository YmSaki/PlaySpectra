#!/usr/bin/env bash
# Full-chain check of the PlaySpectra MCP server (the AI-agent-facing interface) against the LIVE
# Windows-built Monado stack. Stands up monado-service (:52702) + hello_xr with the capture layer
# (:52700) via lib_monado_stack.sh, then runs tools/playspectra_mcp_verify.py, which spawns the MCP
# server over stdio and drives every operate/observe/screenshot/scenario tool. Proves an AI agent can
# operate AND observe (screenshots included) a real VR app on Windows via MCP.
#
# Requires the `mcp` Python SDK. Install it in a VENV, never a shared global env -- mcp pulls a newer
# pydantic/starlette and will break other apps in a shared interpreter. Then point PY at the venv:
#   python -m venv .venv-mcp && .venv-mcp/Scripts/python -m pip install mcp
#   PY=.venv-mcp/Scripts/python.exe scripts/run_mcp_verify_monado.sh
# Usage: [PY=<python-with-mcp>] scripts/run_mcp_verify_monado.sh [gfx=D3D11] [secs=60]
set -u
source "$(dirname "${BASH_SOURCE[0]}")/lib_monado_stack.sh"
GFX="${1:-D3D11}"; SECS="${2:-60}"
PY="${PY:-python}"

mstack_env "$GFX"

RC=99
if mstack_up "$GFX" "$SECS"; then
  sleep 1
  echo "=== playspectra_mcp_verify.py (MCP full-chain against live Windows Monado) ==="
  "$PY" tools/playspectra_mcp_verify.py; RC=$?
else
  echo "layer never came up"; grep -iE "error|unsupported|module" "$MSTACK_APPLOG" 2>/dev/null | tail
fi
mstack_down
echo "MCP_VERIFY_RC=$RC"
exit $RC
