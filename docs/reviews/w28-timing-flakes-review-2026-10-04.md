# W28 timing-flakes peer review — 2026-10-04

**Verdict: CHANGES-REQUIRED** for `tests/w28-timing-flakes` at `9828f1b3` (cherry-picked as `1c12379b` onto `origin/master` `cf616c3d` on `review/w28-timing-flakes`). Scope: nostrc-rnql, -d9r6, -i3wq, -12m0, -h5zs. Review tests and edits were confined to `/tmp/rv-w28-timing-flakes`; no checkout, reset, rebase or push was run in the main or author worktree.

## Findings

1. **Blocker — the gate's own required static-check test fails.** `scripts/linux-gate-smoke.sh:214-215` adds `ctest -N -L '^perf$'`, but the scripted CTest in `scripts/test-linux-gate-smoke.sh:38-44` rejects invocations without `--output-on-failure` and has no show-only/label handling. `bash scripts/test-linux-gate-smoke.sh` exits 1 at its first case (`FAIL: a clean run failed`). `.github/workflows/static-checks.yml:31-33` runs that self-test, so this branch makes required CI red even when the real smoke gate passes. Update the mock and add cases proving a perf failure blocks, is not retried, and a missing expected perf test is not silently accepted.

2. **High — the perf registration disappears on a fresh default configure, and hosted CI does not compensate.** `nostr-gtk/CMakeLists.txt:398,459-468` nests the new test behind `BUILD_APPS`; top-level `CMakeLists.txt:420,429` enters `nostr-gtk` *before* declaring that option. My first clean macOS configure (without an explicit `-DBUILD_APPS=ON`) registered **zero** bind-latency/perf tests; rerunning CMake with `-DBUILD_APPS=ON` registered both. The Linux gate's reused volume happened to have `BUILD_APPS=ON` cached and ran one perf test; a fresh volume need not. `scripts/linux-gate-smoke.sh:216` treats a zero perf count as success. Hosted `nostrc-ci.yml:89,96` and `groundhog-ci.yml:57,275` explicitly disable `BUILD_NOSTR_GTK`, so neither runs it. A large bind regression can therefore pass a fresh gate and hosted CI without executing the assertion. Define options before subdirectories, assert the expected perf registration in the gate, and add a CI path that actually builds/runs it.

3. **High — the new CPU budgets do not reject a >10× bind-work regression.** `nostr-gtk/tests/test_bind_latency_budget.c:43-51,223-230` allows 500 ms scroll CPU and 20 ms per callback. A baseline verbose perf run reported 95 scroll binds, 1.4 ms scroll CPU, 0.2 ms bind CPU total and 0.005 ms maximum callback. I injected 100,000 volatile arithmetic operations in every `on_bind` callback; the maximum became 0.135 ms and total 5.4 ms (both about **27×** baseline), yet `test_nostr_gtk_bind_latency_perf` still passed (scroll CPU 7.0 ms). The functional bind-count assertion is sound, but these very loose absolute budgets do not preserve the old performance-regression signal. Calibrate an actionable bound or add a deterministic work/relative regression check; keep the isolated serial registration.

4. **Medium — the inbox deadline masks its intended failure diagnostic with a GLib critical.** `gnome/groundhog/tests/app/gh-test-signer.h:113-121` calls `g_source_remove(timer)` even when the one-shot timeout callback has fired and returned `G_SOURCE_REMOVE`. Forcing `inbox_live` false made `groundhog-e2e-dm` fail after about 30 s with `GLib-FATAL-CRITICAL: Source ID ... was not found when attempting to remove it`, not `test_e2e_dm.c:517`'s relay-state diagnostic. The test still fails, but the promised bounded diagnostic is lost precisely on the regression path. Only remove a still-attached source, or own/destroy a `GSource` safely; add a timeout-path test.

## Regression-injection audit

| Changed test | Regression injected in review worktree | Result |
| --- | --- | --- |
| GTK bind latency (functional and perf registrations) | Suppressed `bind_count++` so no row is recorded as bound | Both registrations failed at `bind_count > binds_before_scroll` (0 > 0). The separate 27×-work injection **passed** perf, as above. |
| `concurrency_channels` | Sender published `ctx->value - 1` | Failed `wrong value` after the receive; the post-`pthread_join` completion check remains non-vacuous. |
| `groundhog-e2e-dm` | Forced `inbox_live` to remain false | Failed after its deadline, but via the invalid-source critical rather than the intended diagnostic. |
| `marmot_test_large_commit_bound` / adopted-commits | Made `marmot_commit_judge` accept invalid commits, then separately gave `MLS_PROPOSAL_ADD` the wrong application order | Failed `refused: 0 (success)` and `sort type at 0`, respectively; exact parser-bound assertions remain present. |
| `relayd_session_relay_storage` | Sent `SIGSTOP` instead of `SIGTERM` to simulate non-exit, with a 1 s review-only deadline | Failed `daemon did not exit cleanly`; the test killed/reaped its child. |

All mutations were restored and their test targets rebuilt. The final seven touched CTests passed together. The D-Bus production change in `apps/relayd/src/session/session_dbus.c:471-488` is logically sound for the observed early-stop race: the quit source is attached to the daemon thread's context and cannot dispatch until that context runs, while `join` keeps the loop and source data alive. The storage test does not specifically exercise stopping *before* `g_main_loop_run`; a focused start/stop stress test would improve coverage, but I found no production-code correctness defect in this patch.

## Verification and version assessment

- macOS clean Ninja build with `BUILD_GROUNDHOG=ON`: **passed**. First clean configure registered no GTK bind tests; explicit `-DBUILD_APPS=ON` reconfigure then built and ran both.
- Touched CTests after restoration: **7/7 passed**. `python3 scripts/check-unsequenced-args.py` and `git diff --check`: passed.
- `scripts/linux-gate.sh /tmp/rv-w28-timing-flakes`: **passed**; 462 functional smoke tests with one unrelated `groundhog-mls-service` first-run failure that passed its allowed isolated rerun, then **one perf test** passed serially in the cached volume.
- `scripts/linux-gate.sh --sanitizers /tmp/rv-w28-timing-flakes`: **60/60 passed**.
- `bash scripts/test-linux-gate-smoke.sh`: **failed** at the first clean-run case (Finding 1).
- Version policy: no bump for test-only changes to nostr-gtk, Groundhog, libmarmot, or libgo; relayd is unversioned, so its shipped D-Bus bug fix has no authoritative version source to bump. Gate scripts are not a versioned component. The test-hook symbols are in internal libmarmot headers and conditional on `MARMOT_TEST_HOOKS`; no installed public API change was found.

**Required before approval:** fix the static-check self-test; make the perf registration mandatory on clean gate/CI builds; tighten the perf regression signal; repair the timeout-path source cleanup.
