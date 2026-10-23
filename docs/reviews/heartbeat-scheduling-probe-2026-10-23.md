# nostrc-0cfsg.11.1 — heartbeat scheduling probe (2026-10-23)

Base: master `663edcd70c0d313c98f87610aa41f1261bce4ee6`. Child of `nostrc-0cfsg.11`, which **stays OPEN**.
Scope: `nostr-gobject/tests/test_main_thread_forbidden_calls.c` (CTest `test_nostr_gobject_main_thread_latency`),
`nostr-gobject/tests/heartbeat_sched/`, and one step in `.github/workflows/nostrc-ci.yml`.
Builds on `docs/reviews/heartbeat-causal-diagnosis-2026-10-23.md`; its conclusions (store blocking refuted for the
current test; post-burst idle wake attribution confirmed; the original failure plausible only; why the host was late
unknown) are unchanged here.

## Verdict

| Hypothesis | Status |
|---|---|
| H1: the hosted test process inherits Darwin background policy | **Untested on the hosted runner.** Locally it is observable, and it reproduces the signature. |
| H2: VM steal or descheduling | **Untested.** |
| H3: timer coalescing without the background flag | **Untested.** |
| H4: store code blocks the main thread (current test) | **Refuted** (earlier report). |

No hosted cause is claimed. No setup repair is landed, because no hosted evidence demonstrates the inherited policy. A
local `taskpolicy -b` reproduction is not proof of the hosted cause. What is delivered instead:

1. Record-only metrics in the test, which print the policy state and discriminators in every run.
2. A child-local foreground counterfactual, which is exact and executable.
3. A hosted probe in the existing legacy macOS seam.

## What the local runs show

All runs use the same binary and command as CTest, with no arguments. Debug ASan+UBSan, CI configure flags, macOS 27.0.1,
GLib 2.90.0. Hosted is macOS 26 with GLib 2.88.3, so it differs. Evidence: `/tmp/nostrc-heartbeat-sched-20261023/`
(`SHA256SUMS`, `runs/`). All failures are retained.

**Observable.** `getpriority(PRIO_DARWIN_PROCESS)` reads 1 under `taskpolicy -b`. QoS reads 0x21 either way, so the
QoS class is not the signal. A background process shows:
- main-thread CPU about 0.2 ms over 0.9–1.2 s of wall time;
- the control sleeper late by about 195 ms, matching the heartbeat gaps (205–210 ms);
- 0 store-burst misses.

A plain GLib `g_timeout_add(10)` loop with no store code shows the same thing: 1249 ms wall and 0.27 ms CPU under `-b`,
against 93 ms wall plain (`glibcpu.c`). The late wakes are process-wide wake latency, not store code.

**Same binary, before and after:**

| Condition | Result |
|---|---|
| plain | pass (missed 0, max gap 12.5–15 ms) |
| `taskpolicy -b` (more than 11 runs across three builds of the test) | FAIL every time: `post_burst_missed=5–6`, max gap 108–210 ms |
| `taskpolicy -b` then `taskpolicy -B -p $$` | FAIL 3/3. `-B` returned 0 but `darwin_bg_process` stayed 1, so it is **ineffective**. |
| outer `-b`, then child-local `setpriority(PRIO_DARWIN_PROCESS,0,0)` | **pass 2/2 plain, 2/2 loaded** (final run); 3/3 earlier |
| outer `-b`, inherited (control) | FAIL 2/2 plain, 2/2 loaded |

The supported child-local prerequisite is `setpriority(PRIO_DARWIN_PROCESS, 0, 0)`. It is unprivileged, affects only the
calling process, and survives `exec`. It does not change global policy or any threshold. It demonstrates that a
*background-inherited* process fails the guard and a foreground one passes. It does **not** show that the hosted runner
is background.

## Record-only change (guard unchanged)

Parent review finding applied: **default CTest does not start any extra thread and keeps the original workload, timing
and exactly the original 8 cases** (verified: `1..8`, `control_thread=off`, CTest registration #228 passes).

`test_main_thread_forbidden_calls.c`:
- Always on, nonintrusive (a few syscalls outside the timed burst): `SchedEnv:` lines with `darwin_bg_process`,
  `darwin_bg_thread` and QoS on Darwin, and post-burst wall time and main-thread CPU (Mach `thread_info`).
- **Opt-in only, `NOSTRC_HEARTBEAT_CONTROL=1`** (set explicitly by the probe script, never by CTest): the no-work
  `hb-control` sleeper thread. Its effect on hosted wake latency is not proven negligible, so it is never part of the
  gating workload.
- **Opt-in only, `NOSTRC_HEARTBEAT_DIAGNOSTICS=1`**: a separate case,
  `/nostr-gobject/latency-diagnostic/sched-env-thread-cpu`, which checks that a sleeping thread reports far less CPU than
  wall time. It is not registered by default, so it cannot reinterpret the gating case count. It exists because my first
  metric, `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`, silently returned wall-clock epoch time *inside this ASan test
  binary* (a standalone binary with the same call was correct; I did not find out why). Mutation check: restoring that
  clock fails the case (`runs/v2-mutant-clock.log`).
- Unchanged: strict `gap > 50 ms`, `missed_count <= 2`, `heartbeat_count > 5`, 50 cycles, final sample, CTest
  `TIMEOUT 90`, sanitizers, and the first-3, final-3 and burst-pressure negative controls. The only removed lines are
  `if`/`else` re-braces around the same `heartbeat_count > 5` condition. No foreground change is made to the gating
  test; the child-local foreground call exists only in the probe launcher until the exact hosted cause is proven.

## Hosted probe (integrated, record-only)

Step "Heartbeat scheduling probe (macOS, record-only)" in the legacy `nostrc-ci.yml` job, after `Test`: `!cancelled()`,
macOS+asan_ubsan only, `continue-on-error`, script always exits 0. The `Test` step stays the only gate. Command:
`bash nostr-gobject/tests/heartbeat_sched/heartbeat-sched-probe.sh build/nostr-gobject/test_nostr_gobject_main_thread_latency 3`.

Per repetition it runs `inherit`, `fg`, `inherit-loaded`, `fg-loaded` (busy-loop siblings, an approximation of `ctest -j2`),
and `inherit-control`, `fg-control` (the only runs that set `NOSTRC_HEARTBEAT_CONTROL=1`). Once it runs `bg-positive`
(expected to fail: a positive control) and `diag-case`. Every run logs a `probe-env:` line with the opt-in values it set
explicitly, the policy before and after, the `setpriority` return code, the TAP case count, the in-test background flag
and the heartbeat line. The summary ends with `all return codes:`. Failures are printed in full and never retried.

Local result of the same script, under an outer `taskpolicy -b` (2 repetitions): `inherit`, `inherit-loaded`,
`inherit-control` all rc=134 (`darwin_bg_process=1`, 5–6 misses, ~208 ms). `fg`, `fg-loaded`, `fg-control` all rc=0
(policy 1 -> 0, setpriority rc=0). `bg-positive` rc=134. `diag-case` rc=0. Plain: every run rc=0 except `bg-positive`.
Logs: `runs/v2-probe-plain.log`, `runs/v2-probe-outer-bg.log`.

Reading the next hosted job (decision rules, not results):

- `inherit` shows `darwin_bg_process=1` and `fg` passes: H1 is demonstrated; a scoped repair can then be justified.
- `inherit` shows `darwin_bg_process=0` with late wakes and near-zero main-thread CPU: H1 is refuted for that run.
- `fg` also fails: the cause is not the process background flag.
- `inherit` passes but `inherit-control` fails: the control thread itself perturbs timing. That is why it is opt-in.

Limits: the probe runs after `Test`, not under the real `ctest -j2`, and only the process flag is cleared by `fg`. The real
failing ctest run prints its own `SchedEnv:` lines, which is the most direct evidence.

## Version assessment

Test-only and CI diagnostic. No shipped artifact, API or ABI change: **no version bump**; `VERSION_MANIFEST.md`
unchanged. No keychain command, relay, JSON-GLib/fuzz, W12 or other workflow touched. Submodules `nostrdb` and `nsync`
were initialised at their pinned commits from git to build locally; this is not a repo change.

## Verification

Run locally: the target built with the CI configure flags and `-DGO_WARNINGS_AS_ERRORS=ON`; default CTest registration
(8 cases) passes; the probe script ran under bash 3.2, plain and under outer `-b`; mutation check; `shellcheck`,
`actionlint`, `git diff --check`, repo static checks.

**NOT RUN:** the fresh all-default build required by AGENTS.md before a push (`rm -rf _build && cmake -B _build &&
cmake --build _build`) and full CTest; target-only is not a substitute. Nothing is pushed from here; the parent
integration owns that build. **NOT RUN:** any Linux build or test, and the test file's non-Apple branches, which have not
been compiled. Hosted proof is pending the parent landing this change.
