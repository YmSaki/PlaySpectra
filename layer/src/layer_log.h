// Shared layer logger. Extracted from openxr_agent_layer.cpp (refactor phase 1) so the three
// translation units that use it stop re-declaring the same extern by hand (the old "implicit ABI").
#pragma once

#include <string>

namespace vr_agent {

// Append a line to the layer log (path from VR_AGENT_LOG, else %TEMP%\vr_agent_layer.log). Implemented
// in layer_log.cpp. Thread-safe.
void LayerLog(const char* msg, const char* detail);

// Action-discovery dump for the control channel's `actions` command. Defined in openxr_agent_layer.cpp
// (where the action registry lives); declared here so control_channel.cpp needn't re-declare it.
// TODO(refactor phase 4): move to action_registry.h alongside the registry.
std::string LayerBuildActionsJson();

}  // namespace vr_agent
