#include <gtest/gtest.h>
#include "xr_math.h"

#include <cmath>

using namespace playspectra;

static constexpr float kEps = 1e-6f;

// --- QMul ---

TEST(XrMathTest, QMul_Identity) {
  XrQuaternionf id{0, 0, 0, 1};
  XrQuaternionf q{0.1f, 0.2f, 0.3f, 0.9f};
  auto r = QMul(id, q);
  EXPECT_NEAR(r.x, q.x, kEps);
  EXPECT_NEAR(r.y, q.y, kEps);
  EXPECT_NEAR(r.z, q.z, kEps);
  EXPECT_NEAR(r.w, q.w, kEps);
}

TEST(XrMathTest, QMul_IdentityRight) {
  XrQuaternionf id{0, 0, 0, 1};
  XrQuaternionf q{0.1f, 0.2f, 0.3f, 0.9f};
  auto r = QMul(q, id);
  EXPECT_NEAR(r.x, q.x, kEps);
  EXPECT_NEAR(r.y, q.y, kEps);
  EXPECT_NEAR(r.z, q.z, kEps);
  EXPECT_NEAR(r.w, q.w, kEps);
}

TEST(XrMathTest, QMul_90DegYaw) {
  // 90 degrees around Y: (0, sin(45), 0, cos(45))
  const float s = std::sin(M_PI / 4.0f);
  const float c = std::cos(M_PI / 4.0f);
  XrQuaternionf q{0, s, 0, c};
  // q*q = 180 degrees around Y: (0, 1, 0, 0)
  auto r = QMul(q, q);
  EXPECT_NEAR(r.x, 0.0f, kEps);
  EXPECT_NEAR(r.y, 1.0f, kEps);
  EXPECT_NEAR(r.z, 0.0f, kEps);
  EXPECT_NEAR(r.w, 0.0f, kEps);
}

TEST(XrMathTest, QMul_NonCommutative) {
  XrQuaternionf a{1, 0, 0, 0};
  XrQuaternionf b{0, 1, 0, 0};
  auto ab = QMul(a, b);
  auto ba = QMul(b, a);
  // i*j = k, j*i = -k
  EXPECT_NEAR(ab.z, 1.0f, kEps);
  EXPECT_NEAR(ba.z, -1.0f, kEps);
}

// --- QConj ---

TEST(XrMathTest, QConj_Identity) {
  auto r = QConj({0, 0, 0, 1});
  EXPECT_FLOAT_EQ(r.x, 0.0f);
  EXPECT_FLOAT_EQ(r.y, 0.0f);
  EXPECT_FLOAT_EQ(r.z, 0.0f);
  EXPECT_FLOAT_EQ(r.w, 1.0f);
}

TEST(XrMathTest, QConj_General) {
  auto r = QConj({1, 2, 3, 4});
  EXPECT_FLOAT_EQ(r.x, -1.0f);
  EXPECT_FLOAT_EQ(r.y, -2.0f);
  EXPECT_FLOAT_EQ(r.z, -3.0f);
  EXPECT_FLOAT_EQ(r.w, 4.0f);
}

// --- QRot ---

TEST(XrMathTest, QRot_Identity) {
  XrQuaternionf id{0, 0, 0, 1};
  XrVector3f v{1, 2, 3};
  auto r = QRot(id, v);
  EXPECT_NEAR(r.x, 1.0f, kEps);
  EXPECT_NEAR(r.y, 2.0f, kEps);
  EXPECT_NEAR(r.z, 3.0f, kEps);
}

TEST(XrMathTest, QRot_90DegAroundY) {
  // 90 degrees around Y rotates +X to -Z
  const float s = std::sin(M_PI / 4.0f);
  const float c = std::cos(M_PI / 4.0f);
  XrQuaternionf q{0, s, 0, c};
  XrVector3f v{1, 0, 0};
  auto r = QRot(q, v);
  EXPECT_NEAR(r.x, 0.0f, kEps);
  EXPECT_NEAR(r.y, 0.0f, kEps);
  EXPECT_NEAR(r.z, -1.0f, kEps);
}

// --- VAdd / VSub ---

TEST(XrMathTest, VAdd_Zero) {
  auto r = VAdd({1, 2, 3}, {0, 0, 0});
  EXPECT_FLOAT_EQ(r.x, 1.0f);
  EXPECT_FLOAT_EQ(r.y, 2.0f);
  EXPECT_FLOAT_EQ(r.z, 3.0f);
}

TEST(XrMathTest, VAdd_General) {
  auto r = VAdd({1, 2, 3}, {4, 5, 6});
  EXPECT_FLOAT_EQ(r.x, 5.0f);
  EXPECT_FLOAT_EQ(r.y, 7.0f);
  EXPECT_FLOAT_EQ(r.z, 9.0f);
}

TEST(XrMathTest, VSub_Zero) {
  auto r = VSub({1, 2, 3}, {0, 0, 0});
  EXPECT_FLOAT_EQ(r.x, 1.0f);
  EXPECT_FLOAT_EQ(r.y, 2.0f);
  EXPECT_FLOAT_EQ(r.z, 3.0f);
}

TEST(XrMathTest, VSub_Self) {
  auto r = VSub({1, 2, 3}, {1, 2, 3});
  EXPECT_FLOAT_EQ(r.x, 0.0f);
  EXPECT_FLOAT_EQ(r.y, 0.0f);
  EXPECT_FLOAT_EQ(r.z, 0.0f);
}

// --- NormalizeQuat ---

TEST(XrMathTest, NormalizeQuat_AlreadyUnit) {
  float x = 0, y = 0, z = 0, w = 1;
  NormalizeQuat(x, y, z, w);
  EXPECT_FLOAT_EQ(x, 0.0f);
  EXPECT_FLOAT_EQ(y, 0.0f);
  EXPECT_FLOAT_EQ(z, 0.0f);
  EXPECT_FLOAT_EQ(w, 1.0f);
}

TEST(XrMathTest, NormalizeQuat_Scaled) {
  float x = 0, y = 0, z = 0, w = 2;
  NormalizeQuat(x, y, z, w);
  EXPECT_NEAR(x, 0.0f, kEps);
  EXPECT_NEAR(y, 0.0f, kEps);
  EXPECT_NEAR(z, 0.0f, kEps);
  EXPECT_NEAR(w, 1.0f, kEps);
}

TEST(XrMathTest, NormalizeQuat_General) {
  float x = 1, y = 1, z = 1, w = 1;
  NormalizeQuat(x, y, z, w);
  float len = std::sqrt(x*x + y*y + z*z + w*w);
  EXPECT_NEAR(len, 1.0f, kEps);
  EXPECT_NEAR(x, 0.5f, kEps);
}

TEST(XrMathTest, NormalizeQuat_Zero) {
  float x = 0, y = 0, z = 0, w = 0;
  NormalizeQuat(x, y, z, w);
  EXPECT_FLOAT_EQ(x, 0.0f);
  EXPECT_FLOAT_EQ(y, 0.0f);
  EXPECT_FLOAT_EQ(z, 0.0f);
  EXPECT_FLOAT_EQ(w, 1.0f);
}
