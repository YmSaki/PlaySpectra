# Roadmap and project status

This page separates current interfaces from work that is planned or not yet verified.

## Current

- Monado Runtime Adapter: working virtual HMD and left/right controller path with control channel on :52702.
- PlaySpectra Core: cgo-free Go implementation for high-level operations, interpolation, state observation, scenario execution, and assertions.
- OpenXR Instrumentation Layer: current screenshot, recording, action discovery, diagnostics, and test-only override path on :52700.
- JSON Scenario Runner: current state and visual assertion path.
- Recorder and Replayer: current device-state trajectory path.
- Go stdio MCP server: current MCP interface, built into the `playspectra` executable.
- Native hello_xr: verified OpenXR application target.
- Godot 4.7 VRAppDummyGame: verified real-engine target in a separate repository.

## Planned or not yet verified

- SteamVR Adapter: separately prove external-driver registration, HMD-present preflight, and physical-HMD-free runtime startup; then embed the shared device core (`devicecore/`), add PlaySpectra-owned controller bindings and bidirectional haptics, and verify the native OpenXR full loop. Keep identity/class/body-role separate so a later protocol revision can add 0..N controllers and generic trackers; track native OpenVR operation and rendered-frame observation separately.
- Unity application E2E: not yet verified.
- Unreal application E2E: not yet verified.
- Physical-HMD display compositor: not yet verified; current Windows E2E uses XRT_COMPOSITOR_NULL=1.
- Application-level deterministic frame timing: not yet verified.
- Broader engine/application coverage: depends on runtime-level evidence rather than design intent alone.

## Transition items

- VRDevApp: historical verification target; current engine evidence uses VRAppDummyGame instead.
- OpenVR through OpenComposite: partially verified as an integration path, but this does not mean that the planned SteamVR Adapter is complete.

A status change should update docs/verification.md with a new reproducible command or primary evidence, not only change a label in this page.

