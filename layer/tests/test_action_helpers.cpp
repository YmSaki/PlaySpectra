// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

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
