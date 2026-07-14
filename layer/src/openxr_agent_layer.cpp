// VR-MCP OpenXR API layer.
//
// Exports only xrNegotiateLoaderApiLayerInterface -- the loader chains us in via
// nextGetInstanceProcAddr, we never link against openxr_loader ourselves.
//
// Walking skeleton (tasks #6 + #7-lite): the layer runs a localhost TCP NDJSON control channel
// (control_channel.cpp) that the MCP server drives. Input mutations are applied on the app's own
// thread inside the xrSyncActions hook via XR_EXT_conformance_automation -- which the layer
// transparently enables at instance creation so the runtime handles isActive / interaction-profile
// binding / sync latching for us. xrApplyHapticFeedback is hooked purely to observe "the app buzzed
// the controller" (hello_xr vibrates when grab > 0.9), giving an end-to-end signal without capture.
//
// Implemented since the skeleton: frame capture (xrCreateSession graphics binding + swapchain +
// xrEndFrame, VULKAN readback -- capture.cpp; D3D11/D3D12 still return an explicit "not implemented"
// error, core-required follow-ups); head/VIEW override (xrLocateViews + xrLocateSpace(VIEW)); and
// controller pose override (xrLocateSpace on grip action spaces) since the Meta sim ignores CA
// orientation. The layer is the authoritative pose source at locate time; CA handles buttons/analog.
// TODO(#7-full): per-instance dispatch state -- this still keeps a single global instance/session and
//           each hook latches its next-pointer in a function static (see the 4-agent review notes).

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "control_channel.h"
#include "hooks_action.h"
#include "hooks_capture.h"
#include "hooks_locate.h"
#include "layer_dispatch.h"
#include "layer_log.h"

namespace {

void Log(const char* msg, const char* detail = nullptr) { vr_agent::LayerLog(msg, detail); }

// The layer's dispatch table and single-instance state (instance / CA flag / next-gipa) live
// TU-private in layer_dispatch.cpp; these using-declarations pull the accessors the remaining
// dispatch/lifecycle code (VrAgentGetInstanceProcAddr / VrAgentCreateApiLayerInstance /
// PublishRuntimeName) uses into the anonymous namespace so it keeps calling them unqualified. The
// per-cluster accessors (action_registry / pose_override / input_inject) moved out with their hooks
// to hooks_*.cpp, so only these dispatch accessors remain here.
using vr_agent::CurrentInstance;
using vr_agent::Dispatch;
using vr_agent::NextGetInstanceProcAddr;
using vr_agent::RebuildLayerDispatch;
using vr_agent::SetCaEnabled;
using vr_agent::SetCurrentInstance;
using vr_agent::SetNextGetInstanceProcAddr;

// ---------------------------------------------------------------------------------------------
// Hooked functions.
// ---------------------------------------------------------------------------------------------
// All intercepted xr* hooks now live in three cluster TUs (refactor R04), grouped by the state
// they touch; the dispatch table (kHooks[] below) references them by name via the hooks_*.h
// prototypes:
//   hooks_capture.cpp -- frame capture (session/swapchain/frame): xrCreateSession,
//     xrCreateSwapchain, xrDestroySwapchain, xrEnumerateSwapchainImages, xrAcquireSwapchainImage,
//     xrReleaseSwapchainImage, xrEndFrame.
//   hooks_locate.cpp  -- head/VIEW + controller-pose override: xrLocateViews,
//     xrCreateReferenceSpace, xrLocateSpace, xrLocateSpaces.
//   hooks_action.cpp  -- action-system observation + GAP-08 non-CA fallback + teardown fan-out:
//     xrSyncActions, xrGetActionState{Boolean,Float,Vector2f}, xrGetCurrentInteractionProfile,
//     xrPollEvent, xrApplyHapticFeedback, xrCreateActionSpace, xrSuggestInteractionProfileBindings,
//     xrCreateActionSet, xrCreateAction, xrDestroyActionSet, xrAttachSessionActionSets,
//     xrDestroySession, xrDestroyInstance, xrDestroySpace.

// ---------------------------------------------------------------------------------------------
// Dispatch.
// ---------------------------------------------------------------------------------------------
XrResult XRAPI_CALL VrAgentGetInstanceProcAddr(XrInstance instance, const char* name,
                                                PFN_xrVoidFunction* function) {
  try {
    if (name == nullptr || function == nullptr) return XR_ERROR_VALIDATION_FAILURE;

    // Return our own hooks for the intercepted functions.
    struct HookEntry {
      const char* name;
      PFN_xrVoidFunction fn;
    };
    static const HookEntry kHooks[] = {
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateSession)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySession)},
        {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrSyncActions)},
        {"xrApplyHapticFeedback", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrApplyHapticFeedback)},
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroyInstance)},
        {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateSwapchain)},
        {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySwapchain)},
        {"xrEnumerateSwapchainImages",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEnumerateSwapchainImages)},
        {"xrAcquireSwapchainImage",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrAcquireSwapchainImage)},
        {"xrReleaseSwapchainImage",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrReleaseSwapchainImage)},
        {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEndFrame)},
        {"xrLocateViews", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrLocateViews)},
        {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateReferenceSpace)},
        {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrLocateSpace)},
        {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySpace)},
        {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateActionSpace)},
        {"xrSuggestInteractionProfileBindings",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrSuggestInteractionProfileBindings)},
        {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateActionSet)},
        {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateAction)},
        {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroyActionSet)},
        {"xrAttachSessionActionSets",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrAttachSessionActionSets)},
        // GAP-08: non-CA input fallback (harmless passthrough when CA is enabled).
        {"xrGetActionStateBoolean",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetActionStateBoolean)},
        {"xrGetActionStateFloat", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetActionStateFloat)},
        {"xrGetActionStateVector2f",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetActionStateVector2f)},
        {"xrGetCurrentInteractionProfile",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetCurrentInteractionProfile)},
        {"xrPollEvent", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrPollEvent)},
    };
    for (const HookEntry& h : kHooks) {
      if (std::strcmp(name, h.name) == 0) {
        *function = h.fn;
        return XR_SUCCESS;
      }
    }

    // xrLocateSpaces (OpenXR 1.1) is optional: only advertise our hook if the runtime provides it,
    // so an app probing support via xrGetInstanceProcAddr isn't misled by a non-null pointer that
    // would then return UNSUPPORTED at call time.
    if (std::strcmp(name, "xrLocateSpaces") == 0) {
      if (Dispatch().locateSpaces != nullptr) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrLocateSpaces);
        return XR_SUCCESS;
      }
      // fall through to passthrough (runtime lacks it -> report as the runtime does)
    }

    PFN_xrGetInstanceProcAddr next = NextGetInstanceProcAddr();
    if (next == nullptr) {
      *function = nullptr;
      return XR_ERROR_HANDLE_INVALID;
    }
    return next(instance, name, function);
  } catch (...) {
    if (function) *function = nullptr;
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Query whether the runtime (below us) advertises an extension, before the instance exists.
bool RuntimeSupportsExtension(PFN_xrGetInstanceProcAddr gipa, const char* ext_name) {
  PFN_xrVoidFunction fn = nullptr;
  if (gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn) != XR_SUCCESS || !fn) {
    return false;
  }
  auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(fn);
  uint32_t count = 0;
  if (enumerate(nullptr, 0, &count, nullptr) != XR_SUCCESS || count == 0) return false;
  std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
  if (enumerate(nullptr, count, &count, props.data()) != XR_SUCCESS) return false;
  for (const XrExtensionProperties& p : props) {
    if (std::strcmp(p.extensionName, ext_name) == 0) return true;
  }
  return false;
}

void PublishRuntimeName() {
  PFN_xrGetInstanceProperties get_props = Dispatch().getInstanceProperties;
  if (!get_props) return;
  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (get_props(CurrentInstance(), &props) == XR_SUCCESS) {
    vr_agent::ControlChannelSetRuntimeName(props.runtimeName);
    Log("runtime", props.runtimeName);
  }
}

XrResult XRAPI_CALL VrAgentCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                   const XrApiLayerCreateInfo* apiLayerInfo,
                                                   XrInstance* instance) {
  try {
    if (apiLayerInfo == nullptr ||
        apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        apiLayerInfo->nextInfo == nullptr ||
        apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        apiLayerInfo->nextInfo->nextGetInstanceProcAddr == nullptr ||
        apiLayerInfo->nextInfo->nextCreateApiLayerInstance == nullptr) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }

    PFN_xrGetInstanceProcAddr next_gipa = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance next_create = apiLayerInfo->nextInfo->nextCreateApiLayerInstance;

    // Advance the chain: the next layer must see its own nextInfo, not ours.
    XrApiLayerCreateInfo next_layer_info = *apiLayerInfo;
    next_layer_info.nextInfo = apiLayerInfo->nextInfo->next;

    SetNextGetInstanceProcAddr(next_gipa);

    // Transparently enable XR_EXT_conformance_automation so we can inject input through the
    // runtime. Only do so if the runtime actually supports it -- enabling an unsupported
    // extension would make xrCreateInstance fail and break the app.
    // Escape hatch: VR_AGENT_NO_CA=1 disables the injection (diagnostic / runtimes where enabling
    // CA changes session lifecycle behaviour).
    const bool ca_disabled = std::getenv("VR_AGENT_NO_CA") != nullptr;
    const bool ca_supported =
        !ca_disabled &&
        RuntimeSupportsExtension(next_gipa, XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME);

    std::vector<const char*> exts(info->enabledExtensionNames,
                                  info->enabledExtensionNames + info->enabledExtensionCount);
    bool already_enabled = false;
    for (const char* e : exts) {
      if (std::strcmp(e, XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME) == 0) already_enabled = true;
    }
    if (ca_supported && !already_enabled) {
      exts.push_back(XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME);
      Log("injecting XR_EXT_conformance_automation into instance extensions");
    } else if (!ca_supported) {
      Log("runtime does NOT support XR_EXT_conformance_automation -- input injection disabled");
    }

    XrInstanceCreateInfo new_info = *info;
    new_info.enabledExtensionNames = exts.data();
    new_info.enabledExtensionCount = static_cast<uint32_t>(exts.size());

    Log("xrCreateApiLayerInstance -> forwarding to next layer");
    XrResult result = next_create(&new_info, &next_layer_info, instance);
    if (XR_SUCCEEDED(result) && instance) {
      SetCurrentInstance(*instance);
      SetCaEnabled(ca_supported);
      // GAP-07: resolve the whole next-layer table now, against THIS instance's chain. Rebuilt on
      // every create so a second instance never inherits the previous runtime's stale pointers.
      RebuildLayerDispatch();
      vr_agent::ControlChannelSetInstance(true);
      vr_agent::ControlChannelSetSession(false);
      vr_agent::ControlChannelSetConformanceAutomation(ca_supported);
      PublishRuntimeName();
      vr_agent::ControlChannelStart();
      Log("instance created; control channel started");
    } else {
      Log("instance creation failed");
    }
    return result;
  } catch (...) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
}

}  // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* apiLayerName,
    XrNegotiateApiLayerRequest* apiLayerRequest) {
  try {
    if (loaderInfo == nullptr || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (apiLayerName != nullptr && std::strcmp(apiLayerName, "XR_APILAYER_vr_agent") != 0) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minApiVersion > XR_CURRENT_API_VERSION ||
        loaderInfo->maxApiVersion < XR_CURRENT_API_VERSION) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (apiLayerRequest == nullptr ||
        apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest)) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr = VrAgentGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = VrAgentCreateApiLayerInstance;

    Log("xrNegotiateLoaderApiLayerInterface OK");
    return XR_SUCCESS;
  } catch (...) {
    // Exceptions must never cross this ABI boundary.
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
