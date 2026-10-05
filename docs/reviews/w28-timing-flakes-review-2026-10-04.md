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

---

## Final re-review addendum — `a6cb991f`, 2026-10-04

**Final verdict: CHANGES-REQUIRED** for one new gate-plumbing regression below. The four findings in the first review are **resolved** by `a6cb991f`; the original findings above are retained as historical review evidence, not as open objections. This addendum was made after cherry-picking `9828f1b3` and `a6cb991f` onto the K-containing `origin/master` at `8e1eba55` (new commit IDs `34e9950e`, `747da72f`) and rebasing the review document on that pair.

### Remaining finding

- **Medium — the optional amd64-emulation Linux gate now fails by construction.** `scripts/linux-gate.sh:133-138` appends `test_nostr_gtk_bind_latency_perf` to `SMOKE_EXCLUDE` whenever `NOSTRC_GATE_AMD64=1` selects a non-native architecture, but `scripts/linux-gate.sh:186-187` still exports `REQUIRED_PERF_TEST=test_nostr_gtk_bind_latency_perf`. `scripts/linux-gate-smoke.sh:216-224` then requires that same test to appear in the *selected* `ctest -N -L '^perf$' -E "$SMOKE_EXCLUDE"` list. I reproduced the selection: the native filter lists one perf test; the emulation exclusion lists **zero**. Thus an emulated run reaches the end of otherwise-passing smoke tests and fails “required perf test ... is not registered and selected.” Keep the native and hosted-CI mandatory perf checks, but do not require a test the emulation mode deliberately excludes (or remove that exclusion if its CPU-relative budget is valid under emulation). The full emulated build was not run; the contradictory selection is deterministic.

### Resolution probes and coverage

| Earlier finding | Re-review result |
| --- | --- |
| Required `test-linux-gate-smoke.sh` failed | `bash scripts/test-linux-gate-smoke.sh` **passed**, including mock and real CTest cases for required registration, serial perf failure/no retry, skip, and execution after a functional rerun. `bash scripts/test-pre-push.sh` also passed. |
| Fresh/CI perf registration absent | A new default configure registered `test_nostr_gtk_bind_latency_perf` with the `perf` label on its *first* pass. A fresh CI-shaped configure with `BUILD_APPS=OFF`, `BUILD_NOSTR_GTK=ON` also registered it. `groundhog-ci.yml` now builds the target, asserts its registration, and runs the perf label serially with a failing CTest exit blocking the job. The native Linux gate required and ran one serial perf test. |
| 27× bind work passed | The same 100,000-operation-per-`on_bind` injection now failed the perf test at a median candidate/reference CPU ratio of **53.73** against the **3.0** limit. A smaller 500-operation injection measured **1.52×** and passed. Suppressing `bind_count++` still failed both functional and perf registrations at `0 > 0`. The nine perf samples each require positive candidate and reference bind counts and a positive reference time, so the ratio is not vacuous. |
| Inbox timeout produced a GLib critical | Forcing `inbox_live` false failed after its deadline with `inbox did not become live: state=... relay=... detail=...`; there was no `Source ID ... was not found` critical. The new ready/timeout helper cases are registered inside `groundhog-e2e-dm`. |

The other original regression guards were not loosened: channel value and post-join completion checks, Marmot refusal/parser-bound/sort checks, and relayd's bounded child-exit check remain. The D-Bus stop-source change remains correct on review; no new shipped public surface or version bump is required beyond the decisions recorded in the manifest.

### Re-review verification

- On `a6cb991f`: macOS Ninja rebuild passed; touched CTests **7/7 passed** on repeat (an initial `groundhog-e2e-dm` run failed at an unrelated `send_stack_select` assertion, then passed without a source change); smoke self-test and pre-push self-test passed; native Linux smoke gate passed **462 functional + 1 serial perf**; sanitizer gate passed **60/60**.
- After cherry-picking onto K-containing `origin/master` `8e1eba55`: macOS Ninja rebuild passed; touched CTests **7/7 passed**; smoke self-test and unsequenced-argument check passed; native Linux smoke gate passed **461 functional + 1 serial perf**; sanitizer gate passed **60/60**. No mutation remains in the review worktree.

**Required before approval:** reconcile the emulation-only exclusion with `REQUIRED_PERF_TEST`, then run at least the gate self-test for that selection. No push was performed.

---

## Round-3 final addendum — `33931e15`, 2026-10-04

**Final verdict: APPROVED** for `tests/w28-timing-flakes` at `33931e15`. This verdict supersedes the earlier changes-required verdicts, which remain above as the audit trail. I cherry-picked the three W28 commits onto `origin/master` `fde4c4c7` (including the subsequently landed C/reaction-ordering work) as `80432b8b`, `611e0aa5`, and `4a30d1bc`, then rebased this review document onto them. No author or main checkout was modified, and nothing was pushed.

The remaining emulation finding is resolved. `run_perf` now determines whether `REQUIRED_PERF_TEST` is selected by the effective `-R` or `-E` filter *before* checking the perf-labelled CTest listing. It therefore still rejects a missing selected registration on native runs, but does not demand the GTK perf test when amd64 emulation explicitly excludes it. Any other selected perf tests still run serially and fail the gate on failure. The self-test derives the emulation exclusion from `scripts/linux-gate.sh` and covers the excluded required test, other selected perf tests, and the `TEST_REGEX` precedence/missing-registration case. The previous round's four regression injections and assertion audit remain valid; this commit changes no guarded test assertion into a vacuous one.

Verification on `33931e15` in the review worktree: `scripts/test-linux-gate-smoke.sh`, the pre-push/sanitizer self-tests, and the unsequenced-argument checks passed; the macOS build and touched suites passed. The native Linux gate passed **462 functional tests plus one required serial perf test**. The full `NOSTRC_GATE_AMD64=1` gate passed **459 selected tests** and explicitly reported that the GTK perf requirement was excluded by selection. Unlike the author's no-retry run, my emulated run had one unrelated `groundhog-two-instance` first-run failure that passed its allowed isolated rerun; this is a verification difference, not an unresolved W28 defect.

After rebasing onto `fde4c4c7`, the macOS rebuild, eight touched/integration CTests, smoke self-test, static check, and native Linux gate all passed again (**462 functional plus one serial perf**). The sanitizer gate had passed **60/60** on the immediately preceding reviewed base `9588c760`; it was not repeated after the final upstream-only Groundhog commits. The W28 CI integration retains both `BUILD_NOSTR_GTK=ON` and C's `BUILD_RELAYD=ON`; a fresh CI-shaped configure registered both the GTK perf and relayd publish tests. `git diff --check` passed. Round-3's gate-script/test-only change requires **no component version bump**; the previous version assessments still apply.
