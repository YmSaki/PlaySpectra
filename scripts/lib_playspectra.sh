#!/usr/bin/env bash
# Resolve the Go control-plane executable for source-tree scripts. Distribution
# users normally provide playspectra on PATH; go run is a development fallback.
PLAYSPECTRA_SOURCE_ROOT="${PLAYSPECTRA_SOURCE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# Builds land in build/ (task build:go). The source root itself is never searched:
# playspectra there is the Go package directory, and a binary left beside it by an
# older -o playspectra invocation would silently outrank a current build.
ps_run() {
  if [ -n "${PLAYSPECTRA_BIN:-}" ]; then
    "$PLAYSPECTRA_BIN" "$@"
  elif [ -f "$PLAYSPECTRA_SOURCE_ROOT/build/playspectra.exe" ] && [ -x "$PLAYSPECTRA_SOURCE_ROOT/build/playspectra.exe" ]; then
    "$PLAYSPECTRA_SOURCE_ROOT/build/playspectra.exe" "$@"
  elif [ -f "$PLAYSPECTRA_SOURCE_ROOT/build/playspectra" ] && [ -x "$PLAYSPECTRA_SOURCE_ROOT/build/playspectra" ]; then
    "$PLAYSPECTRA_SOURCE_ROOT/build/playspectra" "$@"
  elif command -v playspectra >/dev/null 2>&1; then
    command playspectra "$@"
  elif command -v go >/dev/null 2>&1; then
    (cd "$PLAYSPECTRA_SOURCE_ROOT" && go run ./cmd/playspectra "$@")
  else
    echo "ERROR: playspectra binary not found; set PLAYSPECTRA_BIN or install Go" >&2
    return 127
  fi
}
