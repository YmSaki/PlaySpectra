---
slug: noca-override-single-profile
label: DROP
conditions: [D1]
source: `.claude/audit-meaningless-code.csv` (2026-07-25 whole-tree audit, finding M28)
task: none
date: 2026-07-28
---

# Non-CA fallback ignores topLevelUserPath in interaction profile override

## Measurement
Not run; verified by code inspection.

Layer/src/hooks_action.cpp:164–166 shows the non-CA fallback override for `xrGetCurrentInteractionProfile`:
```cpp
if (emulated != XR_NULL_PATH &&
    (XR_FAILED(r) || profileState->interactionProfile == XR_NULL_PATH)) {
  profileState->interactionProfile = emulated;
  r = XR_SUCCESS;
}
```

The `topLevelUserPath` parameter (line 155) is passed through to the runtime but never consulted by the override logic. The override fires for any path, substituting the single global emulated profile regardless of whether the device actually represents that path (e.g., `/user/head`, `/user/gamepad`, `/user/treadmill`). Additionally, any runtime failure (including `XR_ERROR_PATH_UNSUPPORTED`, `XR_ERROR_HANDLE_INVALID`, `XR_ERROR_SESSION_LOST`) is rewritten to `XR_SUCCESS` with the fabricated profile.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running |
| N2 | NO | No current task running |
| N3 | NO | No current task running |
| N4 | NO | No current task running |
| B1 | NO | Planned work (Monado real-HMD measurement, G3 SteamVR Adapter) does not depend on non-CA fallback correctness as a stated premise. The fallback is legacy infrastructure for runtimes without `XR_EXT_conformance_automation` support; the CA path is the primary focus. |
| B2 | NO | No unverified capability in planned design depends on this. |
| L1 | NO | This is actual C++ code that executes in the layer's non-CA fallback path, not prose or documentation. |
| L2 | NO | No workaround is applied. The problem describes an uncompensated fidelity gap. |
| L3 | NO | No current task to define defect population; issue is moot. Conceptually this is legacy non-CA fallback infrastructure, separate from the main CA-based work stream. |
| D1 | YES | Already recorded in `.claude/audit-meaningless-code.csv` line 73 as finding M28. Also cited in `.claude/harness/backlog.md` line 51–52. |
| D2 | NO | Code defect (mechanical oversight: parameter not consulted). Reproduces every time the code path executes; not a flaky test. |

## Why this label
This finding is already recorded in the audit CSV (M28) and cited in the backlog. The duplicate entry should consolidate with the existing audit finding rather than create a parallel track.

## What the caller must do
Close this as a duplicate of the existing M28 entry in `.claude/audit-meaningless-code.csv` (line 73). No new triage record is needed.
