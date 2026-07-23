// Unit tests for the DXGI format classification helpers (dxgi_formats.h). Windows-only:
// dxgiformat.h is a Windows SDK header, and DXGI classification only exists on the D3D path (a
// non-Windows runtime only ever hands a Vulkan binding). These lock both the positive families
// AND the negative "unsupported -> explicit error, never a silently-wrong image" contracts --
// especially the 16-bit TYPELESS exclusion that prevents guessing UNORM-vs-FLOAT.
#include "dxgi_formats.h"

#include <gtest/gtest.h>

TEST(DxgiIsRGBA8, AcceptsRgba8Family) {
  EXPECT_TRUE(DxgiIsRGBA8(DXGI_FORMAT_R8G8B8A8_UNORM));
  EXPECT_TRUE(DxgiIsRGBA8(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
  EXPECT_TRUE(DxgiIsRGBA8(DXGI_FORMAT_R8G8B8A8_TYPELESS));
}

TEST(DxgiIsRGBA8, RejectsOthers) {
  EXPECT_FALSE(DxgiIsRGBA8(DXGI_FORMAT_B8G8R8A8_UNORM));
  EXPECT_FALSE(DxgiIsRGBA8(DXGI_FORMAT_R16G16B16A16_FLOAT));
  EXPECT_FALSE(DxgiIsRGBA8(DXGI_FORMAT_R10G10B10A2_UNORM));
  EXPECT_FALSE(DxgiIsRGBA8(DXGI_FORMAT_UNKNOWN));
}

TEST(DxgiIsBGRA8, AcceptsBgra8Family) {
  EXPECT_TRUE(DxgiIsBGRA8(DXGI_FORMAT_B8G8R8A8_UNORM));
  EXPECT_TRUE(DxgiIsBGRA8(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
  EXPECT_TRUE(DxgiIsBGRA8(DXGI_FORMAT_B8G8R8A8_TYPELESS));
}

TEST(DxgiIsBGRA8, RejectsOthers) {
  EXPECT_FALSE(DxgiIsBGRA8(DXGI_FORMAT_R8G8B8A8_UNORM));
  EXPECT_FALSE(DxgiIsBGRA8(DXGI_FORMAT_R16G16B16A16_FLOAT));
  EXPECT_FALSE(DxgiIsBGRA8(DXGI_FORMAT_UNKNOWN));
}

TEST(DxgiFamilies, RgbaAndBgraAreMutuallyExclusive) {
  // No format may match both orders -- that would make the B<->R swizzle decision ambiguous.
  const DXGI_FORMAT all[] = {
      DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS,
      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS};
  for (DXGI_FORMAT f : all) {
    EXPECT_FALSE(DxgiIsRGBA8(f) && DxgiIsBGRA8(f)) << "format " << f << " matched both orders";
  }
}

TEST(DxgiIsHDR16F, AcceptsOnlyFloat) {
  EXPECT_TRUE(DxgiIsHDR16F(DXGI_FORMAT_R16G16B16A16_FLOAT));
}

TEST(DxgiIsHDR16F, RejectsTypelessToForceExplicitError) {
  // The core silent-corruption guard: a 16-bit TYPELESS swapchain could be UNORM or FLOAT, so
  // classifying it as HDR would risk a silently-wrong image. dxgi_formats.h deliberately excludes it
  // (and UNORM) so the backend emits an explicit error rather than guessing.
  EXPECT_FALSE(DxgiIsHDR16F(DXGI_FORMAT_R16G16B16A16_TYPELESS));
  EXPECT_FALSE(DxgiIsHDR16F(DXGI_FORMAT_R16G16B16A16_UNORM));
  // ...and a 16-bit format is never misclassified into an 8-bit family either.
  EXPECT_FALSE(DxgiIsRGBA8(DXGI_FORMAT_R16G16B16A16_TYPELESS));
  EXPECT_FALSE(DxgiIsBGRA8(DXGI_FORMAT_R16G16B16A16_TYPELESS));
}

TEST(DxgiResolveTyped, ResolvesTypelessToMatchingUnorm) {
  EXPECT_EQ(DxgiResolveTyped(DXGI_FORMAT_R8G8B8A8_TYPELESS), DXGI_FORMAT_R8G8B8A8_UNORM);
  EXPECT_EQ(DxgiResolveTyped(DXGI_FORMAT_B8G8R8A8_TYPELESS), DXGI_FORMAT_B8G8R8A8_UNORM);
}

TEST(DxgiResolveTyped, PassesThroughTypedFormats) {
  // Already-typed formats (incl. SRGB, which shares the byte layout and copies fine) pass unchanged.
  EXPECT_EQ(DxgiResolveTyped(DXGI_FORMAT_R8G8B8A8_UNORM), DXGI_FORMAT_R8G8B8A8_UNORM);
  EXPECT_EQ(DxgiResolveTyped(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  EXPECT_EQ(DxgiResolveTyped(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
  EXPECT_EQ(DxgiResolveTyped(DXGI_FORMAT_R16G16B16A16_FLOAT), DXGI_FORMAT_R16G16B16A16_FLOAT);
}
