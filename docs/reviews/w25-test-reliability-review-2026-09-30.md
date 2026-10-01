# W25 review: slice O, test reliability and the gate (nostrc-7c1v; flakes nostrc-79mi, -juhs, -glzv, -dha3, -8kb5)

- **Reviewer:** independent peer reviewer (AGENTS.md "Peer Review")
- **Branch reviewed:** `infra/w25-test-reliability` at `4218ad3b`, six commits on `08d35b4e` (master)
- **Review branch:** `review/w25-test-reliability` (this document only)
- **Date:** 2026-10-01 (file named for the W24/W25 review series)
- **Verdict:** **APPROVE-WITH-NITS.**
  - The gate change is sound: the rerun is the failed set only, by anchored name, serial, and announced and counted; a failed rerun, a crash or timeout in the rerun, a run that names no failed test, and a run of no tests all block the push. Its self-tests fail with each fix reverted.
  - Each flake fix addresses the real cause, none is a timing bump, and I reproduced four originals under load (dha3 in the Linux gate image, where it lives) and juhs deterministically with its new test; each is at 0 failures after the fix.
  - The juhs fix is correct and closes a real product bug, not a test race; its new test fails 3/3 with the fix reverted, with CI's exact message.
  - Nothing above Low. L1 is a pre-existing weakness of the shared smoke script that the macOS stage now inherits for the whole suite; L2 is a per-event cost the juhs fix adds where a cheap guard already exists.

| Commit | Beads | Change |
|---|---|---|
| `9f539740` | nostrc-7c1v | macOS pre-push stage runs its full CTest through `scripts/linux-gate-smoke.sh` (rerun once, announce, count); history in `<git-common-dir>/nostrc-macos-gate-history`; smoke script gains `HISTORY_DIR`, optional `VOLUME`, `unset CTEST_PARALLEL_LEVEL`; self-tests |
| `6d44fb2b` | nostrc-79mi | eose_order: overflow warning counted by a domain log handler instead of `g_test_expect_message()`; libsoup's accept-race warning not fatal |
| `d1e83af9` | nostrc-juhs (discovered nostrc-8gbp) | Groundhog: own Commit merged on its relay echo before the OK is attributed to the account; wire relay `hold_oks`; new `/groundhog/mls-service/own-commit-echo-before-ok`; groundhog folded into unreleased 0.12.0 |
| `2157422d` | nostrc-glzv | nip55l contract test waits (bounded, by name watcher) for the closed caller's name to vanish before `NameHasOwner` |
| `09b76b67` | nostrc-dha3 | TLS resumption test: control lws client connects and is serviced on one thread |
| `4218ad3b` | nostrc-8kb5 | relayd tests wait for a connectable socket (`ws_wait_accepting()`), not for the file |

No code or beads were changed. Scratch work lived in `/tmp/rv-w25-orig` (pre-fix test binaries built from master's test sources against this tree), `/tmp/rv-w25-amp` (GLib race amplifier), `/tmp/rv-w25-skipprobe` and `/tmp/rv-w25-idprobe` (real-CTest probes of the smoke script), and a clean detached checkout `/tmp/rv-w25-linux-src` for the Linux gates. Every source revert in the review worktree was restored with `git checkout` and rebuilt; the worktree is clean. The Linux gates reused their shared volumes; I created no Docker volume.

## Summary by focus area

### 1. The gate change: a deterministic failure does not pass (with L1's two exceptions)

**What runs.** `scripts/pre-push:233-238` runs `linux-gate-smoke.sh` with `TEST_REGEX=.`, `JOBS=${CTEST_PARALLEL_LEVEL:-1}`, `DISPLAY_WRAP=0` and `CTEST_TIMEOUT=` (CTest's default, as before). The first run is `ctest --no-tests=error --output-on-failure --parallel $JOBS -R .`, the same selection as the old bare `ctest` (every one of the 450 registered names matches `.`).

**The rerun is the failed set only.** `failed_in()` reads the names from CTest's "The following tests FAILED" block; the rerun is `-R ^(a|b|...)$` with no `--parallel`, and `unset CTEST_PARALLEL_LEVEL` (`linux-gate-smoke.sh:56`) makes it serial whatever the caller's environment says (the self-test proves the old script ran it in parallel: "FAIL: the macOS rerun ran in parallel"). CTest adds a failed test's `FIXTURES_SETUP` tests itself (the MDK image fixtures), which is what a correct rerun needs. Today every test name is `[A-Za-z0-9_/-]`, so the anchored pattern selects exactly the failed tests; see L1 for names that are not.

**It fails properly.**
- A rerun that fails again (any CTest failure class: Failed, SEGFAULT, Subprocess aborted, Timeout, Not Run) makes ctest exit non-zero: `:174-180` blocks with the rerun's output.
- A first run that fails without naming a test (ctest killed, crashed, interrupted, configuration error) blocks at `:151-155`.
- No tests: `pre-push:224-228` already refuses a build with no registered tests, and `--no-tests=error` is now on both runs; a rerun pattern that matches nothing fails too.
- A rerun that ran fewer tests than failed blocks at `:185-188`.
- Under `set -euo pipefail`, the hook's unconditional `bash .../linux-gate-smoke.sh` exits the hook on any non-zero status, and the `EXIT` trap kills the Linux stages and removes the worktrees, as the old bare `ctest` did.

**The history log is honest, with L1 and N3's wording caveats.** Every gate appends its id to `gates` at the start; only tests that passed the rerun are appended to `reruns`; the first run's full log is kept (newest 20) for every failure, rerun or not. The notice prints each failed test's first-run output (last 200 lines) even when the rerun passes.

**The shared history under `.git` is fine.**
- `--git-common-dir` is the one place all worktrees of a clone share, and the hook writes nothing else there; git ignores unknown entries (no gc, prune or clean touches it), and it is never pushed.
- Concurrent pushes from several worktrees (the fleet's normal case) append single short lines with `>>` (one `write()` under `O_APPEND`) and use a `date-$$` run id, so lines do not interleave and ids do not collide; pruning removes only the oldest logs.
- Growth is two one-line-per-gate files plus at most 20 logs.
- See N6 for old git.

**Self-tests.** `scripts/test-linux-gate-smoke.sh` and `scripts/test-pre-push.sh` pass. I re-ran them against master's scripts:
- master `pre-push`, new smoke: `test-pre-push.sh` fails at the new case's first check (the old hook runs a bare `ctest`, so the trace has no `--parallel 3 -R .` run);
- new `pre-push`, master smoke: `test-linux-gate-smoke.sh` fails "the macOS rerun ran in parallel", `test-pre-push.sh` fails "unexpectedly found: ^PARALLEL_RERUN$".

**Shells.** Both scripts are bash (`#!/usr/bin/env bash`, and the hook invokes `bash` explicitly); zsh never parses them. This machine's only bash is `/bin/bash` 3.2.57, so the self-tests and the review's own gate runs above ran under 3.2: no bash-4 constructs (`declare -A`, `mapfile`, `${x^^}`, `|&`), arrays are never empty under `set -u`, every path is quoted, and `$failed` is deliberately split (test names). Linux runs the same script under the image's bash 5 as before.

### 2. Each flake fix: real causes, no timing bumps

Load: this host (14 CPUs) was at load 16-59 from other sessions throughout; I added 6 `yes` CPU hogs and ran 12-14 copies at once (`/tmp/rv-w25-load.sh`). "Original" is master's test source built against this tree.

| Bead | Cause (verified) | Fix | Original | Fixed |
|---|---|---|---|---|
| glzv | `NameHasOwner` right after `close_sync`: the bus drops the name only when it processes the disconnect | `wait_for_vanished()`: `g_bus_watch_name_on_connection()` (its GetNameOwner follows its match, so neither a gone name nor a later drop is missed), 20 s bound | 10/60 failed, all `:1050 !alive` | 0/60 |
| 8kb5 | relayd `bind()`s (`relayd_session_main.c:255`), then `chmod`s, then `listen()`s (`:272`); `stat()` saw the file in between and the connect was refused | `ws_wait_accepting()`: a connect probe, same 10 s / 8 s bounds as before; also covers a stale file (refused, retried) | 36/300: 15 at `:220` (first start), 21 at `:240` (restart) | 0/300 |
| 79mi | GLib's `g_logv` reads `g_test_expect_message()`'s list without a lock on every thread | count the warning in a domain handler (looked up under GLib's lock); `fatal_unless_tolerated` only while a flood case expects it | 4/300 SIGSEGV, all in the two flood cases; crash report: thread `burst-relay`, `g_logv` <- `g_log` <- libsoup `send_message` <- `on_relay_message` | 0/300 |
| dha3 | the main thread called `lws_client_connect_via_info()` while another thread ran `lws_service()` on the same context (lws: only `lws_cancel_service()` is thread-safe) | connect and service on one thread; the control clients do nothing after `CLIENT_ESTABLISHED` and the hellos are recorded server-side, so no service thread is needed afterwards | macOS: 0/300 (does not reproduce here, as the bead says). Linux gate image, 14 at once + 10 hogs: **8/1800**: 5 `control lws never connected` (`:389`, the bead's symptom), 2 `control lws connect` (`:383`, the cross-thread connect returning NULL), 1 SIGSEGV | macOS 0/300; Linux **0/1800** |
| juhs | product bug (section 3) | product fix | new test 3/3 fail with the fix reverted | 0/60 (both cases) |

79mi, two more checks:
- **The race is real.** A 12-line amplifier (`/tmp/rv-w25-amp/amp.c`: one thread `g_log`s at DEBUG, the main thread arms and consumes `g_test_expect_message()`) crashes 5/5 with SIGSEGV.
- **The tolerance takes effect.** libsoup imports the old `g_log` API (`nm -u libsoup-3.0.0.dylib`: `_g_log`, no structured logging), so its warning goes through `g_logv`, where GTest's fatal handler and the default handler apply. It is the mechanism `gnome/groundhog/tests/nip29/nip29-relay.h:851-884` already uses for the same libsoup warning, and it forgives only that message. The overflow assertion keeps its strength (exactly 1).

dha3, Linux: the original was master's test source compiled with the gate build's own compile and link commands (`ninja -t commands`) against this tree's `libnostr.a`, and both binaries ran from a host bind mount, not the shared volume. The Linux gate history (`nostrc-linux-gate-arm64:/work/gate-history/reruns`) shows the test needing a rerun in three other gates today (08:34Z, 11:03Z, 13:05Z, branches without this fix).

8kb5: the probe connection is accepted and closed before `test_session_relay_ws`'s 300 ms idle wait, so that test still measures an upgrade on an idle daemon.

### 3. juhs: correct; one cost note (L2); nostrc-8gbp right in substance, wrong in two details (N1)

**Correctness.**
- **Identification.** `own_pending_commit()` (`gh-mls-service.c:1915-1928`) reads libmarmot's pending Commit before `marmot_process_message()` consumes it. It compares that event's id with the envelope's.
- **libmarmot's own test.** libmarmot recognises its echo by the MLS message digest (`commits.c:3081`: `memcmp(digest, p.key.digest, 32)`), and a byte-identical signed event (same id) has the same content, so whenever Groundhog's test is true libmarmot takes the own-echo path. It merges (`COMMIT`, committer NULL) if the pending is live, else `WRONG_EPOCH`, which is not a COMMIT and attributes nothing.
- **No forgery.** A forged envelope claiming our id fails libmarmot's id/signature check before any result. Only a COMMIT with no committer is re-attributed, and it gets the account with `last_committer_leaf = G_MAXUINT32`, exactly what `round_report()` does on MERGED (`:2567-2571`).
- **The other order still works.** If the OK comes first, `round_report()` attributes and refreshes, and the later echo finds nothing pending and is `OWN_MESSAGE`. It also covers the restart path (pending republished, echo first).
- **Narrower than libmarmot.** Groundhog matches by event id, libmarmot by digest. Our Commit re-wrapped in another kind-445 event still merges without attribution, which is today's behaviour, not a regression.

**Races.** Groundhog drives libmarmot from the main loop only, and the read and the processing happen inside the same `gh_store_begin()` transaction with nothing between them, so the pending state cannot change in between. `marmot_get_pending_commit()` is a libmarmot write transaction (it can finish a merge made before a crash). Inside the caller's transaction it is a savepoint (`gh-store-marmot.c:1895-1928`; `gh-store.c:953-973` rolls back only to its own savepoint on failure), so a failed pre-read cannot undo or end `process_event()`'s transaction. Finishing a crash-merged pending a little earlier is harmless: the echo then takes libmarmot's parent-epoch `OWN_MESSAGE` path either way.

**Cost.** See L2.

**Test.** `hold_oks` keeps every OK until `wire_relay_release_oks()`, so `wait_members(ga, 3)` can only be satisfied by the echo merge: deterministic, not timing. With `gh-mls-service.c` reverted to master it fails 3/3 with `assertion failed (by == added_by ? hex[added_by] : NULL): (NULL == "79be667e...")`, CI's message, while `unproven-member-identity` alone still passes 3/3 (the race needs CI's ordering).

**nostrc-8gbp.** The gap is real for **CLEARED**. `marmot_commit_clear_pending()` (`commits.c:2196-2226`) applies the deferred Commits and frees each result, committer included. `round_report()` sets `last_committer` only for MERGED, so the refresh creates their devices with `added_by` NULL, permanently. Two details of the bead are wrong (N1).

### 4. Pre-push script safety (macOS and Linux)

Covered in section 1 ("fails properly", "Shells"). On a Linux host the same stage runs, named "macOS gate", which is cosmetic. N6 is the one portability edge.

## Findings

### L1 (Low) — a rerun counts as passed on ctest's exit status and a test count, not on each failed test passing

`scripts/linux-gate-smoke.sh:174-188` (pre-existing in the Linux smoke run; this branch extends it to the whole macOS suite).

The rerun is accepted when ctest exits 0 and ran at least as many tests as failed. Two concrete ways a test that never passed gets through, reported as "passed alone":

1. **Fail, then skip.** About 15 tests have `SKIP_RETURN_CODE 77` (`gnome/groundhog/CMakeLists.txt`: groundhog-shell, -account-ui, -conversation-list, -launch, -store-key-keyring, -composer, -e2e-dm, …). One that fails in the full run and exits 77 in the rerun (say a GUI test that crashes under load and then cannot get its display, keyring or bus alone) is CTest "Skipped", ctest exits 0, and the gate passes.

   Reproduced with real CTest (`/tmp/rv-w25-skipprobe`): `fail_then_skip` (exit 1, then 77) gives `!! macOS gate: RERUN: failed in the parallel run, passed alone: fail_then_skip` and exit 0, and its rerun log says `***Skipped`. The history counts it as a passing rerun. Before this branch the macOS stage blocked on that first failure.

2. **Names are regex patterns.** A failed test whose name holds an ERE metacharacter does not match its own anchored pattern, and the count check is satisfied by whatever does match. Reproduced (`/tmp/rv-w25-idprobe`): test `x+y` (`COMMAND false`, always fails) and `xy` (passes). The rerun `-R ^(x+y)$` runs `xy`, and the gate exits 0 with "passed alone: x+y". This is latent: no current name has a metacharacter.

**Fix.** Escape each name (`sed 's/[][\.*^$+?(){}|]/\\&/g'`). Then require every failed name to appear in the rerun log as `Test #N: <name> ... Passed`, and treat Skipped or Not Run as a failed rerun (or set `FORBID_PATTERN='\*\*\*Skipped'` for the rerun only).

### L2 (Low) — the juhs pre-read does a full MLS-state load on every kind-445 envelope, needed only while a Commit of ours is pending

`gnome/groundhog/src/mls/gh-mls-service.c:1976` (`own_pending_commit()`, `:1915-1928`); `VERSION_MANIFEST.md:192`.

Every group event now also runs `marmot_get_pending_commit()`, which does:
- a SQLite savepoint;
- `find_group_by_mls_id`;
- `marmot_group_reconcile()`, which loads and deserializes the whole MLS group state (`commits.c:1485-1519`);
- the pending-row lookup;
- while a Commit is pending, a second state load and deserialize (`load_current`), a copy of its event JSON, and a parse (`event_id_of`).

`marmot_process_message()` already does one group lookup, one reconcile and one state load per event (`messages.c:842-997`). So this adds roughly one more full state deserialization per envelope: on a backfill of N messages in a large group, N extra deserializations, of which all but the few during our pending window are wasted. The manifest's "one more pending-Commit read per group event" understates it.

**Fix.** Guard the call with `group->round || group->pending_commit`, the idiom the file already uses at `:2775`. `round_start()` sets `group->round` before `gh_relay_publish_start()`, and `group_refresh()` keeps `pending_commit` true across an unanswered round and a restart, so no echo of ours is missed.

### N1 (Nit) — nostrc-8gbp: correct the mechanism and drop "superseded"

The bead says libmarmot "reports [the deferred Commit] as MARMOT_RESULT_COMMIT with no committer and no state change". It returns `MARMOT_ERR_OWN_COMMIT_PENDING` (`commits.c:2325-2352`, documented at `marmot.h:1393`), which `process_event()` drops at its `g_debug` branch.

The **SUPERSEDED** half does not hold. Our Commit is superseded only when a competitor beat it and was applied inline by `marmot_process_message()`, with its committer reported, so Groundhog attributes its devices. The deferred Commits are then deleted with the STALE pending when the OK's merge finds it stale (`marmot_commit_merge_pending()`, `commits.c:2174-2183`: "Its deferred Commits were built on that state too"), not applied.

The CLEARED gap stands, as the bead describes (section 3). Suggested test as in the bead: wire relay `refuse_events` plus an admin's Add deferred behind our pending Commit.

### N2 (Nit) — the new test sits between another test's doc comment and its body

`gnome/groundhog/tests/mls/test_mls_service.c:2278-2292`: the nostrc-6ukh comment describing `test_unproven_member_identity` (Bob, Verify, restart) now heads `test_own_commit_echo_before_ok`. Move the new block above it.

### N3 (Nit) — "failed in the parallel run" when the run was not parallel

`scripts/linux-gate-smoke.sh:150,194`. The macOS stage defaults to `JOBS=1` (`CTEST_PARALLEL_LEVEL` unset, the old default), so the first run was serial too. The notice and the kept log then misdescribe what happened: the test failed beside the Linux stages' builds, not in a parallel CTest. Say "in the full run" (or "in the parallel run" only when `JOBS > 1`).

### N4 (Nit) — the macOS stage is silent for the length of the full CTest run

`scripts/pre-push:235-238`. CTest's output now goes to `$tmp/macos-gate/ctest.log`, and nothing is printed until all ~450 tests finish (minutes under gate load), where the old stage streamed progress. On Ctrl-C the hook's `EXIT` trap deletes the log. A start line with the test count, or a `tee` of the progress lines, would keep a stuck push diagnosable. This is the Linux stage's behaviour too.

### N5 (Nit) — a broad deterministic break, or a hang, now costs two runs before the push is blocked

`scripts/pre-push:236` (`CTEST_TIMEOUT=`); `linux-gate-smoke.sh:174`.
- A test without a `TIMEOUT` property that hangs waits CTest's 1500 s default in the full run and again in the rerun.
- A break that fails dozens of tests reruns them all serially.

Consider a cap: no rerun when more than a handful of tests failed (a flake is rarely broad), noted in the message.

### N6 (Nit) — `--path-format=absolute` needs git 2.31

`scripts/pre-push:147`. Older git (Ubuntu 20.04 ships 2.25) does not know the option, and `rev-parse` passes an unknown flag through to its output, so the history path begins with `--path-format=absolute`, and the smoke script's `mkdir -p` fails after the full build, blocking the push with a confusing error. The fleet's git is newer. `cd "$(git rev-parse --git-common-dir)" && pwd` works everywhere.

### O1 (observation, not a finding) — `catch-up-5-commits` starved in the Linux smoke run

The plain Linux gate of this branch reran `groundhog-mls-service`: `/groundhog/mls-service/catch-up-5-commits` timed out waiting 90 s for a message in the parallel run (host load ~40 from other sessions) and passed alone. I do not attribute it to this branch:
- the case takes 0.3 s on macOS and passed 9/9 here under the same load;
- the juhs pre-read is microseconds on its 2-member group;
- this binary already needed Linux reruns on master for the same class of 90 s starvation (`key-packages`, `send-republished-after-restart`; gate history `20261001T020332Z-7`, `20260930T075351Z-7`).

The new stage did what it should: it reported the rerun loudly and counted it ("needed a rerun in 1 of the last 20 gates"). If it recurs, it deserves a bead next to nostrc-jcxn (the same binary's ASAN timeout).

## Version assessment

- **groundhog.** The fix is folded into the unreleased 0.12.0 (alone it would be PATCH: a bug fix, with no API, wire or state change), which is correct per AGENTS.md. With L2 applied, the manifest's cost note becomes accurate.
- **nip55l, libnostr, nostr-gobject.** No bump: their changes are test-only.
- **relayd.** Tests only, and no declared version; the W25 row says so.
- **Scripts.** `pre-push` and `linux-gate-smoke.sh` are developer tooling; they ship in no artifact.
- **libmarmot.** Unchanged.

## Quality gates run

- `python3 scripts/check-unsequenced-args.py`: clean.
- macOS (`/tmp/nostrc-macos27-env.sh`, `cmake -G Ninja -DBUILD_GROUNDHOG=ON`, ninja): built. The touched areas (`ctest -R 'groundhog|relayd|nip55l|nostr_gobject|tls'`, 102 tests, 6 at a time) passed 102/102 on the first run at load ~50. I ran them through the branch's own `linux-gate-smoke.sh` in macOS mode, so they also exercised the new stage on real CTest output.
- `scripts/linux-gate.sh --sanitizers` (clean checkout at `4218ad3b`): 52 sanitizer tests passed, no rerun (5m01s).
- `scripts/linux-gate.sh` (clean checkout): GCC build of all targets plus Groundhog; smoke tests passed, 446 run, **after one rerun** (6m17s). `groundhog-mls-service` failed in the parallel run (`/groundhog/mls-service/catch-up-5-commits`: "a message listed did not happen within 90 s", line 472) and passed alone; see O1.
- Load loops as tabulated in section 2.
- Self-tests: `test-linux-gate-smoke.sh` and `test-pre-push.sh` pass, and fail against master's scripts.
