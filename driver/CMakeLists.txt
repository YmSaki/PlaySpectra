# SteamVR virtual controller driver (driver_playspectra). Standalone build, mirrors layer/ MinGW flow:
#   cmake -S driver -B driver/build -G Ninja && cmake --build driver/build
# Output DLL is synced into the driver package dir (driver/playspectra/bin/win64/) POST_BUILD, which is
# what `vrpathreg adddriver <repo>/driver/playspectra` points SteamVR at.
cmake_minimum_required(VERSION 3.16)
project(playspectra_driver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# nlohmann_json: same pin + fetch shape as layer/CMakeLists.txt (NDJSON control channel).
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(driver_playspectra SHARED src/driver_playspectra.cpp)

# openvr_driver.h is header-only for drivers (no import lib): vrserver resolves the interfaces at
# runtime through the factory. v1.8.19 pinned (matches the project-wide openvr pin).
target_include_directories(driver_playspectra PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openvr_driver_sdk)
target_link_libraries(driver_playspectra PRIVATE nlohmann_json::nlohmann_json ws2_32)

# SteamVR expects <package>/bin/win64/driver_<name>.dll with no "lib" prefix.
set_target_properties(driver_playspectra PROPERTIES PREFIX "")

add_custom_command(TARGET driver_playspectra POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/playspectra/bin/win64
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:driver_playspectra>
        ${CMAKE_CURRENT_SOURCE_DIR}/playspectra/bin/win64/driver_playspectra.dll
    COMMENT "Syncing driver_playspectra.dll into the SteamVR driver package")
