#!/usr/bin/env bash
# Fetch the Monado Windows CI build into third_party/monado/ (xcopy-deployable, no install).
# Reproduces the runtime validated by the 2026-07-16 PoC (.claude/monado-poc-plan.md).
#
# Usage: scripts/setup_monado.sh [gitlab-ref]   (default: main -- latest successful CI artifact)
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="${1:-main}"
DEST="${ROOT}/third_party/monado"
ZIP="${TEMP:-/tmp}/monado_win_ci.zip"

echo "[setup_monado] downloading windows job artifact for ref=${REF} ..."
curl -sL -o "$ZIP" \
  "https://gitlab.freedesktop.org/api/v4/projects/2685/jobs/artifacts/${REF}/download?job=windows"

python - "$ZIP" "$DEST" <<'EOF'
import zipfile, os, sys
zpath, dest = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(zpath)
n = 0
for i in z.infolist():
    # install/ tree only; the SteamVR driver plugin is not needed for our test substrate.
    if i.filename.startswith('install/') and not i.filename.startswith('install/share/steamvr-monado'):
        if i.is_dir(): continue
        target = os.path.join(dest, i.filename[len('install/'):])
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'wb') as f: f.write(z.read(i))
        n += 1
print(f'extracted {n} files -> {dest}')
EOF

echo "[setup_monado] ref=${REF} fetched $(date -u +%Y-%m-%dT%H:%MZ)" > "${DEST}/VERSION.txt"
echo "[setup_monado] done. runtime json: ${DEST}/openxr_monado.json"
echo "[setup_monado] note: Monado is service-mode on Windows; integration_test.sh starts"
echo "               third_party/monado/bin/monado-service.exe automatically (VR_RUNTIME=monado)."
