// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Shared layer logger implementation. Moved verbatim from layer_entry.cpp (refactor phase 1),
// then extended with a wall-clock timestamp prefix: correlating this log against the app's own log
// and the integration harness's sleeps is how flakes and glide timing get diagnosed, and without
// timestamps that correlation needed ad-hoc instrumentation. Same env vars, same mutex; consumers
// grep by message substring, which the prefix does not disturb.
#include "layer_log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace playspectra {

static std::mutex g_log_mutex;

static std::ofstream& LogStream() {
  static std::ofstream stream = [] {
    const char* env = std::getenv("PLAYSPECTRA_LOG");
    const char* tmp = std::getenv("TEMP");
    std::string path = env   ? env
                       : tmp ? std::string(tmp) + "\\playspectra_layer.log"
                             : "playspectra_layer.log";
    return std::ofstream(path, std::ios::app);
  }();
  return stream;
}

// Local wall-clock time as "HH:MM:SS.mmm" (date omitted: these logs live per-run and are read
// side by side with same-day harness output). PRECONDITION: caller holds g_log_mutex -- the
// std::localtime result is a shared static, and this logger is its only caller in the layer.
static void FormatNow(char (&buf)[16]) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t secs = std::chrono::system_clock::to_time_t(now);
  const int ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
  const std::tm* tm = std::localtime(&secs);
  if (!tm) { buf[0] = '\0'; return; }
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm->tm_hour, tm->tm_min, tm->tm_sec, ms);
}

void LayerLog(const char* msg, const char* detail) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::ofstream& out = LogStream();
  if (!out) return;
  char ts[16];
  FormatNow(ts);
  out << "[playspectra " << ts << "] " << msg;
  if (detail) out << ": " << detail;
  out << "\n";
  out.flush();
}

}  // namespace playspectra
