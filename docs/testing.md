# Testing

## Local gate

Run the environment-independent suites with:

~~~bash
bash scripts/run_all_tests.sh
~~~

The script currently covers:

| Suite | What it covers | Recorded suite size |
| --- | --- | --- |
| mcp/ | TypeScript node:test math suite | 42 |
| layer/ | CTest host helpers | 92 on Windows, 83 on non-Windows |
| tools/ | Python math, wait_for, and capture-assert tests | 33 |
| Monado submodule | Standalone PlaySpectra protocol parser/content-signature tests | 43 |
| Total | Dependency-complete local gate | 210 on Windows, 201 on non-Windows |

These are suite counts, not a promise that every environment has all dependencies installed. The script reports a missing Python, GCC, submodule, or other prerequisite as a named SKIP. A failure makes the exit code non-zero; SKIP does not. The final line distinguishes ALL GREEN from GREEN WITH SKIPS.

The local gate does not require a physical HMD, live Monado service, or running application. The layer and submodule builds may fetch dependencies when their build trees are not already populated.

Run the gate from one host environment at a time. Do not reuse a Windows layer/build or mcp/node_modules tree from WSL (or the reverse): CMake caches absolute paths and esbuild installs a platform-specific binary.

## Individual commands

~~~bash
# Control-channel tests: require a live Monado adapter on :52702.
python3 tools/playspectra_multiobs_test.py
python3 tools/playspectra_frame_test.py
python3 tools/playspectra_reset_test.py

# Server, recorder, and MCP checks.
python3 tools/playspectra_server.py --verify
python3 tools/playspectra_record.py --verify
python3 tools/playspectra_mcp_verify.py

# Layer integration paths on Windows with Meta XR Simulator.
scripts/integration_test.sh Vulkan
scripts/integration_test.sh D3D11
scripts/integration_test.sh D3D12
PLAYSPECTRA_DISABLE_CA=1 scripts/integration_test.sh Vulkan
VR_RUNTIME=monado scripts/integration_test.sh Vulkan

# Layer host unit tests.
ctest --test-dir layer/build -E loader_test --output-on-failure
~~~

The MCP check needs the mcp package from tools/requirements.txt and a live Monado/app stack for screenshot coverage. The integration scripts need their named runtime and graphics path. The host-unit-test command does not need the live runtime.

## CI

.github/workflows/ci.yml runs two jobs on push and pull request:

- mcp-tests on ubuntu-latest: npm ci, TypeScript build, and the node:test suite.
- layer-tests on windows-latest: configure layer tests, build playspectra_test, and run CTest while excluding the fetched OpenXR loader_test.

Live Monado, graphics, and real-application E2E are intentionally not hosted in this workflow because they require a running runtime, app, and (for the Windows path) a real GPU. The Windows E2E harnesses are scripts/run_hello_xr_monado.sh, scripts/run_scenario_e2e_monado.sh, and scripts/run_mcp_verify_monado.sh.

## Verification discipline

Do not turn a passing unit suite into a claim that a runtime or engine is supported. The [verification matrix](verification.md) records the environment, evidence, and boundary for each claim.
