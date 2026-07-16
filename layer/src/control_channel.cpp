// VR-MCP control channel implementation. See control_channel.h.
//
// A single background thread runs a blocking accept loop on 127.0.0.1:VR_AGENT_PORT (default
// 52700, override via env VR_AGENT_PORT). It handles one MCP client at a time, reading
// newline-delimited JSON requests and writing one JSON reply line per request. Input-mutating
// commands are pushed onto a queue for the layer to drain on the app thread; `status` is
// answered inline from published state.

#include "control_channel.h"
#include "capture.h"
#include "xr_math.h"           // NormalizeQuat

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "action_registry.h"
#include "layer_log.h"

namespace vr_agent {
namespace {

using json = nlohmann::json;

std::thread g_thread;
std::atomic<bool> g_running{false};
SOCKET g_listen_socket = INVALID_SOCKET;
// The currently-served client socket, so ControlChannelStop can unblock its recv() at shutdown.
// Whoever exchanges it to INVALID_SOCKET first owns the close (avoids a double close).
std::atomic<SOCKET> g_client_socket{INVALID_SOCKET};

std::mutex g_queue_mutex;
std::vector<PendingInput> g_queue;

std::mutex g_pose_mutex;
std::map<std::string, StickyPose> g_poses;  // keyed by top_level (one pose per hand)

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

// Rolling log of the most recent app-requested haptic pulses, for the vr_haptics tool.
struct HapticEvent { int seq; std::string hand; float amplitude; };
std::vector<HapticEvent> g_haptic_log;  // under g_state_mutex; capped to kHapticLogMax
constexpr size_t kHapticLogMax = 64;

void LogCC(const std::string& msg) { LayerLog("control_channel", msg.c_str()); }

// Compose "/user/hand/<hand>" from a "left"/"right" field.
std::string TopLevelFromHand(const std::string& hand) { return "/user/hand/" + hand; }

// Build the reply for a `status` request from published layer state.
json BuildStatus() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return json{
      {"ok", true},
      {"instance", g_instance_present},
      {"session", g_session_present},
      {"conformanceAutomation", g_ca_present},
      {"runtimeName", g_runtime_name},
      {"hapticCount", g_haptic_count},
      {"lastHapticHand", g_last_haptic_hand},
      {"lastHapticAmplitude", g_last_haptic_amplitude},
  };
}

// ---- Command handlers. Each takes the parsed request json and returns the reply json. Bodies moved
// verbatim from the former HandleRequest if-chain (R06 move-only); response JSON is unchanged. They
// stay in this TU because they touch TU-internal statics (g_queue_mutex/g_queue/g_state_mutex/
// g_haptic_log/g_pose_mutex/g_poses) and file-local helpers. Handlers that don't read the request
// take an unnamed json&. ----

json Handle_status(const json&) {
  json s = BuildStatus();
  try {
    s["capture"] = json::parse(CaptureStatusJson());
  } catch (...) {
  }
  return s;
}

json Handle_screenshot(const json& req) {
  // { cmd:"screenshot", eye:"left"|"right"|"dominant", timeoutMs:5000, withDepth:false }
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
  // { cmd:"input", hand:"right", input:"squeeze/value", type:"float", value:1.0 }
  // { cmd:"input", hand:"left",  input:"thumbstick",    type:"vec2", x:0.5, y:-0.2 }
  // { cmd:"input", hand:"right", input:"a/click",       type:"bool", value:true }
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
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push_back(std::move(p));
  }
  return json{{"ok", true}, {"queued", "input"}};
}

// Optional durationMs on pose/head: how long the glide to the new target takes (pose_animator).
// Absent/0/negative/non-numeric all mean "snap now" -- the pre-durationMs behaviour.
uint32_t ParseDurationMs(const json& req) {
  const double d = req.value("durationMs", 0.0);
  return d > 0.0 ? static_cast<uint32_t>(d) : 0u;
}

json Handle_pose(const json& req) {
  // { cmd:"pose", hand:"left", x:-0.2, y:-0.2, z:-0.5, qx:0, qy:0, qz:0, qw:1, durationMs:0 }
  // Sets a sticky grip pose (LOCAL space, -Z forward, +Y up), held until pose_clear. durationMs>0
  // glides there from the previous injected pose instead of snapping.
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
  ControlChannelSetStickyPose(sp);
  return json{{"ok", true}, {"queued", "pose"}};
}

json Handle_pose_clear(const json& req) {
  // { cmd:"pose_clear", hand:"left" }
  const std::string hand = req.value("hand", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  ControlChannelClearStickyPose(TopLevelFromHand(hand));
  return json{{"ok", true}, {"cleared", "pose"}};
}

json Handle_pose_get(const json& req) {
  // { cmd:"pose_get", hand:"left" } -> authoritative sticky pose, or {active:false}.
  // The layer is the single source of truth for injected poses; MCP tools query this instead of
  // mirroring state (so vr_move / vr_look_at act on what's really held).
  const std::string hand = req.value("hand", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  const std::string top = TopLevelFromHand(hand);
  for (const StickyPose& sp : ControlChannelGetStickyPoses()) {
    if (sp.top_level == top) {
      return json{{"ok", true}, {"active", true}, {"x", sp.px}, {"y", sp.py}, {"z", sp.pz},
                  {"qx", sp.qx}, {"qy", sp.qy}, {"qz", sp.qz}, {"qw", sp.qw}};
    }
  }
  return json{{"ok", true}, {"active", false}};
}

json Handle_head_get(const json&) {
  // { cmd:"head_get" } -> authoritative head override, or {active:false}.
  HeadPose h;
  if (ControlChannelGetHead(h)) {
    return json{{"ok", true}, {"active", true}, {"x", h.px}, {"y", h.py}, {"z", h.pz},
                {"qx", h.qx}, {"qy", h.qy}, {"qz", h.qz}, {"qw", h.qw}};
  }
  return json{{"ok", true}, {"active", false}};
}

json Handle_head(const json& req) {
  // { cmd:"head", x:0, y:0, z:0, qx:0, qy:0, qz:0, qw:1, durationMs:0 }
  // Overrides the viewpoint (head pose) in the app's world locate space. Held until head_clear.
  // durationMs>0 glides there from the previous injected head pose instead of snapping.
  HeadPose h;
  h.px = req.value("x", 0.0f);
  h.py = req.value("y", 0.0f);
  h.pz = req.value("z", 0.0f);
  h.qx = req.value("qx", 0.0f);
  h.qy = req.value("qy", 0.0f);
  h.qz = req.value("qz", 0.0f);
  h.qw = req.value("qw", 1.0f);
  h.durationMs = ParseDurationMs(req);
  ControlChannelSetHead(h);
  return json{{"ok", true}, {"queued", "head"}};
}

json Handle_head_clear(const json&) {
  ControlChannelClearHead();
  return json{{"ok", true}, {"cleared", "head"}};
}

json Handle_haptics(const json& req) {
  // { cmd:"haptics", limit:20 } -> the most recent app-requested haptic pulses (newest last).
  const int limit = req.value("limit", 20);
  json arr = json::array();
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    size_t start = 0;
    if (limit > 0 && g_haptic_log.size() > static_cast<size_t>(limit))
      start = g_haptic_log.size() - static_cast<size_t>(limit);
    for (size_t i = start; i < g_haptic_log.size(); ++i)
      arr.push_back({{"seq", g_haptic_log[i].seq}, {"hand", g_haptic_log[i].hand},
                     {"amplitude", g_haptic_log[i].amplitude}});
  }
  return json{{"ok", true}, {"haptics", arr}};
}

json Handle_actions(const json&) {
  // { cmd:"actions" } -> dump of the app's registered action sets / actions and their bound
  // interaction-profile paths, so an agent can discover inputs by NAME instead of guessing paths.
  try {
    return json::parse(BuildActionsJson());
  } catch (...) {
    return json{{"ok", false}, {"error", "actions dump failed"}};
  }
}

json Handle_reset(const json&) {
  // Drop every override: all sticky controller poses and the head. Runtime reverts to its own poses.
  {
    std::lock_guard<std::mutex> lock(g_pose_mutex);
    g_poses.clear();
  }
  ControlChannelClearHead();
  return json{{"ok", true}, {"reset", true}};
}

json Handle_active(const json& req) {
  // { cmd:"active", hand:"right", active:true, profile:"/interaction_profiles/oculus/touch_controller" }
  const std::string hand = req.value("hand", "");
  if (hand != "left" && hand != "right") {
    return json{{"ok", false}, {"error", "hand must be 'left' or 'right'"}};
  }
  PendingInput p;
  p.type = InputType::Active;
  p.top_level = TopLevelFromHand(hand);
  p.profile = req.value("profile", std::string("/interaction_profiles/oculus/touch_controller"));
  p.b = req.value("active", true);
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_queue.push_back(std::move(p));
  }
  return json{{"ok", true}, {"queued", "active"}};
}

json Handle_view(const json&) {
  // { cmd:"view" } -> the latest per-eye view pose + FOV captured at xrLocateViews (after any head
  // override). The observe/act bridge: with these an agent maps screen pixels <-> world points, so
  // a button seen in a screenshot maps to a world coordinate to point/look at. {available:false}
  // until the app has located views at least once.
  std::vector<ViewInfo> views;
  std::string space;
  if (!ControlChannelGetViews(views, space)) {
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

// Parse a single request line and return the JSON reply. Never throws (a malformed/mis-typed
// request must produce an error reply, NEVER an exception -- an exception here would unwind through
// the socket thread and std::terminate the whole VR app, i.e. the tool would crash the app it
// observes). nlohmann's json::value throws type_error when a field has the wrong type or the doc
// isn't an object, so the whole dispatch below is guarded.
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

// Serve one connected client until it disconnects. Splits the byte stream on '\n'.
void ServeClient(SOCKET client) {
  std::string buffer;
  char chunk[4096];
  while (g_running.load()) {
    int n = recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) break;  // client closed or error
    buffer.append(chunk, n);
    size_t nl;
    while ((nl = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      json reply = HandleRequest(line);
      // replace error handler: never throw on a non-UTF-8 byte in the reply (would unwind the thread).
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
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    LogCC("WSAStartup failed");
    return;
  }

  g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_listen_socket == INVALID_SOCKET) {
    LogCC("socket() failed");
    WSACleanup();
    return;
  }

  BOOL reuse = TRUE;
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
    if (client == INVALID_SOCKET) break;  // listen socket closed by ControlChannelStop()
    LogCC("client connected");
    g_client_socket.store(client);
    ServeClient(client);
    // Close whoever still owns it (Stop may have already taken and closed it to unblock recv).
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
  if (const char* env = std::getenv("VR_AGENT_PORT")) {
    int p = std::atoi(env);
    if (p > 0 && p < 65536) return static_cast<unsigned short>(p);
  }
  return 52700;
}

}  // namespace

void ControlChannelStart() {
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true)) return;  // already started
  unsigned short port = ResolvePort();
  g_thread = std::thread(AcceptLoop, port);
}

void ControlChannelStop() {
  if (!g_running.exchange(false)) return;
  if (g_listen_socket != INVALID_SOCKET) {
    closesocket(g_listen_socket);  // unblock accept()
    g_listen_socket = INVALID_SOCKET;
  }
  // Unblock a connected client's recv() so ServeClient returns and the thread can join -- otherwise
  // xrDestroyInstance hangs forever whenever an MCP client is still connected (the E2E setup).
  SOCKET c = g_client_socket.exchange(INVALID_SOCKET);
  if (c != INVALID_SOCKET) {
    shutdown(c, SD_BOTH);
    closesocket(c);
  }
  if (g_thread.joinable()) g_thread.join();
}

std::vector<PendingInput> ControlChannelDrainInputs() {
  std::vector<PendingInput> out;
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  out.swap(g_queue);
  return out;
}

// Target generation counter, shared by controller + head targets. Stamped on every set so the
// pose_animator can tell "same target, keep the glide in flight" from "new target, start a glide".
namespace { std::atomic<uint64_t> g_target_seq{0}; }

void ControlChannelSetStickyPose(const StickyPose& pose) {
  StickyPose p = pose;
  NormalizeQuat(p.qx, p.qy, p.qz, p.qw);  // JSON gives no unit-length guarantee
  p.seq = ++g_target_seq;
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  g_poses[p.top_level] = p;
}
void ControlChannelClearStickyPose(const std::string& top_level) {
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  g_poses.erase(top_level);
}
std::vector<StickyPose> ControlChannelGetStickyPoses() {
  std::vector<StickyPose> out;
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  out.reserve(g_poses.size());
  for (const auto& kv : g_poses) out.push_back(kv.second);
  return out;
}

void ControlChannelSetHead(const HeadPose& pose) {
  HeadPose p = pose;
  NormalizeQuat(p.qx, p.qy, p.qz, p.qw);  // JSON gives no unit-length guarantee
  p.seq = ++g_target_seq;
  std::lock_guard<std::mutex> lock(g_head_mutex);
  g_head = p;
  g_head_active = true;
}
void ControlChannelClearHead() {
  std::lock_guard<std::mutex> lock(g_head_mutex);
  g_head_active = false;
}
bool ControlChannelGetHead(HeadPose& out) {
  std::lock_guard<std::mutex> lock(g_head_mutex);
  if (g_head_active) out = g_head;
  return g_head_active;
}

void ControlChannelSetViews(const std::vector<ViewInfo>& views, const std::string& spaceDesc) {
  std::lock_guard<std::mutex> lock(g_view_mutex);
  g_views = views;
  g_views_space = spaceDesc;
  g_views_available = true;
}
bool ControlChannelGetViews(std::vector<ViewInfo>& out, std::string& spaceDescOut) {
  std::lock_guard<std::mutex> lock(g_view_mutex);
  if (!g_views_available) return false;
  out = g_views;
  spaceDescOut = g_views_space;
  return true;
}

void ControlChannelSetInstance(bool present) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_instance_present = present;
}
void ControlChannelSetSession(bool present) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_session_present = present;
}
void ControlChannelSetConformanceAutomation(bool present) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_ca_present = present;
}
void ControlChannelSetRuntimeName(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_runtime_name = name;
}
void ControlChannelRecordHaptic(const std::string& hand, float amplitude) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_haptic_count;
  g_last_haptic_hand = hand;
  g_last_haptic_amplitude = amplitude;
  g_haptic_log.push_back(HapticEvent{g_haptic_count, hand, amplitude});
  if (g_haptic_log.size() > kHapticLogMax)
    g_haptic_log.erase(g_haptic_log.begin(), g_haptic_log.end() - kHapticLogMax);
}

}  // namespace vr_agent
