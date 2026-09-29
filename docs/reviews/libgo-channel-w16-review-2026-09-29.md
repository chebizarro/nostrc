# Peer Review — nostrc-75rv (W16): libgo MPMC slot protocol and libnostr writable re-arm

**Commit:** `37b0203a`, *fix(libgo,libnostr): lost channel elements and stranded writes under load (nostrc-75rv)*
**Parent:** `90c1f9ed`
**Branch reviewed:** `libgo/w16-channel-review`
**Reviewer:** independent peer reviewer (AGENTS.md, "Peer Review (REQUIRED)")
**Date:** 2026-09-29
**Verdict:** **REQUEST CHANGES**. There are two blocking findings (F1, F2). The libnostr change is correct as written.

This review changed no source files and no beads. Reviewer probes live under `/tmp/w16probe/` and are not committed.

---

## Scope

| File | Change |
|---|---|
| `libgo/src/channel.c` | new `mpmc_push()` / `mpmc_pop()` slot-protocol helpers used by all six MPMC send/receive variants. BUSY handling: try paths report full/empty, blocking paths yield and retry. Masked `inc_in/inc_out` are now compiled only for the single-lock ring. `out`-first loads in occupancy/depth |
| `libgo/tests/go_channel_mpmc_slot_test.c` | new white-box and API-level regression test (6 case invocations) |
| `libnostr/src/connection.c` | `CLIENT_WRITEABLE` drained branch re-checks `send_channel` depth under `priv->mutex` and re-arms |
| `tests/test_connection_writable_rearm.c` | new lws round-trip regression test |
| `libgo/CMakeLists.txt`, `libnostr/CMakeLists.txt`, `VERSION_MANIFEST.md`, `tests/CMakeLists.txt` | libgo 0.1.2, libnostr 1.0.9, test registration |

## Verification performed

| # | What | Result |
|---|---|---|
| 1 | `cmake -S . -B /tmp/w16cr -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w16cr && ctest --test-dir /tmp/w16cr -j6 --timeout 300` | Build clean. **414/414 passed**, with 5 skipped (`test_nip5f_tcp`, `groundhog-launch`, `groundhog-store-key-keyring`, `groundhog-background-gui`, `groundhog-notifier-gui`) |
| 2 | Parent check. I exported `37b0203a^` with the two new tests and their CMake registration grafted in, and ran each slot-test case in its own process | Parent: case 1 **FAIL**, 2 pass, 3a **FAIL**, 3b **FAIL**, 4 **FAIL**, 5 **FAIL** (`received 14/200000`). Fix: 6/6 pass, and 15/15 repeated full runs pass |
| 3 | `test_connection_writable_rearm` on the parent, on fixed libgo with the **parent** `connection.c`, and on the fix | Parent **FAIL** (`server received 1 frames: hello|`). Mixed build **FAIL 3/3** with the same message, so the test isolates the `connection.c` fix. Fix passes 10/10 |
| 4 | ASAN: `-DGNOSTR_ENABLE_ASAN=ON` (MPMC on), `libgo/` and `tests/` | libgo 25/25. tests/ 36/38, and both failures are harness issues. `concurrency_subscription_shutdown_asan` forces `detect_leaks=1` ("not supported on this platform" on macOS arm64); it passes 3/3 with `detect_leaks=0`. `gnostr_asan_test` needs the gnostr binary, which the partial build did not produce |
| 5 | TSAN with **MPMC on**: `-fsanitize=thread` via `CMAKE_C_FLAGS`, `GO_ENABLE_TSAN` unset. `go_tsan.h` turns its annotations on via `__has_feature` | libgo 24/25. The one failure, `GoFiberIoTimeoutTest`, hits its 2 s limit, and the 10 s TSAN limit only applies under `GO_ENABLE_TSAN`. Direct runs of `go_channel_mpmc_slot_test`, `go_channel_stress_test`, `go_channel_test`, `go_channel_close_test`, `go_select_closed_test` and `go_select_cancel_test` produced **0 ThreadSanitizer reports** |
| 6 | TSAN, official config: `-DGNOSTR_ENABLE_TSAN=ON`, which forces MPMC **off** and `ATOMIC_TRY=0` | libgo 20/25. `GoTickerTest`, `GoSelectTest`, `GoChannelCloseTest` and `GoSelectMultiTest` time out; this is pre-existing and reproduces on the parent's TSAN build (F8). **`GoChannelMpmcSlotTest` times out, which is new (F2)** |
| 7 | `-DGO_CHANNEL_REFINED_SIGNALING=OFF` | libgo 25/25, tests/ 36/36 |
| 8 | `test_nostr_gobject_subscription_eose_order` × 40 under a continuous `ctest -R groundhog- -j8` loop | **40/40** |
| 9 | `chan_bench`, parent vs fix (see Performance) | no hot-path regression |
| 10 | Reviewer probes: capacity-1 white-box, capacity-1 threaded, capacity-1 blocking consumer, capacity-0 under ASAN | F1, F4 |

---

## Findings summary

| ID | Severity | Blocking | Location | Summary |
|---|---|---|---|---|
| **F1** | **High** | **Yes** | `libgo/src/channel.c:517-576`, `:940`, `:961` | The slot protocol is unsound for **capacity-1** channels. The race loses an element and wedges the channel for good. Blocking receive then livelocks at 100% CPU and never observes close. Newly reachable from the blocking receive variants |
| **F2** | Medium | **Yes (CI)** | `libgo/tests/go_channel_mpmc_slot_test.c:167-178`, `libgo/CMakeLists.txt:381` | The new test hangs forever when MPMC slots are off, which covers every TSAN build including the libgo-ci `tsan` matrix |
| F3 | Medium | No | `channel.c:1236-1246`, `:1449-1460`, `:1636-1646`, `:1845-1856` | Blocking BUSY handling is an unbounded `sched_yield` spin. It never parks, and from a fiber it stalls the whole worker |
| F4 | Low | No | `channel.c:546-552`, `:940`, `:961` | Capacity-0 channel: `mpmc_pop` reads `slot_seq` out of bounds (ASAN heap-buffer-overflow) |
| F5 | Low | No | `channel.c:1224`, `:1435`, `:1836`, `:534` | Stale or unsequenced `in - out` snapshots still gate REFINED_SIGNALING=OFF broadcasts. Fold into nostrc-v624 |
| F6 | Low | No | `libgo/src/select.c:245-256` | select reports "closed" while a claimed element is still being published |
| F7 | Info | No | `libgo/CMakeLists.txt:131`, `channel.c:601`, `:766` | `GO_CHANNEL_ATOMIC_TRY` is silently ignored when MPMC slots are on |
| F8 | Info | No | `libgo/CMakeLists.txt:134-140`, `.github/workflows/libgo-ci.yml:79` | The MPMC path has no TSAN coverage. Separately, the MPMC-off ring can never hold a capacity-1 element (pre-existing) |
| F9 | Info | No | commit message | "4 of 6 cases fail on old code": I measured 5 of 6, and case 2 does not catch the regression |

---

## F1 — High, BLOCKING — capacity-1 channels: a slot-sequence ABA loses an element and wedges the channel for good

**Where.** `mpmc_push` at `libgo/src/channel.c:517-544` and `mpmc_pop` at `:546-576` (CAS `out` at `:555`, read at `:558`, release at `:561`). `go_channel_create` rounds only `cap > 1` (`:940`), so capacity 1 stays a one-slot ring with `mask = 0` (`:961`).

**Root cause.** Vyukov's bounded MPMC queue needs at least **two** slots. In this encoding, slot `t & mask` carries `seq == t` (free for ticket t), `t + 1` (t published) and `t + capacity` (t released, which means free for ticket `t + capacity`). With capacity 1, "t published" and "free for t+1" are the **same value**, `t + 1`. Only the `head - tail >= capacity` check at `:521` tells them apart. That check stops working once a receiver has advanced `out` (`:555`) but has not yet read and released the slot (`:558-562`). A sender then sees `in - out == 0` and `seq == head`, and claims the slot the receiver is still reading.

**Interleaving** (capacity 1; ticket t published, so `in = t+1`, `out = t`, `seq[0] = t+1`, `buffer[0] = A`):

| step | thread | action | state after |
|---|---|---|---|
| 1 | R (any receive variant) | `mpmc_pop`: tail = t, seq = t+1, so it CASes `out` t→t+1. Preempted before `:558` | `out = t+1` |
| 2 | S (any send variant) | `mpmc_push`: head = t+1, tail = t+1, and 0 < 1. `seq[0] = t+1 == head`, so it CASes `in` to t+2, stores `buffer[0] = B` and sets `seq[0] = t+2` | `in = t+2` |
| 3 | R | reads `buffer[0]` and gets **B**, so **A is lost**. Stores `buffer[0] = NULL`, then `seq[0] = t + capacity = t+1`, which **regresses** it from t+2 | `seq[0] = t+1` |
| 4 | anyone | pop needs `seq == t+2`, sees t+1, so diff < 0 and `in > out` give **BUSY forever**. push sees `in - out = 1 >= 1` and returns **NONE forever** | wedged |

If S's store lands between R's read and R's NULL store, B is NULLed instead. Either way one element is gone and `seq` has gone backwards.

**Consequences once wedged:**
- `try_send` and `try_receive` fail forever.
- `go_select` never sees the channel ready.
- `go_channel_receive` loops through `:1449-1460` forever at 100% CPU: occupancy is 1, so it never waits, and `mpmc_pop` keeps returning BUSY, so it keeps calling `sched_yield`. Because occupancy never drops to 0 it never takes the `closed && occupancy == 0` exit (`:1405`), so a thread joined on shutdown hangs.
- `go_channel_receive_with_context` escapes only through cancellation.
- Blocking senders park on `cond_full` forever.

**Evidence** (reviewer probes against the fix build):
- **White-box probe.** This builds the same "receiver preempted after its claim" state as `go_channel_mpmc_slot_test` case 2, but with `go_channel_create(1)`. `try_send` returned **0** and overwrote the in-flight element (buffer held 99 instead of 10), and the receiver took 99. Afterwards: `in=5 out=4 seq=4 depth=1`, `try_receive` returned −1 and `try_send` returned −1, permanently.
- **2 `try_send` producers × 2 `try_receive` consumers, 3 s.** The channel wedged in **3/3** runs, after only 10, 761 and 1306 transfers (`depth_after_drain=1`, both probes −1).
- **1 `try_send` producer × 1 blocking `go_channel_receive` consumer** (the ticker / wake-channel shape), 3 s, then `go_channel_close`. The channel wedged in **3/3** runs, after 10 251, 158 493 and 31 357 transfers. After close the consumer **never returned** and burned **1.94–1.98 s of CPU per 2 s** of wall time.
- **The same blocking workload on the parent** accounts for every element and the consumer returns on close. It does see 283–562 NULL receives, which is the bug this commit fixes.

**Regression assessment.** The try/try form of this ABA predates the commit: on the parent the same white-box state clobbers too. The parent's broken mutex fallback then "healed" the tickets by handing out NULLs, about 5.1–5.5 M NULL receives in 3 s in my threaded probe. This commit changes the behaviour in two ways that matter:
1. **The symptom becomes permanent.** For the libnostr wake channels a NULL token was harmless. A wedged wake channel is dead for the channel's whole lifetime.
2. **The blocking receive variants are newly exposed.** The parent's `go_channel_receive` and `_with_context` released the slot *before* advancing `out`, so a concurrent `try_send` saw `in - out == 1` until the slot was already free. They now use `mpmc_pop`, which advances `out` first. The blocking probe above is a straight regression: the parent completes, the fix hangs.

**Reach.** There are 40 `go_channel_create(1)` call sites. Only channels that receive **more than one send** are exposed:
- `libgo/src/ticker.c:30`: `try_send` on every tick. This is public API, and `libgo/examples/go_ticker_demo.c` consumes it with `go_channel_receive`.
- `libnostr/src/simplepool.c:368`, `wake_ch`: `try_send` from `:861`, `:1347` and `:1440`, consumed by a select plus a drain loop.
- `libnostr/src/simplepool.c:401`, `redial_wake`.
- `libnostr/src/relay.c:518`, `reconnect_now`: once wedged, `nostr_relay_reconnect_now()` silently stops bypassing backoff for that relay.
- `nips/nip46/src/core/nip46_session.c:3749`: two `try_send`s at `:3602` and `:3604`.
- `apps/gnostr/src/sync/neg-client.c:665`, `ready_ch`: the relay state callback `try_send`s on every CONNECTED or DISCONNECTED transition (`:542`). A wedge turns into a spurious handshake timeout.
- `NOSTR_SUB_EOSE_CAP=1` and `NOSTR_SUB_CLOSED_CAP=1` env overrides (`libnostr/src/subscription.c:169-178`).

One-shot channels are **not** exposed, because they get a single send: context `done`, reply and result channels. The libnostr select consumers all have timeouts (200 ms, `wait_ms`, backoff), so they degrade silently to polling rather than hanging. That is the same kind of hidden latency loss this bead set out to remove.

**Required change (small):**
- Give the MPMC ring at least two slots but keep the logical capacity: `ring = max(2, pow2(capacity))` and `mask = ring - 1`.
- Keep `capacity` for the `head - tail >= capacity` check in `mpmc_push` and in `go_channel_is_full`.
- Initialise `ring` entries of `slot_seq`.
- Release with `seq = tail + ring` in `mpmc_pop`, not `tail + capacity`.

With two slots, ticket t+1 maps to the other slot, so the claim in step 2 can no longer touch the slot R is reading. Rounding capacity 1 up to 2 would also fix it, but it changes coalescing semantics: a ticker or wake channel could then hold two tokens.

Add capacity-1 regression cases:
- the existing case 2 with `go_channel_create(1)`;
- a blocking-receiver variant: claim via `out`, then `try_send`, then finish the receive, then check `go_channel_receive` returns the second element and the ring is empty.

Both fail deterministically on `37b0203a`.

---

## F2 — Medium, BLOCKING (CI) — `go_channel_mpmc_slot_test` hangs forever when MPMC slots are off

**Where.** `libgo/tests/go_channel_mpmc_slot_test.c:167-178` (`test_mixed_variants_fifo`) and the header claim at `:18-19`. The test is registered at `libgo/CMakeLists.txt:381` with no `TIMEOUT` property.

**Why it hangs:**
- Every `GO_ENABLE_TSAN` build forces `GO_CHANNEL_MPMC_SLOTS=OFF` (`libgo/CMakeLists.txt:134-140`). The root `CMakeLists.txt` sets `GO_ENABLE_TSAN` for `GNOSTR_ENABLE_TSAN` and for `SANITIZE=thread`.
- The single-lock ring keeps one slot empty (`go_channel_is_full`: `next_in == out`, `channel.c:405-407`), so a capacity-4 channel holds **three** elements.
- Case 4 does four `go_channel_send_with_context(c, …, NULL)` calls into `go_channel_create(4)` (`:171-172`). The fourth blocks forever: the context is NULL and nobody is receiving.
- The header says "The API-level cases run in both modes", so the case is not skipped.

**Evidence.** In the `-DGNOSTR_ENABLE_TSAN=ON` build, ctest reports `GoChannelMpmcSlotTest ***Timeout 300.09 sec`. Run case by case: cases 1–3b print "skipped (MPMC slots disabled)", case 4 exits with **rc=124** (still hung at 60 s), and case 5 prints "received 200000/200000 … ok".

**Impact.** `.github/workflows/libgo-ci.yml` runs a `tsan` cell on `ubuntu-latest` and `macos-latest` for every push and PR to `main`/`master`, configured with `-DGO_ENABLE_TSAN=ON`. Its test step runs `ctest … -E "GoChannelTest|GoTickerTest|GoChannelStressTest|GoSelectTest|GoChannelCloseTest|GoSelectMultiTest"` (`:79`). `GoChannelMpmcSlotTest` is not excluded and has no timeout, so both TSAN cells will hang until ctest's 1500 s default and then fail.

**Fix.**
- Make case 4 mode-aware: skip it when `!mpmc_enabled(c)`, or fill only to `capacity - 1` in single-lock mode and derive the expected depth from that.
- Add `set_tests_properties(GoChannelMpmcSlotTest PROPERTIES TIMEOUT 60)`. The stress case already limits itself to 20 s.

---

## F3 — Medium — the blocking BUSY path is an unbounded yield spin

**Where.** `channel.c:1236-1246` (send), `:1449-1460` (receive), `:1636-1646` (send_with_context), `:1845-1856` (receive_with_context).

**What happens on `MPMC_BUSY`.** The blocking variants unlock, call `sched_yield()`, relock and retry, with no bound and no park.

- **Receivers never reach the condvar.** Occupancy counts a claimed-but-unpublished element, so a blocking receiver does not wait while a claim is pending (`:1338-1344`). Every blocked receiver spins on `NLOCK`/`NUNLOCK` plus `sched_yield` for as long as the claiming thread is descheduled.
- **The spin competes with the thread it waits on.** BUSY windows are only long when the machine is saturated. With N blocked receivers that becomes a thundering herd on `chan->mutex`, which the publisher also needs for its post-publish signal.
- **From a fiber**, `sched_yield()` stalls the whole worker thread instead of parking the fiber.
- **Close is not an exit** for `go_channel_receive`, because occupancy stays above 0.

With capacity ≥ 2 the spin is bounded by the peer's scheduling, so this is not a correctness bug. Combined with F1 it is the permanent 100% CPU livelock.

**Suggestion.** Spin a bounded number of `NOSTR_CPU_RELAX` iterations, as the try paths do with `MPMC_TRY_SPINS`. Then wait with a deadline: `CV_WAIT_DEADLINE_OS` for OS threads and a fiber park, roughly 1 ms.

The claimant always signals `cond_empty`/`cond_full` and the select waiters under `chan->mutex` after it publishes or releases:
- try_send at `:632-641`;
- try_receive at `:796-805`;
- the blocking variants, while they still hold the mutex.

So with REFINED_SIGNALING=ON a plain wait looks lost-wakeup-free. The deadline is cheap insurance for REFINED_SIGNALING=OFF (nostrc-v624).

---

## F4 — Low — capacity-0 channel: `mpmc_pop` reads `slot_seq` out of bounds

`go_channel_create(0)` leaves capacity 0 and `mask = SIZE_MAX` (`:940` skips the rounding, `:961`), and allocates 0-byte `buffer` and `slot_seq` arrays. `mpmc_pop` loads `slot_seq[tail & mask]` (`:550-551`) before any emptiness check, so `go_channel_try_receive` on such a channel reads out of bounds. ASAN reports `heap-buffer-overflow … READ of size 8 … #0 mpmc_pop … #1 go_channel_try_receive`. The parent's try path returned on `head == tail` before touching `slot_seq`.

No in-tree caller passes 0: the subscription env overrides require values > 0. It is still public API. Fix it together with F1 by clamping the ring to at least 2, and decide what capacity 0 means (reject it, or treat it as 1).

---

## F5 — Low — stale snapshots still gate REFINED_SIGNALING=OFF broadcasts (extend nostrc-v624)

The commit fixed the load order in `go_channel_occupancy` and `go_channel_get_depth`. The blocking paths still compute `was_empty` and `was_full` as `(chan->in - chan->out)` (`:1224`, `:1435`, `:1836`). That expression is two seq_cst loads in unspecified order, so a concurrent lock-free receive can make the difference wrap, and `was_empty`/`was_full` come out false. They are also computed before the transfer. `mpmc_push`'s `occ_before` (`:534`) uses a `tail` loaded before the CAS, which is the same class of problem.

With REFINED_SIGNALING=ON these values only feed metrics and ARM `SEV`. With OFF they decide whether to broadcast, which is exactly the stale-snapshot lost wakeup nostrc-v624 tracks. Recommend that v624 explicitly covers these three expressions, using `go_channel_occupancy()` taken after the transfer. The new code adds no new REFINED_SIGNALING=OFF lost wakeup: the BUSY branches never wait on a condvar. The OFF build passes libgo 25/25 and tests/ 36/36.

---

## F6 — Low — select can report "closed" while a claimed element is still being published

In `try_cases_once` (`libgo/src/select.c:245-256`), `go_channel_try_receive` now returns −1 for a BUSY head. The following `go_channel_is_closed()` check then makes the case ready with `ok = 0` and `*recv_buf = NULL`.

If a sender claimed a ticket before `go_channel_close()` and publishes after it, a select-driven consumer that exits on "closed" leaves that element in the ring. For owned pointers that is a leak. `go_channel_receive` handles the same state correctly, because it keeps waiting while occupancy is above 0.

This is not a regression (the parent handed out the NULL instead), and it needs a send racing close. Suggest checking `closed && go_channel_get_depth(c) == 0` in `try_cases_once`, to mirror `go_channel_receive`.

---

## F7 — Info — `GO_CHANNEL_ATOMIC_TRY` is silently ignored when MPMC slots are on

The try paths used to be gated on `NOSTR_CHANNEL_MPMC_SLOTS && NOSTR_CHANNEL_ATOMIC_TRY`; they are now gated on `NOSTR_CHANNEL_MPMC_SLOTS` alone (`:601`, `:766`). The mutex try path that `ATOMIC_TRY=OFF` used to select in MPMC mode was the broken one, so the change is right. The option's description (`libgo/CMakeLists.txt:131`) should say it applies only with `MPMC_SLOTS=OFF`. No in-tree configuration combines `MPMC_SLOTS=ON` with `ATOMIC_TRY=OFF`, because TSAN turns both off.

## F8 — Info — no TSAN coverage of the MPMC path; the MPMC-off ring cannot hold a capacity-1 element (pre-existing)

- **No TSAN coverage.** `GO_ENABLE_TSAN` forces `MPMC_SLOTS=OFF`, so neither `GNOSTR_ENABLE_TSAN` nor libgo-ci's `tsan` job exercises the code under review. My custom TSAN build with MPMC on produced 0 reports, so the gap is coverage only, but it should be closed.
- **Capacity 1 in the single-lock ring.** There, `mask = 0` for capacity 1, so `go_channel_is_full` (`next_in == out`) is **always** true and a capacity-1 channel can never accept an element. That is why `GoSelectTest`, `GoChannelCloseTest`, `GoSelectMultiTest` and `GoTickerTest` time out deterministically under TSAN; I confirmed the same hangs on the parent's `-DGNOSTR_ENABLE_TSAN=ON` build. It is also why `libgo-ci.yml:79` excludes them as "flaky".
- **Consequence.** Every context `done` channel has capacity 1, so the TSAN configuration does not build the library faithfully.

Recommended follow-up bead: a TSAN job with `MPMC_SLOTS=ON`, and a fix for (or removal of) the one-empty-slot ring. I did not file it, per the review brief.

## F9 — Info — test-failure claims

The commit says "4 of its 6 cases fail on the old code". Run case by case on `37b0203a^`:
- 1 fails;
- 2 passes;
- 3a and 3b fail;
- 4 fails;
- 5 fails (`received 14/200000`).

That is **5 of 6**. Case 2 (try_send against a slot still being released) passes on the parent, because the old lock-free `try_send` already honoured `slot_seq`. It guards the behaviour but does not catch the regression. The `test_connection_writable_rearm` claim holds, and that test does not depend on the libgo fix (step 3 above).

---

## libgo: the algorithm, point by point

**Memory ordering (capacity ≥ 2): correct.**
- **Publish.** The sender stores `buffer` and then `slot_seq = t+1`, both with release. The receiver acquire-loads `slot_seq == t+1` before its CAS and acquire-loads `buffer` after it. The buffer store is sequenced before the release store of `slot_seq`, so the receiver sees the value.
- **Release for the next lap.** The receiver's relaxed `buffer = NULL` store is sequenced before its release store of `slot_seq = t + capacity`. The next-lap sender acquire-loads that value before its data store. So the NULL store happens-before the new value and cannot clobber it.
- **Tickets.** The `in`/`out` CASes use acq_rel and the loads use acquire. Nothing needs seq_cst.

**ABA and wraparound.**
- Tickets are monotonic `size_t`, so there is no practical wrap on 64-bit.
- On 32-bit, wrap after 2^32 operations is harmless. Capacity is a power of two dividing 2^32, and every comparison is a signed difference whose magnitude is at most capacity, which is far below 2^31.
- The only ABA is the capacity-1 value collision (F1).
- Compiling the masked `go_channel_inc_in/out` only for the single-lock ring (`:367-381`) correctly removes the "full forever" ticket corruption.

**Progress.**
- `mpmc_push`/`mpmc_pop` are lock-free among themselves: a CAS retries only when another thread made progress.
- Blocking progress depends on the claimant finishing. That is inherent to Vyukov's design and bounded by OS scheduling.
- Fibers are cooperative (`libgo/fiber/README.md:359-361`), and there is no yield point between claim and publish/release. So a fiber cannot be parked mid-transfer, and no same-worker deadlock exists.
- A thread that never publishes, reachable only via `fork()` mid-push, leaves the slot BUSY forever. Try and select calls then report empty, and blocking receivers spin (F3).

**`close()` and cancellation against in-progress slots.**
- The blocking send re-checks `closed` on every BUSY retry and returns −1.
- The blocking receive keeps going while occupancy is above 0, so it delivers the in-flight element (except in the F1 wedge).
- The `*_with_context` variants re-evaluate cancellation on every BUSY retry: the `goto` re-enters the loop condition and the `canceled` check. Cancel wins and nothing is consumed or enqueued. Ownership stays with the caller, and `connection.c:1342-1348` frees the message on failure.

**Close semantics.** Elements published before close, and elements claimed before close, are delivered to `go_channel_receive`/`_with_context`. They only exit on `closed && occupancy == 0`, and occupancy counts claimed tickets. `try_receive` ignores `closed`. A `try_send` can land after close, since it checks `closed` at `:603` before claiming; the parent behaved the same. The select nuance is F6.

**Wakeups, REFINED_SIGNALING ON (the default).**
- Every lock-free transfer signals under `chan->mutex` after publish or release, and blocking waiters check under the same mutex, so no wakeups are lost.
- `go_select` registers, then re-tries. The claimant's post-publish `go_channel_signal_select_waiters` covers the BUSY → published transition, so a monitor that saw BUSY is woken.

**Wakeups, REFINED_SIGNALING OFF.** Nothing new: the BUSY branches never wait. The pre-existing stale-snapshot issues are F5 (nostrc-v624).

**Fairness and starvation** are unchanged.
- Blocking waiters can repeatedly lose to lock-free try callers; they simply re-wait on NONE.
- The fiber waiter list is LIFO: `fiber_waiter_enqueue` pushes at the head (`:65`) and `wake_one` pops the head. This is pre-existing.
- BUSY spinners are not queued, so whichever thread retries first after the publish wins.

### Performance

`chan_bench`: 1 M messages, three runs each, messages per second.

| capacity / producers / consumers | parent `90c1f9ed` | fix `37b0203a` |
|---|---|---|
| 1024 / 1 / 1 | 5.42 M, 3.04 M, 2.40 M | 2.50 M, 2.37 M, 2.46 M |
| 1024 / 4 / 4 | 2.52 M, 1.61 M, 2.38 M | 2.05 M, 2.15 M, 1.92 M |
| 8 / 4 / 4 | 1.70 M, **29**, **17** | 1.04 M, 1.05 M, 1.00 M |
| 64 / 8 / 8 | 1.83 M, 1.81 M, 1.79 M | 1.74 M, 1.73 M, 1.64 M |

- There is no meaningful hot-path regression. The default try paths are structurally the parent's `MPMC && ATOMIC_TRY` code. The blocking paths swap plain stores for one CAS under a mutex they already hold.
- The parent's 8/4/4 runs collapsed to 17–29 msgs/s: that is the "full forever" ticket corruption. Its one completed run was not a like-for-like baseline, because the parent was also returning NULLs.
- The fix's numbers are markedly more stable.
- The only new cost is the F3 spin, and it is confined to BUSY windows.

### Callers of the channel API

`go_channel_` references: libnostr 204, nostr-gobject 28, apps/gnostr 7, gnome/groundhog 2, signet 1.

- **nostr-gobject.** Files: `nostr_subscription.c:280-321`, `nostr_pool.c:809-827`, `nostr_simple_pool.c`, `nostr_query_batcher.c:278`.
  - The drain loops (`while (try_receive == 0)`) treat failure as "nothing right now", and that still holds. A BUSY head now reads as "nothing yet", and the claimant's post-publish select signal wakes the monitor.
  - The EOSE-ordering argument (`nostr_subscription.c:290-299`) still holds. libnostr's `dispatch_event` pushes events under `sub->priv->sub_mutex` (`libnostr/src/subscription.c:406-440`), so events are fully published before the EOSE is pushed. An EOSE observed with acquire therefore implies those event slots read as published, not BUSY. The test passed 40/40 under load.
- **libnostr `connection.c`.**
  - `send_with_context` now publishes `slot_seq`. The old path did not, which is the root of "a ring filled to capacity read as empty".
  - The reader treats a NULL from `go_select` as "Receive channel closed" (`:1448-1452`). That is now sound, because `try_receive` no longer fabricates NULL. A NULL now arrives only through select's closed path, apart from the F6 edge.
- **`libnostr/src/subscription.c:440`.** `try_send` failure means "drop the event". With BUSY it can fail only when the ring is effectively full, meaning the slot for `in` is still held by the previous lap's receiver. The parent behaved the same after its 64 spins.
- **`subscription.c:474`, `:1070`, `:1087`.** `get_depth` feeds metrics and adaptive sizing. It no longer underflows to about `SIZE_MAX`, which caused the "depth 4096 in a 2048-slot ring" symptom.
- **Capacity-1 channels with more than one send.** These still get the wrong semantics: see F1 (simplepool, relay `reconnect_now`, nip46, gnostr `neg-client`, ticker).
- **Groundhog and signet** only reach channels through libnostr and nostr-gobject.

---

## libnostr `connection.c`: correct

**The change.** `CLIENT_WRITEABLE` → empty `try_receive` → lock `priv->mutex` → clear `writable_pending` → if `go_channel_get_depth(send_chan) > 0`, set it again and re-arm → unlock → `lws_callback_on_writable(wsi)` (`:380-409`).

**Why it closes the race.** The writer (`nostr_connection_write_message`, `:1342-1370`) claims its ticket inside `go_channel_send_with_context`, which advances `in` and so counts in depth. Only after that does it take `priv->mutex` to set the flag.

- **The writer's critical section comes first.** Its claim happens-before the service thread's depth read via the mutex, so the check sees the frame and re-arms.
- **The service thread's critical section comes first.** The writer's set follows the clear, so the flag stays up. The all-protocol sweep (`:937-939`) then fires `CLIENT_WRITEABLE`.

No interleaving strands a frame.

**Lock ordering and lws.**
- `go_channel_get_depth` does two atomic loads, one magic check and takes no lock. It runs on a channel the callback already holds a ref to (`:318-326`). So no new `priv->mutex → chan->mutex` edge exists.
- The writer never nests the two locks.
- `lws_callback_on_writable` is called after `priv->mutex` is released, on the service thread, from inside the writeable callback. The existing code already does this at `:379`, so there is no deadlock risk with lws internals.

**Minor behaviour note.** If the head is BUSY (a writer between claim and publish), the drained branch re-arms. The next `CLIENT_WRITEABLE` then spins `MPMC_TRY_SPINS` × `CPU_RELAX` in `try_receive` before failing and re-arming again. That is a short service-thread busy-loop until the writer publishes; it is bounded and acceptable.

The idle busy-poll fix (libnostr-idle-writable-busy-poll-20260817) is preserved: the flag stays up only while depth is above 0.

**Evidence.**
- Fails on the parent.
- Fails 3/3 with fixed libgo and the old `connection.c`.
- Passes 10/10 on the fix, and passes under the REFINED_SIGNALING=OFF build.

**No changes requested.**

---

## Versioning (AGENTS.md, "Component Versioning Policy")

- **libgo 0.1.1 → 0.1.2 and libnostr 1.0.8 → 1.0.9, both PATCH.** These are backward-compatible bug fixes. `channel.h` is untouched, the `GoChannel` layout is unchanged and no API is added, so PATCH is the correct bump.
- **Sources.** The authoritative sources named in `VERSION_MANIFEST.md` are updated: `libgo/CMakeLists.txt:27` and `libnostr/CMakeLists.txt:8`. The manifest's **Declared version** is updated in the same commit. **Latest release** and **Release tag** are correctly left alone.
- **Generated artifacts agree.** `LIBGO_VERSION_STRING "0.1.2"`, `NOSTR_VERSION_STRING "1.0.9"`, `libnostrgo.pc Version: 0.1.2` and `nostr.pc Version: 1.0.9`.
- **nostr-gobject: no bump**, which is correct. It has no source change, and this matches the libnostr-only precedent in `ae389499`. The bead and commit message both record the decision.
- The F1–F4 fixes are also PATCH-level. Since 0.1.2 is unreleased, folding them into the same declared version is consistent with the manifest process.

---

## Required before push

1. **F1.** Give the MPMC ring at least two slots while keeping the logical capacity (release with `tail + ring`). Add capacity-1 regression cases: try/try white-box, and blocking receive against `try_send`.
2. **F2.** Make `test_mixed_variants_fifo` mode-aware, and give `GoChannelMpmcSlotTest` a `TIMEOUT`. Re-run it under `-DGNOSTR_ENABLE_TSAN=ON`.

## Recommended (non-blocking)

- F3: bounded spin, then a deadline wait or fiber park, in the four blocking BUSY branches.
- F4: fold into F1 (clamp the ring, decide what capacity 0 means).
- F5: widen nostrc-v624 to cover `:1224`, `:1435`, `:1836` and `occ_before`.
- F6: `closed && depth == 0` in `try_cases_once`.
- F7: document that `GO_CHANNEL_ATOMIC_TRY` only matters with `MPMC_SLOTS=OFF`.
- F8: follow-up bead for a TSAN job with `MPMC_SLOTS=ON`, and for the single-lock ring's capacity-1 "always full" bug.
- F9: correct the "4 of 6" wording if the commit is amended.

## Verdict

**REQUEST CHANGES.** The root-cause analysis is right, the libnostr re-arm is correct, and the unified slot protocol fixes the NULL receives, the ticket corruption and the stranded REQ, with good evidence. However, the shared protocol loses elements and wedges capacity-1 channels (F1), which now includes the blocking receive paths and, after close, becomes a 100% CPU hang. And the new test will hang libgo-ci's TSAN cells (F2). Both fixes are small.

---
---

# Addendum 1 — re-review of `bb001698` (2026-09-29)

**Commit:** `bb001698`, *fix(libgo): address the nostrc-75rv channel review (capacity-1 wedge, CI hang)*, on `nostr-gobject/w16-eose-timeout` (parent `37b0203a`)
**Files:** `libgo/src/channel.c`, `libgo/src/select.c`, `libgo/include/channel.h`, `libgo/CMakeLists.txt`, `libgo/tests/go_channel_mpmc_slot_test.c`, `.github/workflows/libgo-ci.yml`. libnostr is unchanged.
**Verdict:** **APPROVED.** Both blocking findings are fixed and verified. The new findings (N1–N4) are non-blocking follow-ups.

## Verification performed

| # | What | Result |
|---|---|---|
| 1 | Full build and `ctest -j6 --timeout 300`, run three times on a loaded host (another session was running a Docker Groundhog CI loop; load average 10–22) | **414/414** on the third run. The first two runs were 412/414 each, and every failure is a pre-existing load flake that reproduces on `37b0203a` (N3) |
| 2 | My original capacity-1 reproductions (review F1), against `bb001698` | White-box: the second send lands in the other slot and element 10 is **not clobbered**; the receiver gets 10; the channel keeps working afterwards. 2 try producers × 2 try consumers: 2.25 M / 3.65 M / 3.78 M tokens with **0 lost**, `depth_after_drain=0`, both probes succeed. 1 `try_send` producer × 1 blocking `go_channel_receive`, then close: **5/5** runs deliver every token (62 k – 3.4 M), and the consumer returns on close using 0.00 s of CPU |
| 3 | Every case of the new slot test, each in its own process, on `bb001698`, `37b0203a` and the parent `90c1f9ed` | `bb001698`: all 12 pass. `37b0203a`: the six new cases (6a, 6b, 7a, 7b, 8, 9) all **FAIL**. Parent: those six also fail (7b hangs, which the new 60 s TIMEOUT bounds). The author's claim "every new case fails on both 90c1f9ed and 37b0203a" holds |
| 4 | libgo standalone with `-DGO_WARNINGS_AS_ERRORS=ON`, reproducing libgo-ci's three cells exactly: `asan_ubsan` (−E GoChannelStressTest), `tsan` (six exclusions), `tsan_mpmc` (no exclusions); TSAN with `halt_on_error=1` | asan_ubsan **24/24**, tsan (slots off) **19/19**, tsan_mpmc (slots on, confirmed `NOSTR_CHANNEL_MPMC_SLOTS=1`) **25/25**. No sanitizer reports |
| 5 | Both slot modes × both REFINED settings, standalone `-Werror` | slots ON / REFINED ON **25/25**. slots OFF (size-counted ring) **25/25**, including GoTickerTest/GoSelectTest, which now pass thanks to the `chan->size` fix. slots ON / REFINED OFF **25/25**. slots OFF / REFINED OFF **25/25**. All four `-Werror` builds compile clean |
| 6 | Workspace ASAN (`-DGNOSTR_ENABLE_ASAN=ON`), `libgo/` and `tests/` | libgo **25/25**, tests/ **36/36**. `test_concurrency_subscription_shutdown` exits 0 with `detect_leaks=0`, since leak detection is unsupported on macOS arm64 |
| 7 | Capacity-0 ASAN probe (review F4) against the ASAN libgo | capacity becomes 1 on a two-slot ring (`mask=1`), `try_receive` returns −1, **no ASAN report** |
| 8 | `test_nostr_gobject_subscription_eose_order` × 40 alongside a continuous `ctest -R groundhog- -j8` | **40/40** |
| 9 | `chan_bench`, `37b0203a` vs `bb001698` (see Performance) | on par; capacity-1 now works |
| 10 | Fiber probes for the new `mpmc_wait_busy` fiber path | see N1 |

## Status of the original findings

| ID | Status | Evidence |
|---|---|---|
| **F1** capacity-1 ABA | ✅ **Fixed** | `go_channel_create` sizes the MPMC ring as `max(2, pow2(cap))` (`channel.c:1009`) with `mask = ring − 1` (`:1022`). `slot_seq` is initialised for `ring` entries, and `mpmc_pop` releases with `tail + mask + 1` (`:566`). The logical `capacity` still bounds `head − tail` in `mpmc_push` and `go_channel_is_full`, so capacity-1 wake and ticker channels still coalesce to one token. Header comment updated (`channel.h`). Probes and cases 6a/6b/7a/7b confirm it |
| **F2** test hangs with slots off | ✅ **Fixed** | Case 4 measures what the ring actually holds (`channel_holds`), and 7a/7b skip where a capacity-1 ring holds nothing (nostrc-ecz3). `set_tests_properties(GoChannelMpmcSlotTest PROPERTIES TIMEOUT 60)`. It passes in the libgo-ci `tsan` cell configuration (19/19) |
| F3 unbounded BUSY yield spin | ✅ **Fixed for OS threads**; the fiber path has a latent clock bug (N1) | `mpmc_wait_busy` (`channel.c:598-616`): spin `MPMC_TRY_SPINS`, then `CV_WAIT_DEADLINE_OS` with a 1 ms nsync (realtime) deadline. Cases 3a/3b publish **without signalling** and pass, so the OS-thread deadline works |
| F4 capacity-0 out-of-bounds | ✅ **Fixed** | `size_t cap = capacity ? capacity : 1;` (`:985`), documented in `channel.h`. Case 9 plus the ASAN probe |
| F5 stale snapshots with REFINED OFF | ✅ **Filed** | nostrc-v624 widened, naming all three expressions and `occ_before` |
| F6 select "closed" over an in-flight element | ✅ **Fixed** | `go_channel_is_closed(c->chan) && go_channel_get_depth(c->chan) == 0` (`select.c:256`). Case 8 (fails on `37b0203a`, passes now). Minor note in N4 |
| F7 `ATOMIC_TRY` ignored with slots on | ✅ **Fixed** (documented) | Comment and option help text in `libgo/CMakeLists.txt` |
| F8 no TSAN coverage of MPMC; single-lock capacity-1 | ✅ **Fixed / filed** | New `GO_TSAN_MPMC_SLOTS` option, which also undoes a stale FORCE left in the cache. New `tsan_mpmc` CI cell running the full suite with no exclusions (25/25 locally). The single-lock capacity-1 bug is filed as nostrc-ecz3, and the `tsan` cell's exclusion comment now names the real cause |
| F9 "4 of 6" | ✅ Corrected in the `bb001698` message | — |

**Extra fixes the author found through the configuration matrix, reviewed and correct:**
- The retry labels are now inside `#if NOSTR_CHANNEL_MPMC_SLOTS`, which fixes `-Werror` with slots off.
- `fiber_waiter_wake_one` is marked `unused` for REFINED OFF.
- The single-lock try paths now maintain `chan->size` when `!DERIVE_SIZE`. The try paths hold `chan->mutex` there, so the update is ordered with the blocking paths. This is what makes the slots-off build pass GoTickerTest/GoSelectTest.

## New findings (all non-blocking)

### N1 — Low (latent) — fiber deadlines use CLOCK_REALTIME, but the scheduler compares against CLOCK_MONOTONIC

**Where.** `mpmc_wait_busy`'s fiber branch builds its deadline from `clock_gettime(CLOCK_REALTIME)` (`channel.c:600`) and passes it to `gof_hook_block_current_until` (`:608`). The pre-existing `go_select_timeout` fiber path does the same (`select.c:528`, `:533`). The hook's doc says "nanoseconds since epoch" (`fiber_hooks.h:62`). But the scheduler files and wakes sleepers against `gof_now_ns()`, which is `CLOCK_MONOTONIC` (`libgo/fiber/sched/sched.c:310-318`, compared at `:582` and `:628`), and `__gof_sleep_ns` (`timers/timer_bridge.c:8-18`) builds monotonic deadlines. On Linux a realtime deadline is therefore about 1.79e18 ns while "now" is uptime, so the deadline lies decades in the future. A BUSY-parked fiber then wakes only if a peer's signal happens to reach it.

**Why the macOS runs don't show it.** Homebrew's `libnsync.dylib` exports its own `clock_gettime` (`nm -gU … T _clock_gettime`; nsync `platform/macos/platform.h:34-38`: "Some versions(!) of MacOS don't implement clock_gettime()"). Every binary linking nsync binds to that shim, which returns wall-clock time for every clock id.
- A plain program printed MONOTONIC = 745 515 s (uptime).
- The same `mono()` linked against libgo and nsync returned 1.79e18.
- Fiber deadlines of REALTIME + 2 s and MONOTONIC + 2 s both fired at exactly 2 s. A deadline of 1 ns returned immediately, and 2^62 never returned. The only consistent explanation is that the scheduler's "monotonic" clock *is* wall time on this platform.

So no macOS test can catch a clock-domain mismatch in libgo. Monotonic timers there also jump with wall-clock changes.

**Impact.** It is latent today:
- In-tree goroutines run through `go_fiber_compat`, on OS threads ("Fiber scheduler removed", `nostr-homed/src/fs/nostrfs.c:893`; `nostr-gobject/src/nostr_simple_pool.c:1821`, `:2112`).
- No in-tree configuration builds REFINED_SIGNALING=OFF.
- With REFINED ON, the claimant's post-publish/release `CV_SIGNAL_*` still reaches a parked fiber.

Where it bites is the case this deadline exists for. The widened nostrc-v624 note says the 1 ms deadline guarantees BUSY waits "cannot hang on a skipped broadcast in the OFF configuration". That is true for OS threads but **not for fibers on Linux**, and the pre-existing `go_select_timeout` fiber path would never time out on Linux.

**Recommended follow-up bead:**
- Define the hook deadline as `CLOCK_MONOTONIC`, matching `gof_now_ns` and `__gof_sleep_ns`.
- Fix both callers.
- Correct `fiber_hooks.h:62`.
- Add a Linux CI fiber test that parks a fiber through `go_select_timeout`.

Not blocking, because nothing in the tree reaches it.

### N2 — Info — `get_depth` in `select` reads `chan->size` without the lock in size-counted single-lock builds

The F6 check calls `go_channel_get_depth`. With slots OFF and `DERIVE_SIZE` OFF, that returns plain `chan->size` read outside `chan->mutex`. It is a benign racy read: select re-checks after a signal. TSAN can't see it, because TSAN builds force `DERIVE_SIZE`. It could be folded into nostrc-ecz3.

### N3 — Info — pre-existing load flakes seen in the full runs (none are channel regressions)

Each was reproduced in a loop under CPU load, on both `37b0203a` and `bb001698`:

| test | failure | `37b0203a` | `bb001698` | cause |
|---|---|---|---|---|
| `concurrency_channels` | `FAIL: sender didn't complete` (`tests/test_concurrency_channels.c:99`) | 1/600 | 0/600 (1/300 in an earlier loop) | the test's own race: the receiver can return between the sender's publish and its `completed = true` store. Join the sender before asserting |
| `groundhog-group-ui` | `test_admin_gating: events_seen (4 == 3/2)` (`test_group_ui.c:840`) | 3/150 | 3/150 | the admin-gating flake, fixed on master by nostrc-qp24.87; not in this branch's base |
| `gnostr-test-event-item-txn-budget` | `elapsed < TIMING_BUDGET_US(1000)`: 2207 µs (`test_event_item_txn_budget.c:202`) | 1/300 | 0/300 (1 abort in a full run) | a 1 ms wall-clock budget under host load; the test doesn't touch channels |

Recommend beads for the first and third if they aren't already tracked. The commit message reports "Full ctest -j6: 419/419", but `bb001698` registers 414 tests; the author's count probably came from a working tree with other changes.

### N4 — Info — F6 trades "closed" for "wait" if a claimant never publishes

A select on a closed channel whose claimed slot is never published now waits (until its timeout) instead of reporting closed. That is the correct trade-off, and it matches `go_channel_receive`. A claimant can only fail to publish if its thread dies mid-push.

## Performance

`chan_bench`: 1 M messages, messages per second. The host was loaded by another session, so the numbers are noisy.

| capacity / producers / consumers | `37b0203a` | `bb001698` |
|---|---|---|
| 1024 / 1 / 1 | 2.62 M, 2.60 M, 2.59 M | 2.61 M, 2.49 M, 2.55 M |
| 1024 / 4 / 4 | 2.31 M, 2.36 M, 2.36 M | 2.29 M, 2.45 M, 2.43 M |
| 8 / 4 / 4 (×8) | 0.63–0.98 M | 0.94–0.99 M |
| 8 / 8 / 8 (×8) | 0.77–0.85 M | 0.76–0.86 M |
| 64 / 8 / 8 | 1.44 M, 1.50 M, 1.54 M | 1.38 M, 1.41 M, 1.24 M |
| **1 / 1 / 1** | **timeout ×3 (wedged)** | 0.59 M, 0.60 M, 0.75 M |
| **1 / 2 / 2** | **timeout ×3 (wedged)** | 0.44 M, 0.42 M, 0.42 M |

- There is no regression. The two-slot ring for capacity 1 costs one extra pointer per channel.
- The 1 ms BUSY wait does not show up in the contended small-ring configurations: an earlier single 8/4/4 outlier at 212 k did not reproduce in 8 repeats, and `37b0203a` has a similar outlier (632 k).
- **The capacity-1 rows are the F1 fix measured directly:** `chan_bench` wedges on `37b0203a` and runs on `bb001698`.

## Versioning

libgo stays at **0.1.2**. That is correct: 0.1.2 is unreleased (manifest *Latest release*: Unreleased), and these are PATCH-level fixes to it. `channel.h` changes only comments; the `GoChannel` layout and API are unchanged. The one behaviour change is that `go_channel_create(0)` now creates a 1-slot channel where it used to create a broken one; it is documented and backward compatible. No libnostr change.

## Addendum verdict

**APPROVED.**
- F1 and F2, the blocking findings, are fixed and independently verified: the probes that wedged `37b0203a` now pass, the new regression cases fail on both older commits, and the libgo-ci cell configurations pass with `-Werror` and no sanitizer reports.
- F3, F4, F6, F7 and F8 are fixed; F5 and the single-lock capacity-1 bug are filed (nostrc-v624, nostrc-ecz3); F9 is corrected.

Recommended follow-ups (not blocking):
- **N1:** a bead for the fiber deadline clock domain (`mpmc_wait_busy`, `go_select_timeout`, `fiber_hooks.h`), plus a Linux fiber-timeout test; and qualify the nostrc-v624 note.
- **N2:** fold into nostrc-ecz3.
- **N3:** beads for the `concurrency_channels:99` race and the txn-budget timing test, if not already tracked.
