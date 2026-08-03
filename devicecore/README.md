# devicecore — PlaySpectra Virtual Device Core

The runtime-neutral implementation of PlaySpectra's virtual HMD and Touch controllers:

| File | Role |
| --- | --- |
| `playspectra_proto.{c,h}` | NDJSON protocol parsing, frame-sync verdicts, content signatures (spec §2/§4) |
| `playspectra_state.{c,h}` | Shared, refcounted `VirtualDeviceState` (poses, inputs, haptics queue) |
| `playspectra_control.{c,h}` | TCP control channel (`hello`/`set_state`/`get_state`/`status`/`reset`, spec §5) |
| `ps_os.{c,h}` | Minimal Win32/pthread mutex+thread shim |
| `playspectra_proto_test.c` | Standalone protocol test (43 cases; built by `scripts/run_all_tests.sh`) |

This code knows nothing about any runtime: no Monado, SteamVR, or OpenXR types. Poses are plain
`playspectra_pose` (double arrays); runtime-specific values (port, runtime name, descriptor,
logging) are injected by the embedding adapter via `playspectra_control_config`.

Each Runtime Adapter compiles these sources into its runtime's process, because runtimes query
device state through synchronous in-process callbacks (`get_tracked_pose` etc.) and accept no
out-of-process devices. The Monado fork pulls them in via `PLAYSPECTRA_DEVICECORE_DIR`
(`runtime/monado-playspectra/src/xrt/drivers/CMakeLists.txt`); the SteamVR driver will link the
same sources. The wire protocol on `127.0.0.1:52702` (Monado) is the only contract a client —
the Go `playspectra` control plane — depends on.

Protocol specification: [docs/device-core-spec.md](../docs/device-core-spec.md).
Architecture context: [docs/architecture.md](../docs/architecture.md).
