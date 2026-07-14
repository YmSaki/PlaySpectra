cmake_minimum_required(VERSION 3.21)
project(vr_agent_layer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# OpenXR-SDK: headers only (the layer talks to the loader via xrNegotiateLoaderApiLayerInterface,
# it does not link against openxr_loader). Also provides hello_xr + openxr_loader used by task #3
# for bring-up, via the same fetched source tree.
FetchContent_Declare(
  openxr_sdk
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK-Source.git
  GIT_TAG        release-1.1.42
)
FetchContent_MakeAvailable(openxr_sdk)

FetchContent_Declare(
  lodepng
  GIT_REPOSITORY https://github.com/lvandeve/lodepng.git
  GIT_TAG        master
)
FetchContent_Populate(lodepng)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(vr_agent_layer SHARED
  src/openxr_agent_layer.cpp
  src/layer_log.cpp
  src/layer_dispatch.cpp
  src/action_registry.cpp
  src/control_channel.cpp
  src/capture.cpp
  src/capture_vulkan.cpp
  src/capture_d3d11.cpp
  src/capture_d3d12.cpp
  ${lodepng_SOURCE_DIR}/lodepng.cpp
)

target_include_directories(vr_agent_layer PRIVATE
  ${lodepng_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Vulkan-Headers/include
)

target_link_libraries(vr_agent_layer PRIVATE
  OpenXR::headers
  nlohmann_json::nlohmann_json
  d3d11
  d3d12
  dxgi
  dxguid
  ws2_32
)

# Exceptions must never cross the exported C ABI (xrNegotiateLoaderApiLayerInterface and the
# hooked xr* functions) -- wrap those function bodies in try/catch and translate to XrResult.
if(MINGW)
  target_link_options(vr_agent_layer PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

target_compile_definitions(vr_agent_layer PRIVATE VR_AGENT_LAYER_EXPORTS)

set_target_properties(vr_agent_layer PROPERTIES
  OUTPUT_NAME "vr_agent_layer"
  PREFIX ""
)
