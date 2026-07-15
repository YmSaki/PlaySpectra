cmake_minimum_required(VERSION 3.21)
project(vr_agent_layer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# OpenXR-SDK-Source include(CTest)s, which would otherwise cache BUILD_TESTING=ON and register the
# SDK's own loader_test into our ctest run (it is never built here, so it reports Not Run). Seed the
# cache OFF first so that option() is a no-op there. Our tests have their own knob:
# VR_AGENT_BUILD_TESTS, below.
set(BUILD_TESTING OFF CACHE BOOL "Build the fetched dependencies' testing trees")

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
  src/hooks_action.cpp
  src/hooks_capture.cpp
  src/hooks_locate.cpp
  src/layer_log.cpp
  src/layer_dispatch.cpp
  src/action_registry.cpp
  src/pose_animator.cpp
  src/pose_override.cpp
  src/input_inject.cpp
  src/control_channel.cpp
  src/capture.cpp
  src/capture_common.cpp
  src/pixel_convert.cpp
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

# The manifest (manifest/XrApiLayer_vr_agent.json) points the OpenXR loader at
# ./vr_agent_layer.dll NEXT TO the manifest -- keep that copy in sync with every build, otherwise
# integration runs silently load a stale layer (this bit us: a green build, an old DLL under test).
add_custom_command(TARGET vr_agent_layer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:vr_agent_layer>
          ${CMAKE_CURRENT_SOURCE_DIR}/manifest/vr_agent_layer.dll
  COMMENT "Syncing vr_agent_layer.dll next to the loader manifest"
)

# --- Host-run unit tests (opt in with -DVR_AGENT_BUILD_TESTS=ON) ---
# Off by default so a plain `cmake ..` never network-fetches googletest. Kept separate from
# BUILD_TESTING (see the seed near the top) so enabling ours does not also drag in the SDK's tests.
option(VR_AGENT_BUILD_TESTS "Build the host-run unit tests" OFF)
if(VR_AGENT_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(vr_agent_test
    tests/test_capture_common.cpp
    tests/test_pose_animator.cpp
    # Tests require the source files they are testing.
    src/capture_common.cpp
    src/pose_animator.cpp
    ${lodepng_SOURCE_DIR}/lodepng.cpp
  )

  target_include_directories(vr_agent_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${lodepng_SOURCE_DIR}
  )

  target_link_libraries(vr_agent_test PRIVATE
    gtest_main
    nlohmann_json::nlohmann_json
    OpenXR::headers
  )

  if(MINGW)
    target_link_options(vr_agent_test PRIVATE -static-libgcc -static-libstdc++ -static)
  endif()

  include(GoogleTest)
  gtest_discover_tests(vr_agent_test)
endif()
