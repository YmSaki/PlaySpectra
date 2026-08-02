// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

#ifndef PLAYSPECTRA_DXGI_FORMATS_H
#define PLAYSPECTRA_DXGI_FORMATS_H

#include <cstdint>  // int64_t (self-contained: consumers must not have to include this first)

#include <dxgiformat.h>

// 8-bit-per-channel RGBA-order formats (channel bytes already in R,G,B,A order in memory).
inline bool DxgiIsRGBA8(int64_t f) {
  return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}
// 8-bit-per-channel BGRA-order formats (need B<->R swizzle to get RGBA for PNG).
inline bool DxgiIsBGRA8(int64_t f) {
  return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}
// 16-bit float HDR (decoded half->sRGB via the shared DecodeHdrRowsToSrgb). Deliberately excludes
// R16G16B16A16_TYPELESS: unlike the 8-bit families a typeless 16-bit swapchain could be UNORM or
// FLOAT and guessing the interpretation risks a silently-wrong image -- explicit error instead.
inline bool DxgiIsHDR16F(int64_t f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT; }

// A fully-typed DXGI format for operations that reject typeless (MSAA ResolveSubresource, placed
// footprint copy destinations). TYPELESS resources have an undefined copyable layout, so resolve
// them to the matching UNORM member of the same format family. SRGB carries the same byte layout
// and copies fine, so it is left as-is.
inline DXGI_FORMAT DxgiResolveTyped(int64_t f) {
  if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) return DXGI_FORMAT_R8G8B8A8_UNORM;
  if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) return DXGI_FORMAT_B8G8R8A8_UNORM;
  return static_cast<DXGI_FORMAT>(f);
}

#endif  // PLAYSPECTRA_DXGI_FORMATS_H
