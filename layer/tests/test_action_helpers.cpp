#include <gtest/gtest.h>
#include "action_registry.h"

using playspectra::HandTopFromBindingPath;

TEST(HandTopFromBindingPath, LeftHand) {
  EXPECT_EQ(HandTopFromBindingPath("/user/hand/left/input/grip/pose"), "/user/hand/left");
}

TEST(HandTopFromBindingPath, RightHand) {
  EXPECT_EQ(HandTopFromBindingPath("/user/hand/right/input/squeeze/value"), "/user/hand/right");
}

TEST(HandTopFromBindingPath, HandOnly) {
  EXPECT_EQ(HandTopFromBindingPath("/user/hand/right"), "/user/hand/right");
}

TEST(HandTopFromBindingPath, NotUserHand) {
  EXPECT_EQ(HandTopFromBindingPath("/user/head/pose"), "");
}

TEST(HandTopFromBindingPath, Empty) {
  EXPECT_EQ(HandTopFromBindingPath(""), "");
}

TEST(HandTopFromBindingPath, ShortPrefix) {
  EXPECT_EQ(HandTopFromBindingPath("/user/hand"), "");
}
