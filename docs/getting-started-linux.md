# Getting started on Linux / WSL2

This guide covers the Linux execution path for both native Ubuntu 22.04 and WSL2. It targets a headless Monado run with software Vulkan by default, while a hardware Vulkan ICD can be selected separately. Linux is a distinct platform path from native Windows, which is covered in [Getting started on Windows](getting-started-windows.md). A physical HMD is not required.

## Prerequisites

Install Git, Go 1.22+, Python 3 for the native Monado source build, CMake, Ninja, Go Task, and `build-essential` (including GCC/G++), plus the Ubuntu packages needed by Monado. The repository includes the one-shot Task entry point:

~~~bash
task bootstrap:linux
~~~

That task runs setup:linux-deps, initializes the Monado submodule recursively, runs the standalone protocol test, configures Monado with Ninja, and builds it into build/monado.

On WSL2, use the GCC/G++ installed inside the Linux distribution. Do not substitute a Windows MSYS2 compiler for the Linux build.

If Task is not available, the package list used by the task is:

~~~bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config glslang-tools \
  libvulkan-dev libeigen3-dev libcjson-dev \
  libx11-dev libx11-xcb-dev libxcb-randr0-dev libxrandr-dev libxxf86vm-dev \
  libgl1-mesa-dev libegl1-mesa-dev libglvnd-dev libwayland-dev wayland-protocols \
  libudev-dev libusb-1.0-0-dev libsdl2-dev libbsd-dev \
  libopenxr-dev libopenxr-loader1 mesa-vulkan-drivers
git submodule update --init --recursive
cmake -S runtime/monado-playspectra -B build/monado -G Ninja
cmake --build build/monado --parallel
~~~

## Build the layer and hello_xr

The layer CMake project fetches the pinned OpenXR SDK, lodepng, and nlohmann/json. Its default target builds the layer and the SDK hello_xr target used by the E2E script:

~~~bash
cmake -S layer -B layer/build -G Ninja
cmake --build layer/build --parallel
~~~

The layer output is layer/build/playspectra_layer.so. The build also synchronizes a copy beside the loader manifest in layer/manifest/.

## Run the complete headless loop

Use explicit paths so the script uses the builds made in this checkout:

~~~bash
MONADO_BUILD="$PWD/build/monado" HELLOXR="$PWD/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr" LAYER_SO="$PWD/layer/build/playspectra_layer.so" bash scripts/e2e_playwright_loop.sh
~~~

The script:

1. Selects the lavapipe ICD at /usr/share/vulkan/icd.d/lvp_icd.x86_64.json by default.
2. Sets the Monado runtime manifest, PlaySpectra layer manifest, and capture directory.
3. Starts hello_xr with Vulkan2.
4. Waits for the operation channel on :52702.
5. Captures a baseline, runs tools/scenarios/big_view_change.json through the Server, and captures a post-injection frame.
6. Exits non-zero unless the captured contents contain at least two distinct frames and the post-injection frame differs from the baseline.

Override VK_ICD_FILENAMES, WORK, MONADO_BUILD, HELLOXR, or LAYER_SO when using different paths. The script is Linux/WSL2-oriented; the Windows counterpart is scripts/run_hello_xr_monado.sh.

## Use the interfaces directly

With a live adapter on :52702:

~~~bash
go build -o playspectra ./cmd/playspectra
./playspectra verify server --port 52702
./playspectra verify record --port 52702
./playspectra run tools/scenarios/assert_demo.json --port 52702
~~~

For visual assertions, load the layer into the app and add the capture channel:

~~~bash
./playspectra run tools/scenarios/capture_assert_demo.json --port 52702 --capture-port 52700
~~~

The scenario and MCP formats are documented in [Scenario format](scenario-format.md) and [MCP tools](mcp-tools.md).

## Linux-specific boundaries

- The Linux layer build contains the Vulkan capture backend. D3D11 and D3D12 capture sources are Windows-only.
- The recommended E2E uses lavapipe or another Vulkan ICD and the Monado headless/null compositor path.
- A successful headless run proves rendering and capture without a display; it does not prove presentation to a physical HMD.
