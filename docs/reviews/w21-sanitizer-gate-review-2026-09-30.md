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
