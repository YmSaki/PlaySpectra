---
slug: setup-extract-cannot-fail
label: BEFORE-NEXT
conditions: [B2]
source: .claude/audit-meaningless-code.csv (2026-07-25 whole-tree audit, finding M08; verified by adversarial verifier)
task: none
date: 2026-07-28
---

# setup_monado.sh extraction succeeds silently without verifying success

## Measurement
Not run: this is a code review finding, not a reproducible runtime failure. The defect is present in `scripts/setup_monado.sh:16-30`.

Script extracts files from CI artifact zip and counts them into `n`, but:
- Never checks `if n == 0` before declaring success
- Never verifies that `${DEST}/openxr_monado.json` exists before writing VERSION.txt
- Prints identical success messages regardless of extraction count

Downstream: `integration_test.sh:26` and `integration_openvr_test.sh:15` set `XR_RUNTIME_JSON` to the target json without checking file existence, leaving stale or missing Monado trees invisible.

## Conditions
| id | answer | basis |
|---|---|---|
| N1 | NO | No current task running; all N conditions auto-NO per instructions |
| N2 | NO | No current task running |
| N3 | NO | No current task running |
| N4 | NO | No current task running; problem describes potential failure mode, not a blocker right now |
| B1 | NO | Problem reveals verification gap, not a false premise. Premise "setup_monado.sh succeeds" is unverified but not yet proven false |
| B2 | YES | Planned work (a) "measure whether Monado path still passes while real HMD is running" depends on correct Monado setup. The setup script claims extraction success (line 29: `print(f'extracted {n} files...')`) but does not verify it — no count check, no file existence assertion. This is an unverified claimed capability. |
| L1 | NO | setup_monado.sh is executable code, not prose-only; extraction verification is a mechanism decision |
| L2 | NO | No applied workaround; integration tests do not check file existence before use |
| L3 | NO | No current task, so no "remainder of current task's population"; problem is first-time audit finding M08 |
| D1 | NO | Recorded in audit CSV M08, but not yet in triage system; CSV entry is not a triage record |
| D2 | NO | Not applicable; this is a code review defect (missing check), not a reproducible failure |

## Why this label
Planned work (a) depends on "Monado is correctly set up", which is currently claimed but unverified. The setup script satisfies B2: a planned design depends on it, and the claimed capability (successful extraction) has no verification check. This must be settled before the measurement work proceeds.

## Caller's disagreement (recorded, label left as issued — 2026-07-28)

The label stands as written; this section records why the caller did not route it as BEFORE-NEXT.

B2 was answered YES on the premise that planned work (a) "measure whether the Monado path still passes
while the real HMD is running" depends on this script. Checked against primary sources, that premise is
false:

- `setup_monado.sh` installs into `third_party/monado/` from a GitLab CI artifact. The planned work runs
  against `runtime/monado-playspectra/build-win/` — a locally built tree that `lib_monado_stack.sh:19-20`
  pins as `MSTACK_SVC` / `MSTACK_MANIFEST`.
- `grep -l setup_monado scripts/*` matches only `integration_openvr_test.sh` and `setup_hellovr.sh`.
  Neither `run_vrapp_monado.sh` nor `run_hello_xr_monado.sh` — the harnesses the planned work uses —
  references it.
- Those two harnesses passed 25/25 and 20/20 on 2026-07-25, so "Monado is correctly set up" for that path
  is measured, not merely claimed, which is what B2 asks about.

The defect is real and stays open at P2; its blast radius is the OpenVR/hellovr harnesses, which the
planned work does not touch. Verifying which setup path the planned work actually uses is the caller's
job, not the triage agent's — the agent applies the condition table to the text it is given, and the text
did not say which Monado tree was involved.

## What the caller must do
Add to `.claude/harness/plan.md` Findings for the next task (measurement or G3), or to `.claude/harness/backlog.md` as a BEFORE-NEXT entry. Suggested fix: (1) add `if n == 0: sys.exit(...)` after line 29, (2) add `[ -f "$DEST/openxr_monado.json" ] || exit 1` after line 30 and before line 32 success messages.
