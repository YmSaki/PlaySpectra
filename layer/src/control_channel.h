// VR-MCP control channel: localhost TCP NDJSON server embedded in the OpenXR API layer.
//
// The MCP server (TypeScript) connects here and sends one JSON object per line. Mutating commands
// (input/active queued as PendingInput; pose/head/reset stored as sticky state) are applied by the
// layer on the app's own thread inside the xrSyncActions / locate hooks -- never touched from the
// socket thread (OpenXR runtime state, like a D3D11 immediate context, is not safe to poke
// cross-thread). Query commands (status, pose_get, head_get, haptics, screenshot) are answered on
// the socket thread from state the layer publishes (screenshot hands off to the xrEndFrame thread).

#pragma once

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
// The pose is expressed in the layer's own LOCAL reference space (matches hello_xr's app space).
struct StickyPose {
  std::string top_level;                     // e.g. "/user/hand/left"
  std::string source;                        // e.g. "/user/hand/left/input/grip/pose"
  float px = 0.0f, py = 0.0f, pz = 0.0f;     // position (metres, LOCAL space, -Z forward, +Y up)
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;  // orientation quaternion (default identity)
};

// Lifecycle -- started once when the first XrInstance is created, stopped at layer unload.
void ControlChannelStart();
void ControlChannelStop();

// Called by the layer (app thread) to pull all queued input mutations.
std::vector<PendingInput> ControlChannelDrainInputs();

// Sticky controller poses (set/cleared by the `pose`/`pose_clear` commands, re-applied by the layer
// every xrSyncActions). GetStickyPoses returns the current set to apply this sync.
void ControlChannelSetStickyPose(const StickyPose& pose);
void ControlChannelClearStickyPose(const std::string& top_level);
std::vector<StickyPose> ControlChannelGetStickyPoses();

// Head / viewpoint override (set/cleared by the `head`/`head_clear` commands). Unlike controller
// poses, the head is NOT an input device, so conformance_automation can't set it -- the layer
// overrides it directly inside xrLocateViews / xrLocateSpace(VIEW). The pose is expressed in the
// world reference space the app locates views in (LOCAL for hello_xr; -Z forward, +Y up).
struct HeadPose {
  float px = 0.0f, py = 0.0f, pz = 0.0f;
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
};
void ControlChannelSetHead(const HeadPose& pose);
void ControlChannelClearHead();
bool ControlChannelGetHead(HeadPose& out);  // true if a head override is currently active

// One located view (per eye) captured at xrLocateViews time: the pose the app renders from and the
// projection FOV. Published for the `view` command so an agent can map between world points and
// screen pixels -- the observe(screenshot)/act(pose) bridge. The view matrix is the inverse of
// `pose`; the projection is built from the FOV half-angles. Expressed in the app's view-locate
// reference space (see spaceDesc from GetViews). Angles are OpenXR XrFovf half-angles in radians
// (angleLeft/angleDown are typically negative).
struct ViewInfo {
  float px = 0.0f, py = 0.0f, pz = 0.0f;             // eye position (metres, in the locate space)
  float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;  // eye orientation quaternion
  float angleLeft = 0.0f, angleRight = 0.0f;         // FOV half-angles (radians, signed)
  float angleUp = 0.0f, angleDown = 0.0f;
};

// Latest views captured at xrLocateViews (AFTER any head override), published for the `view` command.
// Set from the app thread inside the locate hook; read on the socket thread. GetViews returns false
// until the app has located views at least once (spaceDesc names the locate reference space).
void ControlChannelSetViews(const std::vector<ViewInfo>& views, const std::string& spaceDesc);
bool ControlChannelGetViews(std::vector<ViewInfo>& out, std::string& spaceDescOut);

// State published by the layer for the `status` command (thread-safe setters).
void ControlChannelSetInstance(bool present);
void ControlChannelSetSession(bool present);
void ControlChannelSetConformanceAutomation(bool present);
void ControlChannelSetRuntimeName(const std::string& name);

// Record an app-requested haptic pulse (from the xrApplyHapticFeedback hook) so `vr_haptics`
// and `status` can report "the app buzzed the controller" -- a near-free observability signal.
void ControlChannelRecordHaptic(const std::string& hand, float amplitude);

}  // namespace vr_agent
