#include "layer_dispatch.h"
#include "control_channel.h"
#include <vector>

namespace vr_agent {
std::string PathToStr(XrPath p) {
    if (p == 1) return "/user/hand/left";
    if (p == 2) return "/user/hand/right";
    return "/unknown";
}

void LayerLog(const char*, const char*) {}

LayerDispatch g_dispatch_table = {};
const LayerDispatch& Dispatch() { return g_dispatch_table; }

std::vector<StickyPose> ControlChannelGetStickyPoses() {
    return {};
}
}
