#include <gtest/gtest.h>
#include "pixel_convert.h"

#include <cmath>
#include <limits>

using namespace vr_agent;

// --- HalfToFloat (cases from capture_vulkan.cpp HalfFloatSelfTest + extras) ---

TEST(PixelConvertTest, HalfToFloat_Zero) {
  EXPECT_FLOAT_EQ(HalfToFloat(0x0000), 0.0f);
}

TEST(PixelConvertTest, HalfToFloat_NegativeZero) {
  float v = HalfToFloat(0x8000);
  EXPECT_FLOAT_EQ(v, -0.0f);
  EXPECT_TRUE(std::signbit(v));
}

TEST(PixelConvertTest, HalfToFloat_One) {
  EXPECT_FLOAT_EQ(HalfToFloat(0x3C00), 1.0f);
}

TEST(PixelConvertTest, HalfToFloat_Half) {
  EXPECT_FLOAT_EQ(HalfToFloat(0x3800), 0.5f);
}

TEST(PixelConvertTest, HalfToFloat_NegativeOne) {
  EXPECT_FLOAT_EQ(HalfToFloat(0xBC00), -1.0f);
}

TEST(PixelConvertTest, HalfToFloat_MaxNormal) {
  EXPECT_FLOAT_EQ(HalfToFloat(0x7BFF), 65504.0f);
}

TEST(PixelConvertTest, HalfToFloat_SmallestSubnormal) {
  EXPECT_FLOAT_EQ(HalfToFloat(0x0001), std::ldexp(1.0f, -24));
}

TEST(PixelConvertTest, HalfToFloat_Inf) {
  EXPECT_TRUE(std::isinf(HalfToFloat(0x7C00)));
  EXPECT_GT(HalfToFloat(0x7C00), 0.0f);
}

TEST(PixelConvertTest, HalfToFloat_NegInf) {
  EXPECT_TRUE(std::isinf(HalfToFloat(0xFC00)));
  EXPECT_LT(HalfToFloat(0xFC00), 0.0f);
}

TEST(PixelConvertTest, HalfToFloat_NaN) {
  EXPECT_TRUE(std::isnan(HalfToFloat(0x7E00)));
}

// --- LinearToSrgb ---

TEST(PixelConvertTest, LinearToSrgb_Zero) {
  EXPECT_FLOAT_EQ(LinearToSrgb(0.0f), 0.0f);
}

TEST(PixelConvertTest, LinearToSrgb_Negative) {
  EXPECT_FLOAT_EQ(LinearToSrgb(-1.0f), 0.0f);
}

TEST(PixelConvertTest, LinearToSrgb_NaN) {
  EXPECT_FLOAT_EQ(LinearToSrgb(std::numeric_limits<float>::quiet_NaN()), 0.0f);
}

TEST(PixelConvertTest, LinearToSrgb_One) {
  EXPECT_FLOAT_EQ(LinearToSrgb(1.0f), 1.0f);
}

TEST(PixelConvertTest, LinearToSrgb_AboveOne) {
  EXPECT_FLOAT_EQ(LinearToSrgb(2.0f), 1.0f);
}

TEST(PixelConvertTest, LinearToSrgb_PosInf) {
  EXPECT_FLOAT_EQ(LinearToSrgb(std::numeric_limits<float>::infinity()), 1.0f);
}

TEST(PixelConvertTest, LinearToSrgb_LinearRegion) {
  float c = 0.001f;
  EXPECT_NEAR(LinearToSrgb(c), c * 12.92f, 1e-6f);
}

TEST(PixelConvertTest, LinearToSrgb_MidRange) {
  float v = LinearToSrgb(0.5f);
  EXPECT_GT(v, 0.5f);
  EXPECT_LT(v, 1.0f);
}

// --- QuantizeSrgb ---

TEST(PixelConvertTest, QuantizeSrgb_Zero) {
  EXPECT_EQ(QuantizeSrgb(0.0f), 0);
}

TEST(PixelConvertTest, QuantizeSrgb_One) {
  EXPECT_EQ(QuantizeSrgb(1.0f), 255);
}

TEST(PixelConvertTest, QuantizeSrgb_Negative) {
  EXPECT_EQ(QuantizeSrgb(-1.0f), 0);
}

// --- QuantizeLinearUnit ---

TEST(PixelConvertTest, QuantizeLinearUnit_Zero) {
  EXPECT_EQ(QuantizeLinearUnit(0.0f), 0);
}

TEST(PixelConvertTest, QuantizeLinearUnit_One) {
  EXPECT_EQ(QuantizeLinearUnit(1.0f), 255);
}

TEST(PixelConvertTest, QuantizeLinearUnit_Half) {
  EXPECT_EQ(QuantizeLinearUnit(0.5f), 128);
}

TEST(PixelConvertTest, QuantizeLinearUnit_NaN) {
  EXPECT_EQ(QuantizeLinearUnit(std::numeric_limits<float>::quiet_NaN()), 0);
}

TEST(PixelConvertTest, QuantizeLinearUnit_Negative) {
  EXPECT_EQ(QuantizeLinearUnit(-0.5f), 0);
}

TEST(PixelConvertTest, QuantizeLinearUnit_AboveOne) {
  EXPECT_EQ(QuantizeLinearUnit(2.0f), 255);
}

// --- LinearizeViewDepth ---

TEST(PixelConvertTest, LinearizeViewDepth_NormalProjection_NearEnd) {
  EXPECT_FLOAT_EQ(LinearizeViewDepth(0.0f, 0.1f, 100.0f), 0.1f);
}

TEST(PixelConvertTest, LinearizeViewDepth_NormalProjection_FarEnd) {
  EXPECT_NEAR(LinearizeViewDepth(1.0f, 0.1f, 100.0f), 100.0f, 0.01f);
}

TEST(PixelConvertTest, LinearizeViewDepth_ReversedZ_NearEnd) {
  // reversed-Z: z=0 maps to nearZ (the "near" clip plane distance)
  EXPECT_NEAR(LinearizeViewDepth(0.0f, 100.0f, 0.1f), 100.0f, 0.01f);
}

TEST(PixelConvertTest, LinearizeViewDepth_ReversedZ_FarEnd) {
  // reversed-Z: z=1 maps to farZ
  EXPECT_NEAR(LinearizeViewDepth(1.0f, 100.0f, 0.1f), 0.1f, 0.01f);
}

TEST(PixelConvertTest, LinearizeViewDepth_InfFar_NonZeroZ) {
  float inf = std::numeric_limits<float>::infinity();
  EXPECT_FLOAT_EQ(LinearizeViewDepth(0.5f, 0.1f, inf), 0.2f);
}

TEST(PixelConvertTest, LinearizeViewDepth_InfFar_ZeroZ) {
  float inf = std::numeric_limits<float>::infinity();
  EXPECT_TRUE(std::isinf(LinearizeViewDepth(0.0f, 0.1f, inf)));
}

TEST(PixelConvertTest, LinearizeViewDepth_DenomNearZero) {
  // denom = farZ - z*(farZ - nearZ). With near≈0, z=1: denom = near ≈ 0 → infinity
  EXPECT_TRUE(std::isinf(LinearizeViewDepth(1.0f, 1e-30f, 100.0f)));
}
