// Host-run unit tests for pose_animator (the durationMs glide evaluator). The animator is pure
// state-machine logic over (target, seq, durationMs, now) -- no runtime, no sockets -- so it is
// unit-testable without a headset. The animator state is TU-global, so every test starts from
// AnimatorReset() (which doubles as coverage for the xrDestroyInstance reset path).
#include "pose_animator.h"

#include <gtest/gtest.h>

#include <cmath>

using playspectra::AnimatorEvalController;
using playspectra::AnimatorEvalHead;
using playspectra::AnimatorLastDisplayTime;
using playspectra::AnimatorNoteDisplayTime;
using playspectra::AnimatorReset;
using playspectra::EvaluatedPose;
using playspectra::HeadPose;
using playspectra::StickyPose;

namespace {

constexpr XrTime kMs = 1'000'000;  // XrTime is nanoseconds
constexpr double kPi = 3.14159265358979323846;

StickyPose Ctrl(const char* top, float x, float y, float z, uint64_t seq, uint32_t durMs,
                float qx = 0, float qy = 0, float qz = 0, float qw = 1) {
  StickyPose sp;
  sp.top_level = top;
  sp.source = std::string(top) + "/input/grip/pose";
  sp.px = x; sp.py = y; sp.pz = z;
  sp.qx = qx; sp.qy = qy; sp.qz = qz; sp.qw = qw;
  sp.seq = seq;
  sp.durationMs = durMs;
  return sp;
}

HeadPose Head(float x, float y, float z, uint64_t seq, uint32_t durMs) {
  HeadPose h;
  h.px = x; h.py = y; h.pz = z;
  h.seq = seq;
  h.durationMs = durMs;
  return h;
}

class PoseAnimatorTest : public ::testing::Test {
 protected:
  void SetUp() override { AnimatorReset(); }
};

TEST_F(PoseAnimatorTest, DurationZeroSnapsToTarget) {
  const EvaluatedPose e = AnimatorEvalController(Ctrl("/user/hand/right", 1, 2, 3, 1, 0), 5000 * kMs);
  EXPECT_FLOAT_EQ(e.pose.position.x, 1.0f);
  EXPECT_FLOAT_EQ(e.pose.position.y, 2.0f);
  EXPECT_FLOAT_EQ(e.pose.position.z, 3.0f);
  EXPECT_FALSE(e.animating);
}

TEST_F(PoseAnimatorTest, FirstEverSetWithDurationSnaps) {
  // No previous evaluated value -> nothing to glide from -> snap even with durationMs > 0.
  const EvaluatedPose e = AnimatorEvalController(Ctrl("/user/hand/right", 1, 0, 0, 1, 1000), 5000 * kMs);
  EXPECT_FLOAT_EQ(e.pose.position.x, 1.0f);
  EXPECT_FALSE(e.animating);
}

TEST_F(PoseAnimatorTest, GlideInterpolatesPositionLinearly) {
  const XrTime t0 = 10'000 * kMs;
  AnimatorEvalController(Ctrl("/user/hand/right", 0, 0, 0, 1, 0), t0);  // anchor at origin
  const StickyPose target = Ctrl("/user/hand/right", 2, 0, 0, 2, 1000);
  const EvaluatedPose start = AnimatorEvalController(target, t0);  // glide begins at first eval
  EXPECT_TRUE(start.animating);
  EXPECT_FLOAT_EQ(start.pose.position.x, 0.0f);

  const EvaluatedPose mid = AnimatorEvalController(target, t0 + 500 * kMs);
  EXPECT_TRUE(mid.animating);
  EXPECT_NEAR(mid.pose.position.x, 1.0f, 1e-4f);
  // Closed-form linear velocity: 2 m over 1 s.
  EXPECT_NEAR(mid.linVel.x, 2.0f, 1e-4f);

  const EvaluatedPose done = AnimatorEvalController(target, t0 + 1500 * kMs);
  EXPECT_FALSE(done.animating);
  EXPECT_FLOAT_EQ(done.pose.position.x, 2.0f);
  EXPECT_FLOAT_EQ(done.linVel.x, 0.0f);  // settled: velocity back to zero
}

TEST_F(PoseAnimatorTest, GlideSlerpsOrientationHalfway) {
  const XrTime t0 = 10'000 * kMs;
  AnimatorEvalController(Ctrl("/user/hand/left", 0, 0, 0, 1, 0), t0);  // identity anchor
  // Target: 90 degrees about +Y.
  const float s = std::sin(kPi / 4.0), c = std::cos(kPi / 4.0);
  const StickyPose target = Ctrl("/user/hand/left", 0, 0, 0, 2, 1000, 0, s, 0, c);
  AnimatorEvalController(target, t0);
  const EvaluatedPose mid = AnimatorEvalController(target, t0 + 500 * kMs);
  // Halfway = 45 degrees about +Y.
  EXPECT_NEAR(mid.pose.orientation.y, std::sin(kPi / 8.0), 1e-3f);
  EXPECT_NEAR(mid.pose.orientation.w, std::cos(kPi / 8.0), 1e-3f);
  // Constant angular velocity: pi/2 rad over 1 s, about +Y.
  EXPECT_NEAR(mid.angVel.y, static_cast<float>(kPi / 2.0), 1e-3f);
}

TEST_F(PoseAnimatorTest, RetargetMidGlideStartsFromCurrentEval) {
  const XrTime t0 = 10'000 * kMs;
  AnimatorEvalController(Ctrl("/user/hand/right", 0, 0, 0, 1, 0), t0);
  const StickyPose g1 = Ctrl("/user/hand/right", 2, 0, 0, 2, 1000);
  AnimatorEvalController(g1, t0);
  AnimatorEvalController(g1, t0 + 500 * kMs);  // now at x=1
  // Retarget to x=0 with a fresh 1 s glide: must start from x=1 (no warp back to 2 or 0).
  const StickyPose g2 = Ctrl("/user/hand/right", 0, 0, 0, 3, 1000);
  const EvaluatedPose e = AnimatorEvalController(g2, t0 + 500 * kMs);
  EXPECT_TRUE(e.animating);
  EXPECT_NEAR(e.pose.position.x, 1.0f, 1e-4f);
  const EvaluatedPose mid = AnimatorEvalController(g2, t0 + 1000 * kMs);
  EXPECT_NEAR(mid.pose.position.x, 0.5f, 1e-4f);
}

TEST_F(PoseAnimatorTest, TimeBeforeGlideStartClampsToStartPose) {
  const XrTime t0 = 10'000 * kMs;
  AnimatorEvalController(Ctrl("/user/hand/right", 0, 0, 0, 1, 0), t0);
  const StickyPose target = Ctrl("/user/hand/right", 2, 0, 0, 2, 1000);
  AnimatorEvalController(target, t0 + 100 * kMs);
  // A locate time slightly BEHIND the latch time must clamp to t=0, not extrapolate backwards.
  const EvaluatedPose e = AnimatorEvalController(target, t0 + 50 * kMs);
  EXPECT_NEAR(e.pose.position.x, 0.0f, 1e-4f);
}

TEST_F(PoseAnimatorTest, ZeroNowSnapsAndLatches) {
  // Before any frame (no display time), evaluation snaps -- and stays snapped once time appears.
  const StickyPose target = Ctrl("/user/hand/right", 1, 0, 0, 1, 1000);
  const EvaluatedPose e0 = AnimatorEvalController(target, 0);
  EXPECT_FLOAT_EQ(e0.pose.position.x, 1.0f);
  EXPECT_FALSE(e0.animating);
  const EvaluatedPose e1 = AnimatorEvalController(target, 5000 * kMs);
  EXPECT_FLOAT_EQ(e1.pose.position.x, 1.0f);
  EXPECT_FALSE(e1.animating);
}

TEST_F(PoseAnimatorTest, HandsAndHeadAreIndependent) {
  const XrTime t0 = 10'000 * kMs;
  AnimatorEvalController(Ctrl("/user/hand/left", 0, 0, 0, 1, 0), t0);
  AnimatorEvalHead(Head(0, 0, 0, 2, 0), t0);
  const StickyPose lg = Ctrl("/user/hand/left", 2, 0, 0, 3, 1000);
  AnimatorEvalController(lg, t0);
  // A right-hand eval and a head eval mid-glide must not disturb the left glide.
  AnimatorEvalController(Ctrl("/user/hand/right", 9, 9, 9, 4, 0), t0 + 250 * kMs);
  const EvaluatedPose h = AnimatorEvalHead(Head(0, 5, 0, 5, 0), t0 + 250 * kMs);
  EXPECT_FLOAT_EQ(h.pose.position.y, 5.0f);
  const EvaluatedPose mid = AnimatorEvalController(lg, t0 + 500 * kMs);
  EXPECT_NEAR(mid.pose.position.x, 1.0f, 1e-4f);
}

TEST_F(PoseAnimatorTest, HeadGlideInterpolates) {
  const XrTime t0 = 10'000 * kMs;
  AnimatorEvalHead(Head(0, 1.3f, 0, 1, 0), t0);
  const HeadPose target = Head(2, 1.3f, 0, 2, 1000);
  AnimatorEvalHead(target, t0);
  const EvaluatedPose mid = AnimatorEvalHead(target, t0 + 500 * kMs);
  EXPECT_TRUE(mid.animating);
  EXPECT_NEAR(mid.pose.position.x, 1.0f, 1e-4f);
  EXPECT_FLOAT_EQ(mid.pose.position.y, 1.3f);
}

TEST_F(PoseAnimatorTest, NoteDisplayTimeIsCachedAndResetDropsIt) {
  EXPECT_EQ(AnimatorLastDisplayTime(), 0);
  AnimatorNoteDisplayTime(42 * kMs);
  EXPECT_EQ(AnimatorLastDisplayTime(), 42 * kMs);
  AnimatorNoteDisplayTime(0);  // a zero displayTime must not clobber the cache
  EXPECT_EQ(AnimatorLastDisplayTime(), 42 * kMs);
  AnimatorReset();
  EXPECT_EQ(AnimatorLastDisplayTime(), 0);
}

}  // namespace
