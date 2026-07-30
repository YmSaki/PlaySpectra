---
slug: vulkan-selftest-discarded
label: LATER
conditions: [L1, L2]
source: .claude/audit-meaningless-code.csv (2026-07-25 whole-tree audit, M27; adversarial verification verdict PLAUSIBLE)
task: none
date: 2026-07-28
---

# HalfFloatSelfTest() result discarded as dead code; regression already gated elsewhere

## Measurement

Code still present: `layer/src/capture_vulkan.cpp:683-684`

```cpp
static const bool kHalfTested = HalfFloatSelfTest();  // one-time regression check
(void)kHalfTested;
```

Verified via read: the static variable is computed once, result cast to void and discarded. Log output goes to file only (layer_log.cpp writes to `$PLAYSPECTRA_LOG` or `%TEMP%`), no test or script parses the file for 'HalfFloatSelfTest: FAIL'.

Verified via grep: test_pixel_convert.cpp:9-93 contains 10 gtests (HalfToFloat_Zero through HalfToFloat_NaN) using the identical HalfToFloat function from pixel_convert.cpp:16 that the layer calls. These tests are wired into scripts/run_all_tests.sh:50-53 via `cmake --build ... --target playspectra_test` and `ctest --test-dir ... --output-on-failure`, with rc propagation (failure halts the suite).

## Conditions

| id | answer | basis |
|---|---|---|
| N1 | NO | No current task; N conditions cannot be YES |
| N2 | NO | No current task; N conditions cannot be YES |
| N3 | NO | No current task; N conditions cannot be YES |
| N4 | NO | No current task; N conditions cannot be YES |
| B1 | NO | No planned work depends on the premise that HalfFloatSelfTest is HDR's runtime gate — test_pixel_convert.cpp's 10 gtests are already integrated into the automated suite and will catch bit-shuffle regressions |
| B2 | NO | No planned design (SteamVR Adapter, MCP migration, OpenVR, etc. in backlog.md) depends on this self-test's presence or correctness |
| L1 | YES | The mechanism (Log call inside HalfFloatSelfTest via Layer logging) produces prose output to a log file that no automated test, script, or mechanism reads to decide anything — the failure mode is silent logging, not a gating failure |
| L2 | YES | Workaround already applied: layer/tests/test_pixel_convert.cpp:9 carries 10 gtests (HalfToFloat_Zero, _NegativeZero, _One, _Half, _NegativeOne, _MaxNormal, _SmallestSubnormal, _Inf, _NegInf, _NaN) over the same pixel_convert.cpp:16 HalfToFloat function; these are wired into scripts/run_all_tests.sh:50-53 and execute with rc propagation |
| L3 | NO | No current task to define a defect population; standalone finding |
| D1 | NO | Not recorded in triage system (only in audit CSV source document, which is not a triage record) |
| D2 | NO | Code reproduces: capture_vulkan.cpp:683-684 still contains the discarded result |

## Why this label

The runtime self-test is dead as a gate (nothing acts on its result) but not meaningless as a diagnostic (it logs). The regression it nominally guards is already covered by comprehensive unit tests that ARE integrated into the automated suite via ctest, so the functional defect (undetected bit-shuffle regressions) does not exist. The problem is cosmetic: misleading impression of runtime gating where the real gating is in the unit-test suite.

Two courses of action were identified in the audit (both acceptable): leave the diagnostic as-is (harmless insurance against toolchain-specific float flags diverging between libm and MinGW layer DLL), or delete it and rely on the unit-test gate. The decision is deferred to a future session where a task owns this code.

## What the caller must do

If a future session picks up this cosmetic issue, add this triage record to backlog.md § Open or decide it is nice-to-have and close it as-is.
