// Shared color-readback helpers for every capture backend (Vulkan / D3D11 / D3D12).
//
// Extracted move-only (refactor R01) from the three backends, where the same three tail steps were
// duplicated: (1) repack a mapped GPU readback into a tight 8-bit RGBA buffer honoring the source
// RowPitch, swizzling BGRA->RGBA when needed; (2) lodepng-encode that buffer to a PNG; (3) assemble
// the shared success-result JSON. Behaviour is unchanged: each backend still owns its own error-JSON
// shape (deliberately asymmetric today; unification is a separate behaviour-changing task) and any
// API-specific success fields (e.g. Vulkan's sampleCount/msaaResolved/tonemapped), which it appends
// to the base object returned by BuildCaptureSuccessJson.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vr_agent {

// Repack a mapped 8-bit RGBA/BGRA readback into a tightly-packed w*h*4 RGBA buffer. `src` points at
// the first row; each source row is `rowPitch` bytes (>= w*4 -- pass w*4 when the staging buffer is
// already tight, as on Vulkan). When `bgra` is true, swap B<->R per pixel. Returns the packed buffer.
std::vector<unsigned char> RepackRows(const unsigned char* src, std::size_t rowPitch,
                                      std::uint32_t w, std::uint32_t h, bool bgra);

// Decode a mapped R16G16B16A16_FLOAT readback (8 bytes/texel, rows `rowPitch` bytes apart) into a
// tightly-packed w*h*4 8-bit RGBA buffer: RGB are sRGB-encoded from linear half-floats
// (QuantizeSrgb), alpha is linearly quantized (QuantizeLinearUnit) -- the same fixed conversion as
// the Vulkan HDR path (capture_vulkan.cpp), shared here for the D3D11/D3D12 backends (R10), which
// unlike Vulkan's tight staging buffer have row padding to honor.
std::vector<unsigned char> DecodeHdrRowsToSrgb(const unsigned char* src, std::size_t rowPitch,
                                               std::uint32_t w, std::uint32_t h);

// Encode a tight w*h*4 8-bit RGBA buffer to a PNG at `path` via lodepng. Returns the lodepng error
// code (0 = success); callers build their own error JSON so per-backend error shapes stay unchanged.
unsigned EncodeRgbaPng(const std::string& path, const std::vector<unsigned char>& pixels,
                       std::uint32_t w, std::uint32_t h);

// Build the base success-result JSON shared by every backend:
//   {ok:true, path, eye, viewIndex, api, width, height, arrayIndex, format}
// Backends with extra fields (Vulkan) append them to the returned object.
nlohmann::json BuildCaptureSuccessJson(const std::string& path, const std::string& eye,
                                       int viewIndex, const char* api, std::uint32_t width,
                                       std::uint32_t height, std::uint32_t arrayIndex,
                                       std::int64_t format);

// Resolve "left"/"right"/"dominant" to a 0-based eye index, clamped to [0, viewCount-1].
// DominantEyeIndex checks VR_AGENT_DOMINANT_EYE env (default right=1).
int DominantEyeIndex();
int EyeToIndex(const std::string& eye, uint32_t viewCount);

}  // namespace vr_agent
