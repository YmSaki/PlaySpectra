# VR-MCP setup script.
#
# Responsible for acquiring the base headless OpenXR runtime (Meta XR Simulator) and
# registering the vr_agent API layer for a target process. Does not install anything
# system-wide by default; env vars are set per-process/per-session.
#
# TODO(#3): confirm Meta XR Simulator MSI install scope (per-user vs elevated) and automate
#           silent install here; locate the resulting meta_openxr_simulator.json + SIMULATOR.dll.
# TODO(#4): $env:XR_API_LAYER_PATH / $env:XR_ENABLE_API_LAYERS toggle for enabling vr_agent_layer
#           against a target app (hello_xr, Unity Editor launcher, etc.).

param(
  [switch]$InstallSimulator,
  [switch]$EnableLayer
)

Write-Host "VR-MCP setup: see docs/plans and README.md for manual steps until this script is implemented (task #3)."
