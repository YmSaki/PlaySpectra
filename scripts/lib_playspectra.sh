#!/usr/bin/env bash
# Resolve the Go control-plane executable for source-tree scripts. Distribution
# users normally provide playspectra on PATH; go run is a development fallback.
PLAYSPECTRA_SOURCE_ROOT="${PLAYSPECTRA_SOURCE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

ps_run() {
  if [ -n "${PLAYSPECTRA_BIN:-}" ]; then
    "$PLAYSPECTRA_BIN" "$@"
  elif [ -f "$PLAYSPECTRA_SOURCE_ROOT/playspectra.exe" ] && [ -x "$PLAYSPECTRA_SOURCE_ROOT/playspectra.exe" ]; then
    "$PLAYSPECTRA_SOURCE_ROOT/playspectra.exe" "$@"
  elif [ -f "$PLAYSPECTRA_SOURCE_ROOT/playspectra" ] && [ -x "$PLAYSPECTRA_SOURCE_ROOT/playspectra" ]; then
    "$PLAYSPECTRA_SOURCE_ROOT/playspectra" "$@"
  elif command -v playspectra >/dev/null 2>&1; then
    command playspectra "$@"
  elif command -v go >/dev/null 2>&1; then
    (cd "$PLAYSPECTRA_SOURCE_ROOT" && go run ./cmd/playspectra "$@")
  else
    echo "ERROR: playspectra binary not found; set PLAYSPECTRA_BIN or install Go" >&2
    return 127
  fi
}
