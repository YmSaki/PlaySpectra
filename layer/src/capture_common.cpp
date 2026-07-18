// Shared color-readback helpers. See capture_common.h for the contract and the R01 rationale.

#include "capture_common.h"

#include <algorithm>  // std::swap
#include <cstdlib>    // std::getenv (DominantEyeIndex)
#include <cstring>    // std::memcpy

#include "lodepng.h"
#include "pixel_convert.h"  // HalfToFloat / QuantizeSrgb / QuantizeLinearUnit (HDR decode)

namespace playspectra {

std::vector<unsigned char> RepackRows(const unsigned char* src, std::size_t rowPitch,
                                      std::uint32_t w, std::uint32_t h, bool bgra) {
  const std::size_t rowBytes = static_cast<std::size_t>(w) * 4;
  std::vector<unsigned char> pixels(rowBytes * h);
  for (std::uint32_t r = 0; r < h; ++r) {
    const unsigned char* srcRow = src + static_cast<std::size_t>(r) * rowPitch;
    unsigned char* dstRow = pixels.data() + static_cast<std::size_t>(r) * rowBytes;
    std::memcpy(dstRow, srcRow, rowBytes);
    if (bgra) {
      for (std::size_t p = 0; p + 3 < rowBytes; p += 4) std::swap(dstRow[p], dstRow[p + 2]);
    }
  }
  return pixels;
}

std::vector<unsigned char> DecodeHdrRowsToSrgb(const unsigned char* src, std::size_t rowPitch,
                                               std::uint32_t w, std::uint32_t h) {
  std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t r = 0; r < h; ++r) {
    const unsigned char* srcRow = src + static_cast<std::size_t>(r) * rowPitch;
    unsigned char* dstRow = pixels.data() + static_cast<std::size_t>(r) * w * 4;
    for (std::uint32_t px = 0; px < w; ++px) {
      std::uint16_t half[4];
      std::memcpy(half, srcRow + static_cast<std::size_t>(px) * 8, 8);  // R,G,B,A half-floats
      dstRow[px * 4 + 0] = QuantizeSrgb(HalfToFloat(half[0]));
      dstRow[px * 4 + 1] = QuantizeSrgb(HalfToFloat(half[1]));
      dstRow[px * 4 + 2] = QuantizeSrgb(HalfToFloat(half[2]));
      dstRow[px * 4 + 3] = QuantizeLinearUnit(HalfToFloat(half[3]));
    }
  }
  return pixels;
}

unsigned EncodeRgbaPng(const std::string& path, const std::vector<unsigned char>& pixels,
                       std::uint32_t w, std::uint32_t h) {
  return lodepng::encode(path, pixels, w, h, LCT_RGBA, 8);
}

nlohmann::json BuildCaptureSuccessJson(const std::string& path, const std::string& eye,
                                       int viewIndex, const char* api, std::uint32_t width,
                                       std::uint32_t height, std::uint32_t arrayIndex,
                                       std::int64_t format) {
  return {{"ok", true},
          {"path", path},
          {"eye", eye},
          {"viewIndex", viewIndex},
          {"api", api},
          {"width", width},
          {"height", height},
          {"arrayIndex", arrayIndex},
          {"format", format}};
}

int DominantEyeIndex() {
  if (const char* e = std::getenv("PLAYSPECTRA_DOMINANT_EYE")) {
    if (std::string(e) == "left") return 0;
  }
  return 1;
}

int EyeToIndex(const std::string& eye, uint32_t viewCount) {
  int idx = 1;
  if (eye == "left") idx = 0;
  else if (eye == "right") idx = 1;
  else idx = DominantEyeIndex();
  if (viewCount == 0) return 0;
  if (idx >= static_cast<int>(viewCount)) idx = static_cast<int>(viewCount) - 1;
  return idx;
}

}  // namespace playspectra
