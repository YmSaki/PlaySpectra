// Shared layer state: mutex-protected stores consumed by hooks and the control channel.
// Extracted from control_channel.h (R19 move-only) so the state API is independent of the
// transport mechanism.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vr_agent {

enum class InputType { Float, Bool, Vector2f, Active };

struct PendingInput {
  InputType type;
  std::string top_level;
  std::string source;
  std::string profile;
  float f = 0.0f;
  bool b = false;
  float x = 0.0f, y = 0.0f;
};

struct StickyPose {
  std::string top_level;
  std::string source;
  float px = 0.0f, py = 0.0f, pz = 0.0f;
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
  uint32_t durationMs = 0;
  uint64_t seq = 0;
};

struct HeadPose {
  float px = 0.0f, py = 0.0f, pz = 0.0f;
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
  uint32_t durationMs = 0;
  uint64_t seq = 0;
};

struct ViewInfo {
  float px = 0.0f, py = 0.0f, pz = 0.0f;
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
  float angleLeft = 0.0f, angleRight = 0.0f;
  float angleUp = 0.0f, angleDown = 0.0f;
};

// Input queue (one-shot mutations drained by xrSyncActions hook).
void LayerStateEnqueueInput(const PendingInput& input);
std::vector<PendingInput> LayerStateDrainInputs();

// Sticky controller poses (re-applied every xrSyncActions until cleared).
void LayerStateSetStickyPose(const StickyPose& pose);
void LayerStateClearStickyPose(const std::string& top_level);
void LayerStateClearAllStickyPoses();
std::vector<StickyPose> LayerStateGetStickyPoses();

// Head / viewpoint override.
void LayerStateSetHead(const HeadPose& pose);
void LayerStateClearHead();
bool LayerStateGetHead(HeadPose& out);

// Located views (per-eye pose + FOV, published by the locate hook).
void LayerStateSetViews(const std::vector<ViewInfo>& views, const std::string& spaceDesc);
bool LayerStateGetViews(std::vector<ViewInfo>& out, std::string& spaceDescOut);

// Instance/session/CA presence flags.
void LayerStateSetInstance(bool present);
void LayerStateSetSession(bool present);
void LayerStateSetConformanceAutomation(bool present);
void LayerStateSetRuntimeName(const std::string& name);

// Haptic observation.
void LayerStateRecordHaptic(const std::string& hand, float amplitude);

struct LayerStatus {
  bool instance = false;
  bool session = false;
  bool ca = false;
  std::string runtimeName;
  int hapticCount = 0;
  std::string lastHapticHand;
  float lastHapticAmplitude = 0.0f;
};
LayerStatus LayerStateGetStatus();

struct HapticEntry {
  int seq = 0;
  std::string hand;
  float amplitude = 0.0f;
};
std::vector<HapticEntry> LayerStateGetHapticLog(int limit);

}  // namespace vr_agent
