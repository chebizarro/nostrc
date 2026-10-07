# Peer review: concurrency-audit fixes (HEAD~3..HEAD)

**Date:** 2026-10-06
**Commits:** `9ab89238` (libgo), `493dfa26` (libnostr), `ad2dfe53` (libjson + manifest)
**Scope:** Correctness only. I checked every load-bearing claim against the code at HEAD (`git show`, file reads). I built `go_fiber`, `nostr`, and the `gof_test_*`/`go_select_test`/`go_ticker_test` targets in `_build` and they pass, including 100× `gof_test_chan` and 50× `gof_test_starvation` with no failures. These tests do not exercise the multi-worker or external-waker interleavings below, so passing them does not show those paths are correct.

## Verdicts

| Commit | Verdict |
| --- | --- |
| `9ab89238` libgo park protocol | **REQUEST CHANGES**: 1 critical, 3 high, 1 medium |
| `493dfa26` libnostr | **REQUEST CHANGES**: 1 high, 1 medium, 2 low |
| `ad2dfe53` libjson / manifest | **APPROVED**. The manifest's "no API or ABI change" line needs a caveat (see L2) |
| **Overall** | **REQUEST CHANGES** |

---

## 1. `9ab89238` — libgo fiber scheduler prepare/commit park protocol

### C1 (critical): the worker's commit loses the wake when it sees `GOF_WOKEN` directly
`libgo/fiber/sched/sched.c:727-775`

```c
gof_state fstate = atomic_load_explicit(&f->state, memory_order_acquire);
if (fstate == GOF_PARKING) { ... CAS PARKING->BLOCKED, else-branch requeues WOKEN ... }
else if (fstate == GOF_RUNNABLE) { rq_push }
else if (fstate == GOF_FINISHED) { free }
else { /* "GOF_BLOCKED/GOF_WOKEN should not be observable here"; treat as parked */ }
```

The WOKEN requeue only runs when the first load sees `PARKING` and the CAS then fails. Suppose a waker on another thread (another worker, the poller, the timer path, or a channel op on an OS thread) runs `fiber_wake_claim` → `PARKING→WOKEN` between the parker's PARKING store and this load. That window covers the whole `gof_ctx_swap` and the `W->rq_mu` lock/unlock at 724-726, which is the case the protocol was built for. The load then reads `GOF_WOKEN` and the code falls into the final `else`, which does nothing. The fiber stays `WOKEN` forever, and every later waker gets claim 0 (sched.c:392). **The fiber is parked permanently, and the channel handoff value delivered with `done=1` is stranded.**

The comment at 772-773 is wrong: WOKEN is observable at this point. On a single worker with fiber-only wakers the window cannot open, which is why the tests pass.

**Fix:** handle `fstate == GOF_PARKING || fstate == GOF_WOKEN` in the first branch. Always try the `PARKING→BLOCKED` CAS, and on failure (state is `WOKEN`) do the requeue. Make the final `else` assert or abort instead of silently dropping the fiber.

### H1 (high): lost wake through `fiber_wake_claim`'s RUNNABLE branch
`sched.c:384-391`

The waker does `load(state)==RUNNABLE` and then, as a separate operation, `store(wake_pending,1)`. Nothing stops the parker and its worker from completing every check in between. This seq_cst total order is legal:

1. Waker L: loads `RUNNABLE`.
2. Parker P2: stores `PARKING`.
3. Parker P3: `xchg(wake_pending)` returns 0.
4. Parker swaps out.
5. Worker C1: CAS `PARKING→BLOCKED` succeeds.
6. Worker C2: `xchg(wake_pending)` returns 0.
7. Waker S: stores `wake_pending=1`.

The fiber ends up `BLOCKED` with an unconsumed pending flag, and the wake is lost. It only takes the waker thread being preempted between two instructions. The comments in `sched.h` ("One of those three checks always observes the pending wake") and at sched.c:385-388 claim something that does not hold.

**Fix:** after setting `wake_pending`, re-load `state`. If it is no longer `RUNNABLE`, try to take the wake back with `xchg(wake_pending,0)`:
- If you get 1, nobody consumed it, so `continue` the loop and claim through the CAS paths (`PARKING→WOKEN` or `BLOCKED→RUNNABLE`).
- If you get 0, the parker or the commit consumed it, so return 1.

### H2 (high): `make_runnable*` calls `sleepers_cancel(f)` after a claim==1, which can cancel a later sleep
`sched.c:849-852` and `sched.c:881-884`

```c
int claim = fiber_wake_claim(f);
if (claim == 0) return;
gof_worker *W = cur_worker();
sleepers_cancel(f);          // runs for claim==1 too
if (claim == 1) return;
```

For claim==2 this order is safe, because the fiber cannot run until this thread `rq_push`es it. For claim==1 it is not: the owning worker requeues the fiber concurrently (WOKEN branch), or the parker consumes the pending flag itself. Concrete failure:

1. A sender calls `handoff_to_waiter` → claim `PARKING→WOKEN`, then is preempted before `sleepers_cancel` (which contends on `sleepheap.mu`).
2. The receiver's worker requeues it. It returns from `gof_chan_recv` through the lock-free `done` fast path, calls `gof_sleep()`, and `park_until` registers a new heap entry and commits `BLOCKED`.
3. The sender resumes, and `sleepers_cancel(f)` sets the new entry to NULL. **The fiber sleeps forever.**

The cancel is redundant for claim==1 anyway: the WOKEN commit (758), the pending commit (748), and the park_until P3 path (927) each cancel for themselves.

**Fix:** move `if (claim == 1) return;` above `sleepers_cancel(f)` in both functions.

### H3 (high, regression): `sleepers_wake_ready` claims after releasing `sleepheap.mu`, which allows a UAF
`sched.c:418-425`

The old code ran the claim CAS while holding `sleepheap.mu`. Any competing waker (e.g. `on_ready` → `make_runnable`) has to take that mutex in `sleepers_cancel` before it `rq_push`es, so the fiber could not run and finish, and be freed in worker_main (766-770), until the timer's claim was done. The new code unlocks first:

1. The timer pops `f` and unlocks.
2. The IO waker claims `BLOCKED→RUNNABLE`, `sleepers_cancel` (now uncontended), and `rq_push`.
3. The fiber runs to completion and is freed.
4. The timer calls `fiber_wake_claim(f)` on freed memory. If the memory was reused and reads as 0 (`RUNNABLE`), the claim writes `wake_pending=1` into it, corrupting the heap.

`fiber_wake_claim` takes no locks, so nothing required dropping the mutex first.

**Fix:** call `fiber_wake_claim(f)` while still holding `sleepheap.mu`, and unlock only around `rq_push` for claim==2, as before.

### M1 (medium): stale pending wakes are now created routinely, and some parkers don't loop

Under the old code, a late second waker (a timer firing after an IO wake, `gof_chan_close` waking a waiter that already returned, a select signal after the fiber moved on) failed its CAS harmlessly. Now any claim that finds `RUNNABLE` leaves `wake_pending=1`, and the fiber's *next* unrelated park returns immediately. Parkers that don't re-check their condition misbehave:
- `__gof_sleep_ns` (`libgo/fiber/timers/timer_bridge.c:17`) calls `park_until` once, so a sleep can return early.
- `wait_event_with_deadline` (`libgo/fiber/io/io.c:189,197`) also returns early. This one is benign because its callers retry.

Two `chan.c` patterns make the same class of problem worse:
- **Unsynchronized `done` read** (`libgo/fiber/chan/chan.c:142`, `:184`). `done` is a plain `int` written under `c->mu` by `handoff_to_waiter`/`handoff_from_waiter` (chan.c:39-41, 50-52) and read here without the lock. That is a C11 data race (UB; TSAN will report it). It also lets a parker that resumed spuriously see `done==1` and leave while the waker is still inside `gof_sched_make_runnable(r->f)`. The fiber can then finish and be freed while the waker claims it, which is a UAF on `f`.
  - **Fix:** drop the unlocked fast path and read `done` only under `c->mu`. `handoff_*` holds `c->mu` across `make_runnable`, so this serializes correctly, and the first check after the wake still returns 0 on success. Alternatively, make `done` `_Atomic` and have the waker set it *after* `make_runnable`.
- **`gof_chan_close` wakes outside the mutex** (chan.c:83-84). A parker that saw `closed` under the lock (chan.c:143, 186) returns -1 before close calls `make_runnable(w->f)`. That produces the same stale-wake and late-claim-on-freed-fiber exposure.
  - **Fix:** wake while holding `c->mu`. The lock order `c->mu → sched locks` is already established by `handoff_*`.

**Waiter-node lifetime (asked explicitly): correct.** After `qpush` the parker never touches `w`. Only the side that pops a waiter (`handoff_*`) or detaches it (`close`) frees it. The under-mutex recheck covers the three cases (`done`, `closed` → detached and owned by close, or still queued → re-park). The stack-allocated `done` is never written after the parker returns, because writers set it before waking and the parker returns only after seeing it or after seeing `closed`, and close never writes `done`.

### Interleaving walk-through (as requested), assuming C1/H1–H3 are fixed

| Waker arrives… | Path | Outcome |
| --- | --- | --- |
| before PARKING store | RUNNABLE → `wake_pending` | P1 or P3 consumes it and the fiber never leaves the CPU. **Broken today by H1** (split load/store) |
| between PARKING store and swap | CAS PARKING→WOKEN (claim 1) | The worker commit must requeue. **Broken today by C1** if the waker wins before the load at 727. If P3 also sees a pending flag, the parker's plain store of RUNNABLE (sched.c:835/926) overwrites WOKEN. That is benign: the fiber never swapped out, so the two wakes merge and nothing is enqueued twice |
| between swap and commit CAS | same as above | same (**C1**) |
| after commit | CAS BLOCKED→RUNNABLE (claim 2), caller enqueues | The C2 pending path (743-751) races with a CAS, so exactly one side enqueues. **No double-enqueue** ✔ |

**Return-value handling:**
- `sleepers_wake_ready` (420) enqueues only on claim 2. ✔ (but see H3)
- `make_runnable` and `make_runnable_from_poller` enqueue only on claim 2. ✔ (but see H2)

**`gof_sched_yield`** (807-815) still stores `RUNNABLE` and gets requeued once at 762. Unchanged. ✔

**`fiber_entry_tramp`** (300-309): `FINISHED` gives claim 0. ✔ A RUNNABLE-branch waker racing a finishing fiber can write into it after it is freed. That is the same class of problem as before, made worse by M1.

**ABI:** `gof_fiber` is opaque in the public headers (`libgo/include/go.h:84`, `libgo/fiber/include/libgo/fiber.h:25`), so adding `wake_pending` is internal only. ✔

### Bundled items
- **CLOCK_MONOTONIC condvar** (sched.c:461-467, 695-707): correct. The macOS fallback is unchanged. ✔
- **`gof_sched_run` create check** (sched.c:789-804): it no longer joins indeterminate tids. ✔ *Low:* `S.nworkers` is not reduced after a failure, so partition, affinity, and rebalance routing can still pick a worker that never started, and fibers enqueued there will never run. Clamp the count before any fiber runs, or document that this is fatal.
- **`select.c`** (388-395, 539-546): clearing `fiber_handle` under `waiter->mutex` matches what the signaler does (select.c:88-95, 197-204). ✔ `go_select` loops, so spurious wakes are tolerated.
- **`ticker.c`** (27-46): failures are now checked and unwound. ✔
- **Out of scope, pre-existing:** `mpmc_wait_busy` (`libgo/src/channel.c:600-608`) builds a **CLOCK_REALTIME** deadline and passes it to `gof_hook_block_current_until`, which goes to `park_until` and compares it against **monotonic** time. The fiber effectively parks for decades unless something else wakes it. This is only reached under `NOSTR_CHANNEL_MPMC_SLOTS`. Track it separately.

---

## 2. `493dfa26` — libnostr

### H4 (high, regression): `nostr_simple_pool_start()` can start a worker that exits immediately
`libnostr/src/simplepool.c:1325-1328` together with `:1172`

`start()` now runs `pthread_create(...)` and stores `running=true` **after** that call returns. The worker's first action is `while (atomic_load(&pool->running))` (1172), which happens before it takes any lock. If the new thread is scheduled before the creator's store (allowed by POSIX, and common on Linux), it reads `false` and returns. The pool then reports `running==true` with no worker, so no subscription events are ever drained. `stop()`/`free()` will later join the dead thread without error, which hides the problem.

**Fix:** store `running=true` *before* `pthread_create` while holding `pool_mutex`, and reset it to `false` on failure. The original concern is still covered, because `pool_worker_claim_join` reads `running` and `thread` under the same mutex, and `start()` holds that mutex until `pool->thread` is valid. Joining an uninitialized handle stays impossible.

Other simplepool checks:
- Every other `running` access is atomic or under the mutex. `simplepool.c:344` is an implicit seq_cst store at init, which is fine.
- `pool->thread` is only read under `pool_mutex` (1356) or by the creator. ✔
- **`pthread_create` while holding `pool_mutex`:** no deadlock. The worker just blocks on its first `pool_mutex` lock until `start()` unlocks, and `start()` takes no other lock while holding it. ✔
- **Exactly-once join:** the claim is guaranteed by the `running` flip under the mutex. ✔

### M2 (medium): the atomic `json_interface` is still read with TOCTOU at every use site
`libnostr/src/json.c:23-24, 29-30, 55-56, 106-108, 129-130, 156-158, 181-182, 208-210`

```c
if (json_interface && json_interface->serialize_event)
    return json_interface->serialize_event(event);
```

Every mention of an `_Atomic` object is a separate seq_cst load. If `nostr_json_provider_uninstall()` / `nostr_set_json_interface(NULL)` runs between the check and the call, the next load returns NULL and the call dereferences it. The commit message says this install/uninstall race is fixed, but this change only turns UB into a well-defined NULL dereference.

**Fix:** load once into a local (`NostrJsonInterface *ji = atomic_load_explicit(&json_interface, memory_order_acquire);`) and use only `ji`. Pre-existing callers outside json.c (e.g. `tests/test_fuzz_filter_parse.c:61`) only compare the pointer and are fine.

*Low, related:* `nostr_json_provider_install` and `nostr_json_provider_uninstall` (`nostr_json_glib.c:125-141`) update `s_provider` and `json_interface` in separate steps. A concurrent install and uninstall can leave `json_interface=&s_iface` with `s_provider=NULL`, in which case the trampolines return NULL/-1, or the reverse. It doesn't crash, but the provider silently stops working. To fix, call `nostr_set_json_interface` while holding `s_provider_mu`. The strong-ref trampolines themselves are correct. ✔

### L1 (low): `connection.c` priv graveyard is correct, but its guarantee is a timing heuristic
- **Lock order:** `g_priv_graveyard_mu` is a leaf lock. It is taken inside `g_lws_mutex` (connection.c:992) and possibly inside `priv->mutex` callers through `priv_unref` → `priv_graveyard_add`, but nothing taken while it is held acquires another lock. **No cycle.** ✔
- **No remaining immediate free of a *published* priv.**
  - `priv_unref` (783) goes to the graveyard.
  - The test-mode close (`priv_close_and_unref`, ~1399) goes to the graveyard.
  - The `free(priv)` calls at 1137, 1212, 1228, and 1311 are on `nostr_connection_new` failure paths before the connection is returned. At 1311 the dial failed (`wsi==NULL`), so lws holds no wsi pointing at it. ✔
- **`deferred_cleanup_add`:** its only caller passes `NULL` priv (1015). ✔ The `node->priv` branch in `deferred_cleanup_process` (890-893) is now dead code and could be removed.
- **Double-close fix** (1420-1428): this branch is correct. `conn->priv == NULL` only happens after the service thread detached the connection (1012) and queued `conn` (1015). ✔ A third close more than 2 s later is still a UAF on `conn`, as before.
- **Residual risk:** "2 s guarantees the stray `try_ref` sees `closing`" is probabilistic, the same as the existing conn graveyard. A thread stalled for more than 2 s (SIGSTOP, debugger, heavy swap) still reaches freed memory. That is acceptable as mitigation, but the comments at 676-681 and 776-782 say "guarantees". Soften them, or record the residual risk in the bead.

### L2 (low): ABI/API of the header changes

| Field | Header | Notes |
| --- | --- | --- |
| `_Atomic bool running` | `nostr-simple-pool.h:53` | Size and alignment are 1/1 on the GCC/Clang SysV, AAPCS64, and Darwin ABIs. ✔ |
| `_Atomic int redial_stop` | `nostr-simple-pool.h:100` | 4/4. ✔ |
| `NostrJsonInterface * _Atomic json_interface` | `json.h:28` | Pointer-sized; symbol size unchanged. ✔ |

Layout is **unchanged**, so there is no ABI break. However, `nostr-simple-pool.h` has `extern "C"` guards (lines 16/327), which shows it is meant to be included from C++. `_Atomic` plus `<stdatomic.h>` in a C++ translation unit is a hard error on g++ before C++23. Clang accepts it as an extension. There are no in-repo C++ consumers, and the GIR scanner (CMakeLists.txt:684-686) doesn't read these headers. Even so, the manifest's "No API … change" should mention this C++ source-compatibility caveat. The other option is to keep the public field as plain `bool`/`int` and do the atomic access through `__atomic_*` builtins inside simplepool.c.

### Other libnostr items, verified ✔
- **subscription.c cancel claim (246-256, 271-274, 679-682, 773-776, 1263-1266):** `cancel` is only assigned at creation (227) and never re-armed. Nulling it in `unsubscribe` therefore only skips repeat calls to an idempotent go_context cancel. **No caller depended on `cancel` still being set after unsubscribe.** None of the claim sites runs with `sub_mutex` already held (nsync is non-recursive), and the cancel is invoked after the lock is released.
- **nostr_log.c:** the rate limiter now holds the mutex across check+increment, and `fprintf` happens outside the lock. ✔
- **relay.c `random_double`:** the CAS loop is correct, and seed 0 is excluded. ✔
- **relay.c / relay_optimized.c:** `pthread_once` is correct. ✔
- **`json.c` `g_json_force_fallback`:** the atomic store-on-init is idempotent. ✔
- **`shutdown_requested` / `redial_stop`:** the atomic conversions are correct. ✔

---

## 3. `ad2dfe53` — libjson + VERSION_MANIFEST.md

`navigate_path` (`libjson/src/json.c:1416-1435`): `strtok_r` with a local `saveptr` on a `strdup` copy is correct and reentrant. Feature-macro visibility is the same as for the existing `strdup`. **APPROVED.** The manifest row should carry the L2 C++ caveat, and should not describe the park protocol as fixed until C1/H1–H3 land.

---

## Recommendations (in priority order)

1. **C1:** handle `GOF_WOKEN` in the worker_main commit dispatch.
2. **H1:** re-validate state after setting `wake_pending` in `fiber_wake_claim`, taking the wake back with xchg.
3. **H2:** return early on claim==1 before `sleepers_cancel` in both `make_runnable` variants.
4. **H3:** run the claim under `sleepheap.mu` in `sleepers_wake_ready`.
5. **H4:** set `running=true` before `pthread_create` in `nostr_simple_pool_start`, and roll it back on failure.
6. **M1:** read `done` only under `c->mu` in `gof_chan_send`/`gof_chan_recv`, and wake under `c->mu` in `gof_chan_close`. Make `__gof_sleep_ns` loop until its deadline.
7. **M2:** snapshot `json_interface` once per call in json.c. Set the interface while holding `s_provider_mu`.
8. Add a multi-worker park/wake stress test that uses wakers on external threads (poller/timer thread and an OS-thread channel sender). None of the current `gof_test_*` tests reach the C1/H1/H2/H3 windows.
9. Low: clamp `S.nworkers` after a worker creation failure; soften the "guarantees" wording on the graveyard; add the C++ caveat to the manifest; file the `mpmc_wait_busy` REALTIME/monotonic deadline bug.

---

# Re-review: `18db72d2` "address review of audit fixes"

**Date:** 2026-10-06
**Method:**
- Read `git show 18db72d2` in full, plus the surrounding code at HEAD.
- Rebuilt `_build`; `ctest` passed **381/381** (1 skipped: `test_nip5f_tcp`).
- Ran 60× each of `gof_test_chan`, `gof_test_starvation`, `gof_test_io`, and `gof_test_io_timeout` with 1 worker: 0 failures.
- Ran a new **multi-worker** stress harness (described below).

## Verdict: **APPROVED**

All blocking findings (C1, H1–H4, M2) are resolved. M1 is resolved except for one narrow residual that I filed as a follow-up. That residual, the open low-severity items, and a pre-existing multi-worker shutdown hang I found while testing are tracked in beads and do not block this merge.

| # | Finding | Status | Evidence |
| --- | --- | --- | --- |
| C1 | Commit missed WOKEN at initial load | **RESOLVED** | `sched.c:749-752`: an explicit `fstate == GOF_WOKEN` arm, plus the PARKING CAS-failure path (747), both go to `requeue_woken` (770). That label stores RUNNABLE, then `sleepers_cancel`, then `rq_push`, then clears `current`. The final `else` (BLOCKED) can no longer be reached from a legal transition: only the worker commit writes BLOCKED, and the fiber itself always announces PARKING first. |
| H1 | `wake_pending` two-step store | **RESOLVED** | The flag is removed from `gof_fiber` (`sched.h`) and nothing references it (`grep` is empty). `fiber_wake_claim` (378-398) now claims `RUNNABLE/PARKING→WOKEN` or `BLOCKED→RUNNABLE` with a single CAS. The parker announces with CAS `RUNNABLE→PARKING` (`block_current` ~831, `park_until` ~930). The claim and the announce are now atomic against each other. |
| H2 | Late `sleepers_cancel` on claim==1 | **RESOLVED** | `make_runnable` (849-855) and `make_runnable_from_poller` (886-892) return on claim==1 before `sleepers_cancel`. On claim==2 the cancel still runs before the enqueue, which is safe because the fiber cannot run until it is enqueued. |
| H3 | `sleepers_wake_ready` UAF | **RESOLVED** | The claim at 423 runs under `sleepheap.mu`, and the lock is dropped only around `rq_push` for claim==2. The cancel invariant the author describes holds (details below). |
| H4 | simplepool startup race | **RESOLVED** | `simplepool.c:1329`: `running=true` is set before `pthread_create`, both under `pool_mutex`, and rolled back on failure (1336). `pool_worker_claim_join` reads `running` and `thread` under the same mutex, so it can never join an invalid handle, and the worker's first check (1172) sees `true`. *Low (non-blocking):* `redial_enabled=true` is now set before create, so on create failure redial stays re-armed for a pool that isn't running. That's harmless, but you could move it to the success path. |
| M1 | `chan.c` unlocked `done` / sleep early return | **RESOLVED except residual** | The unlocked `done` reads are gone (`chan.c:142-150`, `183-191`); `done` is only read under `c->mu`, which the waker holds while writing it and claiming. `__gof_sleep_ns` now loops until its deadline (`timer_bridge.c:18-26`). **Residual:** `gof_chan_close` still calls `make_runnable` after releasing `c->mu` (chan.c:83-84). A parker that resumed through a stale WOKEN claim can see `closed` under the lock, return -1, finish, and be freed before close's late claim reaches its state, which is a UAF on `f`. The window is narrow, and io.c's `on_ready` has the same pre-existing pattern. Filed as **nostrc-bme1g** (P3). |
| M2 | `json_interface` multi-load TOCTOU | **RESOLVED** | All 8 call sites in `json.c` take one `atomic_load_explicit` snapshot and use only that. `grep` finds no remaining multi-load expression. |
| L2 | `_Atomic` in C++-includable headers | **RESOLVED** | `nostr-simple-pool.h` uses `NOSTR_POOL_ATOMIC(T)`, which is plain `T` under `__cplusplus`, and only includes `<stdatomic.h>` for C. `json.h` declares a plain pointer for C++. Size, alignment, and symbol size are unchanged. The manifest row is updated. Note that a C++ consumer accessing these fields directly is formally non-atomic. That's acceptable and documented. |
| — | `mpmc_wait_busy` clock domain | **TRACKED** | **nostrc-e9ou3** (P2) is open. |
| L1 / low | graveyard "guarantees" wording, `S.nworkers` clamp, json_glib install/uninstall ordering | **OPEN (non-blocking)** | Not changed in this commit. They remain cosmetic or degraded-mode issues. |

## Park-protocol windows, re-walked against HEAD

States: R=RUNNABLE, P=PARKING, B=BLOCKED, W=WOKEN, F=FINISHED. Every transition a waker makes is a single CAS in `fiber_wake_claim`. Only the owning parker or worker writes R/P/B from W/P.

| Waker arrives… | Waker's CAS | Owner's response | Lost wake? | Double enqueue? |
| --- | --- | --- | --- | --- |
| **before announce** | R→W (claim 1, no enqueue) | Announce CAS R→P fails, the parker stores R and returns without swapping. The caller rechecks its predicate (chan: `done`/`closed` under `c->mu`; select: `signaled`; io: retries IO; sleep: checks the deadline). | No: the CAS order is total, so either the announce sees W or the waker sees P. | No: nothing is enqueued, and the fiber never left the CPU. |
| **between announce and swap** | P→W (claim 1) | After `gof_ctx_swap` has fully saved the context, the worker reads W (749) or reads P and then fails its CAS (747), and goes to `requeue_woken`. | No | No: wakers never enqueue a W fiber, and only the worker leaves W. |
| **between swap and commit CAS** | P→W (claim 1) | Same as above. The fiber's context is already saved, because the worker code only runs after the swap returns on that thread. | No | No |
| **after commit** | B→R (claim 2), then cancel, then enqueue | None. Competing wakers now see R and only produce a stale W (claim 1, no enqueue). | No | No: exactly one claim from B can succeed. |

**Stale W (late wakers):** a late waker can turn a queued or running fiber's R into W. The effect is one spurious return from the fiber's next announce, never a missed wake: while the state is W, a real waker gets claim 0, but the parker's next announce fails and it rechecks the predicate, which the waker set before claiming. I checked that every park site rechecks its predicate:
- `chan.c` send/recv loops
- `CV_WAIT_FIBER` (`libgo/src/channel.c:1227…1839`, all inside predicate `for` loops)
- `go_select` / `go_select_timeout` outer `for(;;)`
- `blocking_executor` (explicit loop)
- `io.c` (caller retries)
- `__gof_sleep_ns` (deadline loop)

**`gof_sched_yield`** stores R unconditionally, which would absorb a pending W. That only matters if a fiber yields between publishing a wait and announcing a park. No library code does that (`gof_sched_yield` has a single caller, the public `gof_yield`, `api.c:49-50`). This is an implicit invariant; consider stating it in a `sched.h` comment.

**`fiber_entry_tramp`**: storing FINISHED overwrites any W, and later claims return 0. A claim racing with the free remains the residual tracked in nostrc-bme1g.

**Cancel invariant for H3:** a fiber with a live heap entry cannot be freed. Every way out of `park_until` removes the entry before the fiber can run again:
- the announce-abort path cancels (~935);
- `requeue_woken` cancels before pushing;
- a claim==2 in `make_runnable*` cancels before pushing;
- a claim==2 in `wake_ready` pops the entry under the lock.

So any `f` that `wake_ready` reads while holding `sleepheap.mu` points to a fiber that has not yet resumed past that park. ✔

## Test evidence

- `ctest -j8`: **381/381 passed**.
- **New finding (pre-existing, not a regression):** every `gof_test_*` runs with 1 worker. With `GOF_NWORKERS=4`, `gof_test_chan` **hangs at shutdown 5/5**. In the hung process, main is in `pthread_join` (sched.c:802) and workers 1–3 are in the untimed `pthread_cond_wait` (sched.c:688). Nothing broadcasts `S.cv` when `live_fibers` reaches 0. I reproduced the same hang on the **pre-audit 90253a1d runtime** (compiled in a separate worktree), 3/3, so the audit did not introduce it. Filed as **nostrc-e08s7** (P1). It also explains why the existing suite cannot exercise multi-worker parking.
- **Multi-worker stress harness**, written for this review and kept out of the tree:
  - **Workload:** 16 producer/consumer pairs at 20k messages each (mixed unbuffered and cap-4 channels, with yields), 8 fibers doing 200× `gof_sleep_ms(1)` (timer wakes across workers), and 64 close-vs-parked-receiver races.
  - **Exit rules:** it exits 0 as soon as every fiber finishes, which sidesteps nostrc-e08s7. A watchdog exits 2 if progress stalls for 5 s.
  - **Results:** current runtime 25/25 OK with 4 workers (work stealing on) and 25/25 OK with 8 workers. The pre-audit runtime also passed 25/25 with 4 workers, so the harness shows **no regression** but is not sensitive enough to reproduce the original races. The correctness argument for the windows above rests on the CAS reasoning, not on this test.
  - **Recommendation:** once nostrc-e08s7 is fixed, add a multi-worker variant (e.g. `GOF_NWORKERS=4`) of `gof_test_chan` and a sleep/close stress test to ctest.

## Follow-ups filed

- **nostrc-e08s7** (P1, bug): `gof_run` never returns with `GOF_NWORKERS>1`. Pre-existing.
- **nostrc-bme1g** (P3, bug): `gof_chan_close` wakes waiters after dropping `c->mu`, so a late claim can reach a finished fiber. This is the M1 residual.
- **nostrc-e9ou3** (P2, already filed by the author): `mpmc_wait_busy` clock domain.

---

# Follow-up review: `9c469355` (nostrc-e08s7, nostrc-e9ou3, nostrc-bme1g)

**Date:** 2026-10-06

## Verdict: **REQUEST CHANGES**. The nostrc-bme1g change introduces a deadlock that I reproduced on unmodified HEAD.

| Issue | Status |
| --- | --- |
| nostrc-e08s7: multi-worker shutdown hang | **RESOLVED** |
| nostrc-e9ou3: busy-wait clock domain | **NOT RESOLVED**: `channel.c` is fixed, but `select.c` has the same bug and the hook's documented contract contradicts the fix |
| nostrc-bme1g: late claim after finish | **NOT RESOLVED**: the `gof_chan_close` part is correct, but the `on_ready` part adds an `io_mu` ↔ `S.mu` lock-order inversion, which is a **NEW DEADLOCK** |

## 1. nostrc-e08s7: RESOLVED

`sched.c:761-769`: when `atomic_fetch_sub(&S.live_fibers,1) == 1`, the code locks `S.mu` and broadcasts `S.cv`. Waiters hold `S.mu` between evaluating the predicate (sched.c:664-686) and calling `pthread_cond_wait`. The broadcaster takes `S.mu` *after* its decrement, so no waiter can read the old `have_live` and then miss the broadcast. The idle-exit predicate is `!gof_io_have_waiters() && !have_live`. I checked whether any other input can make it true without a wakeup:

- **`io_have_waiters` going false:** registrations are removed only by the owning fiber (`io.c:191,197`) or popped by `on_ready`, which then wakes that fiber. In both cases a live fiber still exists, so `live_fibers` reaches 0 *after* the waiters are gone, and the broadcast covers it. ✔
- **Sleepers:** while the heap is non-empty (cancelled NULL entries count too), workers do a *timed* wait, then recheck. ✔
- **Inject queue and background stop:** they already signal (`sched.c:882-885`) or broadcast (`gof_sched_wake_all` via `gof_request_stop`). ✔

Not a defect, but worth noting: in background mode with `GOF_NWORKERS>1`, every worker now exits when `live_fibers` reaches 0, as a single worker always did. Before this fix, workers 2..N happened to stay parked and could serve fibers spawned later. The default is 1 worker (`sched.c:486`), so the default behavior is unchanged. Callers of `gof_start_background` (`apps/gnostr/src/main_app.c`, `gnome/nostr-homed/src/fs/nostrfs.c`) that set `GOF_NWORKERS>1` should keep a fiber alive, as they already must with one worker.

**Tests:** all 4 `*W4` tests pass. There is no `GoFiberIoTestW4`. I'd add one, but it would not have caught the deadlock below; a single-waiter IO test is what exposes it (see §3).

## 2. nostrc-e9ou3: NOT RESOLVED

`channel.c:606-622` now computes a CLOCK_MONOTONIC deadline for the fiber path and keeps CLOCK_REALTIME for `CV_WAIT_DEADLINE_OS`. That part is correct for the implementation (`gof_sched_park_until` → sleeper heap → `gof_now_ns()` monotonic). However:

- **`libgo/src/select.c:531-537`, `go_select_timeout` fiber path:** it builds `_abs_deadline_ns` from `CLOCK_REALTIME` and passes it to `gof_hook_block_current_until`. On the monotonic heap, a wall-clock-epoch value (~1.79e18 ns) is decades in the future, so **`go_select_timeout` never times out when called from a fiber**; it returns only on a channel event. Callers with short windows, such as `relay_optimized.c:427` (5 ms) or the `timeout_ms` select in `test_reconnect_same_second.c:260`, would block indefinitely if they ran on a fiber. This bug predates the commit, but it is exactly the class of bug nostrc-e9ou3 covers.
- **Contract mismatch:** `libgo/include/fiber_hooks.h:61-63` documents `deadline_ns` as "nanoseconds since epoch … CLOCK_REALTIME". After this commit, `channel.c` passes MONOTONIC to a hook documented as REALTIME, while `select.c` follows the documented contract and is still broken.

**Fix:** pick one domain. The least invasive option is to keep the documented REALTIME contract and convert inside `gof_hook_block_current_until` (`libgo/fiber/sched/fiber_hooks_impl.c:20-26`): compute `remaining = deadline - realtime_now` (clamped at 0) and pass `gof_now_ns() + remaining` to `park_until`. Then revert the `channel.c` fiber path to REALTIME so both callers are consistent. The other option is to change the header to MONOTONIC and fix `select.c:532`.

## 3. nostrc-bme1g: NOT RESOLVED, NEW DEADLOCK

**`gof_chan_close` (`chan.c:74-89`): correct.** Waking under `c->mu` matches what `handoff_to_waiter`/`handoff_from_waiter` already do (they call `gof_sched_make_runnable` under `c->mu`). No scheduler code acquires a fiber-channel `c->mu`, so `c->mu → {sleepheap.mu, rq_mu, S.mu}` has no cycle. ✔

**`on_ready` (`io.c:142-167`): introduces an ABBA deadlock.** The commit comment says "nothing takes io_mu while holding scheduler locks". That is false:

- **Worker idle loop:** `worker_main` takes `S.mu` at `sched.c:664` and, when there is no inject work, no queued runnables, and no sleeper entries, calls `gof_io_have_waiters()` at `sched.c:686`. That function takes `io_mu` (`io.c:131`). **Order: `S.mu → io_mu`.**
- **Poller thread:** `on_ready` now holds `io_mu` across the claim. On claim==2, the poller thread is not a worker (`cur_worker()==NULL`), so the external enqueue paths lock `S.mu`: `sched.c:873`, `882`, and `921` (the partition path in `make_runnable_from_poller`). **Order: `io_mu → S.mu`.**

**Reproduced on unmodified HEAD**, using a harness kept out of the tree: one fiber does `gof_read` on a pipe (2 ms park slices), and an OS thread writes one byte every 1.5–2.5 ms.

| Build | 1 worker | 4 workers |
| --- | --- | --- |
| HEAD 9c469355 (`_build` lib) | **1/10 deadlocked** | **3/10 deadlocked** |
| Parent 15a6152f | 0/10 | 0/10 |

A sampled deadlocked process shows exactly the cycle:
```
main/worker_main  sched.c:686 → gof_io_have_waiters io.c:131 → pthread_mutex_lock(io_mu)   [holds S.mu]
poller_main       sched.c:964 → gof_netpoll_wait → on_ready io.c:161
                  → gof_sched_make_runnable_from_poller → pthread_mutex_lock(S.mu)       [holds io_mu]
```

**How it happens:** the poller claims the BLOCKED reader. Its 2 ms slice entry is popped by the worker's `sleepers_wake_ready` (`sleepheap.len` → 0) before the poller reaches `S.mu`. The worker then finds nothing to run and enters the `!have_sleepers` arm while the poller still holds `io_mu`. Every other worker and every external enqueue then blocks on `S.mu`, so the whole scheduler stops.

**Fix (either):**
- **(a)** Have `gof_io_have_waiters()` read an `_Atomic` registration counter, maintained in `io_waiter_add` and `io_waiter_remove_fiber_locked`/`on_ready` under `io_mu`, without taking `io_mu`. That removes the `S.mu → io_mu` edge. This is the preferred option because it keeps the UAF fix.
- **(b)** In `worker_main`, call `gof_io_have_waiters()` before taking `S.mu`, or drop and retake `S.mu` around it and re-validate the predicate.

Please add the single-reader pipe stress test (or an equivalent `GoFiberIoTest` variant) as a regression test.

**Lock order, as it should hold after the fix:** `c->mu → sleepheap.mu`; `c->mu → rq_mu`; `c->mu → S.mu`; `io_mu → sleepheap.mu/rq_mu/S.mu`; `S.mu → sleepheap.mu`. With (a) there is no `S.mu → io_mu` edge. `sleepers_wake_ready` releases `sleepheap.mu` before `rq_push`, and `rq_steal_one` releases the victim's `rq_mu` before `rq_push_to`, so no other edges point back into `c->mu` or `io_mu`.

---

# Round 3: `4a15c1a0` (io waiter count lock-free, sleeper-add signal, select monotonic)

**Date:** 2026-10-06

## Verdict: **REQUEST CHANGES**

The three code changes are correct, and the ABBA deadlock is gone. However, your question 2 ("is there any remaining path where the idle predicate changes without a signal?") has a yes answer. That remaining path is a pre-existing multi-worker livelock, and it makes this commit's new gating test, `GoFiberIoDeadlockTestW4`, **intermittently red**. Filed as **nostrc-y1y8n** (P1).

| Item | Status |
| --- | --- |
| 1. ABBA deadlock (`S.mu` ↔ `io_mu`) | **RESOLVED** |
| 2. `sleepers_add` signal, and whether the idle wait/exit signals are now complete | **The fix is correct (RESOLVED). Completeness: NO**, because targeted enqueues wake the wrong worker (pre-existing, nostrc-y1y8n) |
| 3. nostrc-e9ou3 monotonic contract | **RESOLVED** (one doc nit) |

### 1. ABBA deadlock: RESOLVED
- `gof_io_have_waiters()` (`io.c:137-142`) now just reads `g_io_waiter_count` and takes no lock. That function was the only `io_*` call made from sched.c (`sched.c:692`), so the `S.mu → io_mu` edge is gone. `on_ready` keeps `io_mu → {sleepheap.mu, rq_mu, S.mu}`, which is now acyclic.
- **Counter bookkeeping is consistent:**
  - +1 per node in `waiter_push` (`io.c:92`). A READ|WRITE registration pushes two nodes and counts 2.
  - −1 in `waiter_pop` (`io.c:96-98`).
  - −1 per node actually found in `io_waiter_remove_fiber_locked` (`io.c:108`, `112`). After `on_ready` pops a node, the later `io_waiter_remove_by_fiber` doesn't find it and doesn't decrement twice. ✔
- **Ordering:** relaxed increments under `io_mu` with an acquire read are sufficient. The count only matters when `live_fibers==0`, and a fiber never finishes while it is still registered. ✔
- **Evidence:** my jittered single-reader harness against HEAD had 0/10 failures with 1 worker and 0/10 with 4 (it was 1/10 and 3/10 on 9c469355). No hung sample shows a mutex wait anymore.

### 2. `sleepers_add` signal: the fix is correct, but the signal set is incomplete
- **The `sleepers_add` signal itself** (`sched.c:407-412`) is correct. `sleepheap.mu` is released before `S.mu` is taken. Callers of `park_until` hold no locks: `io.c` releases `io_mu` in `io_waiter_add`, `select.c` releases `waiter->mutex` before the hook, `channel.c` releases `chan->mutex` (`NUNLOCK`) before the hook, and `timer_bridge` holds none.
- **Why the signal can't be missed:** a worker that read `have_sleepers==0` holds `S.mu` until it is inside `cond_wait`, so the signal cannot slip in between. The signal also covers a second case: a new sleeper that is *earlier* than the deadline a worker is already timed-waiting on.
- **Why `signal` instead of `broadcast` is enough here:** any woken worker recomputes the minimum deadline, so it doesn't matter which waiter wakes.
- *Low (performance):* every `park_until`, including each 2 ms `gof_read` slice, now takes the global `S.mu`.

**Remaining path: targeted enqueues wake an arbitrary waiter.** This is **pre-existing** and filed as **nostrc-y1y8n**.
- Three paths push a fiber onto a *specific* worker's queue and then call `pthread_cond_signal`, which wakes one arbitrary idle worker:
  - `gof_sched_enqueue` (`sched.c:595-597`)
  - the `gof_sched_make_runnable` affinity path (`sched.c:878-880`)
  - the `gof_sched_make_runnable_from_poller` partition path (`sched.c:926-928`)
- These are on by default: `GOF_AFFINITY` and `GOF_POLL_PARTITION` default to 1, while `GOF_WORKSTEAL` defaults to 0.
- When the signal reaches a non-target worker B, B's idle predicate breaks on *any* non-empty queue (`sched.c:683-685`), but B cannot pop the target's queue. Stealing is off, and even when it is on, `rq_steal_one` needs at least 2 queued nodes. So B spins at ~100% CPU and never waits again, the target worker stays in `pthread_cond_wait` (`sched.c:694`), and the fiber never runs. `maybe_rebalance` (`sched.c:172`, opt-in) also migrates fibers with no signal at all.

**Evidence:**
- **Repro harness** (`GOF_NWORKERS=4`; one fiber `gof_chan_recv` on an unbuffered `gof_chan`; an OS thread `gof_chan_try_send`):

  | Runtime | Result |
  | --- | --- |
  | HEAD | **10/10 stall** |
  | 15a6152f | 10/10 stall |
  | pre-audit 90253a1d | 10/10 stall |
  | HEAD, 1 worker | 0/10 |

  A sample shows one worker spinning in `worker_main` (`sched.c:649-685`) at 101% CPU, three in `pthread_cond_wait` at `sched.c:694`, and progress stuck at 0.
- **The new regression test hits it through the poller partition path.** `ctest -R GoFiber` failed with `GoFiberIoDeadlockTestW4 ... ***Timeout 15.02 sec`, and a 20-run loop of `gof_test_io_deadlock` at `GOF_NWORKERS=4` failed 2/20. A sampled hang has the same signature: one worker spinning at 99.7% CPU, the rest in `cond_wait`, the poller idle in `kevent`, and **no mutex waits**. It is not the ABBA deadlock.

**Fix (in nostrc-y1y8n):**
- Wake the *target*: `pthread_cond_broadcast` at `sched.c:597/880/928` and after a successful `rq_steal_one` in `maybe_rebalance`, or use per-worker condvars.
- Have the idle predicate count only the worker's own queue, the inject queue, and stealable queues (when stealing is enabled), so non-targets wait instead of spinning. That also fixes a related pre-existing problem: idle workers spin whenever a busy worker has a backlog and stealing is off.

All the other inputs to the idle predicate now have a wakeup: inject (`sched.c:607/891`), stop (`gof_sched_wake_all`, `sched.c:988`), last-fiber exit (`sched.c:773`), and new sleepers (`sched.c:411`). The IO-waiter count needs no wake (see §1).

### 3. nostrc-e9ou3: RESOLVED
These are all the deadline callers in the tree (`grep` of `gof_hook_block_current_until`, `gof_park_current_until`, and `gof_sched_park_until`, excluding tests):

| Caller | Clock |
| --- | --- |
| `select.c:529-539` (now CLOCK_MONOTONIC) | monotonic ✔ |
| `channel.c:606-614` | monotonic ✔ |
| `io.c:190,198` (`now_ns()`, CLOCK_MONOTONIC, `io.c:34-38`) | monotonic ✔ |
| `timer_bridge.c:21` | monotonic ✔ |
| `park.c:6` (`gof_park_current_until`) | no in-tree callers |
| stub `fiber_hooks_stub.c:34` | no-op |

The OS-thread paths in `select.c` and `channel.c` correctly stay on CLOCK_REALTIME for nsync.

**There is no remaining REALTIME caller.**
- *Nit:* `libgo/include/fiber_hooks.h:62` still says "deadline_ns (nanoseconds since epoch) expires", which contradicts the new `@param` at 64-65.
- *Low, pre-existing:* `go_select_timeout` measures its timeout budget with `gettimeofday` (`select.c:427-431`, `455-456`), so a wall-clock step changes the effective timeout. If the deadline passes between the two `now_us()` reads, `deadline_us - now_us()` (`select.c:501`) underflows, but the unsigned arithmetic wraps to a deadline a few µs in the past, so the call times out immediately. Moving `now_us` to CLOCK_MONOTONIC would close both issues.

### Note on the new test
`test_io_deadlock.c` is a sound regression test for the ABBA deadlock, and it relies on the CTest timeout to catch a hang. Until nostrc-y1y8n is fixed, the W4 variant will intermittently time out for the unrelated targeted-wake reason. Fix y1y8n in this series, which I recommend because the fix is small, or temporarily mark `GoFiberIoDeadlockTestW4` with `GOF_AFFINITY=0 GOF_POLL_PARTITION=0` and a comment pointing to nostrc-y1y8n.
