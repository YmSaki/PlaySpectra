// Shared layer logger. Extracted from openxr_agent_layer.cpp (refactor phase 1) so the three
// translation units that use it stop re-declaring the same extern by hand (the old "implicit ABI").
#pragma once

namespace vr_agent {

// Append a line to the layer log (path from VR_AGENT_LOG, else %TEMP%\vr_agent_layer.log). Implemented
// in layer_log.cpp. Thread-safe.
void LayerLog(const char* msg, const char* detail);

}  // namespace vr_agent
