#!/usr/bin/env bash
# Build the OpenVR sample app (hellovr_dx12, x64) and deploy it with the OpenComposite openvr_api.dll
# to third_party/hellovr/ -- the OpenVR-side test app for the vr_agent stack (no Steam/SteamVR).
#
# Notes discovered while establishing this (2026-07-16, M2/M3):
# - The openvr CLONE MUST BE TAG-PINNED to an interface era OpenComposite implements. OC(openxr
#   branch) tops out at IVRSystem_022 / IVRCompositor_022 / IVRInput_010 / IVRRenderModels_006
#   (GEN_INTERFACE list in OpenOVR/Reimpl/CVR*.cpp). openvr master requests IVRSystem_026 ->
#   VR_GetGenericInterface returns null -> hellovr blocks forever in a silent SDL message box
#   (looks alive, renders nothing). v1.8.19 = System_021/Compositor_022/Input_007/RenderModels_006,
#   all within OC's list.
# - The openvr repo has NO hellovr_dx11 sample (dx12 / opengl / vulkan only), and hellovr_dx12's
#   vcxproj ships Win32-only -- we transform it to x64 mechanically (Win32->x64, win32->win64).
#   x64 is required because a 32-bit process cannot load the x64 Monado runtime.
# - The samples' top-level CMake does not cover hellovr_dx12 (and drags in Qt/GLEW), hence MSBuild.
# - SDL2.dll is not in the openvr repo (import libs only); we reuse Monado's bundled SDL2.dll
#   (SDL2 has a stable ABI). Run scripts/setup_monado.sh and scripts/setup_opencomposite.sh first.
# - At runtime the exe resolves ../cube_texture.png, ../shaders/*.hlsl, ../hellovr_actions.json
#   relative to its dir, so the deploy mirrors samples/bin -> third_party/hellovr/bin.
#
# Usage: scripts/setup_hellovr.sh
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPENVR_TAG="${OPENVR_TAG:-v1.8.19}"   # OC-compatible interface era; see note above
SRC="${TEMP:-/tmp}/vr_agent_openvr_${OPENVR_TAG}"   # short path (FTK1011); tag in path = no stale-clone ambiguity
DEST="${ROOT}/third_party/hellovr/bin"
# Find MSBuild across VS editions (first match wins)
MSBUILD=""
_VS_EDITIONS=(BuildTools Community Enterprise Professional)
for _ed in "${_VS_EDITIONS[@]}"; do
  _candidate="/c/Program Files (x86)/Microsoft Visual Studio/2022/${_ed}/MSBuild/Current/Bin/amd64/MSBuild.exe"
  if [ -f "$_candidate" ]; then
    MSBUILD="$_candidate"
    break
  fi
done
if [ -z "$MSBUILD" ]; then
  echo "ERROR: MSBuild.exe not found. Tried these paths:"
  for _ed in "${_VS_EDITIONS[@]}"; do
    echo "  /c/Program Files (x86)/Microsoft Visual Studio/2022/${_ed}/MSBuild/Current/Bin/amd64/MSBuild.exe"
  done
  echo "Install Visual Studio 2022 BuildTools (or Community/Enterprise/Professional) with the C++ workload."
  exit 1
fi

[ -f "${ROOT}/third_party/monado/bin/SDL2.dll" ] || { echo "run scripts/setup_monado.sh first"; exit 1; }
[ -f "${ROOT}/third_party/opencomposite/openvr_api.dll" ] || { echo "run scripts/setup_opencomposite.sh first"; exit 1; }

[ -d "$SRC/.git" ] || git clone --depth 1 --branch "$OPENVR_TAG" https://github.com/ValveSoftware/openvr "$SRC"

# vcxproj ships Win32-only; generate an x64 twin (idempotent overwrite).
python - "$SRC/samples/hellovr_dx12/hellovr_dx12.vcxproj" <<'EOF'
import sys
src = sys.argv[1]
s = open(src, encoding='utf-8-sig').read()
s = s.replace('Win32Proj','__W32PROJ__').replace('Win32','x64').replace('__W32PROJ__','Win32Proj')
s = s.replace('win32','win64')
open(src.replace('.vcxproj','_x64.vcxproj'),'w',encoding='utf-8').write(s)
EOF

"$MSBUILD" "$(cygpath -w "$SRC/samples/hellovr_dx12/hellovr_dx12_x64.vcxproj")" \
  -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v143 \
  -p:WindowsTargetPlatformVersion=10.0 -v:q -nologo

python - "$SRC/samples" "$DEST" "$ROOT" <<'EOF'
import os, shutil, sys
t, dest, root = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(os.path.join(dest,'win64'), exist_ok=True)
for f in ['cube_texture.png','hellovr_actions.json','hellovr_bindings_generic.json',
          'hellovr_bindings_vive_controller.json']:
    shutil.copy2(os.path.join(t,'bin',f), dest)
shutil.copytree(os.path.join(t,'bin','shaders'), os.path.join(dest,'shaders'), dirs_exist_ok=True)
shutil.copy2(os.path.join(t,'bin','win64','hellovr_dx12_x64.exe'), os.path.join(dest,'win64','hellovr_dx12.exe'))
shutil.copy2(os.path.join(root,'third_party','monado','bin','SDL2.dll'), os.path.join(dest,'win64'))
shutil.copy2(os.path.join(root,'third_party','opencomposite','openvr_api.dll'), os.path.join(dest,'win64'))
print('deployed:', sorted(os.listdir(os.path.join(dest,'win64'))))
EOF

echo "source: ValveSoftware/openvr ${OPENVR_TAG} (shallow) + OpenComposite dll, built $(date -u +%Y-%m-%dT%H:%MZ)" \
  > "${ROOT}/third_party/hellovr/VERSION.txt"
echo "[setup_hellovr] done -> ${DEST}/win64/hellovr_dx12.exe (OpenComposite openvr_api.dll applied)"
