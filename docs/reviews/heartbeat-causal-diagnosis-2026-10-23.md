# nostrc-0cfsg.11 — macOS sanitizer heartbeat: causal diagnosis (2026-10-23)

Base: `587ebea254fb3b7c19c8f91a42b23a8a4d950db3` (master). Scope: heartbeat test
`nostr-gobject/tests/test_main_thread_forbidden_calls.c` (CTest registration 227
`test_nostr_gobject_main_thread_latency`) only. Bead `.11` **stays OPEN**.

## Verdict

| Question | Status |
|---|---|
| Did the *current* (post-`23ca21c19`) hosted failures block the main thread in store code? | **Confirmed no.** 5 of 5 captured hosted failures show 0 gaps during the 50-cycle store burst, store operations 0.05–1.32 ms, and all misses in the post-burst idle `g_main_loop_run`. |
| Is the cause of those late idle wake-ups known? | **No.** Host timer/scheduler wake latency is the only mechanism consistent with the data; whether it is VM steal, QoS/timer coalescing or something else is not observed on the hosted runner. |
| Original failure (`5467c02d1`, job 116758090689, count=18 missed=3 max_gap=60.3 ms) | **Plausible only, not confirmed.** Same mechanism fits (below), but the original log has no phase timeline. |
| Safe remedy | **Unresolved.** No guard/threshold/coverage change is made or proposed here (see "Rejected remedy"). |

A later passing run is not used as causal evidence anywhere in this report.

## Sources (all hashes sha256)

- Original failed job: run 38833745982, job 116758090689 (`macos-latest • asan_ubsan`, image macos-26-arm64 20260907.0351.1, SHA `5467c02d1`), raw log `d472c9a3d9521a61aaf728b5715572d6b99aa83cc0a22582f8f997b802095e6a`. Exact command: `ctest --test-dir build --output-on-failure -j2 -E "test_json_envelope|concurrency_subscription_shutdown_asan|MockRelayUnit"` with `ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1`, `-DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON`, Debug.
- Original test blob (`5467c02d1`) sha256 `e99df5380b634824b6beb2497db430f039be32c24b2c1b34dc5f4f77719a718d`.
- Hosted collection (macOS asan_ubsan jobs, 14 logs, newest runs; **not a random sample, no failure rate is claimed**): `/tmp/nostrc-heartbeat-claude-20261023/data/`, run list `runs.json`.
- Local exact-source build: HEAD 587ebea25 + nostrdb 2d459e8 / nsync 5b420ef (initialised locally, no network), Debug ASan+UBSan with the CI configure flags, macOS 27.0.1, GLib 2.90.0 (hosted: macOS 26 SDK, GLib 2.88.3 — different).
- Evidence directory (outside the repo): `/tmp/nostrc-heartbeat-claude-20261023/` (logs, scripts, instrumented copy, mutants, rejected fix).

## Original failure (plausible attribution)

Hosted log lines 2166–2202: test #227 ran 0.98 s alongside only `marmot_test_interop` (#216, started 01:14:23.97, passed 7.69 s) under `-j2`; cases 1–3 passed with 0.14/0.12/0.32 ms operations; case 4 reported `Heartbeat: count=18, missed=3, max_gap=60.3 ms`.

Local record-only copy of the *original* test (not committed; `orig/test_orig_instrumented.c`) shows the original structure: the 50 store cycles take **1.46 ms** (3.7 ms under `taskpolicy -b`) and dispatch **no** beat; every beat and every gap falls in the following 20 × (`g_main_context_iteration(FALSE)` + `g_usleep(5000)`) phase, where no store code runs. Idle: 9 beats ≈12.5 ms apart, missed 0. A hosted count of 18 (≈ one beat per iteration) means each 5 ms sleep-iteration took ≥10 ms on average with three 50–60 ms excursions — consistent with late wake-ups in that sleep phase, and not with 30× slower store code. This is an inference from structure plus the revised-test data below; it is not an observation.

## Current-test hosted recurrences (confirmed attribution)

Test revised in `23ca21c19` (ancestor of all rows) prints per-cycle operation time and flags a cycle `MISSED` if the gap closes during the burst.

| macOS asan job | run | SHA | result | count | missed | max_gap | cycles flagged | max op |
|---|---|---|---|---|---|---|---|---|
| 119634793144 | 39721202157 | c5509f6c1 | FAIL | 6 | 3 | 85.6 ms | 0 | 0.05 ms |
| 119634860301 | 39721222926 | ed7063170 | FAIL | 6 | 3 | 78.4 ms | 0 | 1.32 ms |
| 119694790212 | 39740044644 | fc3bf1579 | FAIL | 6 | 3 | 90.0 ms | 0 | 0.28 ms |
| 119738914144 | 39753293299 | f646db033 | FAIL | 6 | 4 | 89.7 ms | 0 | 0.26 ms |
| 120078574935 | 39851489170 | 0ce430299 | FAIL | 6 | 3 | 88.7 ms | 0 | 0.41 ms |

(9 other collected jobs passed; passing logs print no heartbeat line, so they carry no timeline.) Full hashes, run/job/head-SHA bindings from the Actions jobs API, and all other evidence digests are in the evidence-only `MANIFEST.json` / `SHA256SUMS` under `/tmp/nostrc-heartbeat-claude-20261023/` (not committed).

Interpretation: the burst (50 cycles) lasts a few ms, so at most one beat lands in it; the remaining ≥5 beats are produced by the post-burst `g_main_loop_run`, in which the main thread only polls for a 10 ms timer. Three or more of those wake-ups were 78–90 ms apart while no store call was executing and no cycle was flagged. Therefore these failures are not main-thread blocking by the store. The cluster of 78–90 ms gaps is notable but its source is not identified.

## Local reproduction of the signature (not hosted reproduction)

- Idle: original 20/20 pass (count 9, max_gap ≈12.5 ms); HEAD 20/20 pass (count 6, max 12.5 ms) — recorded as baseline only.
- 28 busy-loop processes on 14 cores: original 20/20 pass; plain CPU contention does **not** reproduce.
- `taskpolicy -b` (Darwin background, coalesced timers/sleeps), no code change: original `count=19, missed=16–18, max_gap≈105 ms` (beats ≈100 ms apart, all in the sleep phase); HEAD `count=6, missed=5–6, max_gap≈108–110 ms`, 0 cycles flagged, operations ≤0.85 ms — the same shape as the five hosted recurrences. `taskpolicy -t/-l` tiers did not reproduce.
- This shows the guard fails purely from timer/sleep wake latency of the process and that the original guard is sensitive to that class of environment. It does **not** show the hosted runner applies such a policy.

## Rejected remedy

A candidate that stopped counting post-burst gaps was built and verified locally (a CPU-time-attributed variant was proposed but never built) (`orig/rejected_post_burst_exclusion_fix.c`, sha256 `70f8186feb2ce79563237c9d92f43ab10c0e08712ae4b145ced552fb46100e66`, evidence dir only) but is **not landed**: it narrows existing coverage, and the original cause is only plausible. No relaxation, exclusion, extra sleep, retry or timeout change is made.

## Retained change (diagnostic only; counts/thresholds/coverage unchanged)

`nostr-gobject/tests/test_main_thread_forbidden_calls.c`:
- `HeartbeatData.post_burst_missed_count` (increments only for post-burst gaps that already count in `missed_count`) and `post_burst_max_gap_us` (tracks **all** post-burst gaps, including those at or below 50 ms, so idle runs print e.g. 12.5 ms), both updated in `heartbeat_record_at`.
- New message `Attribution: burst_missed=%u post_burst_missed=%u post_burst_max_gap=…` so any future hosted failure states, without inference, whether the gap closed inside the store burst or in the idle phase.

Unchanged: `gap > 50 ms` strict rule, `missed_count <= 2`, `heartbeat_count > 5`, the 50 unpaced cycles, final explicit sample, CTest `TIMEOUT 90`, sanitizer flags, the three blocking controls.

### Regression and mutation proof (local, exact-source build)

- `heartbeat-gap-boundary` now locks the policy: a 500 ms post-burst gap **still increments `missed_count`** (3→4) and `post_burst_missed_count`; exactly 50 ms increments neither.
- Mutant "post-burst gaps not counted" (`orig/mutant_post_burst_not_counted.c`) fails that test: `assertion failed (hb.missed_count == 4): (3 == 4)`. Mutation is relevant because it is exactly the rejected exclusion.
- Fixed source, idle: all 8 cases pass (`runs/diag-idle.log`); the existing negative controls (first-3, final-3, burst-pressure) still fail their children as intended.
- Fixed source under `taskpolicy -b`: `heartbeat-not-starved` **still fails** with `Attribution: burst_missed=0 post_burst_missed=5 post_burst_max_gap=109.3 ms` (`runs/diag-bg.log`) — the guard is intact and now self-attributing.
- Not run: Linux/arm64, the hosted environment, repeat-pass counts (deliberately; they are not causal evidence).

## Version assessment

Test-only change in `nostr-gobject` (2.2.2 unreleased): no shipped artifact, API or ABI change → **no bump**, `VERSION_MANIFEST.md` unchanged. No keychain command run or configuration touched. No JSON-GLib/fuzz, W11 ledger, workflow or other docs edited.

## Next evidence needed

1. Next hosted failure will print the Attribution line; if `post_burst_missed>0` with `burst_missed=0` again, that adds one more instance, nothing more.
2. To identify the wake-latency source, a record-only hosted probe (parent approval, not done here): in the post-burst phase log `mach_absolute_time`/thread CPU time and a no-work control timer alongside the heartbeat, to separate descheduling from timer coalescing.
3. A safe remedy requires deciding what the post-burst phase is meant to guard; that is a policy decision for the parent, not made here.
