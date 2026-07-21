// Pure pixel / numeric conversion helpers. See pixel_convert.h for the contract and the R03 rationale.
// Bodies moved verbatim from capture_vulkan.cpp (only the linkage changed: anonymous namespace ->
// playspectra::).

#include "pixel_convert.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace playspectra {

// ---- IEEE half-float (binary16) -> float, hand-rolled (no compiler intrinsic dependency). ----
// Handles zero, subnormals (normalized on the way out), inf and NaN, exponent bias 15 -> 127.
float HalfToFloat(std::uint16_t hbits) {
  const uint32_t sign = static_cast<uint32_t>(hbits & 0x8000u) << 16;  // -> float sign bit (bit 31)
  uint32_t exp = (hbits >> 10) & 0x1Fu;
  uint32_t mant = hbits & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;  // +/- zero
    } else {
      // Subnormal half: renormalize into a normal float. Shift mantissa left until the implicit
      // leading 1 (bit 10) appears, decrementing the exponent for each shift.
      exp = 127u - 15u + 1u;  // = 113, the exponent of 2^-14 in float bias
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3FFu;  // drop the now-explicit leading 1
      bits = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 0x1Fu) {
    // inf (mant==0) or NaN (mant!=0): float exponent all-ones, mantissa left-shifted to keep the
    // quiet/signalling bit and payload nonzero for NaN.
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    // Normalized: rebias exponent (15 -> 127) and left-align the 10-bit mantissa into 23 bits.
    bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

// Linear (scene-referred) -> sRGB electro-optical transfer function, clamped to [0,1]. inf clamps to
// 1.0 (>=1 branch); callers pre-map NaN to 0. This is the whole "tone mapping": no operator, no
// exposure knob -- HDR values above 1.0 saturate to white. Adjustable operators are a post-core
// extension (CLAUDE.md: keep the core loop minimal and honest).
float LinearToSrgb(float c) {
  if (!(c > 0.0f)) return 0.0f;   // handles <=0 and NaN (NaN>0 is false)
  if (c >= 1.0f) return 1.0f;     // handles +inf too
  if (c <= 0.0031308f) return c * 12.92f;
  return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
unsigned char QuantizeSrgb(float linear) {
  const float s = LinearToSrgb(linear);
  long q = std::lroundf(s * 255.0f);
  if (q < 0) q = 0;
  if (q > 255) q = 255;
  return static_cast<unsigned char>(q);
}
// Alpha is linear straight alpha (not sRGB-encoded); clamp + quantize directly.
unsigned char QuantizeLinearUnit(float a) {
  if (std::isnan(a)) a = 0.0f;
  if (a < 0.0f) a = 0.0f;
  if (a > 1.0f) a = 1.0f;
  long q = std::lroundf(a * 255.0f);
  if (q < 0) q = 0;
  if (q > 255) q = 255;
  return static_cast<unsigned char>(q);
}

// NDC depth (z in [0,1]; Vulkan convention: 0=near plane, 1=far plane in the non-reversed case) ->
// positive view-space distance in metres. Endpoints: z=0 -> nearZ, z=1 -> farZ, so reversed-Z
// (nearZ>farZ) falls out of the same expression. Infinite far plane (farZ==+inf, the classic
// reversed-Z infinite projection) uses zView = nearZ / z, with z==0 meaning infinitely far.
float LinearizeViewDepth(float z, float nearZ, float farZ) {
  if (std::isinf(farZ)) {
    if (z <= 0.0f) return std::numeric_limits<float>::infinity();
    return nearZ / z;
  }
  const float denom = farZ - z * (farZ - nearZ);
  if (std::fabs(denom) < 1e-20f) return std::numeric_limits<float>::infinity();
  return farZ * nearZ / denom;
}

}  // namespace playspectra
