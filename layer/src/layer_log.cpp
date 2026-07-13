// Shared layer logger implementation. Moved verbatim from openxr_agent_layer.cpp (refactor phase 1);
// behaviour is unchanged (same env vars, same format, same mutex).
#include "layer_log.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace vr_agent {

static std::mutex g_log_mutex;

static std::ofstream& LogStream() {
  static std::ofstream stream = [] {
    const char* env = std::getenv("VR_AGENT_LOG");
    const char* tmp = std::getenv("TEMP");
    std::string path = env   ? env
                       : tmp ? std::string(tmp) + "\\vr_agent_layer.log"
                             : "vr_agent_layer.log";
    return std::ofstream(path, std::ios::app);
  }();
  return stream;
}

void LayerLog(const char* msg, const char* detail) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::ofstream& out = LogStream();
  if (!out) return;
  out << "[vr_agent] " << msg;
  if (detail) out << ": " << detail;
  out << "\n";
  out.flush();
}

}  // namespace vr_agent
