#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Fetch the OpenComposite (OpenVR -> OpenXR translation) openvr_api.dll into third_party/opencomposite/.
#
# OpenComposite implements the OpenVR API and forwards calls to the active OpenXR runtime, letting an
# OpenVR app run against our OpenXR stack (playspectra layer + Monado) with NO SteamVR install. We use the
# per-app method only: copy this DLL next to the target app, replacing its own openvr_api.dll (M2 does
# that for hellovr_dx11). The system-wide "runtime switcher" is deliberately NOT used -- it mutates
# global state, against the xcopy principle (G7).
#
# License note: OpenComposite is GPLv3. third_party/ is not committed and we do not redistribute it.
#
# Fallback if the direct download ever disappears: grab the artifact from a green pipeline at
# https://gitlab.com/znixian/OpenOVR/pipelines (openxr branch) and place openvr_api.dll manually.
#
# Usage: scripts/setup_opencomposite.sh
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib_playspectra.sh"
DEST="${ROOT}/third_party/opencomposite"
URL="https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr"

mkdir -p "$DEST"
echo "[setup_opencomposite] downloading x64 openxr-branch openvr_api.dll ..."
curl -fSL -o "${DEST}/openvr_api.dll" "$URL"

# Mechanical sanity check: PE header, COFF machine field must be 0x8664 (x64). Catches HTML error
# pages and wrong-arch downloads before M2 ever loads the DLL into a process.
ps_run internal check-pe-x64 "${DEST}/openvr_api.dll"

echo "source: ${URL} fetched $(date -u +%Y-%m-%dT%H:%MZ) (AppVeyor build, openxr branch)" > "${DEST}/VERSION.txt"
echo "[setup_opencomposite] done -> ${DEST}/openvr_api.dll"
