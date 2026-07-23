// Unit tests for the pure pose-override helpers (pose_override_pure.h): the field-copy + flag-bit
// contracts hooks_locate.cpp relies on. Cross-platform (OpenXR headers only, no D3D/Vulkan/session).
#include "pose_override_pure.h"

#include <gtest/gtest.h>

using playspectra::ApplyHeadToLocation;
using playspectra::FindInNextChain;
using playspectra::HeadPose;
using playspectra::ZeroVelocity;

TEST(ApplyHeadToLocation, CopiesPoseAndSetsAllValidTrackedBits) {
  HeadPose h;
  h.px = 1.0f; h.py = 2.0f; h.pz = 3.0f;
  h.qx = 0.1f; h.qy = 0.2f; h.qz = 0.3f; h.qw = 0.9f;
  XrPosef pose{};
  XrSpaceLocationFlags flags = 0;
  EXPECT_TRUE(ApplyHeadToLocation(h, pose, flags));
  EXPECT_FLOAT_EQ(pose.position.x, 1.0f);
  EXPECT_FLOAT_EQ(pose.position.y, 2.0f);
  EXPECT_FLOAT_EQ(pose.position.z, 3.0f);
  EXPECT_FLOAT_EQ(pose.orientation.x, 0.1f);
  EXPECT_FLOAT_EQ(pose.orientation.y, 0.2f);
  EXPECT_FLOAT_EQ(pose.orientation.z, 0.3f);
  EXPECT_FLOAT_EQ(pose.orientation.w, 0.9f);
  EXPECT_EQ(flags, XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                       XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
}

TEST(ApplyHeadToLocation, OrsFlagsRatherThanOverwriting) {
  HeadPose h;  // identity
  XrPosef pose{};
  const XrSpaceLocationFlags unrelated = 0x40000000;  // a bit outside the ones we set
  XrSpaceLocationFlags flags = unrelated;
  ApplyHeadToLocation(h, pose, flags);
  EXPECT_TRUE(flags & unrelated) << "must OR, not overwrite the caller's flags";
  EXPECT_TRUE(flags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
}

TEST(ZeroVelocity, ZeroesVectorsAndSetsBothValidBits) {
  XrVector3f lin{1.0f, 2.0f, 3.0f};
  XrVector3f ang{4.0f, 5.0f, 6.0f};
  XrSpaceVelocityFlags flags = 0;
  ZeroVelocity(flags, lin, ang);
  EXPECT_FLOAT_EQ(lin.x, 0.0f);
  EXPECT_FLOAT_EQ(lin.y, 0.0f);
  EXPECT_FLOAT_EQ(lin.z, 0.0f);
  EXPECT_FLOAT_EQ(ang.x, 0.0f);
  EXPECT_FLOAT_EQ(ang.y, 0.0f);
  EXPECT_FLOAT_EQ(ang.z, 0.0f);
  EXPECT_EQ(flags, XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
}

TEST(FindInNextChain, FindsEachNodeByType) {
  // A(view) -> B(location) -> C(velocity)
  XrBaseOutStructure c{};
  c.type = XR_TYPE_SPACE_VELOCITY;
  c.next = nullptr;
  XrBaseOutStructure b{};
  b.type = XR_TYPE_SPACE_LOCATION;
  b.next = &c;
  XrBaseOutStructure a{};
  a.type = XR_TYPE_VIEW;
  a.next = &b;
  EXPECT_EQ(FindInNextChain(&a, XR_TYPE_VIEW), static_cast<void*>(&a));  // head matches
  EXPECT_EQ(FindInNextChain(&a, XR_TYPE_SPACE_LOCATION), static_cast<void*>(&b));
  EXPECT_EQ(FindInNextChain(&a, XR_TYPE_SPACE_VELOCITY), static_cast<void*>(&c));  // tail
}

TEST(FindInNextChain, ReturnsNullWhenAbsentOrEmpty) {
  XrBaseOutStructure a{};
  a.type = XR_TYPE_VIEW;
  a.next = nullptr;
  EXPECT_EQ(FindInNextChain(&a, XR_TYPE_SPACE_VELOCITY), nullptr);
  EXPECT_EQ(FindInNextChain(nullptr, XR_TYPE_VIEW), nullptr);
}
