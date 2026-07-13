// PLACEHOLDER (not compiled; not in CMakeLists sources).
//
// The frame-capture implementation actually lives in capture.cpp, which holds the GfxApi dispatch
// and the VULKAN backend (xrEndFrame readback -> lodepng PNG). The original scaffold envisaged one
// file per backend (capture_d3d11/_d3d12/_vulkan.cpp); that was NOT how it landed -- Vulkan went into
// capture.cpp, so D3D11/D3D12 should extend capture.cpp's dispatch the same way (or move all three
// out together). Until then, capture.cpp returns an explicit "capture backend for D3D11/D3D12 not
// implemented yet" error -- never a silently-corrupt image (CLAUDE.md).
//
// Status vs the v1.0.0 DoD (.claude/harness/v1.0.0-dod.md):
//   - Vulkan color capture: DONE (capture.cpp).
//   - D3D11 / D3D12 color capture: "未対応" (pattern of the existing capture feature) -- core, must
//     land by v1.0.0; currently an honest explicit error. Verification gap: MinGW hello_xr builds
//     OpenGL+Vulkan only, so a D3D OpenXR test app must be sourced (or compile-only + review).
//   - MSAA resolve / HDR (RGBA16F) formats: "未対応" within Vulkan too.
//   - Depth (XrCompositionLayerDepthInfoKHR -> 16-bit grayscale PNG): a "機能なし" item on the DoD.
