// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Pure pixel / numeric conversion helpers -- GPU-API-independent (no Vulkan / D3D / OpenXR headers).
//
// Extracted move-only (R03) from capture_vulkan.cpp's anonymous namespace so they become
// unit-testable (R11) and shareable across capture backends (e.g. the D3D HDR decode, R10). Behaviour
// is unchanged; only the linkage moved from internal (anonymous namespace) to external (playspectra::).

#pragma once

#include <cstdint>

namespace playspectra {

// IEEE half-float (binary16) -> float. Handles zero, subnormals, inf and NaN (exponent bias 15->127).
float HalfToFloat(std::uint16_t hbits);

// Linear (scene-referred) -> sRGB EOTF, clamped to [0,1]. inf clamps to 1.0; NaN and <=0 map to 0.
float LinearToSrgb(float c);

// sRGB-encode a linear color channel and quantize to 8-bit [0,255].
unsigned char QuantizeSrgb(float linear);

// Clamp + quantize a linear straight-alpha channel to 8-bit [0,255] (no sRGB encoding).
unsigned char QuantizeLinearUnit(float a);

// NDC depth (z in [0,1]) -> positive view-space distance in metres. Handles reversed-Z and an
// infinite far plane (farZ == +inf).
float LinearizeViewDepth(float z, float nearZ, float farZ);

}  // namespace playspectra
