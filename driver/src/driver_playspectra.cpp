// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// PlaySpectra SteamVR virtual controller driver (driver_playspectra).
//
// WHY THIS EXISTS: the OpenXR API layer can inject controller input only through
// XR_EXT_conformance_automation, which SteamVR does not implement (The Lab test, 2026-07-17:
// conformanceAutomation:false). A SteamVR *driver* sits below that limitation: vrserver loads this
// DLL and we present two virtual controllers whose poses and buttons we control — reaching any
// SteamVR game regardless of OpenXR extension support. Capture/head-override stay in the layer.
//
// Implements: NDJSON/TCP control channel (127.0.0.1:52701, same wire protocol as the layer's
// :52700) driving sticky pose injection + input components, oculus_touch masquerade (hypothesis
// H3 — so existing games' Touch bindings resolve), and a real_poses command exposing
// IVRServerDriverHost::GetRawTrackedDevicePoses (hypothesis H2). Coexistence with the physical
// Rift Touch pair depends on SteamVR assigning hand roles to the
// most-recently-active device (hypothesis H1, unverified in this environment — see docs/steamvr-adapter.md).
//
// This code runs inside vrserver.exe: an uncaught exception kills the whole SteamVR session, so
// every entry point swallows exceptions (same "never throws" discipline as the layer's
// HandleRequest). While no injection is active the pose pump repeats the identical rest pose
// every frame so the virtual pair reads as INACTIVE to any activity-based role arbitration (H1).

#include <winsock2.h>
#include <ws2tcpip.h>

#include "openvr_driver.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using namespace vr;
using json = nlohmann::json;

namespace {

constexpr uint16_t kPort = 52701;

void Log(const std::string& m) {
  if (VRDriverLog()) VRDriverLog()->Log(("[playspectra] " + m).c_str());
}

// One queued input-component update; socket thread enqueues, RunFrame applies (keeps all
// IVRDriverInput calls on the driver frame thread).
struct InputUpdate {
  VRInputComponentHandle_t handle = k_ulInvalidInputComponentHandle;
  bool isBool = false;
  bool bval = false;
  float fval = 0.0f;
};

struct InjectedPose {
  bool active = false;
  double x = 0, y = 0, z = 0;
  double qx = 0, qy = 0, qz = 0, qw = 1;
};

class VirtualController : public ITrackedDeviceServerDriver {
 public:
  explicit VirtualController(bool isLeft) : is_left_(isLeft) {}

  EVRInitError Activate(uint32_t unObjectId) override {
    object_id_ = unObjectId;
    PropertyContainerHandle_t p = VRProperties()->TrackedDeviceToPropertyContainer(object_id_);

    // oculus_touch masquerade (H3): name the exact type the user's real Rift CV1 Touch pair uses,
    // so games' existing Touch bindings resolve against the virtual pair with no per-game setup.
    // Input profile + render model borrowed from the co-resident real oculus driver ({oculus}).
    VRProperties()->SetStringProperty(p, Prop_ModelNumber_String, "Oculus Rift CV1 (PlaySpectra virtual)");
    VRProperties()->SetStringProperty(p, Prop_ManufacturerName_String, "Oculus");
    VRProperties()->SetStringProperty(p, Prop_ControllerType_String, "oculus_touch");
    VRProperties()->SetStringProperty(p, Prop_InputProfilePath_String,
                                      "{oculus}/input/touch_profile.json");
    VRProperties()->SetStringProperty(p, Prop_RenderModelName_String,
                                      is_left_ ? "oculus_cv1_controller_left"
                                               : "oculus_cv1_controller_right");
    VRProperties()->SetInt32Property(p, Prop_ControllerRoleHint_Int32,
                                     is_left_ ? TrackedControllerRole_LeftHand
                                              : TrackedControllerRole_RightHand);

    // Touch component layout (asymmetric: left has X/Y, right has A/B).
    auto boolC = [&](const char* path, VRInputComponentHandle_t* h) {
      VRDriverInput()->CreateBooleanComponent(p, path, h);
    };
    auto scalarC = [&](const char* path, VRInputComponentHandle_t* h) {
      VRDriverInput()->CreateScalarComponent(p, path, h, VRScalarType_Absolute,
                                             VRScalarUnits_NormalizedOneSided);
    };
    boolC("/input/system/click", &c_system_);
    scalarC("/input/trigger/value", &c_trigger_);
    boolC("/input/trigger/click", &c_trigger_click_);
    boolC("/input/trigger/touch", &c_trigger_touch_);
    scalarC("/input/grip/value", &c_grip_);
    VRDriverInput()->CreateScalarComponent(p, "/input/joystick/x", &c_joy_x_,
                                           VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
    VRDriverInput()->CreateScalarComponent(p, "/input/joystick/y", &c_joy_y_,
                                           VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
    boolC("/input/joystick/click", &c_joy_click_);
    boolC("/input/joystick/touch", &c_joy_touch_);
    if (is_left_) {
      boolC("/input/x/click", &c_primary_);
      boolC("/input/x/touch", &c_primary_touch_);
      boolC("/input/y/click", &c_secondary_);
      boolC("/input/y/touch", &c_secondary_touch_);
    } else {
      boolC("/input/a/click", &c_primary_);
      boolC("/input/a/touch", &c_primary_touch_);
      boolC("/input/b/click", &c_secondary_);
      boolC("/input/b/touch", &c_secondary_touch_);
    }
    boolC("/input/thumbrest/touch", &c_thumbrest_);
    VRDriverInput()->CreateHapticComponent(p, "/output/haptic", &c_haptic_);

    Log(std::string("activated ") + (is_left_ ? "LEFT" : "RIGHT") +
        " objectId=" + std::to_string(object_id_));
    return VRInitError_None;
  }

  void Deactivate() override { object_id_ = k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void* GetComponent(const char*) override { return nullptr; }
  void DebugRequest(const char*, char* buf, uint32_t size) override {
    if (size > 0) buf[0] = '\0';
  }

  DriverPose_t GetPose() override {
    DriverPose_t pose = {};
    pose.result = TrackingResult_Running_OK;
    pose.poseIsValid = true;
    pose.deviceIsConnected = true;
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (injected_.active) {
      pose.vecPosition[0] = injected_.x;
      pose.vecPosition[1] = injected_.y;
      pose.vecPosition[2] = injected_.z;
      pose.qRotation.x = injected_.qx;
      pose.qRotation.y = injected_.qy;
      pose.qRotation.z = injected_.qz;
      pose.qRotation.w = injected_.qw;
    } else {
      // Rest pose: constant every frame — reads as "no activity" for role arbitration (H1).
      pose.qRotation.w = 1.0;
      pose.vecPosition[0] = is_left_ ? -0.2 : 0.2;
      pose.vecPosition[1] = 1.2;
      pose.vecPosition[2] = -0.3;
    }
    return pose;
  }

  void PumpPoseAndInputs() {
    if (object_id_ == k_unTrackedDeviceIndexInvalid) return;
    VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, GetPose(), sizeof(DriverPose_t));
    std::vector<InputUpdate> updates;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      updates.swap(input_queue_);
    }
    for (const InputUpdate& u : updates) {
      if (u.handle == k_ulInvalidInputComponentHandle) continue;
      if (u.isBool) VRDriverInput()->UpdateBooleanComponent(u.handle, u.bval, 0.0);
      else VRDriverInput()->UpdateScalarComponent(u.handle, u.fval, 0.0);
    }
  }

  void SetInjectedPose(const InjectedPose& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    injected_ = p;
  }

  // Resolve an input name from the control channel to a component handle. Bool inputs:
  // a/b/x/y (primary/secondary aliases per hand), system, trigger_click, trigger_touch,
  // joystick_click, joystick_touch, thumbrest. Scalar: trigger, grip, joystick_x, joystick_y.
  bool QueueInput(const std::string& name, bool isBool, bool bval, float fval) {
    InputUpdate u;
    u.isBool = isBool;
    u.bval = bval;
    u.fval = fval;
    if (name == "trigger") u.handle = c_trigger_;
    else if (name == "grip") u.handle = c_grip_;
    else if (name == "joystick_x") u.handle = c_joy_x_;
    else if (name == "joystick_y") u.handle = c_joy_y_;
    else if (name == "trigger_click") u.handle = c_trigger_click_;
    else if (name == "trigger_touch") u.handle = c_trigger_touch_;
    else if (name == "joystick_click") u.handle = c_joy_click_;
    else if (name == "joystick_touch") u.handle = c_joy_touch_;
    else if (name == "system") u.handle = c_system_;
    else if (name == "thumbrest") u.handle = c_thumbrest_;
    else if (name == "a" || name == "x" || name == "primary") u.handle = c_primary_;
    else if (name == "a_touch" || name == "x_touch") u.handle = c_primary_touch_;
    else if (name == "b" || name == "y" || name == "secondary") u.handle = c_secondary_;
    else if (name == "b_touch" || name == "y_touch") u.handle = c_secondary_touch_;
    else return false;
    std::lock_guard<std::mutex> lock(mutex_);
    input_queue_.push_back(u);
    return true;
  }

  bool activated() const { return object_id_ != k_unTrackedDeviceIndexInvalid; }
  uint32_t objectId() const { return object_id_; }
  bool injectionActive() {
    std::lock_guard<std::mutex> lock(mutex_);
    return injected_.active;
  }

 private:
  bool is_left_;
  uint32_t object_id_ = k_unTrackedDeviceIndexInvalid;
  std::mutex mutex_;
  InjectedPose injected_;
  std::vector<InputUpdate> input_queue_;

  VRInputComponentHandle_t c_system_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_trigger_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_trigger_click_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_trigger_touch_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_grip_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_joy_x_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_joy_y_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_joy_click_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_joy_touch_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_primary_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_primary_touch_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_secondary_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_secondary_touch_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_thumbrest_ = k_ulInvalidInputComponentHandle;
  VRInputComponentHandle_t c_haptic_ = k_ulInvalidInputComponentHandle;
};

class ServerProvider : public IServerTrackedDeviceProvider {
 public:
  EVRInitError Init(IVRDriverContext* pDriverContext) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
    try {
      left_ = new VirtualController(true);
      right_ = new VirtualController(false);
      VRServerDriverHost()->TrackedDeviceAdded("VRAGENT_LEFT", TrackedDeviceClass_Controller, left_);
      VRServerDriverHost()->TrackedDeviceAdded("VRAGENT_RIGHT", TrackedDeviceClass_Controller, right_);
      StartChannel();
    } catch (...) {
      return VRInitError_Driver_Failed;
    }
    return VRInitError_None;
  }

  void Cleanup() override {
    running_ = false;
    if (listen_sock_ != INVALID_SOCKET) {
      closesocket(listen_sock_);
      listen_sock_ = INVALID_SOCKET;
    }
    if (channel_thread_.joinable()) channel_thread_.join();
    WSACleanup();
    delete left_;
    left_ = nullptr;
    delete right_;
    right_ = nullptr;
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }

  const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }

  void RunFrame() override {
    try {
      if (left_) left_->PumpPoseAndInputs();
      if (right_) right_->PumpPoseAndInputs();
    } catch (...) {
    }
  }

  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}

 private:
  VirtualController* ByHand(const std::string& hand) {
    if (hand == "left") return left_;
    if (hand == "right") return right_;
    return nullptr;
  }

  // H2 verification: dump every connected device's raw pose as vrserver sees it.
  json RealPoses() {
    TrackedDevicePose_t poses[16] = {};
    VRServerDriverHost()->GetRawTrackedDevicePoses(0.0f, poses, 16);
    json arr = json::array();
    for (int i = 0; i < 16; ++i) {
      if (!poses[i].bDeviceIsConnected) continue;
      const auto& m = poses[i].mDeviceToAbsoluteTracking.m;
      arr.push_back({{"index", i},
                     {"poseValid", poses[i].bPoseIsValid},
                     {"x", m[0][3]}, {"y", m[1][3]}, {"z", m[2][3]}});
    }
    return arr;
  }

  json Handle(const json& req) {
    const std::string cmd = req.value("cmd", "");
    if (cmd == "status") {
      return {{"ok", true},
              {"left", {{"activated", left_ && left_->activated()},
                        {"objectId", left_ ? left_->objectId() : 0},
                        {"injected", left_ && left_->injectionActive()}}},
              {"right", {{"activated", right_ && right_->activated()},
                         {"objectId", right_ ? right_->objectId() : 0},
                         {"injected", right_ && right_->injectionActive()}}}};
    }
    if (cmd == "real_poses") return {{"ok", true}, {"devices", RealPoses()}};
    VirtualController* c = ByHand(req.value("hand", ""));
    if (!c) return {{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
    if (cmd == "pose") {
      InjectedPose p;
      p.active = true;
      p.x = req.value("x", 0.0);
      p.y = req.value("y", 0.0);
      p.z = req.value("z", 0.0);
      p.qx = req.value("qx", 0.0);
      p.qy = req.value("qy", 0.0);
      p.qz = req.value("qz", 0.0);
      p.qw = req.value("qw", 1.0);
      c->SetInjectedPose(p);
      return {{"ok", true}, {"queued", "pose"}};
    }
    if (cmd == "pose_clear") {
      c->SetInjectedPose(InjectedPose{});
      return {{"ok", true}, {"cleared", "pose"}};
    }
    if (cmd == "input") {
      const std::string name = req.value("path", "");
      if (req.contains("value") && req["value"].is_boolean()) {
        if (!c->QueueInput(name, true, req["value"].get<bool>(), 0)) {
          return {{"ok", false}, {"error", "unknown bool input: " + name}};
        }
      } else {
        if (!c->QueueInput(name, false, false, req.value("value", 0.0f))) {
          return {{"ok", false}, {"error", "unknown scalar input: " + name}};
        }
      }
      return {{"ok", true}, {"queued", "input"}};
    }
    return {{"ok", false}, {"error", "unknown cmd: " + cmd}};
  }

  void StartChannel() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      Log("WSAStartup failed; control channel disabled");
      return;
    }
    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) {
      Log("socket() failed; control channel disabled");
      return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(listen_sock_, 1) == SOCKET_ERROR) {
      Log("bind/listen on 52701 failed (port in use?); control channel disabled");
      closesocket(listen_sock_);
      listen_sock_ = INVALID_SOCKET;
      return;
    }
    running_ = true;
    channel_thread_ = std::thread([this] { ChannelLoop(); });
    Log("control channel listening on 127.0.0.1:52701");
  }

  // One client at a time, one JSON object per line — identical wire protocol to the layer's
  // control channel so the MCP side can reuse its client.
  void ChannelLoop() {
    while (running_) {
      SOCKET client = accept(listen_sock_, nullptr, nullptr);
      if (client == INVALID_SOCKET) {
        if (running_) continue;
        break;
      }
      std::string buf;
      char chunk[4096];
      for (;;) {
        int n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
          std::string line = buf.substr(0, nl);
          buf.erase(0, nl + 1);
          std::string reply;
          try {
            reply = Handle(json::parse(line)).dump();
          } catch (...) {
            reply = R"({"ok":false,"error":"request handling failed"})";
          }
          reply += "\n";
          send(client, reply.c_str(), static_cast<int>(reply.size()), 0);
        }
      }
      closesocket(client);
    }
  }

  VirtualController* left_ = nullptr;
  VirtualController* right_ = nullptr;
  std::thread channel_thread_;
  std::atomic<bool> running_{false};
  SOCKET listen_sock_ = INVALID_SOCKET;
};

ServerProvider g_provider;

}  // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* pInterfaceName,
                                                        int* pReturnCode) {
  if (std::strcmp(pInterfaceName, IServerTrackedDeviceProvider_Version) == 0) {
    return &g_provider;
  }
  if (pReturnCode) *pReturnCode = VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
