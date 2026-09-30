# W23 review: libnostr/libgo/nostr-gobject teardown leaks, empty lsan.supp

- **Reviewer:** independent peer reviewer (AGENTS.md "Peer Review")
- **Branch reviewed:** `fix/w23-libnostr-teardown-leaks` at `82815489`, 5 commits on `43f39adf` (origin/master)
- **Review branch:** `review/w23-teardown-leaks`
- **Date:** 2026-09-30
- **Verdict:** **REQUEST CHANGES**. There is one blocking finding, **B1**: the new "the loop's reconnect adopts a connection a dial installed meanwhile" path can deadlock the relay.
  - The loop waits for a relay-wide lease count to reach zero.
  - The lease it waits on is the writer's lease on the *adopted* connection.
  - Only the loop itself would ever close that connection's send channel.
  - I forced it 3 of 3 times in the gate image. The fix is small (see B1).
- **Everything else holds:**
  - Each leak fix fails its test when reverted (14 mutations).
  - The CI-equivalent sanitizer set with an empty `lsan.supp` passed 11 of 11 official gate runs, with no reruns.
  - The libgo context change is correct in both MPMC slot modes.
  - `OPENSSL_thread_stop()` on a pooled GLib thread is safe.

**Commits**

| Commit | Bead | Change |
|---|---|---|
| `3dc15316` | nostrc-xfjg | every connection release goes through `relay_retire_connection()` → `nostr_connection_release()` (detach, close, drain both channels); a reconnect releases the connection it replaces; the writer leases `relay->connection` |
| `2eda5741` | nostrc-xbso | refcounted async-cleanup handle and an exit wait; inline `write_error()`; late answers freed (`nostr_relay_write_answer_release()`, Groundhog's gnostr write); `nostr_relay_close()` answers queued writes; unread CLOSED/COUNT freed |
| `25aa9fbc` | nostrc-jw23 | libgo frees background contexts; a subscription releases its context and owned filters; GNostrSubscription hands its filters over and makes no background context |
| `3db1ba16` | nostrc-vpha | `nostr_relay_connect()` publishes under the relay mutex; the loser releases its connection; a dial landing mid-reconnect is adopted by the loop. Groundhog's open worker calls `OPENSSL_thread_stop()`. Dial hook and OpenSSL thread-hold tests |
| `82815489` | nostrc-vpha | `lsan.supp` emptied; the gate's library-leak rerun rule reverted |

No code or beads were changed. The mutations, demos and extra builds ran in a private Docker volume (`nostrc-w23rev-asan`) from a copy of this tree. Scratch scripts were in `/tmp/w23rev`.

## Summary

**The leak fixes are right, and each one is pinned by a test.**
- One release path for every connection is a real simplification: the old failed-dial, close and free copies are gone, and the drain now also frees frames that were queued but never written.
- The async-cleanup handle's two references replace a flag that was checked only once.
- Inline `write_error()` is safe: the answer channel has capacity 1 and nothing else sends to it.
- The answer-release drain is sound in both slot modes. A blocking `go_channel_send()` checks `closed` and pushes under the channel mutex that `go_channel_close()` takes.
- The subscription destroy drains only ever see heap values: strdup'd CLOSED reasons and malloc'd COUNT results.

**B1 is the one blocking finding.** `conn_leases` counts leases on *whatever* `relay->connection` was when the writer leased it. `relay_retire_connection()` waits for that count on every release, including releases of connections that were never published.
- For a connection that *was* published (reconnect's `old_conn`, close, free), waiting is right. Those paths close the connection's channels first, which wakes a writer blocked on them.
- For a connection that was *never* published, waiting is wrong. That covers the loop's own dial on the adopted path, and a losing or failed `nostr_relay_connect()` dial. The writer can hold its lease on a *different* connection, blocked on that connection's full send channel.
- On the adopted path this is a circular wait (B1). On the losing-dial path it is an unbounded stall (N1).

**Test runs.**
- Official gate: `scripts/linux-gate.sh --sanitizers` 11 times on this tree. Each run was 49 tests at the job's `--parallel 2`, GCC ASAN+UBSAN+LSan, empty `lsan.supp`: 49/49 passed first time, no reruns.
- Private builds, same image and configure arguments:
  - 14 mutations: every one fails.
  - MPMC slots OFF: the W23 tests pass 3 of 3.
  - Stress: 24/24 for `test_relay_teardown_leaks` at 4-way parallel, and 10/10 for `groundhog-account-store`.
- `scripts/check-unsequenced-args.py`: clean.

## Findings

### B1 (High, blocking): the reconnect's adopt path deadlocks on a lease held on the adopted connection

**Where**
- `relay_attempt_reconnect()`, `relay.c:1186-1194`. When `adopted` is true, it calls `relay_retire_connection(r, new_conn)`.
- `relay_retire_connection()`, `relay.c:644-654`. It closes `conn`'s channels, then waits `while (r->priv->conn_leases > 0)`.
  - That count is a lease on whatever `relay_connection_lease()` (`relay.c:622`) handed out, not a lease on `conn`.
- `write_operations()`, `relay.c:994-1009`. It leases `relay->connection`, then calls `nostr_connection_write_message()`.
  - That call blocks in `go_channel_send_with_context(conn->send_channel, …)` (`connection.c:1425`).
  - It returns only when the channel has room, the channel is closed, or the relay's context is cancelled.
- `conn_wsi_gone()`, `connection.c:143-187`. When a socket goes away, it closes only the **recv** channel.
  - The send channel is closed only by `nostr_connection_release()`, which in practice means by the loop's next reconnect.

**Sequence**
1. The relay loses its connection. The loop's `relay_attempt_reconnect()` retires it and dials C.
2. During that dial, a `nostr_relay_connect()` dials B, finds `relay->connection == NULL` and publishes B.
   - Examples of the caller: Groundhog's 1 s retry (the nostrc-xfjg message names this race), or a shared relay's other owner.
   - `message_loop_active` is set, so no second loop starts. The state becomes CONNECTED.
3. Callers write, and the writer leases B. B's handshake is still pending, so the LWS thread writes nothing yet.
   - 16 frames fill B's send channel. The writer blocks in the 17th send, still holding the lease.
4. The loop installs its connection. It finds `relay->connection == B`, so `adopted` is true, and calls `relay_retire_connection(r, C)`. That waits for `conn_leases == 0`.
5. What happens next depends on B:
   - **B's handshake completes.** B drains and the loop goes on, after being stalled for B's whole handshake.
   - **B's handshake fails.** A flaky relay is why the loop is reconnecting at all, so this is plausible. `conn_wsi_gone()` closes only B's recv channel, and B's send channel stays full and open.
     - The writer waits for B's send channel. The loop waits for the writer. Nothing else closes B's send channel.
     - The relay is wedged: nothing reads, nothing reconnects, and the refire never runs.
     - The 16-slot write queue fills, and later `nostr_relay_write()` callers block in their enqueue (for example `nostr_relay_publish()` before its 5 s wait, or the gnostr write GTask).
     - Only `nostr_relay_close()` or free (context cancel) releases it.

**Evidence.** Forced in the gate image, 3 of 3 runs. Reviewer instrumentation added a hook in `relay_attempt_reconnect()` right after its dial, the same shape as the author's dial hook (Appendix A). The steps were:
1. The loopback server stops answering handshakes.
2. A `nostr_relay_connect()` publishes B.
3. 24 writes are issued.
4. The loop is released.
5. The server goes away.
```
after 3 s: unreleased=2 (was 2) leases=1 B send depth=16 -> loop BLOCKED in retire(new_conn) [adopted path]
B's socket gone (recv closed); 13.0 s after release: unreleased=2 leases=1 -> WEDGED (loop waits for writer, writer waits for B's send channel)
nostr_relay_close returned after 0.000 s
```
C, the loop's own dial, is still unreleased 13 s later, because the loop never left `relay_retire_connection()`. Before this branch, the same interleaving leaked B and started a second loop and writer (the nostrc-vpha bug), but it did not deadlock. The deadlock is new.

**Fix (small).** A connection that was never stored in `relay->connection` can hold no lease, because `relay_connection_lease()` hands out only `relay->connection`, under the mutex.
- Release never-published connections without the wait. That means calling `nostr_connection_release(conn)` directly, which closes both channels itself. The paths are:
  - `relay_attempt_reconnect()`: the `closing` and `adopted` paths.
  - `nostr_relay_connect()`: the lost path, and the no-context/allocation-failure path.
- Keep `relay_retire_connection()` for connections taken out of `relay->connection`: reconnect's `old_conn`, close and free.
- Per-connection lease counts would also work, but need more code.
- This also fixes N1.

**Test.** Nothing covers the adopted path. `test_racing_dials_publish_one_connection` races two `nostr_relay_connect()` calls with no loop running and no writer holding a lease.
- Add a reconnect-side test hook next to `nostr_relay_test_set_dial_hook()`.
- Add a case that:
  1. holds the loop after its dial;
  2. publishes B from `nostr_relay_connect()` against a server that holds handshakes;
  3. writes until the writer blocks;
  4. releases the loop.
- Assert that the loop's dial is released (`nostr_connection_unreleased_count()` drops) without waiting for B.
- Assert that the relay recovers once B's socket dies.

Appendix A has the recipe.

### N1 (Medium): a losing dial waits on the winner's writer

This has the same root cause as B1. In `nostr_relay_connect()`, a dial that lost the race calls `relay_retire_connection(relay, conn)` (`relay.c:886`), which waits on leases on the *winner*.

**Evidence.** Demo, 3 of 3 runs:
- The winner dials a listener that never answers the upgrade.
- 24 writes are issued, so the writer blocks with a lease on the winner.
- Then the loser is released.

```
losing dial STILL BLOCKED after 5.0 s (writer holds a lease on the winner)
losing dial returned 5.007 s after release (after close)
```

**What it blocks.** The loser's caller: a pool redial worker, `gnostr_relay_connect()`'s caller, or Groundhog's retry. It stays blocked until one of these happens:
- the winner's in-flight write finishes (its handshake);
- the loop retires the winner;
- the relay is closed.

It is not a deadlock, because the loop is free to retire the winner. The B1 fix removes it.

It does **not** race the LWS thread. The loser's release is the old `relay_discard_failed_connection()` sequence:
1. Detach both channels under `conn->priv->mutex`, so `recv_channel_enqueue()` and the WRITEABLE callback see NULL.
2. Close them.
3. Hand the WSI to the service thread (`nostr_connection_close()`).
4. Leave the struct to the graveyard.

The racing-dials test shows the loser's socket closed (`srv.open` → 1).

### N2 (Low): late answers in callers that still close and unref

`apps/gnostr/src/sync/neg-client.c:732, 772, 788` still call `go_channel_close(wch); go_channel_unref(wch);` without draining.
- `write_error()` is now inline, so an unqueued write's Error is already in the channel when `nostr_relay_write()` returns.
- These callers therefore leak it *every* time the relay is closing.
- Before, the detached thread usually lost the race to the caller's close, and freed the Error itself.

**Suggestion.** Make `nostr_relay_write_answer_release()` public, or document close → drain → unref in `nostr_relay_write()`'s header, and fix neg-client.

**Related, pre-existing.** `nostr-relay.h:246` documents `@msg: (transfer full) … will be freed by callee`. In fact `nostr_relay_write()` copies `msg` and never frees it.
- Callers that follow the doc and strdup their message leak the copy: neg-client.c three times, and `signet/src/relay_pool.c:296`.
- Signet also never releases its answer channel.

### N3 (Low): GI annotations contradict filter ownership, and it now matters

`gnostr_subscription_new()` (`nostr_subscription.h:73`) and `gnostr_pool_subscribe()` (`nostr_pool.h:233`) annotate `@filters` as **(transfer none)**. But:
- both take ownership on success, and every in-tree caller treats them that way;
- as of this branch, the core subscription frees the filters.

A binding that honours the annotation frees them too, which is a double free. Change both to **(transfer full)**, stating that on failure the caller keeps them. (`nostr_pool.h:301`, the multi-subscribe API, copies its filters, so **(transfer none)** is right there.)

### N4 (Low): libgo child contexts do not reference their parent

`go_context_with_cancel()` stores `ctx->parent` without a reference (`context.c:244`), and `hierarchical_context_is_canceled()` reads it.
- Now that background contexts are really freed, "free the parent first" is a use-after-free. The new `context.h` comment says so.

I audited every `go_context_background()` caller in the library, apps and tests. **None frees a background context while a child is live.**
- Most never free it, so they still leak it (not this branch's scope):
  - `nostr_simple_pool.c:514/693/1046/1086/1434/1726`
  - `simplepool.c:611/1106/1409/1480`
  - `subscription.c:224` (relay-less subscriptions)
- libhanami frees one that has no children.
- The tests free the relay before the context.

**Suggestions.**
- Take `go_context_ref(parent)` in `hierarchical_context_init()` and unref it in the child's free, so ordering cannot matter.
- Note that `go_context_free()` now frees a background context regardless of its refcount. Mixing `free` and `unref` on one context is now a double free.

### N5 (Low): a connect landing as the loop exits, or during backoff

`nostr_relay_connect()` publishes without spawning whenever `message_loop_active` is set. Two edge cases follow:
- **The loop is exiting.** The loop can be past its last look at `relay->connection` and about to clear the flag, for example when auto-reconnect is switched off after a failed reconnect left `relay->connection == NULL`.
  - The new connection then has no reader until close, while the state says CONNECTED.
  - This is narrow. Before the branch it started a second loop instead.
  - Fix: re-check under the mutex in `relay_message_loop_retire()` before clearing the flag.
- **The loop is in backoff after a failed reconnect.** A connect there publishes B. The loop's next attempt then takes B as `old_conn`, retires it and dials again. That is a wasted dial, not a leak.

### N6 (Low): R3 only answers queued writes when connected

`nostr_relay_close()` returns early ("relay not connected") when `relay->connection` is NULL, for example mid-reconnect or in backoff.
- That return comes before it joins the workers and before `relay_write_queue_drain()`.
- Writes queued at that moment are still answered only when the relay is freed.
- The "17 of 25 → 25 of 25" claim holds for the connected case.
- Fix: close the queue, join the writer, then drain, all before the early return.

### N7 (Info): hosted CI with no suppressions

My evidence and the author's are both arm64 (the gate image). Hosted `groundhog-sanitizers` runs x86_64 on smaller runners, and the rerun rule is gone. I looked for the exit-race classes that could remain:
- **Other pool threads doing OpenSSL work.** Only the open worker runs SQLCipher off the owner's thread. The image's glib-networking 2.80 ships only the GnuTLS module (`libgiognutls.so`), so libsoup's TLS never touches OpenSSL.
- **Async cleanups at exit.** These are now waited for, with a 5 s bound.
- **The LWS thread's TLS state.** The tests use `ws://`.

I think removing every entry is safe. Watch the first hosted runs, and fix any new report at its source rather than restoring a broad entry.

`test_relay_teardown_leaks` also runs in `nostrc-ci` (ubuntu and macOS, `detect_leaks=0`). There it is only the counter and answer checks, about 3 s.

### N8 (Info): `OPENSSL_thread_stop()` on a GLib pool thread is safe

**How it works in OpenSSL 3.0.**
- It frees only the *calling* thread's registered thread-event state: the per-thread public and private DRBGs, the error queue, and async state, for every library context.
- It clears the thread-local, so the key destructor later finds nothing.
- The next OpenSSL call on that thread re-registers and rebuilds the state lazily. The header's "may use OpenSSL again afterwards" is right.
- It is a no-op before OpenSSL is initialized, and after `OPENSSL_cleanup()` (`destructor_key.sane == -1`).

**Why a reused pool thread is fine.**
- A GThreadPool thread runs one task at a time, so no other library has an operation in flight on that thread when `open_worker` calls it.
- What it drops is per-thread cache (DRBG, error queue), never state an SSL object needs across tasks. For example, glib-networking's OpenSSL backend, where it is used, runs its own handshake tasks and reads its errors within the same task.
- The call runs before `g_task_return_*`, so it happens before the owner can exit and race `OPENSSL_cleanup()`.

**Scope and one caveat.**
- It covers only the open worker. That is the only pool-thread SQLCipher user today, and `gh-store.h` states the rule for future ones.
- `find_package(OpenSSL)` may resolve to a different libcrypto than the one SQLCipher links through pkg-config on some hosts (a Homebrew keg vs the system). The call would then be a harmless no-op on the wrong library, and Linux CI would catch the leak.

## Answers to the review checklist

**Lock ordering and deadlocks.**
- The relay mutex is held only for field updates and the `conn_leases_cv` wait. The publish under the lock calls nothing.
- `nostr_connection_release()` takes `conn->priv->mutex` without the relay mutex.
- The writer takes `conn->priv->mutex` and `g_lws_mutex` separately. No path takes the relay mutex while holding a connection, channel or context lock, so there is no new inversion.
- The deadlock is a lease wait, not a lock: **B1**.
- A losing dial's free can block (**N1**), but does not race the LWS thread.

**Every free.** No double free or use-after-free found:
- the release path and graveyard;
- reconnect vs concurrent connect: exactly one of reconnect, connect, close and free takes each pointer, under the mutex;
- late answers, both in `write_answer_release` and Groundhog's drain;
- queued-write answers: `write_error`, the close drain and the free drain;
- the async handle's refcount, including abandon after completion;
- subscription destroy: context, filters, CLOSED and COUNT drains;
- `set_filters` ownership, including the same-pointer handover;
- GNostrSubscription's handover: a refire holds a subscription reference, so the filters outlive it;
- background-context frees (audit in N4).

**libgo context changes, both MPMC slot modes.**
- `base_context_free()` on a vtable-less background context closes and frees its done channel. The token that `go_context_cancel()` may leave in it is a non-owned pointer.
- The drains that matter never race a sender:
  - recv: detached under `priv->mutex` first;
  - send: no writer holds a lease at release;
  - answers and write queue: blocking sends, closed under the channel mutex;
  - subscription channels: last reference.
  So an MPMC `try_receive` cannot stop early on a BUSY (unpublished) slot.
- Built with `-DGO_CHANNEL_MPMC_SLOTS=OFF` (`NOSTR_CHANNEL_MPMC_SLOTS=0` confirmed in `build.ninja`), 3 runs each. These pass 3/3:
  - `test_relay_teardown_leaks`, `test_connection_recv_drain`
  - `groundhog-relay-wire`, `groundhog-relay`, `groundhog-account-store`
  - `GoContextStressTest`, `GoChannelCloseTest`
- `GoSelectCancelTest` and `GoContextCancelTest` report LSan leaks in their *test code*: contexts never freed, and an unfreed `go_context_err()` Error. That is pre-existing and not in any leak-checked set (P2).

**`OPENSSL_thread_stop()`:** see N8.

**Removing every suppression:** 11/11 official gate runs clean (evidence below), plus the stress runs. See N7 for hosted risk.

**Tests fail without the fixes:** all 14 mutations fail (table below). Not pinned by any test:
- the adopted path (B1);
- the connect-while-loop-active path (N5);
- Groundhog's `gh-relay-gnostr-write.c` late-answer drain;
- the atexit wait for async cleanups (the set-filters case would likely pass without it).

**`scripts/check-unsequenced-args.py`:** "No call modifies and uses a variable in different arguments" (exit 0).

## Evidence

**Environment**
- Docker 28.0.4, aarch64 (14 CPUs).
- `nostrc-linux-ci:arm64` (sha256:aa281f0c8e0a): Ubuntu 24.04.5, GCC 13.3.0, OpenSSL 3.0.13, libwebsockets 4.3.3, SQLCipher 4.5.6.
- The macOS host toolchain was not used.

**Official gate:** `scripts/linux-gate.sh --sanitizers <this tree>`, run 11 times in sequence.
- The job config is read from `groundhog-ci.yml`: 49 tests, `--parallel 2`, `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`, and `LSAN_OPTIONS=suppressions=…/lsan.supp` with the file empty.
- Every run: 49/49 passed on the first pass, with no rerun notice. The test phase took 1m07s to 1m55s.
- `check-inputs`: all 849 inputs covered.

**Mutations.** Each reverts one fix, rebuilt with the job's configure arguments and run under the job's sanitizer environment.

| Fix reverted | Test | Result |
|---|---|---|
| none (baseline) | `test_relay_teardown_leaks` / `groundhog-account-store` / `groundhog-relay-wire` | pass (1/1, 3/3, 3/3) |
| xfjg: reconnect only closes the old connection | `test_relay_teardown_leaks` | `CHECK failed: nostr_connection_unreleased_count() == base + 1` |
| same, with the counter CHECKs removed | same | LSan: 33,920 bytes in 6 allocations, `go_channel_create` ← `nostr_connection_new` (`connection.c:1104/1105`), matching the commit message |
| vpha: publish unconditionally, always spawn | same | `CHECK failed: … == base + 1` |
| xbso R3: close does not drain | same | `CHECK failed: answered == N` |
| xbso R2: answer release does not drain | same | LSan 61 B / 2: the Error from `nostr_connection_write_message` |
| xfjg: send channel freed without draining | same | LSan 527 B / 34: WebSocketMessages from `write_operations` |
| xbso: abandon never drops the caller's reference | same | LSan: both `free_async` handles (`:297` abandon case, `:412` set-filters case) |
| jw23: background context not freed | same | LSan 664 B / 4 (`test_contexts_are_released`) |
| jw23: subscription keeps its context | same | LSan 2,880 B / 20 (`nostr_subscription_new:225`) |
| jw23: set filters not freed | same | LSan 768 B / 8 (`any_filters` ← set-filters case) |
| xbso: unread CLOSED reason not freed | same | LSan 33 B / 1 (`nostr_subscription_dispatch_closed`) |
| vpha: `gh_store_thread_release_crypto()` removed | `groundhog-account-store` | LSan 7,678 B / 46 (`sqlcipher_codec_key_derive` → `RAND_bytes_ex`, `EVP_RAND_CTX_new`, `ERR_set_mark`), 3/3, matching the commit |
| jw23: GNostrSubscription drops its filters | `groundhog-relay-wire` | LSan 22,847 B / 248 (`subscription_filters` ← `ensure_subscription`), 3/3 |

**Stress (pristine tree)**
- `test_relay_teardown_leaks`: 4 copies in parallel × 6 rounds, 24/24.
- `groundhog-account-store`: 10/10. Its worker is held past `OPENSSL_cleanup()` every time.

**Demos**
- B1 adopted-path wedge: 3/3 (Appendix A).
- N1 losing-dial block: 3/3, blocked ≥ 5.0 s until close.

## Pre-existing, outside the diff

- **P1.** `nostr_relay_query_events()` selects on `closed_reason` with `recv_buf = NULL` (`relay.c:1971`), so a strdup'd CLOSED reason is lost when it fires. The COUNT producer (`relay.c:432`) leaks `val` when its send fails.
- **P2.** libgo `GoSelectCancelTest` and `GoContextCancelTest` leak in their test code under `detect_leaks=1`. That is visible only outside `nostrc-ci`, which runs with `detect_leaks=0`.
- **P3.** The `nostr_relay_write()` `@msg` annotation (N2).

## Appendix A: forcing B1

This is reviewer instrumentation, not a revert. It adds a hook in `relay_attempt_reconnect()` after its own dial and before it installs or adopts, next to the existing dial hook:

```c
/* relay.c, beside g_dial_hook */
_Atomic(NostrRelayDialHook) g_reconnect_hook = NULL;

/* relay_attempt_reconnect(), right after `new_conn = nostr_connection_new(r->url)` succeeds */
{ NostrRelayDialHook rh = atomic_load(&g_reconnect_hook); if (rh) rh(r, NULL); }
```

The demo uses a loopback lws server whose service thread can be paused (handshakes stay pending), plus `test_relay_teardown_leaks`' drop trick:

```c
/* established relay; the loop is running */
atomic_store(&g_reconnect_hook, reconnect_hook);          /* hook: set loop_dialled, wait for loop_go */
server_drop();  WAIT_FOR(state == NOSTR_RELAY_STATE_BACKOFF);
nostr_relay_reconnect_now(relay);  WAIT_FOR(loop_dialled); /* loop retired A, dialled C, held */
server_pause();                                            /* B's handshake will not complete */
CHECK(nostr_relay_connect(relay, &err));                   /* publishes B, no second loop */
for (i = 0; i < 24; i++) ans[i] = nostr_relay_write(relay, frame);
WAIT_FOR(relay->priv->conn_leases == 1 && go_channel_get_depth(B->send_channel) == 16);
int cnt0 = nostr_connection_unreleased_count();
atomic_store(&loop_go, true);                              /* loop adopts B, retires C ... */
usleep(3 s);   /* fixed: cnt0 - 1 (C released). Now: still cnt0, leases 1 */
server_stop(); WAIT_FOR(go_channel_is_closed(B->recv_channel));
usleep(10 s);  /* fixed: the loop has reconnected. Now: still cnt0, leases 1 (wedged) */
nostr_relay_close(relay, NULL);                            /* cancels ctx: the only way out */
```

As a regression test, assert `nostr_connection_unreleased_count() == cnt0 - 1` shortly after `loop_go`, with no dependence on B.
