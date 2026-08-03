# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# PlaySpectra setup script.
#
# Responsible for acquiring the base headless OpenXR runtime (Meta XR Simulator) and
# registering the playspectra API layer for a target process. Does not install anything
# system-wide by default; env vars are set per-process/per-session.
#
# TODO: confirm Meta XR Simulator MSI install scope (per-user vs elevated) and automate
#       silent install here; locate the resulting meta_openxr_simulator.json + SIMULATOR.dll.
# TODO: $env:XR_API_LAYER_PATH / $env:XR_ENABLE_API_LAYERS toggle for enabling playspectra_layer
#       against a target app (hello_xr, Unity Editor launcher, etc.).

param(
  [switch]$InstallSimulator,
  [switch]$EnableLayer
)

Write-Host "PlaySpectra setup: see README.md for manual steps until this script is implemented."
