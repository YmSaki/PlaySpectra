// Shared layer state implementation. Extracted from control_channel.cpp (R19 move-only).
#include "layer_state.h"
#include "xr_math.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vr_agent {
namespace {

std::mutex g_queue_mutex;
std::vector<PendingInput> g_queue;

std::mutex g_pose_mutex;
std::map<std::string, StickyPose> g_poses;

std::mutex g_head_mutex;
bool g_head_active = false;
HeadPose g_head;

std::mutex g_view_mutex;
bool g_views_available = false;
std::vector<ViewInfo> g_views;
std::string g_views_space;

std::mutex g_state_mutex;
bool g_instance_present = false;
bool g_session_present = false;
bool g_ca_present = false;
std::string g_runtime_name;
int g_haptic_count = 0;
std::string g_last_haptic_hand;
float g_last_haptic_amplitude = 0.0f;

struct HapticEvent { int seq; std::string hand; float amplitude; };
std::vector<HapticEvent> g_haptic_log;
constexpr size_t kHapticLogMax = 64;

std::atomic<uint64_t> g_target_seq{0};

}  // namespace

void LayerStateEnqueueInput(const PendingInput& input) {
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  g_queue.push_back(input);
}

std::vector<PendingInput> LayerStateDrainInputs() {
  std::vector<PendingInput> out;
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  out.swap(g_queue);
  return out;
}

void LayerStateSetStickyPose(const StickyPose& pose) {
  StickyPose p = pose;
  NormalizeQuat(p.qx, p.qy, p.qz, p.qw);
  p.seq = ++g_target_seq;
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  g_poses[p.top_level] = p;
}
void LayerStateClearStickyPose(const std::string& top_level) {
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  g_poses.erase(top_level);
}
void LayerStateClearAllStickyPoses() {
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  g_poses.clear();
}
std::vector<StickyPose> LayerStateGetStickyPoses() {
  std::vector<StickyPose> out;
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  out.reserve(g_poses.size());
  for (const auto& kv : g_poses) out.push_back(kv.second);
  return out;
}

void LayerStateSetHead(const HeadPose& pose) {
  HeadPose p = pose;
  NormalizeQuat(p.qx, p.qy, p.qz, p.qw);
  p.seq = ++g_target_seq;
  std::lock_guard<std::mutex> lock(g_head_mutex);
  g_head = p;
  g_head_active = true;
}
void LayerStateClearHead() {
  std::lock_guard<std::mutex> lock(g_head_mutex);
  g_head_active = false;
}
bool LayerStateGetHead(HeadPose& out) {
  std::lock_guard<std::mutex> lock(g_head_mutex);
  if (g_head_active) out = g_head;
  return g_head_active;
}

void LayerStateSetViews(const std::vector<ViewInfo>& views, const std::string& spaceDesc) {
  std::lock_guard<std::mutex> lock(g_view_mutex);
  g_views = views;
  g_views_space = spaceDesc;
  g_views_available = true;
}
bool LayerStateGetViews(std::vector<ViewInfo>& out, std::string& spaceDescOut) {
  std::lock_guard<std::mutex> lock(g_view_mutex);
  if (!g_views_available) return false;
  out = g_views;
  spaceDescOut = g_views_space;
  return true;
}

void LayerStateSetInstance(bool present) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_instance_present = present;
}
void LayerStateSetSession(bool present) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_session_present = present;
}
void LayerStateSetConformanceAutomation(bool present) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_ca_present = present;
}
void LayerStateSetRuntimeName(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_runtime_name = name;
}
void LayerStateRecordHaptic(const std::string& hand, float amplitude) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_haptic_count;
  g_last_haptic_hand = hand;
  g_last_haptic_amplitude = amplitude;
  g_haptic_log.push_back(HapticEvent{g_haptic_count, hand, amplitude});
  if (g_haptic_log.size() > kHapticLogMax)
    g_haptic_log.erase(g_haptic_log.begin(), g_haptic_log.end() - kHapticLogMax);
}

LayerStatus LayerStateGetStatus() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return {g_instance_present, g_session_present, g_ca_present, g_runtime_name,
          g_haptic_count, g_last_haptic_hand, g_last_haptic_amplitude};
}

std::vector<HapticEntry> LayerStateGetHapticLog(int limit) {
  std::vector<HapticEntry> out;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  size_t start = 0;
  if (limit > 0 && g_haptic_log.size() > static_cast<size_t>(limit))
    start = g_haptic_log.size() - static_cast<size_t>(limit);
  for (size_t i = start; i < g_haptic_log.size(); ++i)
    out.push_back({g_haptic_log[i].seq, g_haptic_log[i].hand, g_haptic_log[i].amplitude});
  return out;
}

}  // namespace vr_agent
