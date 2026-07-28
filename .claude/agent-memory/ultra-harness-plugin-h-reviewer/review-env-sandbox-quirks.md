---
name: review-env-sandbox-quirks
description: Bash-tool sandbox quirks in this repo that can produce FALSE findings during review (empty ls output, curl probes failing rc=43)
metadata:
  type: project
---

Environment quirks of the review sandbox (observed 2026-07-16, M1 review).

**Why:** these nearly caused a false "files missing" finding — `ls -l third_party/opencomposite/` returned "(empty)" twice while the files demonstrably existed (`stat` and `cat` on the same paths succeeded).
**How to apply:** before filing a "file does not exist" finding, cross-check with `stat`/`cat`/`test -f` — do not trust an empty `ls` alone. Related: [[review-checklist-frame-capture]].

- `ls` output is sometimes swallowed entirely by the sandbox (returns empty even for non-empty dirs). `stat -c '%n %s bytes' <paths>` is reliable.
- **Never run runtime proofs as one compound Bash line with `&`.** `&` binds the whole preceding `&&`-chain into ONE background subshell, so `VAR=x && export Y=z && server &; client` leaves VAR/Y unset in the foreground where the client runs — the client silently runs with the wrong env and "ALIVE after Ns" becomes a false positive (observed M2 review 2026-07-16: hellovr ran without XR_RUNTIME_JSON, monado log had zero client lines). Write the proof as a script file in the scratchpad and `bash script.sh` instead.
- Grep tool (ripgrep) respects .gitignore, so searching for content in `.claude/harness/` or other ignored paths from repo root returns "No matches" even when present — pass the ignored directory as an explicit `path`.
- Bash-tool `grep` is shimmed by an `rtk` wrapper ("rtk: Failed to resolve 'rg' via PATH") whose output format is nonstandard (`[file] N (1):` blocks, line numbers stripped/re-mapped). The matches are real, but don't parse counts/line-numbers from it — use the Grep tool (with explicit `path` for ignored dirs) when the exact location matters. Observed M4 review 2026-07-16.
- Ad-hoc `curl` probes to arbitrary URLs may fail with `curl_rc=43` / "curl 000" inside the sandbox even though the same host works from a script that was run as a verification command. Don't treat a failed probe as evidence about network behavior; fall back to documented behavior (e.g. curl without `-f` exits 0 on HTTP 4xx/5xx and writes the error body to the output file).
- **Paths handed to `python` must be Windows-native (`C:\...`), not the Git Bash `/c/...` form.** The Bash tool is Git Bash but `python` is a native Windows build, so `/c/Users/...` reaches it as a literal relative path and it fails with `FileNotFoundError` on a file that plainly exists. Observed 2026-07-25 reading a workflow result file. Use `r'C:\...'` in `-c` snippets, or `cygpath -w`.
- **The scratchpad directory is volatile — it can vanish mid-session.** Observed 2026-07-25→28: the session scratchpad was removed when the date rolled over, and a build redirect into it failed with "No such file or directory" (the harness prompt says it already exists, which is true only until it isn't). `mkdir -p` before writing, and keep anything that must survive (reports, CSVs, generated assets) inside the repo rather than the scratchpad.
- **The Read tool has returned fabricated/stale file content** (non-monotonic line numbers, invented functions/strings absent from the file). Observed 2026-07-22: first Reads of `tools/playspectra_mcp.py` and `tools/README.md` returned fictional structures (a nonexistent `build_server()`/`--selftest`; a nonexistent "(7 tools)" line) — nearly filed as real findings. Before reporting a doc/impl discrepancy, or building an Edit `old_string` from a Read, confirm the exact text with the Grep tool (ground truth). A Read whose line numbers are not strictly monotonic is the tell.
