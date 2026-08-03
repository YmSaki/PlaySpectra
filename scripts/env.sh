#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Source this to set up the PlaySpectra dev environment (base runtime + layer enablement).
#   source scripts/env.sh
#
# The base runtime is the Meta XR Simulator (Vulkan), simulating a Meta Quest 3 with 6DoF
# tracking, swapchains created, XR_EXT_conformance_automation present; hello_xr (the MinGW
# build) + openxr_loader run against it.

PLAYSPECTRA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- Base OpenXR runtime, selectable via VR_RUNTIME=metasim|monado (default metasim) ---
# metasim: Meta XR Simulator (extracted via `msiexec /a`, no elevation)
# monado:  Monado CI build (third_party/monado, xcopy-deployable; needs monado-service.exe running)
if [ "${VR_RUNTIME:-metasim}" = "monado" ]; then
  export XR_RUNTIME_JSON="${PLAYSPECTRA_ROOT}/third_party/monado/openxr_monado.json"
else
  export XR_RUNTIME_JSON="${PLAYSPECTRA_ROOT}/third_party/meta_xr_sim/PFiles/MetaXRSimulator/v201.0/meta_openxr_simulator.json"
fi

# --- playspectra API layer enablement ---
export XR_API_LAYER_PATH="${PLAYSPECTRA_ROOT}/layer/manifest"
export XR_ENABLE_API_LAYERS="XR_APILAYER_playspectra"

echo "XR_RUNTIME_JSON=$XR_RUNTIME_JSON"
[ -f "$XR_RUNTIME_JSON" ] && echo "  runtime json: OK" || echo "  runtime json: MISSING (run scripts/setup notes)"
