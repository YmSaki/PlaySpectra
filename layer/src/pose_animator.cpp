// durationMs interpolation implementation. See pose_animator.h for the semantics, the target/state
// ownership split (control channel owns targets, animator owns glide state), the time source, and
// the leaf-mutex lock discipline.
#include "pose_animator.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace playspectra {

namespace {

// One glide: from startPose (captured when the target generation changed) towards the target over
// its durationMs. lastEval is what the NEXT glide starts from (the "previous evaluated value").
struct Segment {
  uint64_t seq = 0;      // target generation this segment animates towards (0 = never evaluated)
  XrTime startTime = 0;  // when the glide began (first evaluation after the seq change)
  XrPosef startPose{};
  XrPosef lastEval{};
  bool haveLast = false;
};

std::mutex g_anim_mutex;                    // leaf mutex: nothing else is taken while held
std::map<std::string, Segment> g_hands;    // keyed by top_level ("/user/hand/left|right")
Segment g_head_seg;
XrTime g_last_display_time = 0;

XrVector3f Lerp(const XrVector3f& a, const XrVector3f& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

XrQuaternionf QNormalize(const XrQuaternionf& q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n < 1e-9f) return {0.0f, 0.0f, 0.0f, 1.0f};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}

// Shortest-path slerp (dot < 0 negates one end), with an nlerp fallback for near-parallel ends.
XrQuaternionf Slerp(const XrQuaternionf& a, XrQuaternionf b, float t) {
  float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (dot < 0.0f) { b = {-b.x, -b.y, -b.z, -b.w}; dot = -dot; }
  if (dot > 0.9995f) {  // nearly parallel: lerp + normalize is numerically safer
    return QNormalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                       a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
  }
  const float theta0 = std::acos(dot);
  const float s0 = std::sin((1.0f - t) * theta0) / std::sin(theta0);
  const float s1 = std::sin(t * theta0) / std::sin(theta0);
  return QNormalize({a.x * s0 + b.x * s1, a.y * s0 + b.y * s1,
                     a.z * s0 + b.z * s1, a.w * s0 + b.w * s1});
}

// Constant angular velocity of the whole glide (axis * angle / duration), shortest path -- the
// closed-form slerp derivative (§5.2 receptacle; not consumed by the located-velocity paths yet).
XrVector3f GlideAngVel(const XrQuaternionf& from, XrQuaternionf to, float durSec) {
  float dot = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
  if (dot < 0.0f) { to = {-to.x, -to.y, -to.z, -to.w}; dot = -dot; }
  // delta = to * conj(from): the rotation carrying `from` onto `to`.
  const XrQuaternionf d = {
      to.w * -from.x + to.x * from.w + to.y * -from.z - to.z * -from.y,
      to.w * -from.y - to.x * -from.z + to.y * from.w + to.z * -from.x,
      to.w * -from.z + to.x * -from.y - to.y * -from.x + to.z * from.w,
      to.w * from.w - to.x * -from.x - to.y * -from.y - to.z * -from.z};
  const float w = d.w > 1.0f ? 1.0f : (d.w < -1.0f ? -1.0f : d.w);
  const float angle = 2.0f * std::acos(w);
  const float s = std::sqrt(1.0f - w * w);
  if (angle < 1e-6f || s < 1e-6f || durSec <= 0.0f) return {0.0f, 0.0f, 0.0f};
  const float k = angle / (s * durSec);
  return {d.x * k, d.y * k, d.z * k};
}

// Shared evaluator: target pose T (already in LOCAL space) + generation + duration, against the
// per-key segment. PRECONDITION: caller holds g_anim_mutex.
EvaluatedPose Eval(Segment& s, const XrPosef& T, uint64_t seq, uint32_t durationMs, XrTime now) {
  EvaluatedPose out;
  out.pose = T;

  // No time base yet (before the first frame): snap and latch, so a glide never starts against
  // time 0 and later jumps. Real commands arrive long after the first frame in practice.
  if (now == 0) {
    s.seq = seq;
    s.startTime = 0;
    s.lastEval = T;
    s.haveLast = true;
    return out;
  }

  if (seq != s.seq) {  // new target generation: begin a new glide from the previous evaluated value
    s.seq = seq;
    if (s.haveLast) {
      s.startTime = now;
      s.startPose = s.lastEval;
    } else {
      s.startTime = 0;  // first ever set: nothing to glide from -> already settled on the target
      s.startPose = T;
    }
  }

  const double durNs = static_cast<double>(durationMs) * 1e6;
  double t = 1.0;
  if (durNs > 0.0 && s.startTime != 0) {
    t = static_cast<double>(now - s.startTime) / durNs;
    if (t < 0.0) t = 0.0;  // locate times may run slightly behind the latch time
    if (t > 1.0) t = 1.0;
  }

  if (t < 1.0) {
    const float ft = static_cast<float>(t);
    out.pose.position = Lerp(s.startPose.position, T.position, ft);
    out.pose.orientation = Slerp(s.startPose.orientation, T.orientation, ft);
    out.animating = true;
    const float durSec = static_cast<float>(durNs / 1e9);
    out.linVel = {(T.position.x - s.startPose.position.x) / durSec,
                  (T.position.y - s.startPose.position.y) / durSec,
                  (T.position.z - s.startPose.position.z) / durSec};
    out.angVel = GlideAngVel(s.startPose.orientation, T.orientation, durSec);
  }

  s.lastEval = out.pose;
  s.haveLast = true;
  return out;
}

}  // namespace

void AnimatorNoteDisplayTime(XrTime displayTime) {
  if (displayTime == 0) return;
  std::lock_guard<std::mutex> lock(g_anim_mutex);
  g_last_display_time = displayTime;
}

XrTime AnimatorLastDisplayTime() {
  std::lock_guard<std::mutex> lock(g_anim_mutex);
  return g_last_display_time;
}

EvaluatedPose AnimatorEvalController(const StickyPose& target, XrTime now) {
  XrPosef T{};
  T.orientation = XrQuaternionf{target.qx, target.qy, target.qz, target.qw};
  T.position = XrVector3f{target.px, target.py, target.pz};
  std::lock_guard<std::mutex> lock(g_anim_mutex);
  return Eval(g_hands[target.top_level], T, target.seq, target.durationMs, now);
}

EvaluatedPose AnimatorEvalHead(const HeadPose& target, XrTime now) {
  XrPosef T{};
  T.orientation = XrQuaternionf{target.qx, target.qy, target.qz, target.qw};
  T.position = XrVector3f{target.px, target.py, target.pz};
  std::lock_guard<std::mutex> lock(g_anim_mutex);
  return Eval(g_head_seg, T, target.seq, target.durationMs, now);
}

void AnimatorReset() {
  std::lock_guard<std::mutex> lock(g_anim_mutex);
  g_hands.clear();
  g_head_seg = Segment{};
  g_last_display_time = 0;
}

}  // namespace playspectra
