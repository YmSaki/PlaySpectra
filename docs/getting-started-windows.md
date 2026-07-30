# Getting started on Windows

This guide covers the currently verified Windows path: a Windows-built Monado service, the PlaySpectra OpenXR layer, and hello_xr running headless on a real GPU. Run the shell commands from Git Bash.

## Prerequisites

Install:

- Visual Studio 2022 with the C++ workload and MSVC.
- CMake and Ninja.
- A Vulkan SDK with glslang, for example under C:/VulkanSDK.
- Git Bash and Python 3.
- The repository submodule.

~~~bash
git submodule update --init --recursive
~~~

The Monado fork uses its vcpkg manifest. The tested build uses the vcpkg toolchain bundled with Visual Studio:

~~~bash
export VULKAN_SDK="/c/VulkanSDK/1.4.350.0"
export PATH="/c/VulkanSDK/1.4.350.0/bin:$PATH"
TC="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
BUILD="runtime/monado-playspectra/build-win"

cmake -S runtime/monado-playspectra -B "$BUILD" -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$TC" -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON \
  -DXRT_BUILD_DRIVER_PLAYSPECTRA=ON -DXRT_MODULE_MONADO_CLI=ON
cmake --build "$BUILD" --config Release --target monado-service cli openxr_monado
~~~

The build must include monado-service, cli, and openxr_monado. The last target produces the OpenXR client runtime used by an application and the Release runtime manifest. The expected manifest is runtime/monado-playspectra/build-win/Release/openxr_monado-dev.json.

## Build the layer and hello_xr

Configure and build the layer first. This also fetches the OpenXR SDK source used by the hello_xr setup script:

~~~bash
cmake -S layer -B layer/build -DPLAYSPECTRA_BUILD_TESTS=ON
cmake --build layer/build --config Release
~~~

Build the MSVC hello_xr with its D3D plugins:

~~~bash
bash scripts/setup_helloxr_msvc.sh
~~~

The resulting executable is third_party/hello_xr_msvc/hello_xr.exe. The layer build also provides a MinGW hello_xr for Vulkan at layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe.

## Run a verified headless E2E

The shared harness sets the per-process runtime and layer environment, starts monado-service on :52702, launches hello_xr with the layer on :52700, and tears both down:

~~~bash
scripts/run_hello_xr_monado.sh D3D11
~~~

Use D3D12 or Vulkan for the other graphics paths. Run all three in sequence with:

~~~bash
scripts/run_hello_xr_monado.sh all
~~~

The harness uses XRT_COMPOSITOR_NULL=1. It verifies rendered frames and runtime coupling, but it does not exercise the physical-HMD display compositor.

## Other verified harnesses

~~~bash
scripts/run_scenario_e2e_monado.sh D3D11
scripts/run_mcp_verify_monado.sh D3D11
~~~

The MCP harness needs the MCP Python SDK in an isolated environment:

~~~bash
python -m venv .venv-mcp
.venv-mcp/Scripts/python.exe -m pip install -r tools/requirements.txt
PY=.venv-mcp/Scripts/python.exe scripts/run_mcp_verify_monado.sh D3D11
~~~

VRAppDummyGame is a separate Godot 4.7 repository. Export its console wrapper, then set PLAYSPECTRA_VRAPP_EXE or place it at the default sibling path before running:

~~~bash
Godot_v4.7.1-stable_mono_win64_console.exe --headless --path <VRAppDummyGame> --export-release "Windows Desktop" build/vrapp.exe
scripts/run_vrapp_monado.sh
~~~

## Windows-specific notes

- D3D11 and D3D12 use the MSVC hello_xr build; Vulkan uses the layer-bundled MinGW build.
- The harness sets XR_RUNTIME_JSON per process and does not change the system ActiveRuntime.
- :52702 is the Monado operation/state channel; :52700 is the layer capture channel.
- If the Monado client and service were built from different commits, the harness sets IPC_IGNORE_VERSION=1. Rebuilding both from the same submodule commit is the cleaner option.

