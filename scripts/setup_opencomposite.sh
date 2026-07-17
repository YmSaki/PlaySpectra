#!/usr/bin/env bash
# Fetch the OpenComposite (OpenVR -> OpenXR translation) openvr_api.dll into third_party/opencomposite/.
#
# OpenComposite implements the OpenVR API and forwards calls to the active OpenXR runtime, letting an
# OpenVR app run against our OpenXR stack (vr_agent layer + Monado) with NO SteamVR install. We use the
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
DEST="${ROOT}/third_party/opencomposite"
URL="https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr"

mkdir -p "$DEST"
echo "[setup_opencomposite] downloading x64 openxr-branch openvr_api.dll ..."
curl -fSL -o "${DEST}/openvr_api.dll" "$URL"

# Mechanical sanity check: PE header, COFF machine field must be 0x8664 (x64). Catches HTML error
# pages and wrong-arch downloads before M2 ever loads the DLL into a process.
python - "${DEST}/openvr_api.dll" <<'EOF'
import struct, sys
p = sys.argv[1]
b = open(p, 'rb').read()
ok = len(b) > 0x1000 and b[:2] == b'MZ'
if ok:
    pe_off = struct.unpack_from('<I', b, 0x3C)[0]
    ok = b[pe_off:pe_off+4] == b'PE\x00\x00' and struct.unpack_from('<H', b, pe_off+4)[0] == 0x8664
if not ok:
    print(f'PE check: FAILED ({len(b)} bytes; not a PE32+ x64 DLL - download broken?)')
    sys.exit(1)
print(f'PE check: x64 OK ({len(b)} bytes)')
EOF

echo "source: ${URL} fetched $(date -u +%Y-%m-%dT%H:%MZ) (AppVeyor build, openxr branch)" > "${DEST}/VERSION.txt"
echo "[setup_opencomposite] done -> ${DEST}/openvr_api.dll"
