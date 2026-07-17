// Shared layer state: mutex-protected stores consumed by hooks and the control channel.
// Extracted from control_channel.h (R19 move-only) so the state API is independent of the
// transport mechanism.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vr_agent {

enum class InputType { Float, Bool, Vector2f, Active };

// One queued input mutation, resolved to XrPaths and applied via XR_EXT_conformance_automation
// inside the layer's xrSyncActions hook.
struct PendingInput {
  InputType type;
  std::string top_level;    // e.g. "/user/hand/right"
  std::string source;       // e.g. "/user/hand/right/input/squeeze/value" (unused for Active)
  std::string profile;      // Active only: e.g. "/interaction_profiles/oculus/touch_controller"
  float f = 0.0f;           // Float
  bool b = false;           // Bool / Active
  float x = 0.0f, y = 0.0f; // Vector2f
};

// A controller pose to hold. Unlike PendingInput (one-shot, drained), a StickyPose is re-applied by
// the layer on EVERY xrSyncActions via xrSetInputDeviceLocationEXT until cleared -- so a single
// `vr_set_controller` holds the hand in place instead of the caller having to re-send each frame.
// The pose is expressed in the layer's own LOCAL reference space (-Z forward, +Y up, metres).
struct StickyPose {
  std::string top_level;
  std::string source;
  float px = 0.0f, py = 0.0f, pz = 0.0f;
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
  // durationMs glide (pose_animator): 0 = snap. seq is the target generation, stamped on every set
  // -- a changed seq tells the animator "new glide".
  uint32_t durationMs = 0;
  uint64_t seq = 0;
};

// Head / viewpoint override. Unlike controller poses, the head is NOT an input device, so
// conformance_automation can't set it -- the layer overrides it directly inside xrLocateViews /
// xrLocateSpace(VIEW). The pose is in the app's view-locate reference space (LOCAL for hello_xr).
struct HeadPose {
  float px = 0.0f, py = 0.0f, pz = 0.0f;
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
  uint32_t durationMs = 0;  // glide duration, 0 = snap (see StickyPose)
  uint64_t seq = 0;         // target generation, stamped by LayerStateSetHead
};

// One located view (per eye) captured at xrLocateViews time: the pose the app renders from and the
// projection FOV. Published for the `view` command so an agent can map between world points and
// screen pixels -- the observe(screenshot)/act(pose) bridge.
struct ViewInfo {
  float px = 0.0f, py = 0.0f, pz = 0.0f;
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
  float angleLeft = 0.0f, angleRight = 0.0f;
  float angleUp = 0.0f, angleDown = 0.0f;
};

// Input queue (one-shot mutations drained by xrSyncActions hook).
void LayerStateEnqueueInput(PendingInput input);
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
