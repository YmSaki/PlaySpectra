// PlaySpectra control channel implementation. See control_channel.h.
//
// A single background thread runs a blocking accept loop on 127.0.0.1:PLAYSPECTRA_PORT (default
// 52700, override via env PLAYSPECTRA_PORT). It handles one MCP client at a time, reading
// newline-delimited JSON requests and writing one JSON reply line per request. Input-mutating
// commands are pushed onto layer_state for the layer to drain on the app thread; `status` is
// answered inline from published state.

#include "control_channel.h"
#include "layer_state.h"
#include "capture.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
// POSIX sockets: the accept/serve loop is identical; only the platform types/teardown differ. A small
// compat shim below maps the winsock spellings (SOCKET / INVALID_SOCKET / closesocket / SD_BOTH) onto
// their POSIX equivalents so the loop body stays verbatim.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
static constexpr int INVALID_SOCKET = -1;
static constexpr int SOCKET_ERROR = -1;
static inline int closesocket(int fd) { return ::close(fd); }
static inline void WSACleanup() {}  // no-op on POSIX (no winsock to tear down)
#ifndef SD_BOTH
#define SD_BOTH SHUT_RDWR
#endif
#endif

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "action_registry.h"
#include "layer_log.h"

namespace playspectra {
namespace {

using json = nlohmann::json;

std::thread g_thread;
std::atomic<bool> g_running{false};
SOCKET g_listen_socket = INVALID_SOCKET;
std::atomic<SOCKET> g_client_socket{INVALID_SOCKET};

void LogCC(const std::string& msg) { LayerLog("control_channel", msg.c_str()); }

std::string TopLevelFromHand(const std::string& hand) { return "/user/hand/" + hand; }

json BuildStatus() {
  LayerStatus st = LayerStateGetStatus();
  return json{
      {"ok", true},
      {"instance", st.instance},
      {"session", st.session},
      {"conformanceAutomation", st.ca},
      {"runtimeName", st.runtimeName},
      {"hapticCount", st.hapticCount},
      {"lastHapticHand", st.lastHapticHand},
      {"lastHapticAmplitude", st.lastHapticAmplitude},
  };
}

json Handle_status(const json&) {
  json s = BuildStatus();
  try {
    s["capture"] = json::parse(CaptureStatusJson());
  } catch (...) {
  }
  return s;
}

json Handle_screenshot(const json& req) {
  const std::string eye = req.value("eye", std::string("dominant"));
  const int timeoutMs = req.value("timeoutMs", 5000);
  const bool withDepth = req.value("withDepth", false);
  try {
    return json::parse(CaptureRequestScreenshot(eye, timeoutMs, withDepth));
  } catch (...) {
    return json{{"ok", false}, {"error", "capture returned malformed result"}};
  }
}

json Handle_input(const json& req) {
  const std::string hand = req.value("hand", "");
  const std::string input = req.value("input", "");
  const std::string type = req.value("type", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  if (input.empty()) {
    return json{{"ok", false}, {"error", "missing 'input' (e.g. 'squeeze/value')"}};
  }

  PendingInput p;
  p.top_level = TopLevelFromHand(hand);
  p.source = p.top_level + "/input/" + input;
  if (type == "float") {
    p.type = InputType::Float;
    p.f = req.value("value", 0.0f);
  } else if (type == "bool") {
    p.type = InputType::Bool;
    p.b = req.value("value", false);
  } else if (type == "vec2") {
    p.type = InputType::Vector2f;
    p.x = req.value("x", 0.0f);
    p.y = req.value("y", 0.0f);
  } else {
    return json{{"ok", false}, {"error", "type must be 'float', 'bool', or 'vec2'"}};
  }
  LayerStateEnqueueInput(std::move(p));
  return json{{"ok", true}, {"queued", "input"}};
}

uint32_t ParseDurationMs(const json& req) {
  const double d = req.value("durationMs", 0.0);
  return d > 0.0 ? static_cast<uint32_t>(d) : 0u;
}

json Handle_pose(const json& req) {
  const std::string hand = req.value("hand", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  StickyPose sp;
  sp.top_level = TopLevelFromHand(hand);
  sp.source = sp.top_level + "/input/grip/pose";
  sp.px = req.value("x", 0.0f);
  sp.py = req.value("y", 0.0f);
  sp.pz = req.value("z", 0.0f);
  sp.qx = req.value("qx", 0.0f);
  sp.qy = req.value("qy", 0.0f);
  sp.qz = req.value("qz", 0.0f);
  sp.qw = req.value("qw", 1.0f);
  sp.durationMs = ParseDurationMs(req);
  LayerStateSetStickyPose(sp);
  return json{{"ok", true}, {"queued", "pose"}};
}

json Handle_pose_clear(const json& req) {
  const std::string hand = req.value("hand", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  LayerStateClearStickyPose(TopLevelFromHand(hand));
  return json{{"ok", true}, {"cleared", "pose"}};
}

json Handle_pose_get(const json& req) {
  const std::string hand = req.value("hand", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  const std::string top = TopLevelFromHand(hand);
  for (const StickyPose& sp : LayerStateGetStickyPoses()) {
    if (sp.top_level == top) {
      return json{{"ok", true}, {"active", true}, {"x", sp.px}, {"y", sp.py}, {"z", sp.pz},
                  {"qx", sp.qx}, {"qy", sp.qy}, {"qz", sp.qz}, {"qw", sp.qw}};
    }
  }
  return json{{"ok", true}, {"active", false}};
}

json Handle_head_get(const json&) {
  HeadPose h;
  if (LayerStateGetHead(h)) {
    return json{{"ok", true}, {"active", true}, {"x", h.px}, {"y", h.py}, {"z", h.pz},
                {"qx", h.qx}, {"qy", h.qy}, {"qz", h.qz}, {"qw", h.qw}};
  }
  return json{{"ok", true}, {"active", false}};
}

json Handle_head(const json& req) {
  HeadPose h;
  h.px = req.value("x", 0.0f);
  h.py = req.value("y", 0.0f);
  h.pz = req.value("z", 0.0f);
  h.qx = req.value("qx", 0.0f);
  h.qy = req.value("qy", 0.0f);
  h.qz = req.value("qz", 0.0f);
  h.qw = req.value("qw", 1.0f);
  h.durationMs = ParseDurationMs(req);
  LayerStateSetHead(h);
  return json{{"ok", true}, {"queued", "head"}};
}

json Handle_head_clear(const json&) {
  LayerStateClearHead();
  return json{{"ok", true}, {"cleared", "head"}};
}

json Handle_haptics(const json& req) {
  const int limit = req.value("limit", 20);
  json arr = json::array();
  for (const HapticEntry& e : LayerStateGetHapticLog(limit))
    arr.push_back({{"seq", e.seq}, {"hand", e.hand}, {"amplitude", e.amplitude}});
  return json{{"ok", true}, {"haptics", arr}};
}

json Handle_actions(const json&) {
  try {
    return json::parse(BuildActionsJson());
  } catch (...) {
    return json{{"ok", false}, {"error", "actions dump failed"}};
  }
}

json Handle_reset(const json&) {
  LayerStateClearAllStickyPoses();
  LayerStateClearHead();
  return json{{"ok", true}, {"reset", true}};
}

json Handle_active(const json& req) {
  const std::string hand = req.value("hand", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  PendingInput p;
  p.type = InputType::Active;
  p.top_level = TopLevelFromHand(hand);
  p.profile = req.value("profile", std::string("/interaction_profiles/oculus/touch_controller"));
  p.b = req.value("active", true);
  LayerStateEnqueueInput(std::move(p));
  return json{{"ok", true}, {"queued", "active"}};
}

json Handle_view(const json&) {
  std::vector<ViewInfo> views;
  std::string space;
  if (!LayerStateGetViews(views, space)) {
    return json{{"ok", true}, {"available", false}};
  }
  json arr = json::array();
  for (const ViewInfo& v : views) {
    arr.push_back({
        {"pose",
         {{"x", v.px}, {"y", v.py}, {"z", v.pz},
          {"qx", v.qx}, {"qy", v.qy}, {"qz", v.qz}, {"qw", v.qw}}},
        {"fov",
         {{"angleLeft", v.angleLeft}, {"angleRight", v.angleRight},
          {"angleUp", v.angleUp}, {"angleDown", v.angleDown}}},
    });
  }
  return json{
      {"ok", true},
      {"available", true},
      {"viewCount", views.size()},
      {"space", space},
      {"views", arr},
      {"note",
       "Per-eye view pose (position+orientation) and projection FOV, in the app's view-locate "
       "space. World->camera: view matrix = inverse of the eye pose (rotation R from the "
       "quaternion, translation t from the position; view = [R^T | -R^T t]). Projection: from the "
       "asymmetric FOV half-angles in radians via tan(angleLeft/Right/Up/Down) per the OpenXR "
       "convention (angleLeft/angleDown are typically negative). camera->clip through that "
       "projection gives NDC (x,y in [-1,1]); NDC->pixel: px=(ndcX*0.5+0.5)*width, "
       "py=(1-(ndcY*0.5+0.5))*height using the captured eye image's width/height (from "
       "vr_screenshot metadata). Invert the chain to turn a pixel into a world-space ray."},
  };
}

json Handle_start_recording(const json& req) {
  const uint32_t interval = req.value("intervalFrames", 30u);
  const std::string eye = req.value("eye", std::string("dominant"));
  try {
    return json::parse(CaptureStartRecording(interval, eye));
  } catch (...) {
    return json{{"ok", false}, {"error", "start_recording failed"}};
  }
}

json Handle_stop_recording(const json&) {
  try {
    return json::parse(CaptureStopRecording());
  } catch (...) {
    return json{{"ok", false}, {"error", "stop_recording failed"}};
  }
}

// Never throws: an exception here would unwind the socket thread and std::terminate the whole VR
// app (the tool would crash the app it observes). The entire dispatch is guarded.
json HandleRequest(const std::string& line) {
  json req;
  try {
    req = json::parse(line);
  } catch (...) {
    return json{{"ok", false}, {"error", "invalid json"}};
  }
  try {
    const std::string cmd = req.value("cmd", "");
    static const struct {
      const char* name;
      json (*fn)(const json&);
    } kHandlers[] = {
        {"status", Handle_status},         {"screenshot", Handle_screenshot},
        {"input", Handle_input},           {"pose", Handle_pose},
        {"pose_clear", Handle_pose_clear}, {"pose_get", Handle_pose_get},
        {"head_get", Handle_head_get},     {"head", Handle_head},
        {"head_clear", Handle_head_clear}, {"haptics", Handle_haptics},
        {"actions", Handle_actions},       {"reset", Handle_reset},
        {"active", Handle_active},         {"view", Handle_view},
        {"start_recording", Handle_start_recording},
        {"stop_recording", Handle_stop_recording},
    };
    for (const auto& h : kHandlers) {
      if (cmd == h.name) return h.fn(req);
    }
    return json{{"ok", false}, {"error", "unknown cmd: " + cmd}};
  } catch (const std::exception& e) {
    return json{{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
  } catch (...) {
    return json{{"ok", false}, {"error", "bad request"}};
  }
}

void ServeClient(SOCKET client) {
  std::string buffer;
  char chunk[4096];
  while (g_running.load()) {
    int n = recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    buffer.append(chunk, n);
    size_t nl;
    while ((nl = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      json reply = HandleRequest(line);
      std::string out = reply.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
      out.push_back('\n');
      size_t sent = 0;
      while (sent < out.size()) {
        int m = send(client, out.data() + sent, static_cast<int>(out.size() - sent), 0);
        if (m <= 0) return;
        sent += static_cast<size_t>(m);
      }
    }
  }
}

void AcceptLoop(unsigned short port) {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    LogCC("WSAStartup failed");
    return;
  }
#endif

  g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_listen_socket == INVALID_SOCKET) {
    LogCC("socket() failed");
#ifdef _WIN32
    WSACleanup();
#endif
    return;
  }

  int reuse = 1;
  setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (bind(g_listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    LogCC("bind() failed on 127.0.0.1:" + std::to_string(port));
    closesocket(g_listen_socket);
    g_listen_socket = INVALID_SOCKET;
    WSACleanup();
    return;
  }
  if (listen(g_listen_socket, 1) == SOCKET_ERROR) {
    LogCC("listen() failed");
    closesocket(g_listen_socket);
    g_listen_socket = INVALID_SOCKET;
    WSACleanup();
    return;
  }

  LogCC("listening on 127.0.0.1:" + std::to_string(port));
  while (g_running.load()) {
    SOCKET client = accept(g_listen_socket, nullptr, nullptr);
    if (client == INVALID_SOCKET) break;
    LogCC("client connected");
    g_client_socket.store(client);
    ServeClient(client);
    SOCKET c = g_client_socket.exchange(INVALID_SOCKET);
    if (c != INVALID_SOCKET) closesocket(c);
    LogCC("client disconnected");
  }

  if (g_listen_socket != INVALID_SOCKET) {
    closesocket(g_listen_socket);
    g_listen_socket = INVALID_SOCKET;
  }
  WSACleanup();
  LogCC("accept loop exited");
}

unsigned short ResolvePort() {
  if (const char* env = std::getenv("PLAYSPECTRA_PORT")) {
    int p = std::atoi(env);
    if (p > 0 && p < 65536) return static_cast<unsigned short>(p);
  }
  return 52700;
}

}  // namespace

void ControlChannelStart() {
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true)) return;
  unsigned short port = ResolvePort();
  g_thread = std::thread(AcceptLoop, port);
}

void ControlChannelStop() {
  if (!g_running.exchange(false)) return;
  if (g_listen_socket != INVALID_SOCKET) {
    closesocket(g_listen_socket);
    g_listen_socket = INVALID_SOCKET;
  }
  SOCKET c = g_client_socket.exchange(INVALID_SOCKET);
  if (c != INVALID_SOCKET) {
    shutdown(c, SD_BOTH);
    closesocket(c);
  }
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace playspectra
