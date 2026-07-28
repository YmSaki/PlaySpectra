---
slug: comment-docstring-drift
label: LATER
conditions: [L1]
source: .claude/audit-meaningless-code.csv (2026-07-25, audit findings M14 and M23; verified by adversarial verifier)
task: none
date: 2026-07-28
---

# Comment and docstring drift from implementation

Two instances of documentation that references or describes code state that no longer matches the actual implementation.

## Measurement

**M14** — `layer/src/capture.cpp:226` comment reads:
```c
// Shared color-capture output path (same numbering/dir the Vulkan path uses at line ~460).
```

Verified: Line 226-231 shows `NextColorCapturePath()` is defined here, and the Vulkan backend calls this function (rather than having its own duplicate implementation at ~460, which no longer exists).

**M23** — `tools/playspectra_server.py:16` module docstring claims:
```python
- Interpolation (position lerp, orientation slerp, stick ramp) lives in the Server
```

Verified: `walk_forward:278`, `strafe:285`, and `set_trigger:292` all follow the pattern `lambda t: ctrl.inputs.__setitem__(..., constant_value)` — the parameter `t` is received but ignored. Only position lerp and orientation slerp use `t` for interpolation; stick value ramp is not implemented. Repository-wide grep for "ramp" returns only this docstring line (no implementation exists).

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running (state.json: active=false, phase=done). N conditions do not apply. |
| N2 | NO | No enforcement mechanism trips; documentation does not block task completion. |
| N3 | NO | These are comments/docstrings, not silently-failing decision code. |
| N4 | NO | Work is not blocked by commits/tests; no current loop to stop. |
| B1 | NO | Next planned work (Monado measurement, SteamVR Adapter G3) does not depend on these specific statements being accurate. M23's actual specification requirement ("補間列の生成も Server 側") is met by `_stream()` emitting snapshots. |
| B2 | NO | No planned design depends on verifying these prose items. |
| L1 | YES | Both M14 (comment) and M23 (docstring) are prose-only documentation. No mechanism reads these to decide runtime behavior. Readers may be confused, but no automated test or control-flow depends on their accuracy. |
| L2 | NO | Not applicable; no workaround in place. |
| L3 | NO | No current task to compare population against. |
| D1 | NO | These are in the audit CSV (findings), not a prior triage record. Audit CSV documents what was found; triage records track actionability. |
| D2 | NO | Both problems verified to still exist by direct file reading. |

## Why this label
Both items are documentation drift — comments and docstrings that describe code states that no longer match the implementation. They are purely informational, affecting readability and maintainability, not blocking any work or creating runtime defects. This is a LATER backlog item suitable for a pass-through cleanup or a doc-only PR.

## What the caller must do
Add to `.claude/harness/backlog.md` under the "Open — P3" section (alongside other doc-drift items M12) as a backlog entry, or group M14/M23 into a single fix task once higher-priority work (M01/M02/M07/etc.) clears. Fixes are trivial: (1) M14 — replace line reference with symbol reference: "the Vulkan path calls this from capture_vulkan.cpp:704"; (2) M23 — update docstring to remove "stick ramp" or reword to "stick hold/release list generation" to match implementation.
