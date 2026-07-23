# Harness journal (append-only)

## 2026-07-13 — task#5 Increment B: Vulkan frame-capture readback → PNG  [outcome: done]

**What shipped**: The OpenXR layer now does a real Vulkan GPU→CPU readback at `xrEndFrame` and writes a PNG; `vr_screenshot` (MCP) returns the image as content. Files: `layer/src/capture.cpp` (readback), `mcp/src/server.ts` (image content). Layer DLL rebuilt (2895607 B) and deployed to `layer/manifest/vr_agent_layer.dll`. This completes the color-capture path for the **Vulkan** graphics binding — the critical one (real app = Godot/Vulkan, Meta compositor Vulkan-native). Verified E2E against the real Meta XR Simulator + `hello_xr -g Vulkan`, and by opening the PNGs (genuine rendered scene, both eyes).

**Ship policy (this project)**: There is NO git repository (`git rev-parse` fails) and no CI. "Ship" = build the layer with `mingw32-make -C layer/build vr_agent_layer` and copy `vr_agent_layer.dll` into `layer/manifest/` (the dir named by `XR_API_LAYER_PATH`). No commit/push/PR step exists. If a git repo is later initialized, revisit — until then, do not attempt git operations in h-ship.

**Design decisions**
- Layer loads Vulkan at runtime from `vulkan-1.dll` via `GetProcAddress` (never links libvulkan) — keeps the layer engine/loader-agnostic. Device fns via `vkGetDeviceProcAddr`, instance fns (incl. `vkGetPhysicalDeviceMemoryProperties`) via `vkGetInstanceProcAddr`.
- Readback runs in `CaptureOnEndFrame`, which fires BEFORE forwarding to the runtime → the released image is still `COLOR_ATTACHMENT_OPTIMAL`; barrier from/back to that layout. Copy + fence-wait submitted on the app's own VkQueue from the app's xrEndFrame thread (safe for single-threaded hello_xr; multi-threaded-app queue external-sync is a documented non-goal).
- sRGB bytes (format 43 = R8G8B8A8_SRGB) go straight to an 8-bit RGBA PNG (lodepng). BGRA8 swizzled. Everything else (HDR, MSAA, non-8bit) returns an EXPLICIT error, never a silently-broken image (CLAUDE.md rule).

**Learnings**
- L1 (procedure that worked): the whole inv→plan→work→review→ship loop with an independent h-reviewer subagent caught a real overstatement in my own write-up (see L2). Fresh-eyes delegation earned its keep.
- L2 (correction, primary-source): I claimed the left/right PNGs differing "by horizontal-only parallax" proved correct eye assignment. WRONG — the two captures are taken at DIFFERENT xrEndFrames, so they differ by *temporal motion* of the input-driven cubes, not stereo parallax. Eye correctness is instead true *by construction* (OpenXR PRIMARY_STEREO view[0]=left/[1]=right → EyeToIndex → reply viewIndex). **Rule for future capture work: never use "the two eyes differ" as eye-correctness evidence on a moving scene.**
- L3 (DoD anti-pattern): I gated capture correctness on PNG file size (">100 KB"). A lossless PNG of a flat synthetic scene legitimately compresses to a few KB even when perfectly captured. **Never size-gate image-capture correctness; gate on a visual/content check + reply dimensions.**
- L4 (build): top-level `mingw32-make -C layer/build` rebuilds the entire OpenXR SDK + Catch2 (slow, >2 min). Build only our target: `mingw32-make -C layer/build vr_agent_layer`. Also, `make` reports "Built target" without recompiling if it thinks nothing changed — `touch layer/src/capture.cpp` to force a real recompile when verifying.

**Follow-ons (unprocessed — core-required work remains; NOT nice-to-have)**
- F1 → **D3D11 capture backend** (`XrGraphicsBindingD3D11KHR`) — CORE REQUIRED (CLAUDE.md 2-axis rule). Currently returns an explicit "not implemented" error. Verification gap: MinGW `hello_xr` builds OpenGL+Vulkan only (D3D plugins MSVC-gated), so no local E2E test app — its task must decide compile-only+review vs. sourcing a D3D OpenXR test app.
- F2 → **D3D12 capture backend** (`XrGraphicsBindingD3D12KHR`) — CORE REQUIRED, same status/gap as F1.
- F3 → robustness: an exception mid-readback (bad_alloc / json throw) doesn't signal the capture condvar → requester waits full `timeoutMs`; a GPU hang stalls the app render thread up to 5 s. Graceful (no hang, no ABI break) but harden by wrapping the fulfillment body to always set a result + notify.
- F4 → MSAA resolve + HDR (RGBA16F) color formats within Vulkan (currently explicit-error).
- (Depth capture remains a genuine nice-to-have — task #8 — per the user's "出してもいい".)

## 2026-07-13 — task#7 (part): controller POSE injection (arms-forward "前ならえ" demo)  [outcome: done, conversational]

**What shipped**: Controller *pose* injection — the position/orientation half of task #7 — via `xrSetInputDeviceLocationEXT` (CA extension). New: `StickyPose` in `control_channel.{h,cpp}` with `pose`/`pose_clear` commands (sticky = held every sync, so one command holds the hand); layer resolves `xrSetInputDeviceLocationEXT` + creates its own LOCAL reference space (`EnsureLocalSpace`) and re-applies sticky poses each `xrSyncActions`; space torn down at session destroy. Test harness: `scripts/pose_client.mjs` + `scripts/pose_test.sh`. Layer rebuilt (2913611 B) + redeployed.

**Verified E2E**: Meta sim honored `xrSetInputDeviceLocationEXT` — 562 calls, 0 FAIL. Swept hand height (y=-0.7 → -0.2 → -0.12); the two central cubes moved together as commanded → confirmed they are the two controllers (the blue square + bottom-right cubes are hello_xr's fixed reference-space markers, unmoved across all poses). Captured "前ならえ" (arms extended forward, ±0.22 m apart, ~shoulder height, 0.5 m forward in LOCAL space) — cubes appear front-and-center = correct.

**Learnings**
- L5: **CA (`XR_EXT_conformance_automation`) covers controller POSES too**, not just button/analog state, on the Meta sim — `xrSetInputDeviceLocationEXT` works. So the plan's "head/VIEW override stays in-layer, controller pose via CA" split is validated for controllers. (Head/VIEW/`xrLocateViews` override is still the remaining in-layer piece.)
- L6: hello_xr renders FIXED reference-space marker cubes (View/Local/Stage/etc.) in addition to the 2 hand cubes — when validating a pose visually, identify the hands by *sweeping* the injected pose and seeing which cubes move, not by assuming a cube is a hand.
- L7: hello_xr scales the hand cube *down* as grab→1, so injecting squeeze to "enlarge for identification" backfired. Don't use grab to make hands prominent.
- L8 (design): making poses *sticky* in the layer (re-applied each sync until cleared) is the right model for a `vr_set_controller` tool — set once, holds — vs. the one-shot drain used for button/analog events.

**Follow-ons**
- F5 → MCP tool `vr_set_controller` (hand, x/y/z, optional quat) sending the `pose` command; `vr_clear_controller` → `pose_clear`. (Raw control-channel works today; MCP wrapper not yet added.)
- F6 → head-pose / VIEW-space override (`xrLocateViews` + VIEW space + `xrLocateSpaces`) — the remaining task #7 piece, out of CA scope, must live in-layer.
- F7 → LOCAL-origin height is runtime-defined; a `vr_set_controller` should ideally accept head-relative coords or expose the view pose so callers don't guess the origin height.

## 2026-07-13 — WU1: head / viewpoint override + per-WU dual review  [outcome: done]

**What shipped**: In-layer head/viewpoint override (task#7's remaining piece). Hooks `xrLocateViews` (re-base runtime views onto injected head, preserving per-eye IPD offset + FOV via a quaternion decomposition), `xrCreateReferenceSpace`/`xrDestroySpace` (track VIEW-space handles), `xrLocateSpace`/`xrLocateSpaces` (override VIEW located in a world space). Control channel: `head`/`head_clear` + sticky head state. Own LOCAL reference space reused to transform the injected (LOCAL-defined) head into whatever space the app locates in. Self-contained quaternion math (no external dep). Layer 2922839→ rebuilt. E2E: yaw=0 vs yaw=40 shifted the rendered viewpoint as commanded (hello_xr -g Vulkan + Meta sim).

**Process (user-directed): each work unit reviewed by TWO subagents** — (1) a Haiku "tacit-knowledge" reader that reports understanding only (a comprehension/clarity probe: what it *can't* infer marks implicit knowledge to make explicit), and (2) a metacognition reviewer (correctness + PROTOCOL metacognition lens).

**Review outcomes (both earned their keep)**:
- Haiku (clarity) couldn't infer two things → added comments: (A) why `ApplyHeadToLocation` is a plain copy while `xrLocateViews` needs the per-eye rebase (VIEW space returns the single head origin; views return per-eye poses); (B) why head is a PULL-model interception (read at locate time) vs controller poses' PUSH model (re-applied each sync).
- Metacognition = needs-fix, 5 findings, all addressed: **#1 HIGH** VIEW-handle reuse → added `xrDestroySpace` hook to erase from the set (runtimes recycle handles); **#2 MED** un-normalized JSON quaternions distort IPD (QRot scales by |q|²) → normalize at ingest for BOTH head and controller pose; **#3 MED** injected head assumed app's locate space == LOCAL → now transform LOCAL→locate-space via the layer's LOCAL space (identity for LOCAL apps, correct for STAGE), warn-once fallback; **#4 LOW** asserted VALID/TRACKED without checking runtime validity → gate rebase on incoming valid bits; **#5 LOW** static next-ptr cross-instance staleness → left as-is (already covered by the file's single-global-instance scope note).
- Re-verified after fixes: identical yaw=40 result (no regression; transform is identity for hello_xr's LOCAL space).

**Learnings**
- L9: the two-reviewer split is complementary — the weak-reader (Haiku) surfaces *clarity/implicit-knowledge* gaps a strong reviewer glosses over (it "just understands"), while the strong reviewer finds *latent correctness* bugs (handle reuse, normalization, space assumption) that an E2E on one well-behaved app (hello_xr, LOCAL, always-valid) cannot exercise. Neither alone would have caught both classes.
- L10: normalize any quaternion crossing the JSON boundary at ingest — MCP callers won't send unit quats, and non-unit q silently scales rotated vectors by |q|² (here: distorting the IPD the head-rebase exists to preserve).

**Follow-on**
- F8 → mechanical regression guard (reviewer's suggestion): in the xrEndFrame hook, log/assert that the submitted projection-layer view poses match the active head override within tolerance — turns "looked right once" into a regression check. Not yet added.

## 2026-07-13 — WU2: pose/head MCP wrappers + dual review  [outcome: done]

**What shipped**: MCP tools `vr_set_controller`, `vr_clear_controller`, `vr_set_hmd`, `vr_clear_hmd`, `vr_move`, `vr_reset`, `vr_recenter` (server.ts) over control-channel `pose`/`head`/`reset` (+ new `pose_get`/`head_get`). Ergonomic yaw/pitch/roll → quaternion (numerically verified: yaw90→(0,.707,0,.707), all unit). MCP + layer build clean.

**Dual review outcomes**:
- Haiku (clarity): understood everything correctly — zero implicit-knowledge gaps (WU2's inline comments carried the intent). Its one "needs math verification" (eulerToQuat multiply order) was already covered by the numeric check.
- Metacognition = needs-fix, 8 findings. Key architectural catch (F1): the MCP server was mirroring the layer's authoritative sticky pose in a `lastPose` var to support `vr_move`'s relative delta — a local optimum that desyncs on server restart / multiple clients / raw-command paths / sim-default start. The layer already had getters (`ControlChannelGetStickyPoses`/`GetHead`), just no control-channel command exposing them. **Fix: added `pose_get`/`head_get` commands; `vr_move` now queries the single source of truth and errors if nothing is overridden (instead of warping to origin). Deleted `lastPose` entirely.** This dissolved F2 (origin-warp), F3 (clear→move resurrection), F7 (pre-send mirror update). Also: F4 → `orientationFrom` now errors on mixed quat+euler and normalizes output (was silently dropping euler + storing non-unit quats); F5 → added `vr_clear_hmd` (layer had `head_clear`, MCP didn't expose it); F6 → clarified `vr_recenter` freezes the view (not a runtime recenter). F8 (style) left.
- Verified E2E: pose_get/head_get return the set pose; reset clears both to active:false (SMOKE PASS).

**Learnings**
- L11: **the metacognition reviewer's highest-value finding was architectural, not a bug** — "you're mirroring authoritative state; wire the existing getter instead." Cheap fix (getters already existed), dissolved a whole class of desync bugs, and sets up WU3's vr_look_at to read the true current pose. Single-source-of-truth beats a convenience mirror; check for an existing getter before caching.
- L12: expose EVERY layer capability through MCP — `head_clear` existed but no `vr_clear_hmd` shipped until review caught it ("keyboard with a missing key", CLAUDE.md).

## 2026-07-13 — WU3: vr_point_at / vr_look_at + dual review — DISCOVERED SIM LIMITATION  [outcome: partial, needs user decision]

**What shipped**: `vr_point_at` (aim a controller's grip -Z at a world point) and `vr_look_at` (aim the viewpoint at a point). `lookQuat` (look-rotation via orthonormal basis → Shepperd matrix→quat) numerically verified: -Z aims exactly at target for 5 cases incl. near-vertical (err=0.0000). `resolveFrom` reuses WU2's single-source-of-truth (pose_get/head_get).

**Dual review**: Haiku (clarity) — understood all, no gaps. Metacognition = needs-fix + one needs-human:
- F1/F2 [High] FIXED: `vr_look_at` silently fell back to origin when no head override (a WU2-forbidden silent-warp) and duplicated logic instead of using `resolveFrom`. Now routed through `resolveFrom({cmd:"head_get"})` → errors like vr_point_at/vr_move. Consistency restored.
- F3 [needs-human] → **escalated into a bigger, verified finding** (see below).

**KEY FINDING (E2E, verified byte-identical): the Meta XR Simulator ignores injected controller ORIENTATION via CA `xrSetInputDeviceLocationEXT`.** Set the right controller at a fixed position with identity vs a 90°-down grip orientation → the two rendered frames are byte-identical (same md5, 0-byte diff); 179 CA location calls all returned ok. So CA location applies POSITION but not ORIENTATION on this sim. (前ならえ worked because it used identity orientation = position-only.) Head orientation DOES work because it's an in-layer `xrLocateViews` override, not CA.

**Implication (North Star / CLAUDE.md 2-axis)**: a physically-touched controller has a full pose (position+orientation). Orientation injection is part of input-completeness, not nice-to-have. CA alone is insufficient on the Meta sim. To make controller orientation actually work regardless of runtime, the controller grip pose must be overridden IN-LAYER (hook xrLocateSpace on the hand grip action spaces, exactly like head) — the "full override backend" the plan anticipated. This overlaps WU5's action-space tracking. This is a real scope decision → surfaced to the user.

**Status**: vr_point_at/vr_look_at code + math are correct and shipped; vr_look_at (head) works fully (in-layer). vr_point_at's orientation is correct but not honored by the Meta sim (runtime limitation) until in-layer controller override is added.

**Learnings**
- L13: an injection call returning XR_SUCCESS does NOT mean the runtime applied it — the Meta sim accepted 179 orientation injections and applied none. Verify effect (here: byte-diff of rendered frames), never trust the success code. This is the sharpest instance yet of "a passing call is not a passing result."
- L14: CA (`xrSetInputDeviceLocationEXT`) is position-only on the Meta sim for grip pose; full pose fidelity needs the in-layer override path (which already works for head).

## 2026-07-13 — WU3b: in-layer controller pose override (fixes the CA orientation gap)  [outcome: done, verified]

**Decision (user-approved, "switch early while rework is minimal")**: make the LAYER the authoritative source for controller pose — override xrLocateSpace on the hand's grip action space with the full injected pose (position+orientation), exactly as the head does via xrLocateViews. CA `xrSetInputDeviceLocationEXT` stays only as a harmless position-only fallback. NOT a custom runtime — just more hooks on the existing layer.

**Implemented** (openxr_agent_layer.cpp): generalized `TransformLocalPoseToSpace` (head now uses it too); action tracking — `Hook_xrCreateActionSpace` (XrSpace→{action,hand}), `Hook_xrSuggestInteractionProfileBindings` (actions bound to `.../input/grip/pose` → g_grip_pose_actions), `PathToStr` via xrPathToString; `ApplyGripOverride` writes the injected sticky pose into the located result for grip spaces; wired into `Hook_xrLocateSpace`/`Hook_xrLocateSpaces` (runs independent of head state); cleanup in xrDestroySpace + session destroy; registered 2 new hooks. Layer 2953650 B.

**VERIFIED E2E (the whole point)**: same controller position, identity orientation vs grip-−Z-pointed-straight-down (−90° X pitch, quat (−0.707,0,0,0.707)). Before (CA only): the two frames were byte-identical (orientation ignored). After (in-layer): byte-diff 2956, and the injected cube visibly re-orients — green (top) face rotates to the front, blue (front) to the top edge. Marker cubes unchanged. So controller ORIENTATION now works. (Fixed a flaky test too: `no projection layer submitted this frame` is intermittent; the client now retries screenshots.)

**Status**: vr_set_controller (position+orientation) and vr_point_at now fully functional on the Meta sim. Controller pose fidelity == head fidelity (both in-layer). Grip vs aim (F3): we override the GRIP pose; apps drawing a ray from AIM pose would still see the fixed grip→aim offset — documented in vr_point_at, aim-pose override remains a possible follow-up.

**Learnings**
- L15: this is the "full override backend" the original plan anticipated as CA's complement. The right architecture emerged from a discovered limitation (CA orientation ignored) + the user's "switch early" instinct: LAYER authoritative for pose (in-layer xrLocateSpace/xrLocateViews), CA for what it does well (buttons/analog/active/position-fallback). Hybrid, each part playing to its strength — the plan's original design, now validated by evidence rather than assumed.
- L16: action-space→hand mapping needs both xrCreateActionSpace (subactionPath=hand) AND xrSuggestInteractionProfileBindings (which action is grip-pose). This is exactly the tracking WU5's vr_actions needs, now built — WU5 gets most of its foundation for free.

## 2026-07-13 — WU3b review outcome + head non-regression  [outcome: done]

Metacognition verdict: **pass** (core verified: no deadlock/lock-inversion, no self-recursion, in-layer authoritative, refactor equivalence-preserving, head+controller converged on one "layer-authoritative pose at locate time" model). Applied cheap parity fixes: warn-once Logs in ApplyGripOverride for null-subactionPath and unresolved LOCAL->base transform (matches the head path's disclosure). **Head non-regression confirmed empirically**: post-refactor yaw=40 capture is byte-identical (diff=0) to the pre-refactor one — the TransformHeadToSpace→TransformLocalPoseToSpace refactor is exactly equivalence-preserving.

Findings carried to WU5 (controller-as-rigid-body): (F-med) also override the AIM pose (grip+aim as two offsets of one injected transform + per-profile grip→aim rigid offset) so apps drawing a ray from aim don't desync; (F-med) capture hand identity at xrCreateAction so null-subactionPath action spaces still map to a hand; (F-low) prune g_grip_pose_actions via an xrDestroyAction hook; (F-low) grip located in a VIEW base space should use the injected head; (F-low/perf) cache LOCAL→base transform per (baseSpace,time) if profiling ever matters.

## 2026-07-13 — 4-agent sweep (metacog×2 + bug/local-optima×2, Opus+Fable) + critical fixes  [outcome: done]

User summoned 4 agents (2 metacognition, 2 bug/local-optima; Opus & Fable each). Overall verdict from all 4: the codebase is converging on a coherent design (not a patch pile) — dual-review, equivalence-checked refactors, and SSOT defense (WU2 L11) are working. Convergent CRITICAL bugs (multiple agents, "the measurement tool crashes the observed app" class) — FIXED and where possible E2E-verified:

- **[HIGH] `HandleRequest` type-error → std::terminate crashes the VR app** (bug Opus A1, bug Fable A2). Only `json::parse` was guarded; `req.value(key, default)` throws type_error.302/.306 on a mis-typed/non-object field, unwinding through the socket thread → terminate. FIXED: whole body wrapped in try/catch; `dump()` uses replace error-handler. **E2E-VERIFIED**: 5 malformed/mistyped payloads now return structured errors and hello_xr stays alive ("APP ALIVE after malformed barrage").
- **[HIGH] shutdown hang: client socket not closed** (bug Opus A3, bug Fable A1). ControlChannelStop closed only the listen socket; a connected MCP client blocked in recv() → join() hangs → xrDestroyInstance never returns (exactly the E2E setup; my tests masked it with taskkill). FIXED: track `g_client_socket` (atomic), shutdown()+close it in Stop; ownership handed off to avoid double-close.
- **[MED-HIGH] capture exception fails xrEndFrame + no condvar notify** (bug Opus A2, journal F3). FIXED: fulfillment wrapped in try/catch that always sets an error result + `g_req_done` + notify, never propagates to Hook_xrEndFrame.
- **[MED-HIGH] `snap.views[idx]` OOB when hadProjection && viewCount==0** (bug Opus A4, bug Fable A3). FIXED: guard `snap.views.empty()`.
- **[MED] MCP `request()` no timeout → 2nd client hangs the agent forever** (metacog Fable B6). FIXED: 30 s per-request timeout in server.ts.
- **[LOW-MED] haptic hand always "unknown"** (bug Opus B5, metacog Fable B1, bug Fable A8). FIXED: `PathToStr(hapticActionInfo->subactionPath)`.
- **Doc drift** (3+ agents; docstrings are the LLM consumer's spec): vr_screenshot said "Increment B / metadata only" but returns real PNG; vr_move said "defaults from origin" but errors; eye:'both' now noted as separate-frame (not simultaneous stereo); layer top TODO(#5) and control_channel.h header refreshed. FIXED.

Note: bug-Fable A5 referenced `capture_d3d11.cpp` — that file does NOT exist (inferred from the plan dir); correctly ignored. Good reminder to verify agent file:line claims (one of four hallucinated a path).

**Recorded, NOT yet done (deferred with rationale)** — carried as follow-ups:
- Multi-threaded-engine safety (real for Godot/Unity/Unreal, not hello_xr): lazy-init races on g_local_space / resolve-pointers (bug Fable A4), VkDevice use-after-free if xrDestroySession races an in-flight readback (bug Fable A5), unsynchronized VkQueue submit (bug Fable A7 — document the constraint). → do with per-instance dispatch work.
- **Velocity not zeroed** (metacog Fable B2): overrides write pose+flags but leave the runtime's XrSpaceVelocity in the next-chain → position fixed, velocity nonzero (breaks throw/physics apps). Real "point vs rigid body" gap. → fix with WU5 controller-as-rigid-body.
- static-next → per-instance dispatch struct (bug Fable B3) before more hooks land.
- CA push now redundant on grip spaces (metacog Opus 4, bug Fable B2) → gate it off when a grip space is detected.
- Per-frame haptic/CA log spam (bug Fable A9); reset uses ClearAllStickyPoses helper (bug Fable B6); vr_wait silent no-progress if capture missing (bug Fable A11); dedup head/grip dispatch into OverrideLocatedPose (metacog Opus 5).

**Two DESIGN decisions raised to the user** (not silently deferred):
1. **aim pose is CORE per the CLAUDE.md 2-axis rule** (metacog Fable C1): `/input/aim/pose` is a universal OpenXR path; most ray-drawing apps (incl. Godot XR) use aim, so vr_point_at's grip-only override misses the app's ray. Promote grip+aim (controller-as-rigid-body) to core, not follow-on.
2. **observe/act namespace gap** (metacog Opus Q1): observe=pixels, act=LOCAL-space metres, with no mapping. Playwright's essence is observe+act sharing a namespace. Bridge = expose view pose + FOV + projection so the agent can map world↔screen pixels. A completeness axis distinct from D3D.

**Process learning (metacog Fable Q2)**: knowledge lives in journal.md but code signage lagged (docstrings stale). Since the consumer is an LLM reading docstrings at runtime, **rule: when a learning is written to the journal, fix the code/docstring it came from in the same pass.** Applied here (doc-drift fixes).

## 2026-07-14 — R01: capture 3バックエンド共通部を capture_common へ抽出 (move-only)  [outcome: done]

**What shipped**: `RepackRows`(rowPitch尊重の行de-pad + BGRAスウィズル)/`EncodeRgbaPng`(lodepngラッパ)/`BuildCaptureSuccessJson`(基底9フィールド)を `layer/src/capture_common.{h,cpp}` に新設し、Vulkan/D3D11/D3D12 の色読み出し末尾の重複を一本化。純減 -63/+110 行。`refactor-plan.csv` R01。ブランチ loop/r01-capture-common、4 WIPコミット→(finalize方法はユーザー確認)。

**Design decisions**
- 共通成功JSONヘルパーは**基底オブジェクトを返し呼び出し側が拡張**(inv A1)。Vulkanは戻り値へ sampleCount/msaaResolved/tonemapped(+HDR時 sourceHdrFormat/colorConversion)を追記=上位集合を保存。
- **エラーJSONは3者非対称のまま不変**(D3D11手書き/D3D12 fail()/Vulkan {ok,error})。統一は別タスク R15(挙動変更)の領分。R01では触らない。
- `RepackRows` は rowPitch 引数で3者包含(Vulkanは密stagingなので w*4 を渡す)。HDR decode/MSAA resolve/fence-inflight は Vulkan TU に残置。

**Verified**: `cmake --build build --target vr_agent_layer` exit 0(警告ゼロ)。`scripts/integration_test.sh Vulkan` = 15/15 PASS, rc=0(非退化キャプチャ含む)。レビューは複数観点(correctness/readability/performance)並列委譲で全員 pass(worst-case統合)。

**Learnings / friction**
- L17 [ship policy 更新]: **このプロジェクトは今や git リポジトリを持つ**(過去journalの「NO git repository」は陳腐化)。ただし**リモート未設定**(`git remote -v` 空)= push/PR/CI は対象外。ship = ローカルコミット確定 + (master統合はユーザー確認)。次回も remote 無ければ push なしで良い。
- L18 [friction・環境]: hello_xr が**応答不能ゾンビ**として大量(~20)残留し得る。`taskkill /F` は MSYS パス変換で `F:/` に化け失敗、`//F //IM` 形でも**強制終了不可**(カーネル待機)、PowerShell は deny。→ 全ターゲット build は hello_xr.exe 再リンクがロックで失敗。**回避**: `cmake --build build --target vr_agent_layer` でレイヤーのみビルドすれば hello_xr を触らず検証可。E2E は既存exeを使い、ゾンビがいてもフレッシュ実行が SES を取得し完走。
- L19 [friction・tooling]: Bash の `grep`/`ls` がラッパー(rg フォールバック)で空を誤返しする。専用 Grep/Glob/Read を使うこと。
- L20 [learning]: nlohmann::json は std::map ベース=**辞書順シリアライズ**。挿入順は wire 出力に影響しない(move-only JSON抽出で順序を気にしすぎる必要はなかった)。

**Follow-ons (→ backlog.md Open)**
- F1 → `capture_vulkan.cpp:711,725` 非HDR経路の二重アロケ(3レビュアー一致・非ブロッキング、move-only中立化の後続コミット候補)。
- doc → `capture_backends.h:8-9` 古コメント「Vulkan stays inline in capture.cpp」是正(phase2でmove済み、doc-only)。
- 残 → `refactor-plan.csv` R02〜R21(CSV自体がレジャー)。次の着手候補は R02(Vulkan重複) / R03(pixel_convert抽出、テスト前提)。

## 2026-07-14 — R02: capture_vulkan.cpp の copy→submit→fence→map 重複を共通ヘルパーへ抽出 (move-only)  [outcome: done]  (auto-run 1/14)

**What shipped**: 色 `VulkanReadbackToPng` と深度 `VulkanReadbackDepthToPng` で二重化していた GPU-copy プラミング(reset+begin→submit→fence待ち5s→map→unmap)を `RecordAndSubmitCopy(recordFn)` + `MapStaging(bytes,readFn)`(file-local static)へ単一source化。record本体(色はMSAA resolve含む)・read本体(色RepackRows/HDR、深度memcpy)はラムダ注入。`VkCaptureStatus{Ok,SubmitFailed,Timeout}` を返し、色/深度の各呼出側が自分の非対称エラーJSONを構築。F1(色 非HDR経路の二重アロケ)も解消。master merge 7abb76f。

**Design/learnings**:
- L21: **行数は 949→972(+23)と微増したが net-positive**。dedup 対象は submit/fence/**inflight**プラミングで、従来 depth 側は「see color path」コメントで暗黙複製=fence/inflightバグ修正が2箇所必要だった。単一source化の保守利得 > lambda boilerplate の行数増。CSVの「半減」は楽観的(大半はMSAA/HDR/深度decodeで非重複)。→ **move-only リファクタの価値は行数でなく重複ロジックの単一化で測る**。
- L22: GAP-07(c) inflightセマンティクス(timeout時 g_vk_capture_inflight=true 維持→FreeVulkanResourcesが待ち切る)をヘルパーに集約。fence timing の唯一のsource-of-truthになった。
- L23: 深度 success 経路はこの環境でE2E不可(hello_xr未提出)。record本体が g_vk_cmd→cmd の機械的置換のみ、submit/fence/map は色経路と共有ヘルパーで E2E実証済み、という2点で verbatim等価性を担保しレビュー2名pass。

**検証**: build警告0 / `integration_test.sh Vulkan` 15/15 rc=0。レビュー correctness+readability 両pass(correctnessは旧449a47fとの行単位対照)。

## 2026-07-14 — R03: 純変換関数を pixel_convert.{h,cpp} へ抽出 (move-only)  [outcome: done]  (auto-run 2/14)

**What shipped**: capture_vulkan.cpp の anonymous namespace にあった純関数 HalfToFloat/LinearToSrgb/QuantizeSrgb/QuantizeLinearUnit/LinearizeViewDepth を `pixel_convert.{h,cpp}`(vr_agent:: 公開関数)へ verbatim 移動。capture_vulkan から除去+`#include "pixel_convert.h"`、CMake登録。ClassifyDepthFormat/DepthKind(VK_FORMAT_*依存で移動不可)・HalfFloatSelfTest(Log依存・CSV指定)は残置。master merge 4954eb1。

**Design/learnings**:
- L24: **VK依存関数は「純関数抽出」から意図的に除外**。ClassifyDepthFormat は VK_FORMAT_* に直接依存するため Vulkanヘッダ非依存TU(pixel_convert)へ verbatim 移動できず capture_vulkan に残した。CSVの「純関数を抽出」を字義通り全部移すのでなく、**依存グラフで movable/non-movable を切り分ける**のが正しい(inv で明示)。
- L25: リンケージ変化(anonymous namespace 内部→vr_agent:: 外部)は観測挙動不変。capture_vulkan は `namespace vr_agent` 内なので未修飾呼出が enclosing-ns lookup で新 pixel_convert 版へ解決、旧anon定義除去で二重定義/曖昧なし(build警告0が実証)。
- L26: **除去+新設は1コミット/1ステップ必須**。中間状態で anon-ns版と vr_agent::版が併存すると未修飾呼出が曖昧=ビルド不能。ODR回避のため分割しない。
- L27: 移動5関数は HDR半精度decode/深度linearize経路でのみ呼ばれ、この環境のE2E(fmt43非HDR + depth未提出)では**実行時非カバー**。verbatim移動のバイト等価性(correctnessが449a47fと個別diff)が観測挙動不変の根拠。E2Eはlink/buildスモークとして機能。**R11(ユニットテスト)で本テスト化する前提**が整った。

**検証**: build警告0 / `integration_test.sh Vulkan` 15/15 rc=0。レビュー correctness(バイト等価)+readability 両pass。

## 2026-07-14 — R06: control_channel HandleRequest の14コマンドを handlerテーブルへ分離 (move-only)  [outcome: done, 1 review-fix]  (auto-run 3/14)

**What shipped**: `HandleRequest` の14コマンド巨大if-chain(旧245行)を `Handle_<cmd>(const json&)` 14関数へ verbatim 分離し、`static const struct{name; fn}` 配列の線形ディスパッチへ。preamble(parse/invalid json)・outer try/catch・`cmd`抽出・default(unknown cmd)を HandleRequest に保存。応答JSON全文言不変。ヘッダ不変。master merge aabcfd1(ブランチ放置)。

**Design/learnings**:
- L28: **コマンド=関数=レビュー/テスト単位** に揃える構造改善。行数 net +21(シグネチャ/table/コメントの構造コスト)だが、将来コマンド追加のコスト低下+個別レビュー可能性の利得が上回る(R02同様、move-onlyの価値は行数でなく構造)。
- L29 [review-fix]: **コード移動時はコメントの追従を忘れない**。HandleRequest本体をハンドラ群へ抜き出し関数を230行下へ移した結果、直上のドキュメントコメント「entire body below is guarded」が孤立しハンドラ群(非ガード)を誤って指した。readabilityレビュアーが検出→コメントを実体直上へ移動+文言是正(dispatch below is guarded)。**教訓: 関数を移動したら、その関数を説明するコメントも一緒に動かす**。review_rejections=1で解消。
- L30: 匿名namespace内の関数に `static` は冗長(既に内部リンケージ)。周囲(BuildStatus等)がstaticなしなので統一。readability助言で除去。
- L31: R06はE2Eカバー良好 — 統合テストが status/actions/view/pose_get/screenshot/head/haptics/pose を新ディスパッチ経由で実駆動し応答JSON不変を実証(R02/R03のHDR/深度非カバーと対照的)。

**検証**: build警告0 / `integration_test.sh Vulkan` 15/15 rc=0(修正後も緑)。レビュー correctness pass(14ハンドラ1文字対照)+ readability needs-fix→修正→pass。

## 2026-07-14 — R07: CaptureOnEndFrame の snapshot抽出 + 3バックエンド分岐テーブル化 (move-only)  [outcome: done]  (auto-run 4/14)

**What shipped**: capture.cpp の CaptureOnEndFrame で (1)snapshot構築ループを `ParseEndFrameSnapshot(const XrFrameEndInfo*)` へ verbatim 抽出(frameCount代入は呼出側に残置)、(2)Vulkan/D3D11/D3D12 の3分岐を `Backend{GfxApi,readback fn,depthSupported}` テーブル+線形探索へ畳込。depth非対称(Vulkan=ResolveDepth/D3D=kDepthVulkanOnly)を depthSupported フラグ+三項に集約。応答JSON/unknown API分岐 保存。master merge 58a48a8。

**Design/learnings**:
- L32: **これは行数減の純粋な dedup**(step2 +23/-19)。R02/R06(型外出し/table導入で行数増)と異なり、3分岐の (haveImage判定+readback呼出+depth代入) の三重複製を1本化。3者ReadbackToPngが同一シグネチャ(R01で統一済)だから関数ポインタ表で畳めた=**過去のリファクタが次の畳込を可能にする**連鎖。
- L33: depth非対称は `depthSupported ? ResolveDepth(view) : kDepthVulkanOnly` の1行に。**D3DでResolveDepthを誤呼びしない**(depthSupportedがVulkanのみtrue)ことを correctness が明示検証。move-only で「片側だけ呼ぶ」非対称の保存は要注意ポイント。
- L34 [R06教訓の適用]: 関数抽出時のコメント孤立(L29)を意識し、ParseEndFrameSnapshot は本体ごとコメント移動・CaptureOnEndFrame側に残骸なし。readabilityが「再発チェック→問題なし」と確認。**前タスクのreview指摘が次タスクの自己チェック項目になる**good loop。
- L35: 「3API完全性を構造で保証」— バックエンド追加/修正で触る箇所が kBackends テーブル1点に集約(R08/R09/R10 のD3D対応時にテーブル1エントリのdepthSupported切替等で済む素地)。

**検証**: build警告0 / `integration_test.sh Vulkan` 15/15 rc=0(Vulkan経路実駆動、D3Dはcompile-only+diff)。レビュー correctness(verbatim+depth非対称)+readability 両pass。

## 2026-07-14 — R14: LayerBuildActionsJson 転送を除去、control_channel が BuildActionsJson を直接呼ぶ (move-only)  [outcome: done]  (auto-run 5/14)

**What shipped**: phase4リファクタの後始末。actions dump を得る遠回り(control_channel→layer_log.h宣言→openxr_agent_layer.cpp の `LayerBuildActionsJson(){return BuildActionsJson();}` 転送→action_registry::BuildActionsJson)を除去。control_channel.cpp が `action_registry.h` を直接includeし `BuildActionsJson()` を呼ぶ。layer_log.h:15 の `TODO(refactor phase 4)` を完遂、layer_log.h はロガー専用に純化。net -11行。master merge 97b7f88。

**Design/learnings**:
- L36: **依存を宣言どおりに**。actions dump は action_registry の責務なのに、宣言が layer_log.h(無関係)に同居し openxr_agent_layer.cpp が薄い転送を持つ「実装の漏れ」だった。転送レイヤーを除くことで control_channel→action_registry の直接依存に。過去タスク(phase1でlayer_log分離)が残した TODO を回収=**リファクタは後始末までやって完結**。
- L37 [review発見]: 削除は既存の隠れ残骸を顕在化させる。readabilityが `layer_log.h #include <string>` の未使用化(LayerBuildActionsJson が唯一の std::string 利用者だった)を検出。ただし transitive include 依存の可能性→単純除去はリスクなので別タスク(backlog: layer-log-unused-string)へ。**「削除タスクは、削除で不要化する周辺(include/前方宣言)も点検する」**。
- L38: self-locking の保存に注意した — BuildActionsJson は唯一自分でロックを取る registry entry。Handle_actions(R06で分離)はロック非保持文脈なので直接呼びで二重ロックなし、と correctness が確認。

**検証**: build警告0 / `integration_test.sh Vulkan` 15/15 rc=0(actions 4/4 bound で出力不変実証)。レビュー correctness+readability 両pass。

## 2026-07-14 — R05: Hook_xrDestroyActionSet の erase ロジックを RegistryEraseActionSet へ移動 (move-only)  [outcome: done, 1 review-fix]  (auto-run 6/14)

**What shipped**: Hook_xrDestroyActionSet がレジストリ内部コンテナを直接走査・erase していたロジックを `action_registry::RegistryEraseActionSet(XrActionSet, const std::function<void(XrAction)>&)` へ verbatim 移動。フックは ActionMutex ロック+1呼出に縮小。FallbackEraseForAction は callback 注入(action_registry→input_inject 逆依存を作らない)。master merge db4334c。

**Design/learnings**:
- L39 [process]: **move-only で「最後の使用者」を消したら、依存宣言(using/前方宣言/include)が dead になるか grep で必ず確認する**。inv/plan で using 5本を「他フック使用で残す」と未検証で断定したが readability が dead を検出。自分で `Registry*\(` を grep して0呼出=ground truthを確定し削除。
- L40 [review]: **レビュアー間で事実が食い違った**(correctness「他フック使用」vs readability「dead」)。鵜呑みにせず main-loop が grep で裁定(readability が正、correctness も私と同じ未検証仮定)。事実対立は一次ソースで裁く。
- L41: レジストリ内部知識(handle-reuse安全性)を registry TU に閉じ込め、callback 注入で下位層が上位機能(input_inject fallback)を知らずに済む=依存の向きを正した。

**検証**: build警告0 / `integration_test.sh Vulkan` 15/15 rc=0(teardown clean)。レビュー correctness pass + readability needs-fix→修正→pass。

## 2026-07-14 — R04: openxr_agent_layer.cpp を hooks_capture/locate/action の3TUへ move-only 分割  [outcome: done, 1 review-fix]  (auto-run 7/14)

**What shipped**: openxr_agent_layer.cpp(926行)を3TUへ move-only 分割。hooks_capture.{h,cpp}(7フック: CreateSession/CreateSwapchain/DestroySwapchain/EnumerateSwapchainImages/AcquireSwapchainImage/ReleaseSwapchainImage/EndFrame)、hooks_locate.{h,cpp}(4フック: LocateViews/CreateReferenceSpace/LocateSpace/LocateSpaces)、hooks_action.{h,cpp}(13+teardown3=16フック)。本体は~290行(kHooksテーブル+negotiate/GIPA/CreateApiLayerInstance)。master a7fcd7b(ブランチ放置)。+786/-659。

**特殊事情**: 前セッション(ツール呼出破損で中断)で4コミット完成済み。本セッションでは構造検証(ブレースバランス・include配線・CMake・トランケーションなし)→マージ→ビルド→E2E→レビューのみ。

**Design/learnings**:
- L42: **前セッションの成果物は「コード破損」ではなく「検証・出荷未了」だった**。構造検証(ブレースバランス・include配線・ファイル末尾)で破損を機械的に排除でき、そのまま活用できた。ツール破損=コード破損と早合点しない。
- L43 [review-fix, L39拡張]: **L39(dead using 確認)は include にも適用する**。h-work のStep 4 で using 7本を確認したが `#include <string>` のチェックを漏らした。フック移動で本体の std::string 使用が0になり dead 化。依存ヘッダ5本は全て自分で `<string>` を include → transitive依存もなし。readability レビュアーが検出。**教訓: move-only で使用者が消えたら using + include の両方を grep で確認する。**
- L44 [review quality]: correctness レビュアーが**27フック全数バイト等価検証**(自動body抽出+diff)を実施。サンプリングでなく全数、かつ kHooks テーブルもバイト等価。move-only の correctness 検証はこの水準が理想。
- L45 [process]: 前セッションの inv.md が完全に有効(設計結論・クラスタ分類・配線設計が不変)で、再調査不要だった。inv は「状況が変わっていないか」のデルタ確認のみで済む。

**検証**: build警告0 / `integration_test.sh Vulkan` 15/15 rc=0(全フッククラスタ新TU経由で正常動作、graceful teardown clean)。レビュー correctness pass(27フック全数バイト等価) + readability needs-fix(`#include <string>` dead)→修正→pass。

**Ship policy(確認)**: リモート origin あり(github.com/YmSaki/VR-MCP.git)だが push は未実施(ユーザー確認待ち)。ローカル master は origin から22コミット先行。

## 2026-07-16 — M0: d3d11-flat-capture 修正 (keyed mutex 未取得の根因修正)  [outcome: done]  (OpenVRマイルストーン 1/6)

**What shipped**: D3D11 キャプチャが単色(全ゼロ)画像を返すバグの根因修正。ランタイム共有のD3D11スワップチェーン画像は MiscFlags=0x100(D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)で、keyed mutex 未取得の CopySubresourceRegion は全ゼロを読む。AcquireSync(0,1000ms)→copy→ReleaseSync(0) で修正。失敗2経路(QI/AcquireSync)は明示エラーJSON。fix 3dbb431(+診断18d3ec6/コメント638dc91)。併せて Monado テスト基盤(VR_RUNTIME切替/setup 2本/ハーネス2修正)を出荷。

**Design/learnings**:
- L46: **D3D11 keyed mutex の罠**: AcquireSync は WAIT_TIMEOUT(0x102)/WAIT_ABANDONED が SUCCEEDED() 扱いの HRESULT → **hr==S_OK 厳密比較が唯一正しいガード**。また列挙名は KEYEDMUTEX(アンダースコアなし)。D3D12(fence共有)/Vulkan(external memory)に同種同期は不要=追加する方が誤り(レビュアーも独立に同判定)。
- L47: **診断先行プランが有効だった**: 修正前に1回の診断実行で仮説を三点セット(フラグ/QI/全ゼロ)で確定 → 修正は一発で両ランタイム 17/17。証拠より先に手段へコミットしない構成は work_retry 0 で完走。
- L48: **D3D の E2E 解禁が本バグを可視化した**: 「compile+レビューのみ」では単色キャプチャは検出不能だった(スクショAPIは ok=true を返す)。MSVC hello_xr(third_party/hello_xr_msvc)により Deferred R08/R09/R10/R17 も今後は実行検証つきで進められる。
- L49 [environment]: **ゾンビ hello_xr.exe は exe の書込ロックだけ保持**(実行は可能、taskkill は「インスタンスなし」と矛盾報告)。MinGW hello_xr の relink がブロックされたら `--target vr_agent_layer` 単独ビルドで回避。再起動で消える。
- L50 [ship policy 継続]: push 未実施(ユーザー確認待ち)。ローカル master は origin から更に先行中。

**検証**: レイヤービルド警告0 / D3D11 17/17+graceful ×{metasim,monado}(distinctColors=5) / 回帰 Vulkan・D3D12 17/17。レビュー pass(h-reviewer が E2E 2本再実行+PNG目視+COM参照カウント精読)。

## 2026-07-16 — M1: setup_opencomposite.sh (OpenComposite 取得)  [outcome: done]  (OpenVRマイルストーン 2/6, auto 1/5)

**What shipped**: OpenComposite(OpenVR→OpenXR変換)の公式 x64 openvr_api.dll を third_party/opencomposite/ へ再現可能に取得するスクリプト。直DL(znix.xyz, AppVeyorビルド)+PE x64機械検査+VERSION.txt。per-app差し替え方式のみ(ランタイムスイッチャー不使用=G7)。GPLv3・非コミット・非再配布。

**Learnings**:
- L51: curl -sL は HTTP 404 でも rc=0 で HTML を書く(文書化仕様)。set -eu では捕まらないため、**DL物の機械検査(PEヘッダ等)をバックストップに置く**層構造が正解。-fSL 統一は backlog P3(setup_monado.sh と併修)。

**検証**: rc=0・PE check x64 OK(2447872B)・冪等再実行OK。レビュー pass(独立PEパース+再実行+non_goals機械確認)。

## 2026-07-16 — M2: OpenVRサンプル(hellovr_dx12)+OpenComposite→Monado 到達  [outcome: done, 1 review-fix]  (OpenVRマイルストーン 3/6, auto 2/5)

**What shipped**: scripts/setup_hellovr.sh — openvr shallow clone → hellovr_dx12 vcxproj を Win32→x64 機械変換(原本無改変で _x64.vcxproj 生成、冪等) → MSBuild → third_party/hellovr/ 配備(SDL2.dll は Monado 同梱流用、openvr_api.dll は OpenComposite 版差替)。実証: Monado 上で 15s 生存、**OpenXR 側は OpenComposite が D3D11 client を選択** = M0 の keyed mutex 修正が OpenVR 経路の土台だったことを実証。

**Learnings**:
- L52: **hellovr_dx11 は存在しない**(dx12/opengl/vulkan のみ)。samples CMake は dx12 非対象+Qt/GLEW 要求 → vcxproj 直 MSBuild が正解。vcxproj は Win32 のみ → x64 機械変換(32bit プロセスは x64 ランタイム DLL を読めないため x64 必須)。
- L53 [review-fix]: **偽前提の是正は文書全体を grep してから閉じる**。M2 節だけ直してゴール節/M3 指示/リスク節に hellovr_dx11 が残存 → レビュアーが checkpoint 3 で検出。「前提修正タスクの完了条件は grep 0 件(許容形除く)」をルール化。
- L54: OpenComposite は DX12 提出を受理し内部で D3D11 に変換して OpenXR へ(実測)。アプリ側 API とレイヤーが撮る API は独立 — OpenVR 系の観察経路はほぼ D3D11。

**検証**: setup 冪等 rc=0 / Monado 実証(レビュアー再実行含む2回) / git 1ファイルのみ。レビュー needs-fix(doc残存)→修正→delta pass。

## 2026-07-16 — M3改: openvr v1.8.19 タグ固定+レイヤースモーク完遂  [outcome: done, 1 review-fix]  (OpenVRマイルストーン 4/6, auto 3/5)

**訂正(M2エントリ、append-only)**: M2 の「Monado 到達」は過大だった。実際は openvr master ヘッダの IVRSystem_026 を OpenComposite(上限 System/Compositor=022, Input=010, RM=006 — Reimpl の GEN_INTERFACE 実測)が未実装 → VR_GetGenericInterface が null → hellovr は**無音の SDL モーダルで永久ブロック=0フレーム**だった。「client_d3d11_compositor_init」ログは仮アダプタ段階のもので実フレーム経路ではない。

**What shipped**: setup_hellovr.sh に OPENVR_TAG=v1.8.19 固定(System_021/Compositor_022/Input_007/RM_006 — 全て OC 実装リスト内。タグをクローンパスに含め stale 排除)。スモーク完遂: framesObserved=550、**OpenXR 側=D3D12 client**、screenshot 438KB 非退化、head 注入反映、**actions 実態=「opencomposite-actions」セット+legacy-* 42アクション・6プロファイル束縛**(M4 のアサーション前提)。

**Learnings**:
- L55: **プロセス生存≠フレームループ到達**。以後、実アプリ結合の完了判定は必ず framesObserved>0(機械値)で行う。無音モーダル(SDL_ShowSimpleMessageBox)はヘッドレス環境の偽陽性製造機。
- L56: **サードパーティ互換は「実装版数の一次ソース」で決める**。OC の GEN_INTERFACE 一覧 × openvr タグの Version 文字列を突き合わせて v1.8.19 を機械選定(推量ゼロ)。レビュアーは OC DLL 内の文字列(021/022 存在・026 不在)で独立裏取り。
- L57 [review-fix]: 記録タスクの数値は**生データから機械カウント**して書く(45と誤記→実測42)。目視集計は数え間違う。

**検証**: setup 冪等 rc=0 / framesObserved>0 をレビュアーも独立再実証(196) / PNG 目視 / dod 5項目全成立。レビュー needs-fix(数値)→修正→delta pass。

## 2026-07-16 — M4: OpenVR統合テスト系統(integration_openvr_test.sh + mjs)  [outcome: done]  (OpenVRマイルストーン 5/6, auto 4/5)

**What shipped**: OpenVR経路(hellovr_dx12→OpenComposite→レイヤー→Monado)の統合テスト新設(5efccce、301行、既存不触)。初回一発 **15 PASS/0 FAIL/1 SKIP + graceful PASS rc=0**、回帰2本(metasim Vulkan/monado D3D12) 17/17。graceful は WM_CLOSE(taskkill /F なし)で駆動できることを実証。

**Learnings**:
- L58: **SKIP明示設計が機能**。注入→アプリ反応(haptic)は OC の IVRInput マニフェスト・ルーティングが legacy 合成では届かず(3秒注入 haptics 0→0 実測)、理由つき SKIP+サマリ計上で記録。素通しゼロのまま不確実性を吸収し、M5/実ゲーム検証への引き継ぎ事項として doc 化。レガシー直読み世代のゲームなら届く見込み(要実測)。
- L59: hellovr@v1.8.19 は WM_CLOSE 後の自前 shutdown 後半で Segfault(レイヤーの graceful マーカー到達後=検証には無害)。ヘッドレス試験では「アプリ終了時の後始末クセ」はマーカー基準で切り分ける。

**検証**: レビュアーが実走再現(数値完全一致)+PNG目視+SKIP健全性(rc は FAIL のみ依存)を独立確認。pass。

## 2026-07-16 — M5: OpenVR対応のドキュメント反映(docs-only)  [outcome: done]  (OpenVRマイルストーン 6/6 完了, auto 5/5)

**What shipped**: README(対応状況表に OpenVR 行+テスト手順+GPLv3注記、+7行)とメモリ(openvr-support-direction を実装完了+実測知見5点へ)。レビューは一次ソース全数照合で pass(過大表示なし、45→42 修正後の値・M2訂正後の D3D12 値を正しく採用)。

**マイルストーン総括**: OpenVR対応(M0〜M5)を /h-loop auto 5 の連鎖で完走。コア(レイヤー/MCP)は M0 のバグ修正以外無改修 — アダプタ(OpenComposite)とテスト資産の追加のみで OpenVR アプリの観察・姿勢注入が動いた。「OpenXR 単一介入面」設計の検証成功。残宿題は実ゲームでの注入到達検証(backlog未登録 — 実ゲーム選定がユーザー判断のため)。

**Ship policy(継続)**: push 未実施(ユーザー確認待ち)。journal 未処理 learnings が L46〜L59 で 14 件规模 — **h-evolve 実行を強く推奨**(トークン都合で前回見送り済み)。

## 2026-07-16 — h-evolve 実行記録 (初回)

**L28〜L59 → processed (h-evolve 2026-07-16)**。32件を3資産へ蒸留(ユーザー承認済み A+B+C):
- `.claude/rules/review-checklist.md` — 横断レンズ6本 ← L29/L39/L40/L43/L53/L55/L57/L58 + M4 review観察
- `.claude/rules/native-win-interop.md` (paths: layer/src/**) ← L46/L47/L49 (M0実測)
- `.claude/rules/setup-scripts.md` (paths: scripts/**) ← L51/L56 + M1/M2/M3実測
- 見送り(提案D): CLAUDE.md への「検証の鉄則」追記はユーザー選択で不採用。
- 蒸留対象外と判定: L28/L32/L35/L36/L38/L41/L42/L44/L45/L48/L52/L54/L59(個別事実・journal/メモリ/
  設計書に既在で规則化の一般性なし)、L30/L31/L33/L34/L37(レンズ1に包含)、L50(ship policy として既に機能)。
- metrics 所見: 15ループ・rejections平均0.33・ドリフトなし → レビューキャリブレーション提案なし(健全)。
- agent-memory: h-reviewer の review-checklist-frame-capture は既に蒸留済み品質のため整理不要。

## 2026-07-17 — ship policy 更新

**push 実施済み**(ユーザー実行、リモート master = e018854 で全10コミット同期を ls-remote で確認)。
以後の ship policy: コミットは従来どおりループ内で実施、**push はユーザーの明示指示または `! git push` で行う**
(保護ブランチガードにより自動 push は不可 — これは意図された防御層)。
次タスク: backlog Deferred の R08→R09→R10→R17(ユーザー着手指示 2026-07-16 済み)。

## 2026-07-17 — R08 d3d11-msaa 出荷 (auto連鎖 1/4)

**成果**: D3D11 MSAA キャプチャ対応(コミット済み、squash 1件)。metasim MSAA 18/18 / monado 明示 SKIP /
回帰 17/17×2。レビュー pass(nit 2件: numstat 転記→修正済み、hr 併記→R15 へ追記)。

- L60: **Monado v25.1.0 の D3D11 コンポジタは MSAA スワップチェーンを受理しない**(xrCreateSwapchain →
  XR_ERROR_VALIDATION_FAILURE、実測)。Vulkan compositor への D3D11 MSAA 共有インポート非対応が濃厚。
  R09(D3D12)でも同挙動の可能性が高い — probe を先にやる。
- L61: **ランタイム能力 SKIP は「観測された拒否のみ変換」で実装**(integration_test.sh: rc!=0 かつ env>1 かつ
  ログに xrCreateSwapchain+VALIDATION_FAILURE)。静的な runtime×能力表を埋め込まない — 将来ランタイムが
  受理すればフルアサーションが自動で走る(lens 6 の実装形)。
- L62: **実行中 exe はリネームなら通る**(上書き cp は Device busy でも mv は成功) — kill 不能ゾンビが
  third_party の exe をロックした際の新回避策(hello_xr.exe.zombie へ退避→新 exe 配置→ゾンビ消滅後に削除)。
- L63: hello_xr の MSAA 盲点は sampleCount 定数だけでなく **RTV/DSV の次元と深度 SampleDesc も単一サンプル
  固定**(E_INVALIDARG で実測検出)。R09 の d3d12 プラグインパッチでも同族の盲点を先に疑う
  (graphicsplugin_d3d12.cpp の RTV/DSV/深度ヒープ)。
- 判断記録: h-evolve は未処理 learnings 4件(L60-L63)だが、ユーザー明示指示の R08→R09→R10→R17 連鎖を
  優先し**チェーン完走後に実施**(R09 以降で同族知見が増えるため蒸留効率も良い)。

## 2026-07-17 — R09 d3d12-msaa 出荷 (auto連鎖 2/4)

**成果**: D3D12 MSAA キャプチャ対応(squash 1件)。metasim 18/18 / monado SKIP(D3D11 と同型拒否) /
回帰 17/17×2。レビュー pass(findings なし)。

- L64: **Monado の MSAA スワップチェーン拒否は D3D11/D3D12 共通**(いずれも xrCreateSwapchain →
  XR_ERROR_VALIDATION_FAILURE、v25.1.0-646)。R08 の「観測された拒否のみ SKIP」機構がテスト無改修で
  D3D12 にも効いた — ランタイム非依存の能力 SKIP 設計の妥当性を裏付け。
- L65: hello_xr の D3D12 プラグインは RTV/DSV の MSAA 次元は stock 対応済みで、盲点は
  「override 不在(recommended=1 継承)」「深度 SampleDesc」「**PSO の SampleDesc**(RT と一致必須、
  D3D12 固有)」の3点だった。同族 API でも盲点の所在は別物 — L63 の「先に疑う」が有効だった。
- L66: D3D12 の常在状態キャッシュ(RESOLVE_DEST)は「使用後に同一コマンドリスト内で復元」で不変条件を
  保てる。エラー経路は「barrier 記録前に fail を返す」順序にすれば開いたリスト破棄でも安全
  (レビューで全経路確認済み)。

## 2026-07-17 — R10 d3d-hdr 出荷 (auto連鎖 3/4)

**成果**: D3D11/D3D12 の 16F HDR decode(squash 1件)。HDR E2E 18/18×全4組合せ(monado も HDR は受理 —
MSAA 拒否と対照的)。回帰 17/17×4+MSAA 18/18×2+単体 14/14。レビュー needs-fix→修正→pass。

- L67: **共有ヘルパーに新しい依存を足したら、それを自前コンパイルする全ターゲット(vr_agent_test)の
  ソース列も追従させる**。DoD が本体ビルドのみだとテストターゲットのリンク切れをすり抜ける —
  レビューで検出(lens 1 の変種)。今後の DoD には「vr_agent_test も green」を含める。
- L68: hello_xr の SelectColorSwapchainFormat は find_first_of で**ランタイム列挙順**に選ぶ —
  選好リストへの追加ではフォーマットを強制できない(env 時early return が正解)。
- L69: 16bit TYPELESS は UNORM/FLOAT の解釈が曖昧なため受理しない(8bit TYPELESS との非対称は原理的
  一貫: 8bit は族内でバイト配置同一、16bit はビット解釈が別物)。レビューでも妥当判定。

## 2026-07-17 — R17 d3d11-typeless 出荷 (auto連鎖 4/4、Deferred 全消化)

**成果**: D3D11 の 8bit TYPELESS 受理(squash 1件)。**R08→R09→R10→R17 のユーザー承認チェーン完走** —
D3D 系キャプチャの既知欠落(MSAA/HDR/TYPELESS)は全て解消、3バックエンドのフォーマット規則が統一された。

- L70: **metasim/monado とも D3D11 では TYPELESS スワップチェーンを列挙しない**(実測)。TYPELESS 受理は
  他ランタイム・実アプリへの備え(D3D12 と同規則)であり、現環境の E2E は mjs 防御 SKIP が正路。
- L71: **冪等マーカーは「挿入テキストに実在する部分文字列」であることを機械確認してから使う**。整列用
  スペースの差でマーカー不一致→再実行 assert 死(初回成功のため自己検証をすり抜け、レビューが read-only
  probe で検出)。受入基準「setup 2連続実行 rc=0+patched 0行」は今後の setup パッチ全部に使える。
- 検証マトリクス最終形: 既定/MSAA/HDR/TYPELESS × metasim/monado × D3D11/D3D12/Vulkan(既定のみ) が
  env フラグ(HELLO_XR_SAMPLE_COUNT/HELLO_XR_HDR/HELLO_XR_TYPELESS)+ランタイム能力 SKIP で回せる。
- **未処理 learnings: L60-L71(12件) → チェーン完走につき h-evolve 実行推奨**(前回判断どおり)。

## 2026-07-17 — h-evolve 実行記録 (2回目)

**L60〜L71 → processed (h-evolve 2026-07-17)**。12件を3資産へ蒸留(ユーザー承認 A+B+C、D は見送り):
- `.claude/rules/setup-scripts.md` — (5) ランタイム能力 SKIP 設計 ← L60/L61/L64/L70、(6) 冪等マーカー実在確認+2連続実行基準 ← L71
- `.claude/rules/native-win-interop.md` — 実行中 exe のリネーム退避デプロイ ← L62
- `.claude/rules/review-checklist.md` — lens 1 へ「共有ソースの新依存は全ターゲット追従+テストターゲットを DoD に」 ← L67
- 見送り(提案D): L66 の D3D12 常在状態イディオム(コード内コメントに既記載、ユーザー選択で不採用)。
- 蒸留対象外と判定: L63/L65(hello_xr パッチ固有・スクリプト内コメントに既記載)、L68(setup スクリプト内
  コメントに既記載)、L69(capture_d3d12 コード内コメントに既記載)、L70 は(5)に包含。
- metrics 所見: 19ループ・rejections 平均0.37(直近4=0.50)。直近2件の rejection はいずれも実バグの正当検出
  (R10 リンク切れ/R17 冪等性) — レビュー健全、ドリフト提案なし。plateau 0。
- agent-memory: h-reviewer が R17 教訓を review-checklist-patch-idempotency として自律蒸留済み(検出側)。
  本 h-evolve の setup-scripts (6) は書き手側の規則で相補 — 重複でなく役割分担、整理不要。

## 2026-07-17 — scripts-nits-batch (setup-curl-fsl + setup-hellovr-nits + openvr-test-graceful-promote)  [outcome: done]

**What shipped**: 3つの独立したスクリプト改善をワークツリー並列 fan-out で一括実行。
1. curl -sL → -fSL 統一(setup_monado.sh, setup_opencomposite.sh) — HTTP エラー早期失敗
2. MSBuild edition preflight(setup_hellovr.sh) — BuildTools/Community/Enterprise/Professional 4版自動探索 + `.git` clone guard
3. graceful teardown SKIP→FAIL 昇格(integration_openvr_test.sh) — PASS 実証済みのため退行検知有効化 + rc=1 反映 + scs[1] 寸法チェック(integration_openvr.mjs)

→ resolved 1b9aa0e。レビュー pass(findings なし)。

**L84 (procedure that worked)**: ワークツリー並列 fan-out が初めて正常動作。3エージェント × worktree isolation で独立ファイルの mechanical fix を同時実行。マージはコンフリクトなし。ただし cherry-pick 時に tracked .claude/ ファイルが混入する問題あり — `git checkout -- .claude/` で手動復元が必要だった。
**L85 (friction)**: ワークツリーブランチのマージ時、tracked .claude/harness/ ファイル(state.json 等)が cherry-pick に含まれて working tree の state を上書きする。worktree fan-out 後の join フローで .claude/ を除外する仕組みが欲しい。

## 2026-07-17 — doc-cleanup-batch (R21 stale-comments + layer-log-unused-string)  [outcome: done]

**What shipped**: コメント是正(3ファイル) + 未使用 include 除去(1ファイル)。
1. openxr_agent_layer.cpp: 冒頭コメントを現状反映(全バックエンド実装済み、単一 XrInstance 設計)
2. capture.cpp: stale「not implemented error」記述を除去
3. capture_backends.h: 「Vulkan stays inline in capture.cpp」→ capture_vulkan.cpp 分離済みに修正
4. layer_log.h: 未使用 `#include <string>` 除去(ビルド green + 64/64 テスト PASS で検証)

→ resolved 99b991c。レビュー pass(2 notes、non-blocking)。

**L86 (reviewer finding, not a defect)**: capture.cpp:516 のランタイムエラー文字列 "not implemented" はコメントではなく正当なコード。コントラクトの grep 検証コマンドがコメントと区別できなかった。DoD チェックは grep のスコープを明確にすべき。
**L87 (procedure that worked)**: 小粒タスク(コメント数行+include 1行)では、ワークツリー並列の cmake configure オーバーヘッドが作業量を上回るため、inline sequential の方が効率的。scripts-nits-batch(ビルド不要の script 変更)とは異なるパターン。

## 2026-07-17 — R19 layer-state-split  [outcome: done]

**What shipped**: control_channel の公開状態ストア(5 mutex+globals グループ)を layer_state.{h,cpp} に分離。control_channel は transport+protocol 専任に。型5個 + API 15個超を移動。ClearAllStickyPoses / GetStatus / GetHapticLog を追加して直接 mutex アクセスを解消。消費者9ファイルの include を追従。

→ resolved 9b8efda。レビュー needs-fix(dead include 3件)→修正→pass。

**L88 (recurring finding, R04/R05 と同型)**: API リネーム（ControlChannel* → LayerState*）時に sed で関数呼び出しは置換したが #include 行は元ファイル名のまま残った。コメント部分がリネーム済みで見た目 OK に見えるが、実際は dead include + transitive 依存で成立している脆い状態。API リネームを伴う move-only リファクタでは、sed 後に `grep -rn '#include "旧ヘッダ"'` で全数確認すべき。
**L89 (procedure that worked)**: 大きめ move-only リファクタ(13ファイル +306/-324)でも sequential 実行で品質を保てた。レビュアーが dead include(Lens 1, R04/R05 再発パターン)を1回で検出し、修正は mechanical。

## 2026-07-17 — R20 server-ts-split  [outcome: done]

**What shipped**: mcp/src/server.ts (815行、21ツール) を client.ts / math.ts / tools/{observe,input,pose,head,recording}.ts に分割。server.ts は 19行(import + 登録 + transport のみ)に。

→ resolved 63609d2。レビュー needs-fix(コメント40行欠落、review-checklist lens 1)→復元→pass。

**L90 (recurring finding, lens 1 三度目)**: TS の move-only 分割でもコメントが欠落(C++ の R04/R05/R19 と同型)。言語に関係なくコード移動時のコメント追従は普遍的な罠。Write で新ファイルを作成する際に元コメントを含めて verbatim コピーする習慣が必要。

## 2026-07-17 — unit-tests-registry-capture  [outcome: done]

**What shipped**: HandTopFromBindingPath (action_registry) を inline 化 + EyeToIndex/DominantEyeIndex (capture.cpp anonymous ns) を capture_common に抽出し、テスト14件追加(合計78件)。

→ resolved 0728461。レビュー pass(findings なし)。

**L91 (procedure that worked)**: テスト対象のリンク依存が重い場合、関数を header-only inline 化(HandTopFromBindingPath、R12 パターン)または既にリンク済みの TU に移動(EyeToIndex → capture_common)することでテストターゲットへの追加リンクなしでテスト可能にする。R11 の「スコープ外」判定を覆す正当な手法。

**L84〜L91 → processed (h-evolve 2026-07-17)**。8件を2資産へ蒸留(ユーザー承認済み A+B):
- `.claude/rules/review-checklist.md` lens 1 強化 — API リネーム後の grep 全数確認 + Write 時コメント verbatim コピー(言語非依存) ← L88/L90
- `.claude/harness/backlog.md` — worktree-join-exclude-claude ← L85
- 蒸留対象外: L84(記録のみ・初動作報告)、L86(タスク固有)、L87(判断基準・汎化不要)、L89(記録のみ)、L91(手法知識・規則化不要)
- metrics 所見: 29ループ・rejections 平均0.34(直近5=0.40)。直近2件の rejection はいずれも lens 1 の既知パターン(dead include/コメント欠落) — レビュー健全、ドリフト提案なし。plateau 0。
- 2026-07-18 (steamvr-driver VD1/VD2 準備中): **未検証知識を事実として文書化する失敗が同一セッションで3回再発**(H1 役割割当を仕様書に「決定」と記載 / README で Monado CA を実測と真逆に記載+開発中設計を実装済みと混在 / VD2 で H3 偽装を検証装置と明示せず実装)。ユーザー指摘 2回。恒久対策として CLAUDE.md に「検証済みと未検証を混ぜない」節を新設(仮説ラベル+検証方法の義務化、README 3段階凡例、判別法=リポジトリ内の実測ログを指させるか)。h-evolve 候補: review-checklist に「文書中の技術主張は実測ログ/一次ソースを指せるか」レンズ追加を検討。
  → processed (h-evolve 2026-07-18): review-checklist.md lens 7「技術主張の検証可能性」新設 + CLAUDE.md「検証済みと未検証を混ぜない」節。出典=2026-07-18 の3連発(H1/Monado CA/H3)。

## 2026-07-22 — h-evolve 実行記録 (4回目) + PlaySpectra asset drift 是正

**入力の状態**: journal は 2026-07-18 で全項目 processed・未処理なし。metrics.jsonl は 2026-07-17
(29ループ) 以降エントリなし = **PlaySpectra 改革 (7/19〜22) は h-loop 外 (cron/手動) で実施され
journal/metrics 未記録**。蒸留対象は本セッションの実測 friction + 既存資産の drift。

**適用 (P1・stale asset 是正)**: M0.5 改名 (2026-07-19) の DoD「grep 0件」が **.claude/rules +
agent-memory で未達**だった (vr_agent_layer/vr_agent_test/vr_agent_openvr が残存)。根因は ripgrep が
gitignore の .claude/ をスキップし rename 時の確認 grep が false 0 を返したこと ([[review-env-sandbox-quirks]]
L15 の既知癖)。全て ground truth (layer/CMakeLists=playspectra_layer/playspectra_test、
scripts=playspectra_openvr_/playspectra_integration、capture.cpp の vr_capture_N.png は改名対象外の出力名)
と照合し是正 → 明示パス grep 0件を確認。対象: native-win-interop.md(L27/L30)、review-checklist.md
(vr_agent_test×2)、setup-scripts.md(例パス)、review-checklist-frame-capture.md(memory・4トークン)。

**提案 (P2〜P4・要ユーザー承認、未適用)**:
- P2: review-checklist lens 3 に「grep 完了確認は gitignore 対象 (.claude/ 等) も明示パスで —
  ripgrep は gitignore をスキップし false 0 を出す」を追記。出典=本 P1 の根因。
- P3: review-env-sandbox-quirks memory に「Read tool が捏造/stale なファイル内容を返すことがある —
  doc/impl の相違を報告/編集する前に grep で実文字列を確認」を追記。出典=本セッション2件
  (playspectra_mcp.py/tools/README.md の初回 Read が架空内容→grep で否定→誤 finding 寸前)。
- P4: CLAUDE.md に「.claude/ はユーザー global gitignore 対象 — 設計書は git add -f で追跡し、
  追跡文書は非追跡パスへリンクしない」を追記。出典=本セッション (正典 architecture 等5点が未追跡で
  README リンク切れ、526de7d9 で是正)。

**metrics 所見**: rejections 平均 ~0.31 (29ループ)・plateau 0・新規ループなし → review 健全・ドリフト提案なし。

**訂正(append-only)**: 上記 P2〜P4 は「提案・未適用」と書いたが、ユーザー4度目の授権
(「機械的に進めれるところは進める…質問を控え…」)を承認とみなし **2026-07-22 に適用済み**:
review-checklist.md lens 3(gitignore/grep false-0 注意)、review-env-sandbox-quirks memory
(Read tool の捏造/stale 内容)、CLAUDE.md「リポジトリ運用の注意(.claude/ の git 追跡)」節。
= h-evolve 4回目 完了(P1〜P4 全適用・二重蒸留防止マーク済み)。

## 2026-07-22 — 文書claim の実走再検証 (このセッション・Windows 実GPU)

「検証済み/未検証を混ぜない」規律の最深部として、文書の ✅ を**この Windows マシンで実走**して独立再現した
(doc-trust → verified-here-now)。すべて claim と一致:
- **自己完結(ハードウェア/ライブMonado/pip 不要)**: gtest `layer/build/playspectra_test.exe` **78/78** /
  `waitfor_test.py`(MockAdapter) **9/9** / `capture_assert_test.py`(Mock×2) **7/7**。
- **ライブ Monado(build-win の monado-service.exe/monado-cli.exe・:52702・null compositor)**:
  probe=XRT_SUCCESS(HMD+L/R 3デバイス) / `server --verify` **9/9** / `record --verify` **5/5** /
  frame_test **10/10** / reset_test **20/20** / multiobs **中核8/8**(haptics broadcast 3件は host app 前提で
  期待どおり FAIL=回帰でない)。各制御セマンティクステストは fresh service で実行(writer切断の状態保持による順序汚染回避)。
- 未実行(環境依存): 実アプリE2E(run_hello_xr_monado.sh)は openxr_monado-dev.json 未生成(build-win に openxr_monado
  ターゲット未ビルド)+hello_xr 要 / mcp_verify は要 venv+mcp。回帰でなく未ビルド/未セットアップ。
- サービスは1呼び出し内でライフサイクルを閉じ taskkill で後片付け(ゾンビ0を tasklist で確認)。

## 2026-07-23 — h-evolve 実行記録 (5回目) + テストカバレッジ/CI 着手 (このセッション・h-loop 外)

**このセッションの作業** (ユーザー主導・h-loop 外なので metrics 未記録):
- **A(driver/scripts 整理コミット)**: .gitignore の素パターン整理(/build/・driver/build/・bin/ に錨、素の
  build/.claude/harness 除去) + .gitattributes/.mcp.json/.vscode 追跡 (cbeb7e9d4)。SteamVR driver スケルトン
  driver_playspectra (vd1/vd2 土台・未ビルド/未検証・SteamVR実機不在) + 入力プロファイル2点 (1e3466320/9a35e9be9)。
  VRDevApp 検証ハーネス scripts/run_vrdevapp.sh+vrdevapp_client.mjs (70b4cd201)。
- **テストカバレッジ item 1**: mcp/src/math.ts の node:test 42件 (5f2e00209)。node:test+tsx 採用・tsconfig で
  *.test.ts を build 除外。ローカル 42/42。
- **item 6 CI**: .github/workflows/ci.yml (mcp=ubuntu/layer=windows) + layer に /utf-8 (2026e1d7a)。push → CI は
  **課金ブロックで起動せず** (run 29966605494・private repo)。→ ローカル代替 scripts/run_all_tests.sh 120テスト green (c7218ca7f)。

**蒸留 (h-evolve 5回目・ユーザー承認 P1/P2/P3、P4見送り)**:
- P1 → auto-memory `ci-and-test-gate` 新規: 課金ブロック現況 + ローカルゲート + mcp テスト規約。 → processed
- P2 → `.claude/rules/build-cmake.md` 新規(paths: **/CMakeLists.txt): 新規MSVCターゲットは /utf-8 必須(2回発生:
  Monado cdd429f88 / layer 2026e1d7a)。配置は既存rule(scripts/**・layer/src/**)が CMakeLists 編集で発火しない
  ため専用rule=global最適と判断(ユーザーが global最適を条件に承認)。 → processed
- P3 → review-checklist.md lens 8 追記: git status/.gitignore の可視性を明示コマンドで確認(素パターン全階層マッチ・
  未追跡ツリー畳み込み)。出典 cbeb7e9d4 / 9a35e9be9(resources/ 露出 near-miss)。 → processed
- P4(purpose-first の feedback memory)は**見送り**: 根拠が本セッション1回の指摘のみで状況固有、恒久規則化は早計
  (メタ認知点検で自己格下げ→ユーザーも見送り承認)。
- 記録のみ(非蒸留): loader_test/`-E loader_test` の保険は未検証前提(fresh build 拒否で fresh configure の
  BUILD_TESTING=OFF シード挙動を未確認)→ commit/コメントに明記済み・fresh CI/ビルドが走ったら再評価。

**metrics 所見**: 29ループ不変(平均 rejections ~0.31・plateau 0・新規ループなし = 本セッションは h-loop 外)
→ review 健全・ドリフト提案なし。
