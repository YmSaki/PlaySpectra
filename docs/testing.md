# Testing

## Local gate

Run the environment-independent suites with:

~~~bash
bash scripts/run_all_tests.sh
~~~

The script currently covers:

| Suite | What it covers | Recorded suite size |
| --- | --- | --- |
| Go control plane | Core, protocol, CLI, Scenario, MCP, recording, probes, setup helpers | 377 tests/subtests |
| layer/ | CTest host helpers | 92 on Windows, 83 on non-Windows |
| Monado submodule | Standalone PlaySpectra protocol parser/content-signature tests | 43 |
| Total | Dependency-complete local gate | 512 on Windows, 503 on non-Windows |

These are suite counts, not a promise that every environment has all dependencies installed. The script reports a missing Go toolchain, GCC, submodule, or other prerequisite as a named SKIP. A failure makes the exit code non-zero; SKIP does not. The final line distinguishes ALL GREEN from GREEN WITH SKIPS.

The local gate does not require a physical HMD, live Monado service, or running application. The layer and submodule builds may fetch dependencies when their build trees are not already populated.

Run the gate from one host environment at a time. Do not reuse a Windows layer build tree from WSL or the reverse because CMake caches absolute paths.

## Individual commands

~~~bash
# Control-channel tests: require a live Monado adapter on :52702.
playspectra verify multiobs
playspectra verify frame
playspectra verify reset

# Core, recorder, and MCP checks.
playspectra verify server
playspectra verify record
playspectra verify mcp

# Layer integration paths on Windows with Meta XR Simulator.
scripts/integration_test.sh Vulkan
scripts/integration_test.sh D3D11
scripts/integration_test.sh D3D12
PLAYSPECTRA_DISABLE_CA=1 scripts/integration_test.sh Vulkan
VR_RUNTIME=monado scripts/integration_test.sh Vulkan

# Layer host unit tests.
ctest --test-dir layer/build -E loader_test --output-on-failure
~~~

The MCP check needs a live Monado/app stack for screenshot coverage. The integration scripts need their named runtime and graphics path. The host-unit-test command does not need the live runtime.

## CI

.github/workflows/ci.yml runs these jobs on push and pull request:

- go-tests on ubuntu-latest and windows-latest: unit/integration tests, vet, and a cgo-free executable build.
- layer-tests on windows-latest: configure layer tests, build playspectra_test, and run CTest while excluding the fetched OpenXR loader_test.
- layer-build on ubuntu-latest and windows-latest: build the shippable OpenXR layer binary.
- monado-build on ubuntu-latest and windows-latest: build and stage-install the runtime and device core.

Live E2E is not part of this workflow. The Windows E2E harnesses are scripts/run_hello_xr_monado.sh, scripts/run_scenario_e2e_monado.sh, and scripts/run_mcp_verify_monado.sh.

## Verification discipline

Do not turn a passing unit suite into a claim that a runtime or engine is supported. The [verification matrix](verification.md) records the environment, evidence, and boundary for each claim.
