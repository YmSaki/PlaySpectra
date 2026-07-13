#!/usr/bin/env bash
# Source this to set up the VR-MCP dev environment (base runtime + layer enablement).
#   source scripts/env.sh
#
# Bring-up validated 2026-07-13: MinGW-built hello_xr + openxr_loader run against the
# Meta XR Simulator (Vulkan), simulating a Meta Quest 3 with 6DoF tracking, swapchains
# created, XR_EXT_conformance_automation present.

VRMCP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- Base OpenXR runtime: Meta XR Simulator (extracted via `msiexec /a`, no elevation) ---
export XR_RUNTIME_JSON="${VRMCP_ROOT}/third_party/meta_xr_sim/PFiles/MetaXRSimulator/v201.0/meta_openxr_simulator.json"

# --- vr_agent API layer enablement (task #4 passthrough validated; #6/#7 control channel active) ---
export XR_API_LAYER_PATH="${VRMCP_ROOT}/layer/manifest"
export XR_ENABLE_API_LAYERS="XR_APILAYER_vr_agent"

echo "XR_RUNTIME_JSON=$XR_RUNTIME_JSON"
[ -f "$XR_RUNTIME_JSON" ] && echo "  runtime json: OK" || echo "  runtime json: MISSING (run scripts/setup notes)"
