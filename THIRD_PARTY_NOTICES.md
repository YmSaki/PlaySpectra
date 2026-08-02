# Third-party notices

PlaySpectra itself is licensed under the Mozilla Public License 2.0 (see [LICENSE](LICENSE)).
This file records the third-party components that PlaySpectra combines into its binaries, ships
alongside them, or uses to build them.

Binary release packaging is not implemented yet. The first two sections describe what is compiled
today. The Monado runtime bundle section describes the intended contents of a future release
package; the last two record what is only used to build PlaySpectra, or deliberately left out of it.

## Go control plane (`playspectra` executable)

The executable has no third-party dependencies: `go.mod` declares no module requirements, and the
CI and test builds set `CGO_ENABLED=0`. It contains only first-party Go source and the Go standard
library (BSD-3-Clause, https://go.dev/LICENSE).

## OpenXR instrumentation layer (`playspectra_layer.dll` / `.so`)

Compiled into the layer binary:

| Component | Version | License |
| --- | --- | --- |
| lodepng | `ed6fe5825c6a4fbb7f58ab35a4231c7543cd452a` | zlib |
| nlohmann/json | v3.11.3 | MIT |
| OpenXR-SDK-Source (headers) | release-1.1.42 | Apache-2.0 |
| Vulkan-Headers | v1.3.280 | Apache-2.0 |

Revisions are pinned in `layer/CMakeLists.txt`.

## Monado runtime bundle

Intended for redistribution as binaries in a release package:

| Component | License |
| --- | --- |
| Monado (PlaySpectra fork) | predominantly BSL-1.0, with files under Apache-2.0, BSD-3-Clause, MIT, CC0-1.0 and others |
| Microsoft WIL | MIT |
| Eigen | MPL-2.0, with BSD-3-Clause and LGPL-licensed parts, per its `COPYING.README` |
| cJSON (`cjson.dll`) | MIT |
| pthreads4w (`pthreadVCE3.dll`) | Apache-2.0 |
| Vulkan-Loader (`vulkan-1.dll`) | Apache-2.0, with a few exceptions noted in its own copyright file |

WIL and Eigen are header-only and therefore compiled into the Monado binaries rather than shipped
separately: WIL through the D3D11/D3D12 path in `src/xrt/auxiliary/d3d/`, Eigen through the
auxiliary math code in `src/xrt/auxiliary/math/`.

The DLL list assumes the documented Windows build, which passes
`-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`. Enabling the vcpkg manifest's default `usb` and `gui`
features additionally pulls in libusb (LGPL-2.1), hidapi, zlib, and SDL2, which this list does not
cover. vcpkg also produces `pthreadVC3.dll` and `pthreadVSE3.dll`; which of the three ships is
decided at packaging time. The Monado source revision is the one pinned by the
`runtime/monado-playspectra` submodule gitlink (release packages record the exact tree in their
`SOURCE.txt`); the vcpkg baseline is `4334d8b4c8916018600212ab4dd4bbdc343065d1`.

Licensing boundary: the `runtime/monado-playspectra` submodule is not covered by this repository's
MPL-2.0 license. It is a Monado fork and keeps upstream's licensing, which is REUSE-compliant and
multi-license; the submodule's `LICENSES/` directory holds the full set. The PlaySpectra driver
shell sources inside it (`src/xrt/drivers/playspectra/`) are BSL-1.0. The fork's build also
compiles this repository's first-party device core (`devicecore/`, MPL-2.0) into the Monado
binaries, so a distributed `monado-service` contains MPL-2.0 code and MPL-2.0's source-availability
terms apply to it alongside the licenses above.

## Build-time only

Used to produce the components above; no code from them reaches a release package.

| Component | License |
| --- | --- |
| glslang | mixed: BSD-3-Clause, BSD-2-Clause, Apache-2.0, MIT, and a GPL-3.0 component (with a special bison exception) its notice states is no longer required to build |
| GoogleTest (unit tests only) | BSD-3-Clause |

## Not redistributed

OpenComposite is fetched locally by `scripts/setup_opencomposite.sh` for the OpenVR
interoperability path. It is GPL-3.0 (per the upstream project at
https://gitlab.com/znixian/OpenOVR; the fetched artifact is a prebuilt DLL that carries no license
text, so this repository holds no primary source for the claim). It is never committed and is not
part of a release package.

A single OpenVR driver SDK header (`openvr_driver.h`, pinned at v1.8.19) is present under the
untracked `third_party/openvr_driver_sdk/` and is used only by the unfinished SteamVR driver in
`driver/`, which no release package includes. That copy carries a Valve copyright banner and no
license grant of its own; OpenVR's BSD-3-Clause text is in the tree at
`runtime/monado-playspectra/src/external/openvr_includes/LICENSE`, where the Monado submodule
vendors its own copy of the OpenVR headers.

Verification targets built or fetched under `third_party/` — among them hello_xr, hellovr, and the
Meta XR Simulator — are local development inputs that no release package includes. Each is governed
by its own upstream terms; this project redistributes none of them.

Release packaging must copy each redistributed component's full license text into the package.
