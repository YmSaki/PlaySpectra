cmake_minimum_required(VERSION 3.21)
project(playspectra_layer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Our sources carry UTF-8 (Japanese) comments. Without /utf-8 MSVC decodes them as the system ANSI
# code page -- CP1252 on CI runners, CP932 on JA dev boxes -- which raises C4819 and can cascade into
# phantom parse errors. /utf-8 makes the build locale-independent (same fix the Monado submodule got
# in cdd429f88). Harmless for the ASCII/UTF-8-clean fetched dependencies below.
if(MSVC)
  add_compile_options(/utf-8)
endif()

include(FetchContent)

# OpenXR-SDK-Source include(CTest)s, which would otherwise cache BUILD_TESTING=ON and register the
# SDK's own loader_test into our ctest run (it is never built here, so it reports Not Run). Seed the
# cache OFF first so that option() is a no-op there. Our tests have their own knob:
# PLAYSPECTRA_BUILD_TESTS, below.
set(BUILD_TESTING OFF CACHE BOOL "Build the fetched dependencies' testing trees")

# OpenXR-SDK: headers only (the layer talks to the loader via xrNegotiateLoaderApiLayerInterface,
# it does not link against openxr_loader). Also provides hello_xr + openxr_loader used for bring-up,
# via the same fetched source tree.
FetchContent_Declare(
  openxr_sdk
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK-Source.git
  GIT_TAG        release-1.1.42
)
FetchContent_MakeAvailable(openxr_sdk)

FetchContent_Declare(
  lodepng
  GIT_REPOSITORY https://github.com/lvandeve/lodepng.git
  GIT_TAG        ed6fe5825c6a4fbb7f58ab35a4231c7543cd452a
  SOURCE_SUBDIR  _none
)
FetchContent_MakeAvailable(lodepng)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Vulkan-Headers: headers only, for capture_vulkan.cpp (<vulkan/vulkan.h>) on every platform --
# capture.cpp dispatches to the Vulkan backend on Windows too. SOURCE_SUBDIR _none skips the
# project's own CMake (same trick as lodepng above); we add the include dir by hand. The pin
# matches the tree previously expected at the untracked third_party/Vulkan-Headers/, which made
# fresh clones unbuildable because nothing fetched it.
FetchContent_Declare(
  vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        v1.3.280
  SOURCE_SUBDIR  _none
)
FetchContent_MakeAvailable(vulkan_headers)

add_library(playspectra_layer SHARED
  src/layer_entry.cpp
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
  src/layer_state.cpp
  src/capture.cpp
  src/capture_common.cpp
  src/pixel_convert.cpp
  src/capture_vulkan.cpp
  ${lodepng_SOURCE_DIR}/lodepng.cpp
)

# D3D11/D3D12 color-capture backends are Windows-only (COM/DXGI). On other platforms only the Vulkan
# backend is built; capture.cpp #ifdef _WIN32-guards its D3D dispatch to match (core completeness is
# per-platform: a Linux runtime can only ever hand the app a Vulkan binding).
if(WIN32)
  target_sources(playspectra_layer PRIVATE src/capture_d3d11.cpp src/capture_d3d12.cpp)
endif()

target_include_directories(playspectra_layer PRIVATE
  ${lodepng_SOURCE_DIR}
  ${vulkan_headers_SOURCE_DIR}/include
)

target_link_libraries(playspectra_layer PRIVATE
  OpenXR::headers
  nlohmann_json::nlohmann_json
)

if(WIN32)
  # D3D capture backends + winsock (control channel). POSIX sockets need no extra lib.
  target_link_libraries(playspectra_layer PRIVATE d3d11 d3d12 dxgi dxguid ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(playspectra_layer PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Exceptions must never cross the exported C ABI (xrNegotiateLoaderApiLayerInterface and the
# hooked xr* functions) -- wrap those function bodies in try/catch and translate to XrResult.
if(MINGW)
  target_link_options(playspectra_layer PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

target_compile_definitions(playspectra_layer PRIVATE PLAYSPECTRA_LAYER_EXPORTS)

set_target_properties(playspectra_layer PROPERTIES
  OUTPUT_NAME "playspectra_layer"
  PREFIX ""
)

# The manifest (manifest/XrApiLayer_playspectra.json) points the OpenXR loader at
# ./playspectra_layer.dll NEXT TO the manifest -- keep that copy in sync with every build, otherwise
# integration runs silently load a stale layer (a green build can otherwise test a stale DLL).
add_custom_command(TARGET playspectra_layer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:playspectra_layer>
          ${CMAKE_CURRENT_SOURCE_DIR}/manifest/$<TARGET_FILE_NAME:playspectra_layer>
  COMMENT "Syncing the layer binary next to the loader manifest"
)

# --- Host-run unit tests (opt in with -DPLAYSPECTRA_BUILD_TESTS=ON) ---
# Off by default so a plain `cmake ..` never network-fetches googletest. Kept separate from
# BUILD_TESTING (see the seed near the top) so enabling ours does not also drag in the SDK's tests.
option(PLAYSPECTRA_BUILD_TESTS "Build the host-run unit tests" OFF)
if(PLAYSPECTRA_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(playspectra_test
    tests/test_action_helpers.cpp
    tests/test_capture_common.cpp
    tests/test_pixel_convert.cpp
    tests/test_pose_animator.cpp
    tests/test_xr_math.cpp
    tests/test_pose_override_pure.cpp
    # Tests require the source files they are testing.
    src/capture_common.cpp
    src/pixel_convert.cpp
    src/pose_animator.cpp
    ${lodepng_SOURCE_DIR}/lodepng.cpp
  )
  # DXGI format helpers need dxgiformat.h (Windows-only). Matches the layer's per-platform D3D guard:
  # a non-Windows runtime only ever hands a Vulkan binding, so there is no DXGI classification there.
  if(WIN32)
    target_sources(playspectra_test PRIVATE tests/test_dxgi_formats.cpp)
  endif()

  target_include_directories(playspectra_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${lodepng_SOURCE_DIR}
  )

  target_link_libraries(playspectra_test PRIVATE
    gtest_main
    nlohmann_json::nlohmann_json
    OpenXR::headers
  )

  if(MINGW)
    target_link_options(playspectra_test PRIVATE -static-libgcc -static-libstdc++ -static)
  endif()

  include(GoogleTest)
  gtest_discover_tests(playspectra_test)
endif()
