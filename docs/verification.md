# Verification matrix

This document contains the detailed evidence that is intentionally not part of the README's first-run path. Status labels are:

- **Verified**: implementation and a repository-backed test or E2E result exist for the stated scope.
- **Partially verified**: some environments or integration surfaces work, but the whole target is not established.
- **Planned**: design or an earlier skeleton exists; current implementation/E2E is not complete.
- **Not yet verified**: the interface may exist by design or in code, but no supported application result is claimed.

A status is always scoped. For example, “D3D12 verified” means the D3D12 capture path passed in the recorded Windows E2E; it does not mean Unity or Unreal has been verified.

## Support matrix

| Area | Status | Evidence and boundary |
| --- | --- | --- |
| Monado Adapter: virtual HMD and left/right controllers | **Verified** | Monado enumerates head/left/right devices, an OpenXR app reads poses, and set_state changes pose/input. Sources: runtime/monado-playspectra/src/xrt/drivers/playspectra/ and tools/playspectra_headless_probe.c, tools/playspectra_action_probe.c. |
| Monado control channel on :52702 | **Verified** | set_state, get_state, haptics broadcast, multiple observers, writer exclusion, frame_synchronized, and reset are implemented. The recorded Windows runs include frame 10/10, reset 20/20, and the multi-observer core. Sources: `playspectra verify frame`, `verify reset`, and `verify multiobs`. |
| PlaySpectra Core | **Verified** | move_head, look, walk_forward, strafe, trigger, press, set_input, move_controller, reset, get_state, wait_for, scenario execution, and capture assertions are implemented. The self-checking live run records 9/9. Source: `playspectra verify server`. |
| Recorder and Replayer | **Verified** | An observer records timestamped state frames and a writer replays them with fresh monotonic sequence values. The recorded live Windows Monado verification is 5/5. Source: `playspectra verify record`. |
| JSON Scenario Runner and state assert | **Verified** | assert compares get_state paths and exits non-zero on failure; negative-control failures are part of the verification evidence. Source: tools/scenarios/assert_demo.json. |
| Visual capture-assert | **Verified** | capture stores a PNG reference hash and assert_capture checks stable or changed content. The recorded scenario result is 3/3, including a negative-control failure. Source: tools/scenarios/capture_assert_demo.json. |
| Vulkan capture | **Verified** | Windows real-GPU full E2E recorded 20/20; Linux/WSL2 production Vulkan readback recorded 394 PNG frames. Sources: scripts/integration_test.sh Vulkan and scripts/e2e_playwright_loop.sh. |
| D3D11 capture | **Verified** | Windows real-GPU full E2E recorded 20/20, including screenshot and recording paths. Source: scripts/integration_test.sh D3D11. |
| D3D12 capture | **Verified** | Windows real-GPU full E2E recorded 20/20, including a 395-frame capture path. Source: scripts/integration_test.sh D3D12. |
| Graphics API format coverage | **Verified** | D3D11, D3D12, and Vulkan are treated as core capture paths; unsupported formats return an explicit error rather than a silent blank image. Depth capture remains optional. |
| Go MCP server | **Verified** | The stdio frontend wraps the shared Core: operation/state uses :52702 and capture uses :52700. The recorded live Windows Monado verifier exercised all 13 tools and reported 14/14 checks. Sources: `mcp/`, `playspectra verify mcp`, and scripts/run_mcp_verify_monado.sh. |
| End-to-end operate -> render -> observe loop | **Verified** | Windows real-GPU capture-assert path recorded 3/3; WSL2 path recorded 2/2. Sources: scripts/run_scenario_e2e_monado.sh and scripts/e2e_playwright_loop.sh. |
| Native OpenXR application: hello_xr on Windows Monado | **Verified** | Headless null-compositor E2E reached Monado through client/service IPC and covered D3D11, D3D12, and Vulkan. The recorded runs report 20/20 per API plus 2/2 runtime coupling. Source: scripts/run_hello_xr_monado.sh. |
| Godot 4.7 application: VRAppDummyGame | **Verified** | The separate application self-reports pose/input and game-logic events over its VRTEST contract. The current Go harness is *reported* as 25/25 on Windows Monado, including capture and pacing, but no execution log for that run is stored here, so treat the figure as unverified until scripts/run_vrapp_monado.sh is re-run. The executable is not part of this repository; use PLAYSPECTRA_VRAPP_EXE or the default sibling path. Sources: vrapp/ and scripts/run_vrapp_monado.sh. |
| Runtime-level operate coupling | **Verified** | A set_state sent through :52702 reaches a live app's xrLocateViews without relying on layer pose override. Source: `playspectra verify coupling`. |
| Headless capture resolution | **Verified** | The headless path uses the resolution declared by the virtual HMD; the recorded hello_xr and VRAppDummyGame paths use 1080x1200 per eye. Source: runtime/monado-playspectra/src/xrt/compositor/null/null_compositor.c and the Windows E2E harnesses. |
| OpenVR application through OpenComposite | **Partially verified** | The recorded integration has 15 PASS and 1 SKIP for the OpenVR-to-OpenXR path. This is not SteamVR Adapter completion. Source: scripts/integration_openvr_test.sh. |
| SteamVR Adapter | **Planned** | driver/ contains an earlier skeleton. Reconnection to the current core and Windows verification remain. Source: .claude/steamvr-driver-plan.md. |
| Windows physical-HMD display compositor | **Not yet verified** | The recorded application E2E uses XRT_COMPOSITOR_NULL=1. It proves headless rendering/capture, not presentation to a physical headset. |
| Unity application | **Not yet verified** | The OpenXR-level design is intended to be engine-independent, but no Unity application E2E result is claimed. |
| Unreal Engine application | **Not yet verified** | No Unreal application E2E result is claimed. |
| VRDevApp historical target | **Partially verified** | Earlier Meta XR Simulator verification covered session, D3D12 capture, and movement/rotation, but bin/VRDevApp.exe is not part of a fresh clone and the current engine target is VRAppDummyGame. |
| Frame-synchronized determinism | **Not yet verified** | The protocol path is tested, but full application determinism across delta time, GPU scheduling, physics, predicted display time, and dropped frames is not established. |
| Legacy TypeScript MCP | **Planned** | The TypeScript source remains for compatibility testing but is not part of the canonical build; retirement is still planned. The Go MCP server is current. |

## Dated execution records retained from the previous README

| Date | Execution | Result recorded in the repository |
| --- | --- | --- |
| 2026-07-22 | Windows Monado control, Server, recorder, capture, MCP, and hello_xr E2E | frame 10/10, reset 20/20, multi-observer core, Server 9/9, recorder 5/5, MCP 14/14, and hello_xr D3D11/D3D12/Vulkan 20/20 per API plus coupling 2/2. |
| 2026-07-22 | Linux/WSL2 Vulkan layer path | Production Vulkan readback produced 394 PNG frames; the headless operate -> render -> observe loop was recorded as passing. |
| 2026-07-24 | TypeScript MCP status decision | Legacy implementation classified for retirement. |
| 2026-07-25 | Windows Monado with Godot 4.7 VRAppDummyGame | Real-engine E2E recorded as 24/24, including application self-reported poses, inputs, capture, and interactions. |
| 2026-07-31 | Go control plane with Windows Monado, layer, and VRAppDummyGame | Reported as 25/25, including pose, input, rendered capture, 89.7 FPS pacing, and game interactions. **Unverified**: no execution log backing this run is stored in the repository, so the 25/25 and the 89.7 FPS pacing are transcribed claims rather than evidence. Re-run scripts/run_vrapp_monado.sh to establish them. |

## Recorded environments and boundaries

- WSL2 Ubuntu 22.04 is the software-Vulkan headless path. The recorded run forces the lavapipe ICD; no physical GPU or HMD is required for that path.
- Windows real-GPU E2E uses a Windows Monado build, a headless/null compositor, and a per-process XR_RUNTIME_JSON. It does not change the system ActiveRuntime.
- D3D11 and D3D12 capture backends are built only on Windows. Linux builds include the Vulkan capture backend.
- The current Godot target is an external repository. A missing export is an explicit SKIP in the harness, never a silent pass.
- The earlier VRDevApp result is historical/partial. It is not the current real-engine target; current evidence uses VRAppDummyGame.
- A frame-synchronized control path is not described as deterministic until application-level timing evidence exists.

## Test counts and negative controls

The local aggregate script records these dependency-complete suite sizes:

- mcp/: 42 node:test cases.
- layer/: 92 Windows or 83 non-Windows CTest cases; the DXGI-specific cases are Windows-only.
- Go control plane: 377 tests/subtests across Core, protocol, CLI, Scenario, MCP, recording, probes, and setup helpers.
- Monado submodule protocol suite: 43 standalone C cases.
- Total: 554 on Windows or 545 on non-Windows.

The verification set deliberately includes negative controls: a failed state assertion returns non-zero, capture-assert distinguishes stable from changed images, and the Godot harness reports absent application/capture prerequisites as explicit SKIPs. These details are retained here so README status labels remain auditable without making the README a test journal.

## CI boundary

The current workflow is .github/workflows/ci.yml. It runs the cgo-free Go suite on Windows and Linux, the environment-independent legacy MCP node tests on Ubuntu, and layer host tests on Windows for pushes and pull requests. It does not run Monado/real-app/graphics E2E because those jobs need a live runtime, an application, and (for the Windows path) a real GPU.

The repository is public and the default branch is master. Current GitHub Actions status is represented by the README badge and the workflow page; dated run IDs are intentionally not part of the README.

## Reproduction entry points

- WSL2/Linux full loop: scripts/e2e_playwright_loop.sh.
- Windows native OpenXR loop: scripts/run_hello_xr_monado.sh.
- Windows scenario visual loop: scripts/run_scenario_e2e_monado.sh.
- Windows MCP loop: scripts/run_mcp_verify_monado.sh.
- Local unit gate: scripts/run_all_tests.sh.
