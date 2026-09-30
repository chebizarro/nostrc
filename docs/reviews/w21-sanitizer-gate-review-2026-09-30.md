# W21 review: pre-push sanitizer gate (nostrc-3han) and lsan.supp teardown entries (nostrc-xbso)

Reviewer: independent peer review (AGENTS.md), 2026-09-30.
Branch: `infra/w21-sanitizer-gate` at `61eb2453`, reviewed on `review/w21-sanitizer-gate`.
Base: `384dc266`. Commits under review:

- `e1e4b51d` infra(gate): run the Groundhog sanitizer set (ASAN+UBSAN+LSan) in the pre-push gate (nostrc-3han)
- `61eb2453` test(groundhog): suppress libnostr's subscription and write teardown races under LSan (nostrc-xbso)

## Verdict in one paragraph

The gate itself is carefully built. The job is read from the candidate's workflow. A sanitizer report is never rerun. Every broken-build, missing-test and skipped-test path fails closed. Locks are ordered consistently, and the tests are good. Four things block the merge:

- **B1.** The new `leak:^nostr_relay_write$` hides a real Groundhog leak of the write answer channel, in hosted CI as well as the gate. I proved this with a planted leak. The libnostr bug it papers over is a missing unref on one error branch: a few lines to fix.
- **B2.** The path filter skips pushes that change inputs of the sanitizer build. Ninja lists the root `CMakeLists.txt`, `cmake/`, `gnome/seahorse/`, `libjson/`, `components/nostrdb/`, the `third_party/nostrdb` gitlink and the gnostr gschema among them.
- **B3.** The path filter fails open when git cannot compute the range.
- **B4.** The YAML reader silently drops job-level, workflow-level and configure-step `env:`. The stage can then run weaker than CI while claiming parity.

All four fixes are small.

## What I ran

| Check | Result |
|---|---|
| `python3 scripts/check-unsequenced-args.py` | "No call modifies and uses a variable in different arguments" (exit 0) |
| `bash scripts/test-sanitizer-gate.sh` | passed (48 tests read from the real job, mutations, YAML subset, path filter) |
| `bash scripts/test-pre-push.sh` | passed |
| `bash scripts/test-linux-gate-smoke.sh` | passed |
| Remaining `! grep` under `set -e` in `scripts/test-*.sh` | none bare; every one left is `! grep … \|\| fail` |
| Real gate, clean tip (`scripts/linux-gate.sh --sanitizers <clean checkout of 61eb2453>`) | **passed, 48 run**, 1m36s incremental (build 27 s) |
| Real gate, planted P1: an ordinary Groundhog leak (`g_strdup(frame)` dropped in `write_thread`, gh-relay-gnostr-write.c) | **blocked**, 1m14s: 13 tests with LSan reports at gh-relay-gnostr-write.c:49, "a report is not rerun", whole outputs printed |
| Real gate, planted P2: Groundhog forgets `go_channel_unref(answer)` in `write_thread` | **passed, 48 run** (the leak is hidden, see B1) |
| P2 build, write-path tests, with only `leak:^nostr_relay_write$` removed | **4/4 fail**: 44 channels leaked from `go_channel_create ← nostr_relay_write (relay.c:1546) ← write_thread (gh-relay-gnostr-write.c:48)` |
| Sanitizer set without the three new entries, `-j2`, idle machine, 3× | **3/3 pass** (47–53 s) |
| … `-j2` beside 12 busy-loop CPU hogs (≈ the gate beside two builds), 3× | 2/3 fail; see "The suppressions" |
| … `-j8` 2×, `-j14` 5× | failures listed below; every leak stack inspected |
| CTest 3.28 in the image: failed test printing 400 KB before an LSan report | not truncated; the report grep sees it |
| Real hook, docs-only commit on the tip (`scripts/pre-push` with a crafted ref line) | exit 0: sanitizer stage skipped with its reason; Linux smoke and macOS passed |

My leak experiments ran in a private copy of the gate volume (`nostrc-w21rev-asan`), so they never held other agents' gate locks. The source and build tree were verified identical to the tip.

## Blocking findings

### B1. `leak:^nostr_relay_write$` hides Groundhog's own write-channel leak, in CI too; fix libnostr instead

`nostr_relay_write()` returns its answer channel to the caller. Groundhog's only direct caller is `write_thread` in `gnome/groundhog/src/relay/gh-relay-gnostr-write.c`. Any leak of that channel is therefore allocated under a `nostr_relay_write` frame, whoever owns it. The file's own header rule is "each entry names a function whose allocations are all part of the leak it describes" and "a leak in Groundhog code is fixed, never suppressed". This entry breaks both. The commit acknowledges the blind spot, but it is not hypothetical:

- **P2 (planted).** Deleting Groundhog's `go_channel_unref(answer)` leaks one channel per write: deterministic, 44 channels in groundhog-dm-send alone. It **passes the gate and would pass hosted `groundhog-sanitizers`**. Without the entry, 4/4 write-path tests fail.
- **The race it targets has the same stack.** Unsuppressed runs at `-j14` show the race leaking exactly `go_channel_create ← nostr_relay_write (relay.c:1546) ← write_thread (gh-relay-gnostr-write.c:48)`. It is a Direct leak of the channel only. LSan cannot tell the two apart.

**The libnostr cause is simpler than the lsan.supp comment says.** The comment blames "a relay torn down with the request still queued". In every `nostr_relay_write` leak I captured, the channel alone leaked, as a Direct leak. No request and no frame copy leaked with it. That is the signature of the **enqueue-failure branch** of `nostr_relay_write()` (relay.c:1566–1571):

1. `go_channel_ref(chan)` takes the writer's reference, so refs = 2.
2. The send to `write_queue` fails because the relay is closing.
3. `go_fiber_compat(write_error, chan)` is spawned, and req and msg are freed.
4. `write_error()` (relay.c:864) sends a new Error and never unrefs `chan`. It also drops the Error when the send fails, because Groundhog has already closed the channel. That is the `^write_error$` leak.

The malloc-failure branch just above drops the extra reference before spawning `write_error`; the enqueue-failure branch does not. So every write that loses the race with a relay close leaks a channel. That is a plain refcount bug, not an inherent teardown race.

**How hard the fix is (nostrc-xbso site 2).** Small:
- Make `write_error` own the writer's reference. Send the Error; if the send fails, `free_error()` it; then `go_channel_unref(chan)`.
- Make the malloc-failure branch consistent by not dropping the reference before the spawn.

That is about 5 lines in relay.c. If a queued-at-teardown leak does exist beside it, draining `write_queue` with `go_channel_try_receive` (already used in libnostr) is about 10 more lines. The drain goes in `relay_free_impl` before `go_channel_free(write_queue)`, and in `write_operations` after the cancel `break`: free msg, close and unref answer, free req. I saw no such leak in 12 runs.

**Required:** fix the enqueue-failure path in libnostr and drop `leak:^nostr_relay_write$` and `leak:^write_error$`. If that has to wait, keep these two entries out of the file hosted CI reads, as a gate-only supplement named for xbso. CI then stays able to see a Groundhog answer-channel leak.

`leak:^nostr_subscription_free_async$` is acceptable (see "The suppressions").

### B2. The path filter skips real inputs of the sanitizer build

`SANITIZER_PATHS` (scripts/sanitizer-gate-ci.py:41–51) covers `gnome/groundhog/ libnostr/ nostr-gobject/ libmarmot/ marmot-gobject/ libgo/ nips/ tests/` and the workflow. I computed what the job's targets actually consume in the gate's build tree: `ninja -t inputs <GH_SAN_TARGETS>`, the deps log of every object, and the configure inputs of `build.ninja`. The following are read by the sanitizer build and do **not** trigger the stage:

| Path | Why it matters |
|---|---|
| `CMakeLists.txt` (root) | defines `GNOSTR_ENABLE_ASAN`/`GNOSTR_ENABLE_UBSAN` and `apply_sanitizers`; a change here can switch the sanitizers off |
| `cmake/` (`NostrcTestBus.cmake`, `NostrdbVendored.cmake`, `FindOrVendorNsync.cmake`, `GnTest.cmake`, …) | `NostrcTestBus.cmake` **defines `nostrc-test-bus-selftest`, a test in the set** |
| `NipOptions.cmake` | read at configure |
| `gnome/seahorse/` | `gnostr-secret`, linked by `groundhog-identity` (6 sources compiled into the set) |
| `libjson/` | `nostr_json`, linked by libnostr and `test_connection_recv_drain` |
| `components/nostrdb/`, `third_party/nostrdb` (gitlink) | 140+ compiled files; a submodule bump shows in `git diff --name-only` as `third_party/nostrdb` |
| `apps/gnostr/data/schemas/org.gnostr.gnostr.gschema.xml` | copied into Groundhog's test schemas (gnome/groundhog/CMakeLists.txt:363) |
| `scripts/linux-ci.Dockerfile` | the compiler, sanitizer runtime and every library the tests link |

The same hook already treats `CMakeLists.txt cmake/ gnome/seahorse/` and the gschema as Groundhog inputs for BUILD_GROUNDHOG (scripts/pre-push:192–197). The sanitizer filter is narrower than that list. `scripts/test-pre-push.sh` (the loop at line 282) asserts the wrong expectation for four of these paths. `test-sanitizer-gate.sh` asserts `CMakeLists.txt` skips.

The author's note asks whether root CMake, `cmake/` and `third_party/` should trigger. They should: they are inputs, one defines a test in the set, and one holds the sanitizer switch. Such pushes are rare, and the stage costs about 1.5 min incremental, so the cost is low.

**Required:** add the paths above. Preferably also make the filter fail safe: skip only when every changed path is on a short known-irrelevant list (e.g. `docs/`, `*.md`, `apps/` except the schema, `packaging/`, …), so a new dependency cannot silently fall outside it. Update both tests.

### B3. The filter fails open when git cannot compute the range

```bash
why="$(git -C "$repo_root" diff --name-only --no-renames "$1" "$2" |
       python3 "$repo_root/scripts/sanitizer-gate-ci.py" affected)" || status=$?
```

If `git diff` fails, python reads empty stdin and exits 1. Under `pipefail`, the rightmost non-zero status (python's 1) wins. The result is "sanitizer stage skipped …: the range changes no files". Reproduced with a remote oid absent locally:

```
fatal: bad object 1234567890123456789012345678901234567890
status=1 why=the range changes no files
```

This happens on a `--force` push over a remote tip that was never fetched. The BUILD_GROUNDHOG check beside it fails closed in the same case (`! git diff --quiet` gives ON).

**Required:** run `git diff` on its own, check its status, and run the stage (or block) on error. Add a test case with an unknown remote oid.

### B4. The YAML reader silently drops `env:` it does not reproduce

The docstring promises that a job shape the reader "does not understand … fails instead of guessing". It reads only `GROUNDHOG_SANITIZER_TESTS` from the job `env`, only `run` from the configure and build steps, and nothing at workflow level. I tested these mutations of the real workflow. Each **exits 0** with an unchanged config:

- `UBSAN_OPTIONS` moved from the test step's `env` to the job's `env`. This is a natural refactor. The gate then runs UBSAN **without `halt_on_error=1`**, so UB no longer fails a test, while CI still fails. The stage becomes weaker than CI with no signal.
- A workflow-level `env: ASAN_OPTIONS: detect_leaks=0` is ignored.
- `env: {CC: clang, CFLAGS: -O1}` on the configure step is ignored, so the gate builds with GCC -O0 while CI would not.
- `working-directory:` on the test step is ignored.

**Required:**
- Fail on any workflow-level `env`/`defaults`.
- Fail on any job `env` key other than the test list, and on job `container`/`services`/`defaults`.
- Fail on step keys outside `{name, run, env}` for the test step, and outside `{name, run}` for configure and build.
- Add these as `rejects` cases in `test-sanitizer-gate.sh`.

Anchors and aliases already fail closed ("unexpected indentation", exit 2), as do unknown ctest options and extra commands.

## The suppressions (61eb2453)

**Every leak stack without the three entries.** I ran the set 12 times without the three new lines (the rest of lsan.supp in force):
- `-j2`: 3× on an idle machine, 3× beside 12 CPU hogs.
- `-j8`: 2×. `-j14`: 5×.

I grouped all leak stacks by allocation frames:

| Leak (frames above the allocator) | Seen | Groundhog frame in the stack? | Covered by |
|---|---|---|---|
| `go_channel_create`/`malloc ← nostr_subscription_free_async ← gnostr_subscription_finalize ← g_object_unref ← {subscription_monitor_thread, resubscribe, close_relay}` | mls-service, privacy-e2e (`-j2`+load, `-j14`) | no (nostr-gobject finalize) | `^nostr_subscription_free_async$` |
| `new_error ← write_error ← go_wrapper_func` | dm-send (`-j14`) | no | `^write_error$` |
| `go_channel_create ← nostr_relay_write (relay.c:1546) ← write_thread (gh-relay-gnostr-write.c:48)`, Direct, channel only | mls-service (`-j14` 2×) | **yes, Groundhog is the caller** | `^nostr_relay_write$` (B1) |
| `go_channel_create ← nostr_connection_new ← nostr_relay_connect ← gnostr_relay_connect ← connect_async_thread` | relay-wire (`-j14`) | no | **nothing**: fails even with the entries |
| OpenSSL per-thread DRBG/ERR state: `CRYPTO_zalloc ← EVP_RAND_CTX_new / RAND_get0_public / ERR_set_mark ← RAND_bytes_ex ← libsqlcipher` | privacy-e2e (`-j2` + load) | no | **nothing**: fails even with the entries |

No Groundhog-owned leak is hidden today. The one stack with a Groundhog frame is libnostr's missing unref (B1), not a Groundhog bug. The entries are still too broad, as P2 shows.

**The three entries, one by one:**

- **`^nostr_subscription_free_async$`: acceptable.** Anchored, and Groundhog has no direct caller: every captured stack comes through nostr-gobject's `gnostr_subscription_finalize`, which abandons the handle at once. The real fix (join or own the detached cleanup thread) is jwj0 scope and not small. Two corrections to its comment:
  - "it allocates nothing on behalf of Groundhog" is loose: the handle *is* returned to the caller, so a caller that never abandons it is also hidden. Today that is only nostr-gobject and libnostr's simplepool.
  - `async_cleanup_worker` checks `abandoned` once, after completion. An `abandon()` that lands after that check leaks the handle for good, not only at exit. Worth a line in jwj0.
- **`^write_error$`: not acceptable in hosted CI.** It covers a libnostr bug fixable in a few lines (B1).
- **`^nostr_relay_write$`: not acceptable in hosted CI** (B1).

**Do they make the gate stable?** Not fully. Both uncovered leak classes above are the same exit-race family. They showed up at `-j2` beside CPU load comparable to what the hook creates: a 14-way smoke build in the same Docker VM, plus the macOS build on the host. So "passed 9 runs of 9" will not hold on a busy push. The next false block will be OpenSSL DRBG state or `nostr_connection_new`, and adding entries for those is the wrong direction. See N1.

## Gate robustness, checked and fine

- **set -e.**
  - `if ! configure`, `configure || {…}`, `has_report` and `check_forbidden` are all in tested contexts. `has_report` reads to EOF, avoiding SIGPIPE under pipefail.
  - A failed `ctest -N` in the registration check aborts the container, so it fails closed.
  - `sanitizer_gate_wanted` runs in an `if`, so `set -e` is off inside it. Its `exit 1` on status ≥ 2 still blocks.
  - The pre-push test's bare `! grep` assertions are all converted, and no bare negation remains in `scripts/test-*.sh`.
- **Locks.**
  - Each container takes its volume lock (fd 9, per stage and arch), then the shared tests lock (fd 8). The order is always 9 then 8, and the fd-9 locks are distinct per stage, so there is no cycle and no deadlock.
  - Two pushes of the same stage serialise on fd 9.
  - Locks are released when the container exits. `--init` plus docker's signal proxy means the hook's `kill` of the CLI stops the container.
  - With `NOSTRC_GATE_AMD64=1`, the sanitizer tests wait for the emulated smoke tests: slow, not stuck.
- **Volumes.**
  - `nostrc-linux-gate-asan-arm64` is 781 MB.
  - A change of configure args wipes `/work/build` rather than growing it, and the history keeps 20 first-run logs.
  - The tests-lock volume is empty. `/gate-lock` is created `ci`-owned in the image, and the volume is only ever mounted after the new image is built.
- **Docker absent or broken.**
  - Absent: the hook blocks with a message, or skips both Linux stages with a banner under `NOSTRC_SKIP_LINUX_GATE=1`.
  - Failing `docker info`, `docker build` or `docker run` all exit non-zero, so the push is blocked.
- **Can it pass with a broken build?** No:
  - A failed build of the job's targets prints BUILD FAILED.
  - A test missing from the registered set fails the "every listed test is registered" check.
  - An unbuilt executable shows as "Not Run", and a skip as "***Skipped". Both match `FORBID_PATTERN` on the first run and the rerun.
  - An unreadable job fails before any container starts, since the reader exits 2.
- **Rerun policy.**
  - A failed test whose output has `(ERROR|SUMMARY): …Sanitizer` or `: runtime error:` blocks at once and is not rerun, even beside other failures (P1: 13 tests, no rerun).
  - CTest does not truncate the output the grep reads (400 KB checked).
  - A non-report failure (under load I saw SIGTRAP, timeouts and aborts at `-j8`) is rerun serially with the same LSan options. A report in the rerun blocks, so a **deterministic** leak cannot hide behind a non-report failure. A racy leak that coincides with a timeout can: the gate is laxer than CI, which never reruns. That matches the smoke run's documented policy; acceptable.

## Non-blocking findings

- **N1. The load the tests lock guards against still reaches the sanitizer tests through builds.**
  - The tests lock is taken after each stage's build. The sanitizer tests therefore still run beside the smoke stage's `nproc`-way build in the same VM, and beside the host's macOS build. That is the load that produced the false blocks behind 61eb2453, and the uncovered leaks above.
  - Consider:
    - taking the tests lock before the smoke *build* as well, so the sanitizer tests start only when the smoke stage's heavy phase is over;
    - or `nice`-ing the builds;
    - or capping the sanitizer build's `JOBS`.
  - File the two uncovered exit races (OpenSSL DRBG via sqlcipher, `nostr_connection_new`) under xbso/jwj0 rather than suppressing them.
- **N2. Memory.** Two `nproc`-way (14) builds, one with ASAN links, run at once in an 8 GB Docker VM. Nothing failed in my runs, but an OOM kill would read as BUILD FAILED. A `JOBS` cap on the sanitizer build would also help N1.
- **N3. Comments in lsan.supp describe the wrong mechanism.** The `^nostr_relay_write$` and `^write_error$` comments describe a queued request at teardown and a timed-out caller. The captured leaks match the enqueue-failure refcount bug (B1). They should say so, or go away with the fix.
- **N4. AGENTS.md** lists the filter paths verbatim. Update it with B2, or point it at `SANITIZER_PATHS`, so the two cannot drift.

## Docs-only push

I ran the real `scripts/pre-push` on a docs-only commit on the tip (`docs/reviews/…md`). The ref line was crafted so that the remote oid is `61eb2453`. The hook **exited 0** in 7m59s:

- The sanitizer stage **skipped**, printing: "sanitizer stage skipped for refs/heads/rev-docs-scratch: none of its 1 changed file(s) is under gnome/groundhog/ …".
- Linux smoke: 438 run. It passed after one rerun of `nip55l_dbus_contract_fail_teardown`, a known smoke flake unrelated to this change.
- macOS: BUILD_GROUNDHOG=OFF, 367/367.

A second clean-tip gate run afterwards passed 48/48 in 1m29s. That run also put the shared volume back on the tip's source after my planted-leak runs.

## Reproduction

- **Filter inputs.** In the gate volume, run:
  ```bash
  ninja -C /work/build -t inputs "${GH_SAN_TARGETS[@]}"
  ninja -t deps <objects>
  ninja -t inputs build.ninja
  ```
- **Unsuppressed runs.** Take lsan.supp without the three lines and set:
  ```bash
  LSAN_OPTIONS=suppressions=<that file>:print_suppressions=1
  ```
  Keep the job's `ASAN_OPTIONS`/`UBSAN_OPTIONS`/`G_SLICE`, and run:
  ```bash
  ctest -R "<set>" --parallel {2,8,14} --output-on-failure
  ```
  For the "beside the hook's load" runs, add 12 busy loops.
- **P2.** In `write_thread`, replace `go_channel_unref(answer);` with a comment, then run `scripts/linux-gate.sh --sanitizers <checkout>`.

## Verdict

**REQUEST CHANGES**

The four changes needed:
1. Fix libnostr's enqueue-failure unref and drop `^nostr_relay_write$`/`^write_error$`, or keep them out of hosted CI.
2. Complete the path filter, ideally fail-safe.
3. Make the filter fail closed on git errors.
4. Make the job reader reject `env`/step keys it does not reproduce.

`^nostr_subscription_free_async$` can stay, with its comment corrected.

---

# Final pass: branch rebased onto 15954a4f, tip 797b339a

New commits:

- `67dbbdcd` fix(libnostr): a write the relay cannot queue releases its answer channel (libnostr 1.1.1)
- `cdae6bd7` test(groundhog): drop the `nostr_relay_write` and `write_error` LSan entries
- `24be6e0b` infra(gate): complete fail-closed filter plus `check-inputs`, job env, the stage runs alone
- `797b339a` fix(libnostr): answer and free the writes still queued when a relay is freed

## What I ran on 797b339a

| Check | Result |
|---|---|
| `check-unsequenced-args.py` | exit 0 |
| `test-sanitizer-gate.sh` (now with "the input list against ninja") | passed |
| `test-pre-push.sh` | passed |
| `test-linux-gate-smoke.sh` | passed |
| `libnostr/test_relay_write_unsent` under the job's ASAN/UBSAN/LSan options | 5/5 "ok", no report |
| Real gate, clean tip, run 1 | **falsely blocked**: `groundhog-relay-wire`, `nostr_connection_new` channels (nostrc-vpha) |
| Real gate, clean tip, run 2 | passed, 48 run |
| `check-inputs` in both gate runs | "all 844 source inputs of the job's build are under SANITIZER_BUILD_PATHS" |
| `check-inputs` with `libjson/` removed from the list, same real build graph | exit 1, naming `libjson/CMakeLists.txt`, `libjson/include/nostr_jansson.h`, … |
| Real gate, planted P2 (Groundhog forgets `go_channel_unref(answer)`) | **blocked**: 10 tests with reports at `nostr_relay_write (relay.c:1580) ← write_thread (gh-relay-gnostr-write.c:48)`, no rerun |
| The set, `-j2`, 10×, each run holding the gate's `build.lock` exclusive and `tests.lock` (host load average ≈ 20 from other agents) | 9 pass, 1 fail: `groundhog-relay-wire`, `nostr_connection_new`, library-only stack |
| `groundhog-relay-wire` alone, 20× (same locks) | 0/20 fail |
| My B4 mutations, rerun | job-level `UBSAN_OPTIONS` now reaches the tests; workflow env and configure-step `CC`/`CFLAGS` are applied; `working-directory`, a job `container`, another `runs-on` and a `$GITHUB_ENV` write are rejected (exit 2, named) |

## Blockers 1–4: all closed

**B1 (suppressions): closed.**
- `^nostr_relay_write$` and `^write_error$` are gone.
- My P2 plant, which passed the gate at 61eb2453, now blocks 10 tests.
- The `free_async` comment is corrected, including the returned-handle blind spot.

**B2 (path filter): closed.**
- `SANITIZER_BUILD_PATHS` covers every input I listed.
- `SANITIZER_PATHS` adds the workflow, the Dockerfile and the gate's own scripts.
- `check-inputs` proves the list against the candidate's real ninja graph, in the gate after every sanitizer build and in the hosted job.

**Is check-inputs sound?** Yes, for what it claims:
- It unions three views of the graph: `ninja -t inputs <job targets>` (recursive, so the sources of generated files are included), every header in the deps log, and `ninja -t inputs build.ninja` (the configure inputs).
- It discards generated and system paths, and fails with exit 2 if it finds no Groundhog input, so it cannot pass vacuously.
- I proved the negative case on the real graph, not only the fake ninja in the test.
- It does not see run-time-only inputs. None exist in this set: the only programs its tests spawn are the `groundhog` target and the test binary itself.
- A push that makes a new directory an input must edit a covered build file (Groundhog's or the root CMake, or `cmake/`). So such a push runs the stage, and `check-inputs` then names the new directory.

**B3 (fail closed): closed.** `git diff` runs on its own. On failure the stage runs and prints why. `test-pre-push.sh` covers an unknown remote oid.

**B4 (job env): closed.** Env is applied per phase with Actions' precedence, and every key it does not reproduce is rejected, as tabled above.

## libnostr fixes: correctness and threading

**67dbbdcd (enqueue failure).** Correct.
- `write_error()` now owns the writer's reference: it sends, frees the Error if the send fails, then closes and unrefs, exactly as `write_operations()` does.
- The out-of-memory branch no longer drops that reference early. Before, the caller's unref could free the channel under `write_error`'s send, a latent use-after-free.
- Double close is safe: `go_channel_close` is idempotent under the channel mutex.
- A buffered send cannot block: capacity 1, and exactly one value is ever sent.

**797b339a (drain at free). It does not race the writer thread.** In `relay_free_impl` the order is:

1. Cancel the connection context.
2. Close `write_queue`.
3. Null out and close the connection's channels.
4. `go_wait_group_wait(&workers)`. `write_operations` calls `go_wait_group_done` as its very last statement, so the writer has exited.
5. Only then `relay_write_queue_drain()`, then `go_channel_free`.

On the producer side:
- `nostr_relay_write` after step 2 fails to enqueue and takes the (now fixed) `write_error` path, so nothing new lands in the queue.
- `go_channel_try_receive` does not check `closed`, so it drains the items still buffered in a closed channel. That is what the drain relies on.
- The only way to race the drain would be a caller writing through a relay pointer it holds no reference to. That is already a use-after-free of the relay, not something the drain introduces.

The test covers all three paths deterministically and offline: unqueued with the caller waiting, unqueued with the caller gone first (×50), and queued at free (×3). Each path checks the refcount returns to the caller's.

**Non-blocking residue**, all libnostr-only and all rare:

- **R1.** `nostr_subscription_close()` and `nostr_subscription_fire()` receive a `write_err` in their 0-ms select but store it only `if (err)`. `gnostr_subscription_finalize` passes `NULL`, so a received Error leaks. This was hidden by `^write_error$` before; now it would surface as `new_error ← write_error`, a library-only stack. Fix: free it when `err == NULL`.
- **R2.** `write_error`, the drain and `write_operations` all have the same window: the send succeeds just before the caller closes after its own timeout. The Error then sits in the buffer of a channel whose last unref does not free its items. That is a small race, not a regression.
- **R3.** `nostr_relay_close()` leaves queued writes for `nostr_relay_free()` to answer. Callers wait their own timeout, 5 s in Groundhog. Answering at close too, once the workers have exited, would be friendlier.

## Locks and the host-build signal

The locks cannot deadlock:
- **Order.** Each container takes its volume lock (fd 9), then for a build `build.lock` shared (fd 7), which it releases after the build. The smoke stage then takes `tests.lock` (fd 8). The sanitizer stage waits for the host signal while holding only fd 9, then takes fd 7 exclusive, then fd 8.
- **No cycle.** Nobody holds fd 8 while waiting for fd 7, and fd 9 is per volume.
- **Exits.**
  - A failed host build exits the hook, whose `kill` stops the waiting container (`--init`, signal proxy).
  - A standalone `linux-gate.sh` run has no signal and does not wait.
- **Remaining risk.** A continuous stream of shared build holders could postpone the exclusive test phase. That would need many concurrent gates: acceptable.

One cost to note: job-level env is also configure env. Changing a job-level sanitizer option therefore restarts the build tree from scratch. It is correct, just slow.

## The false-block rate, and what to do about it

**Measured.** 2 false blocks in 12 clean runs of the set (≈17%). Both were `groundhog-relay-wire` leaking `nostr_connection_new`'s `recv_channel`/`send_channel`.

The stacks are library-only:

```
go_channel_create ← nostr_connection_new ← nostr_relay_connect ← gnostr_relay_connect ← connect_async_thread ← gio/glib ← asan_thread_start
```

The leaked objects are Direct leaks of the channels without the connection. That points to a refcount left above zero on some teardown path in libnostr, not merely a thread alive at exit. Alone, the test leaked 0 of 20 times. The author's other class, OpenSSL per-thread RNG/ERR state, is rarer.

**Why landing as-is is not acceptable.** At about one clean push in six blocked, `NOSTRC_SKIP_SANITIZER_GATE=1` becomes habit, or new suppressions creep back. Both undo exactly what this branch is for.

**Why vpha need not be fixed first.** The `nostr_connection_new` leak needs a libnostr teardown investigation. The OpenSSL class is per-thread state of GLib pool threads alive at exit. Neither is small, and neither should hold the gate hostage.

**Recommendation: the library-leak rerun rule, precisely as follows.** It is gate-only; hosted CI stays unchanged and strict.

1. **Hard reports: block, never rerun (as now).** A failed test is hard if its first-run output contains any of:
   - `ERROR: AddressSanitizer:` (use-after-free, overflow, SEGV, double free, …);
   - `: runtime error:` (UBSAN);
   - a `SUMMARY:` that is not LSan's `SUMMARY: AddressSanitizer: <n> byte(s) leaked in <m> allocation(s).`;
   - `ERROR: LeakSanitizer:` followed by anything other than `detected memory leaks`.
2. **Parsing an LSan-only failure.**
   - Split the output into leak records, each from `^(Direct|Indirect) leak of` to the next blank line.
   - Frame lines are `^\s+#\d+ 0x[0-9a-f]+ (in (\S+) (\S+)|\((\S+)\+0x[0-9a-f]+\))`.
   - Strip the build's source root (`/work/src/`) from source paths.
   - A frame is **library** if and only if one of these holds:
     - (a) its source path starts with `libnostr/`, `libgo/`, `nostr-gobject/` or `libjson/`;
     - (b) its path contains `/libsanitizer/`;
     - (c) it has no project source path and its module is under `/lib/` or `/usr/lib/` (glib, gio, libc, libcrypto, libsqlcipher, …).
   - Every other frame **implicates** the push. That includes any `gnome/groundhog/` path (application or tests), any other project directory (`libmarmot/`, `marmot-gobject/`, `nips/`, `tests/`, …), a project module without a source path, and any line that is not a recognised frame.
3. **Classification.**
   - A record is library-only if all its frames are library frames.
   - A test is a **library leak** if it has at least one record, every record is library-only, and it is not hard.
   - Zero parsed records, a truncated record, or any parse surprise makes the test hard. Parsing fails closed.
4. **Decision.**
   - If any failed test is hard, or has an implicating record, block with no rerun, as now.
   - If more than 2 distinct tests are library leaks in one run, block: that is a regression, not a race.
   - Otherwise, rerun the library-leak tests (and any report-free failures, as now) once, serially, under the same locks and env. Any failure in the rerun, of any kind, blocks.
5. **Visibility.**
   - A pass after such a rerun prints `!! LIBRARY LEAK RERUN (nostrc-vpha): <test>`.
   - It also prints the first run's whole output and the first three non-allocator frames of each record.
   - It records the rerun in `gate-history/reruns` tagged `lib-leak`, and reports "needed a library-leak rerun in N of the last 20 gates".
6. **Sunset.** The allow-list lives in one named constant in `linux-gate-smoke.sh`, referenced from AGENTS.md and nostrc-vpha. It is removed when vpha closes.
7. **Tests in `test-linux-gate-smoke.sh`:**
   - a library-only record reruns and passes;
   - the same leaking again in the rerun blocks;
   - a record with a `gnome/groundhog/` frame blocks without a rerun;
   - a library-only record beside a Groundhog record blocks;
   - library-only leaks plus a UAF report block;
   - a `libmarmot/` frame blocks;
   - an unsymbolized project-module frame blocks;
   - an LSan header with no parsable record blocks;
   - three library-leak tests block.

**What the rule preserves and what it gives up.**
- A **deterministic** leak reproduces in the rerun and still blocks, whatever its frames. So a Groundhog bug whose leak is allocated on a library thread (an object handed to a Groundhog callback and never freed) is still caught.
- What it gives up is only a leak that is both racy and library-only, which is the vpha class. CI, which never reruns, still reports those.
- The OpenSSL class keeps blocking, deliberately: its stacks run through Groundhog's store (`… sqlcipher_codec_key_derive … sqlite3_finalize ← store_query_text gh-store.c:925 ← … gh_store_open_with_key ← open_worker gh-account-store.c:358`), so the rule sees a Groundhog frame. It is rare. Fix it under vpha rather than widening the rule.

**Expected rate.** Roughly 17% × P(`relay-wire` leaks again alone): 0 of 20 here, so well under 1%, plus the rare OpenSSL class.

Separately: hosted `groundhog-sanitizers` runs the same set at the same `-j2` without reruns, so it should show the vpha flake too. That argues for giving nostrc-vpha real priority. I have not modified beads.

## Final verdict

All four original blockers are closed, and the libnostr fixes are correct and race-free with the writer thread. One blocking item remains: as it stands, the sanitizer stage falsely blocks about one clean push in six (nostrc-vpha). Land it together with the library-leak rerun rule specified above. The rule is small, keeps every deterministic leak blocking, and leaves hosted CI strict. Fixing nostrc-vpha first is not required.

**REQUEST CHANGES**

---

# Final pass 2: library-leak rerun rule (3fa0be54) and R1 (483d82ef)

## Checks

- Ran `check-unsequenced-args.py`, `test-linux-gate-smoke.sh` ("ok: the library-leak rerun rule (nostrc-vpha)"), `test-sanitizer-gate.sh` and `test-pre-push.sh` at 3fa0be54. All pass.
- Classified the real per-test outputs I kept from the earlier passes, taken from the ctest logs of the unsuppressed and rate runs:

| Failed test's output | Class | Correct? |
|---|---|---|
| dm-send: `new_error ← write_error ← go_wrapper_func` | library | yes |
| privacy-e2e: `nostr_subscription_free_async ← gnostr_subscription_finalize …` | library | yes |
| relay-wire: `go_channel_create ← nostr_connection_new ← … connect_async_thread` (×2 logs) | library | yes (the vpha class) |
| mls-service: `go_channel_create ← nostr_relay_write ← write_thread gnome/groundhog/…` | implicating | yes: a Groundhog frame, so it blocks |
| privacy-e2e: OpenSSL `CRYPTO_zalloc … ← gh-store.c ← gh-account-store.c` | implicating | yes, as specified |
| account-store / nip29-service / group-ui (SIGTRAP, timeout) | none | yes: an ordinary rerun as before |

## Is the classifier correct and fail-closed?

I crafted outputs to try to sneak a Groundhog frame through. These fail closed correctly:
- `hard`:
  - a library record beside a UBSAN `runtime error:`;
  - a library record beside `ERROR: AddressSanitizer: heap-use-after-free`;
  - a record not ended by a blank line (the SUMMARY line is read as an unrecognised frame);
  - a header with no record, a record cut off at the end of the output, and records without the LSan header.
- `implicating`:
  - a Groundhog function in a build-tree module (`(…/gnome/groundhog/test-…+0x10)`);
  - a Groundhog path given without the source root;
  - a lookalike prefix (`libnostr-extra/`);
  - an inline glib header frame (`/usr/include/…`), which is stricter than the spec, harmlessly.

**Holes.** Three inputs get through as `library`. None is reachable in today's build. All are cheap to close:

- **H1. `..` in a frame path is not normalised.** `/work/src/libnostr/../gnome/groundhog/src/relay/x.c` and `/work/src/nostr-gobject/src/../../gnome/groundhog/include/gh.h` both classify as library, as does a module `/usr/lib/../../work/build/gnome/groundhog/libgh.so`.
  - The real build does spell include directories that climb out of an allowed prefix: `-I/work/src/libgo/../libnostr/include`, 1702 uses. But every one of them lands in another allowed prefix, and the deps log holds no header path escaping an allowed prefix.
  - So a Groundhog frame could only arrive this way if libnostr, libgo, nostr-gobject or libjson included Groundhog code: a dependency inversion.
  - Fix: apply `os.path.normpath` to the path (and to module paths) before the prefix test, and treat any result that still starts with `../` as implicating.
- **H2. `/libsanitizer/` matches anywhere in the path.** So `/work/src/gnome/groundhog/libsanitizer/x.c` is a library frame. This is contrived. Fix: accept it only for paths outside the source root, e.g. starting with `../../../../src/libsanitizer/` as GCC's runtime is spelled.
- **H3. `FRAME` is not anchored at the end.** A line holding a library frame followed by a second, Groundhog, location is read as the library frame alone. Real ASan output prints one frame per line, so this is not reachable. Fix: add `(?:\s+\(BuildId: [0-9a-f]+\))?\s*$`.

In every case the damage is bounded by the rule's design:
- only a leak that is *also* racy slips through, since a deterministic one reproduces in the rerun and blocks;
- hosted CI never reruns.

I recommend closing H1–H3 in a small follow-up. They do not block.

## The deviation (one or more spaces before `(`)

Right. Real symbolizer output for a frame with no source info has two spaces between the address and the module, as in my captured logs: `#6 0xffffb56c2810  (/lib/aarch64-linux-gnu/libgio-2.0.so.0+0xc2810)`. Frames with source info have one space before `in`. Accepting ` +` covers both, and admits no frame that would change a classification.

## Does the policy match the spec?

Yes, point by point:
1. **Hard reports** (non-LSan ERROR, `runtime error:`, a non-leak SUMMARY, an LSan ERROR other than "detected memory leaks") block with no rerun.
2. **Parsing:** records run from `Direct|Indirect leak of` to a blank line, and the source root is stripped. The allow-list is the single constant `LIBRARY_LEAK_ALLOW`, plus the sanitizer runtime and modules under `/lib/` or `/usr/lib/`. Any other frame, or an unrecognised line, implicates.
3. **Classification:** zero records, an empty record, or a record cut off makes the test hard.
4. **Decision:**
   - any hard or implicating test blocks with no rerun, and the classifications are printed;
   - more than `LIBRARY_LEAK_MAX=2` library-leak tests block;
   - otherwise there is one serial rerun under the held locks and the same env, and any failure of the rerun blocks.
5. **Visibility:** reruns are announced as `!! LIBRARY LEAK RERUN (nostrc-vpha)` with their records and the first run's whole output. They are tagged `lib-leak` in gate-history and counted separately.
6. **Sunset:** marked for removal in the script header, AGENTS.md and scripts/README.md.
7. **Tests:** every case I listed is covered, including a repeated leak in the rerun, a Groundhog frame, library beside Groundhog, UAF, OpenSSL via store, libmarmot, an unsymbolized project module, a header without a record, and three library-leak tests. They also cover two library leaks plus a report-free flake, and the smoke run being unaffected.
8. **Hosted CI** is unchanged.

A classifier crash (a Python exception) aborts the stage under `set -e`: it blocks without a message, so it fails closed.

**R1 (483d82ef).** Correct. `nostr_subscription_close()` and `fire()` now `free_error()` a received Error when there is no `err` to take it. I agree that a deterministic test isn't feasible, since there is no seam to order `write_error()`'s thread against a 0-ms select.

## Final verdict

The rule is correct on every real leak shape seen in this review. It fails closed on every realistic malformed or mixed output I could construct, and it matches the specification. Both planted leaks still block; the author reports 12/12 clean passes. The three classifier holes (H1 path normalisation, H2 sanitizer-path anchoring, H3 end-anchoring) are unreachable in today's build and are recommended as a follow-up, not a condition.

**APPROVED**
