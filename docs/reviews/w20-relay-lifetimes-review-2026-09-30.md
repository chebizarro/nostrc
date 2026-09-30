# Peer Review — W20 relay callback lifetimes, lossless subscriptions, lws wsi teardown

**Branch:** `fix/w20-gnostr-relay-uaf`, 3 commits on `8cc7c27e`, reviewed on a fresh branch `review/w20-relay-lifetimes` at `dbc680cd`:

| Commit | Bead | Subject |
|---|---|---|
| `2d054eee` | nostrc-flp7 | fix(libnostr,nostr-gobject): relay callback data outlives calls in progress |
| `fcbf8f20` | nostrc-dha5 | fix(nostr-gobject,groundhog): GNostrSubscription is lossless unless asked to bound |
| `dbc680cd` | nostrc-flp7 | fix(libnostr): the lws service thread no longer touches a wsi lws has freed |

**Reviewer:** independent peer reviewer (AGENTS.md, "Peer Review (REQUIRED)")
**Date:** 2026-09-30
**Verdict:** **APPROVED.** There are no blocking findings. One Medium follow-up (M1, a hard cap for lossless mode) should land before the Groundhog release that ships 0.11.1.

This review changed no source files and no beads. Mutations, probes and build trees live under `/tmp` and are not committed.

---

## Verification performed

| # | What | Result |
|---|---|---|
| 1 | macOS: `cmake -S . -B /tmp/w20 -G Ninja -DBUILD_GROUNDHOG=ON && ninja && ctest -j6 --timeout 300` | Build clean. **434/434 passed**, with 5 skipped (`test_nip5f_tcp`, `groundhog-launch`, `groundhog-store-key-keyring`, `groundhog-background-gui`, `groundhog-notifier-gui`). The 4 new or changed tests and `groundhog-net` pass. The commit message says 439 on the author's tree; this tree registers 434 |
| 2 | **Pre-fix, lws (dbc680cd):** `fcbf8f20` with only `test_connection_teardown_stress.c` and its CMake hunk grafted in; Linux gate image `nostrc-linux-ci:arm64` (Ubuntu 24.04, GCC ASan, lws **4.3.3**), 10 runs | **10/10 crash**: `ERROR: AddressSanitizer: SEGV on unknown address 0xbebebebebebebebe` in `lws_dll2_remove <- lws_set_timeout <- lws_service_loop connection.c:883`, the hosted-run stack |
| 3 | **Post-fix, lws:** same image, `dbc680cd`, 20 runs with `detect_leaks=1` | **0/20** |
| 4 | **Pre-fix semantics, callbacks (2d054eee), mutation M1:** in `libnostr/src/relay.c`, an invocation takes no slot reference and drops none (4 edits). This restores the old contract: removing a callback destroys its data while a call may be in flight. macOS ASan | `libnostr_relay_callback_lifetime`: **FAIL** (`CHECK failed: probe_destroyed(&probe) == 0`, `:246`). `test_nostr_gobject_relay_teardown`: **ASan crash with the W20 stack**, `g_pointer_bit_lock_and_get <- g_datalist_id_dup_data <- g_weak_ref_get <- on_core_state_changed <- relay_callback_slot_call_state <- relay_set_state <- message_loop`. Unmutated: both pass, 5/5 teardown cases |
| 5 | **Pre-fix semantics, lossless (fcbf8f20), mutation M2:** the drop-oldest loop ignores `lossless` | `/nostr-gobject/subscription/lossless-backfill-complete`: **FAIL** (`queued == expect_queued: 200 == 1000`). Unmutated: all 3 cases pass (lossless 1,000 then EOSE; bounded newest 200 then EOSE) |
| 6 | **Linux ASan+UBSan, the hosted `groundhog-sanitizers` configuration:** `-DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON`, `BUILD_APPS=OFF`, the hosted 44-test set, `detect_leaks=1` plus the hosted `LSAN_OPTIONS=suppressions=gnome/groundhog/tests/lsan.supp`, under `dbus-run-session xvfb-run` | **43/44.** The one failure, `groundhog-background`, is a LeakSanitizer report entirely inside `FcInit` (fontconfig/expat, reached from pango at startup), a difference between my local image and the hosted runner. It is unrelated |
| 7 | Linux ASan: every `libnostr_*` and `test_nostr_gobject_*` test (`detect_leaks=0`; hosted CI does not leak-check these) | **21/24.** All three failures are one pre-existing UBSan report in vendored LMDB (`third_party/nostrdb/deps/lmdb/mdb.c:7410`, NULL passed to memcpy), in `libnostr_store_txn_audit`, `test_nostr_gobject_main_thread_latency` and `test_nostr_gobject_integ_event_flow`. None touches the changed code. The new tests (`relay_callback_lifetime`, `connection_teardown_stress`, `relay_teardown`, `subscription_eose_order`) all pass. *Note:* the macOS bind mount is case-insensitive, so this container **did** build `testing/`; hosted CI does not (see nostrc-ig0z) |
| 8 | macOS TSan, workspace `-DGNOSTR_ENABLE_TSAN=ON` | Not usable for networked tests: `relay_teardown`, `groundhog-net` and `eose_order` never connect. In that configuration libgo uses the single-lock ring, where a capacity-1 channel can never hold an element (nostrc-ecz3), and libnostr's connect result is a capacity-1 channel (`connection.c:1145`). This is pre-existing, see I1 |
| 9 | macOS TSan with `-DGO_TSAN_MPMC_SLOTS=ON`, which keeps the shipped slot protocol | `libnostr_relay_callback_lifetime` **0 reports**; `test_connection_teardown_stress` **0**; `test_nostr_gobject_subscription_eose_order` **0**; `groundhog-net` **0**. `test_nostr_gobject_relay_teardown` passes all 5 cases with **3 reports, none in the changed code** (I1): the test itself polls `NostrRelay.refcount` with an atomic load outside the relay mutex, twice, and the mock server's stop flag once |
| 10 | lws source audit: `lib/core-net/client/connect.c`, `close.c`, `roles/h1/ops-h1.c` at tags **v4.3.3** (Linux) and **v4.5.8** (Homebrew) | see "lws callback semantics" |
| 11 | Versions: generated headers and pkg-config files | libnostr `1.1.0`, nostr-gobject `2.1.0` (CMake and meson agree), groundhog `0.11.1` |

---

## Findings summary

| ID | Severity | Blocking | Location | Summary |
|---|---|---|---|---|
| M1 | **Medium** | No (before the Groundhog 0.11.1 release) | `nostr-gobject/src/nostr_subscription.c:258`, `:742` | Lossless mode has no hard cap. A hostile or buggy relay that outpaces the main-loop drain grows memory without bound; there should be an explicit error instead |
| L1 | Low | No | `apps/gnostr/src/sync/neg-client.c:651`, `:806-811` | Gnostr's negentropy client has the same detach-then-free pattern and is not in nostrc-tw7f |
| L2 | Low | No | root `CMakeLists.txt:355` (nostrc-ig0z) | The GNostrRelay teardown regression test never builds on Linux CI. Fix ig0z next, but don't block this change on it |
| I1 | Info | No | tests, libgo TSAN config | TSan notes: the workspace TSAN config can't connect (ecz3); the teardown test peeks at `refcount` unlocked; a pre-existing lazy-init race in `shutdown_dbg_enabled`; the mock-server stop flag |
| I2 | Info | No | `libnostr/src/connection.c:909-911` | The close-request path reads and clears `priv->wsi` without `priv->mutex`, while writers read it under the mutex; the connect path now takes it. Pre-existing |
| I3 | Info | No | `nostr-gobject/src/nostr_pool.c` (`pool_apply_auth_handler`) | A shared relay keeps a finalized pool's auth handler: now alive rather than freed, but possibly a stale identity. Latent |
| I4 | Info | No | `nostr-gobject/CMakeLists.txt:173` | Test seams are compiled into the library whenever `BUILD_TESTING` is on (a pre-existing pattern, since 3920eb70) |

---

## 2d054eee — callback lifetimes (nostrc-flp7): correct

### The libnostr slot protocol (`libnostr/src/relay.c`)

- **The protocol.** Each registration is a `NostrRelayCallbackSlot` with an atomic refcount. The registration holds one reference.
- **Taking the invocation reference.** An invocation takes its reference in `relay_callback_slot_ref_locked()` **inside the same `priv->mutex` critical section that reads the slot pointer**: state at `relay_set_state`, `nostr_relay_wait_established`; AUTH and OK in `nostr_relay_dispatch_control_envelope`. A concurrent swap therefore cannot drop the registration reference between the read and the increment. A relaxed increment is enough there, because the mutex orders it against the swap.
- **Releasing.** `relay_callback_slot_unref()` uses acq_rel. The last reference runs `destroy` and frees the slot **outside** the mutex, since nothing calls it with the mutex held.
- **Replacing or removing.** `relay_callback_slot_swap()` exchanges the pointer under the mutex and unrefs the old slot after unlocking. Legacy setters are `_full(..., NULL)`, so their semantics are unchanged: the relay never owns the data.
- **Freeing the relay.** `relay_free_impl()` (`:691`) detaches all three slots under the mutex and unrefs them after unlocking.
- **AUTH.** It now passes the envelope's own `ch` instead of re-reading `priv->challenge` unlocked. That was a real use-after-free if a second AUTH replaced the challenge.
- **The OK setter** now takes the mutex.

**Mutation M1** confirms the tests detect a regression of exactly this protocol (verification rows 4 and 9).

### Lock ordering and deadlocks

- **`priv->mutex` is a leaf** with respect to callbacks. No slot function and no destroy runs under it, so no callback, destroy or re-entrant setter can self-deadlock on it. A callback may remove or replace itself: the swap takes the mutex, which the caller doesn't hold, and the old slot survives on the invocation's reference until the call returns.
- **GNostrRelay:**
  - `gnostr_relay_new()` holds `G_LOCK(relay_registry)` while calling `g_weak_ref_get()` and `g_object_new()`. `constructed` → `nostr_relay_new` + `_full` setters take the new relay's `priv->mutex`, so the order is registry → relay mutex.
  - Nothing takes them in the reverse order. Finalize takes the registry lock with no relay mutex held, and core callbacks run with no libnostr lock held.
  - GLib clears GWeakRefs before it runs dispose/finalize, and it does not hold its weak-ref lock across finalize. So a `g_weak_ref_get()` under the registry lock racing a last unref returns NULL or a live ref, never a finalizing object.
- **`relay_auth_handler`** is a new leaf G_LOCK.
- **The lws service thread** only gained a `priv->mutex` take in `service_loop_process_connect_request()` (`connection.c:860`), a leaf, on the thread that runs all wsi callbacks.

### Can finalize block, and where does destroy run?

**Finalize never waits.** `gnostr_relay_finalize()` detaches through the setters: a brief `priv->mutex`, no wait on in-flight calls. It removes its registry entry only if `entry->owner == self` (`:610`), and dispatches `nostr_relay_free` to a GTask thread as before.

**Finalize can run on a libnostr worker.** `on_core_state_changed` does `g_weak_ref_get()` … `g_object_unref(self)` on `message_loop` before queueing its idle. If the main thread dropped the last external ref meanwhile, finalize runs there. I traced that path and it is safe:
- The detach's swap cannot free the slot, because the worker's invocation reference is still held.
- `destroy` (`relay_callback_data_unref`: `g_weak_ref_clear` + `g_free`) runs when the callback returns, on that worker.
- The blocking join in `relay_free_impl` runs on the GTask thread and completes once `message_loop` returns from the callback and observes cancellation.

**Destroy threads by design:**
- the setter's thread;
- a libnostr worker (`message_loop`), when its call was the last reference;
- the thread calling `nostr_relay_wait_established()`;
- the GTask thread running `relay_free_impl`.

This is documented in `nostr-relay.h` ("runs on whichever thread lets go last") and in the GNostrPool and GNostrRelay headers for user-supplied `destroy`. For GNostrRelay's own data the destroy is thread-agnostic (GWeakRef clear + free), and no destroy can drop a GObject's last reference. For users of `gnostr_pool_set_{auth_handler,event_sink,cache_query}`, `destroy` may now run on a query or relay thread. That is documented, and the manifest notes that every in-tree caller passes `NULL` data.

### GNostrRelay and GNostrPool

- **Registry.** It now holds `RelayRegistryEntry { GWeakRef; owner }`, replaced (not reused) when the weak ref reads NULL. `owner` is compared, never dereferenced, and cannot alias a new relay while the old one is still inside finalize.
- **Callback data.** Each registration owns a `RelayCallbackData` reference; the relay keeps none. Finalize detaches first. The unlocked `self->relay->connection` read in finalize is gone; `relay_free_impl` closes the channels under the mutex (nostrc-ws1).
- **Sign handler.** Refcounted, read and replaced under `relay_auth_handler`; `gnostr_relay_authenticate()` holds a reference while signing.
- **Pool hooks.**
  - `PoolHook` is refcounted. Queries snapshot a reference.
  - Relays get their own reference through `pool_auth_sign_trampoline`.
  - The setters no longer destroy data that a running query or a relay still calls.
- **Results leak fix.** `query_async_data_free` now unrefs `results`. I checked that every return path to the GTask transfers ownership: `g_steal_pointer` at `nostr_pool.c:716`, and `data->results = NULL` at `:910`. There is no double free.

### L1 — Low — Gnostr's negentropy client isn't covered by nostrc-tw7f

The manifest defers "signet's relay pool, nostr-dispatcher's nd-fetch and nip46's client_start" to follow-up beads, and **nostrc-tw7f** tracks exactly those three. `apps/gnostr/src/sync/neg-client.c` has the same pattern with the legacy setters:
- `nostr_relay_set_auth_callback(relay, neg_auth_callback, &neg_ctx)` (`:651`, stack data).
- The `cleanup:` path (`:806-811`) clears the callback, then runs `neg_sessions_unregister(&neg_ctx)` and `neg_session_context_clear(&neg_ctx)` **before** `nostr_relay_free(relay)`. An AUTH already inside `neg_auth_callback` on the worker can then read a cleared context.
- `:676-677` (`set_state_callback(NULL)` then `go_channel_free(ready_ch)`) has the same shape. libgo's graveyard and magic check make that one mostly harmless.

The success path (`:791-799`) frees the relay (joining workers) before clearing, so it is fine. Recommend adding neg-client to nostrc-tw7f, or moving it to `_full`. It is pre-existing and not introduced here.

---

## fcbf8f20 — lossless subscriptions (nostrc-dha5)

**Correctness: good.**
- `lossless` is read and written under `event_queue_mutex`, and the drop loop only runs `while (!self->lossless && …)` (`:258`).
- EOSE and CLOSED items are never dropped, and the single FIFO of nostrc-qp24.10.6 is untouched.
- The monitor keeps draining libnostr's channel in lossless mode. This is the right call: `nostr_subscription_dispatch_event()` uses `go_channel_try_send()` and drops on a full channel, so pausing would move the loss into libnostr.

**Gnostr's behaviour is unchanged.** Gnostr creates subscriptions only through GNostrPool. Both `gnostr_pool_subscribe()` (`nostr_pool.c:1196`) and `multi_sub_subscribe_to_relay()` (`:1372`) select bounded before firing. No other in-tree code calls `gnostr_subscription_new()` except Groundhog's `gh-relay-gnostr.c:144`, which selects lossless explicitly.

**Versioning.** MINOR for nostr-gobject is appropriate: the 200-event drop was never documented API. Direct external users of `gnostr_subscription_new()` do change memory characteristics, and the manifest says so.

### M1 — Medium — lossless mode needs a hard cap with an explicit error

**Why memory is now unbounded.** In lossless mode the queue in front of the main loop has **no bound**. It grows at (relay ingress − main-loop drain):
- The drain is time-sliced (8 ms per tick) and pays Groundhog's per-event handler cost (`gh_relay_scope_event` into the store).
- Ingress is bounded only by the socket and by `message_loop` parsing; GNostrRelay sets `assume_valid`, so there is no signature check in the loop.
- A relay that ignores `limit` and streams events can grow the process until it is OOM-killed.
- In a messenger that talks to user-chosen DM relays and NIP-29 group relays, a hostile relay is in the threat model.

Before this change memory per subscription was bounded, and the price was silent data loss. That was the worse bug, so flipping the default is right. But nothing now turns runaway growth into a visible failure.

**Is a cap needed now?** Not as a merge blocker, for three reasons:
1. The change fixes a silent data-loss bug that hits every Groundhog backfill over 200 events.
2. Exploiting the gap needs a malicious relay the user connected to.
3. nostrc-tjrn tracks true backpressure.

It *should* land before the Groundhog release that ships 0.11.1 (currently unreleased), and it is cheap.

**Suggested design:**
- Count and byte ceilings, well above any legitimate backfill (for example 100 000 events or 64 MiB queued).
- On overflow, stop queueing and close the core subscription.
- Queue a CLOSED item with a reason such as `"error: event backlog overflow"`, so it arrives in order after the events already queued, and log a `g_warning`.
- Groundhog already handles "closed" as a relay notice and can re-page with `until`.

This is explicit failure instead of an OOM kill, and never a silent drop. I recommend a bead. nostrc-tjrn is the long-term fix, not a substitute.

---

## dbc680cd — lws wsi teardown: correct on lws 4.3.3 and 4.5.8

### Is every wsi-destroy path covered?

- **WSI_DESTROY is always delivered when a wsi is freed.** `__lws_close_free_wsi_final()` calls `wsi->a.vhost->protocols[0].callback(wsi, LWS_CALLBACK_WSI_DESTROY, …)` "with user_space still intact" (4.3.3 `close.c:986`, 4.5.8 `:1037`).
  - libnostr's vhost has a single protocol, so `protocols[0]` is `websocket_callback` (`connection.c:585-596`).
  - The opaque pointer is still set, so `conn` resolves and `conn_wsi_gone()` runs.
  - With `CLIENT_CLOSED`, `CLIENT_CONNECTION_ERROR` and `CLOSED_CLIENT_HTTP` also routed through `conn_wsi_gone()`, whichever arrives first clears `priv->wsi`, fails a pending handshake (`handshake = -1`, broadcast) and closes the recv channel.
- **The hosted crash's path is fixed.** The connect-then-hangup-before-upgrade case gets only `CLOSED_CLIENT_HTTP` + `WSI_DESTROY` (the close callback of the h1 client role, `ops-h1.c` `LWS_CALLBACK_CLOSED_CLIENT_HTTP`). Both are now handled. `nostr_relay_wait_established()` also fails promptly instead of waiting out its timeout.
- **Redirects are unaffected.** When lws closes a client wsi to follow a redirect, `close_is_redirect` suppresses the close callback (4.3.3 `close.c:774-783`) and the wsi is reused (`:915-919`). So `conn_wsi_gone()` is not triggered for a dial that is still alive.

### Is there any double close?

No.
- `conn_wsi_gone()` acts only while `priv->wsi == wsi`, under `priv->mutex`, so it is idempotent: `CLIENT_CLOSED` followed by `WSI_DESTROY`, or CCE followed by `CLOSED_CLIENT_HTTP` followed by `WSI_DESTROY`, each does the work once.
- The owner's close request (`connection.c:905-918`, service thread) detaches the opaque pointer, NULLs `priv->wsi` and calls `lws_wsi_close(…, LWS_TO_KILL_ASYNC)`. lws's later WSI_DESTROY finds `conn == NULL` and returns at the top of `websocket_callback`.
- If the peer closed first, `priv->wsi` is already NULL, so the close request never touches a freed wsi. That was the root cause, and it is closed.

### Removing `ci.pwsi` is safe

The contract, from the 4.5.8 header and both sources: `lws_client_connect_via_info()` returns NULL whenever the wsi was scrubbed during the call.
- `rops_client_bind_h1` returns −1 "the connection is already closed and freed", and `connect.c` maps that to `bail2: return NULL` (4.3.3 `:539-555`; 4.5.8 same shape).
- CONNECTION_ERROR is suppressed during the call (`client_suppress_CONNECTION_ERROR = 1`, 4.3.3 `connect.c:230`), except for blocking-DNS NXDOMAIN, which reports and then fails (`connect2.c:353`).

A callback during the call therefore finds `priv->wsi != wsi` and is a no-op, and the NULL return sends `ok = 0` to the requester, which fails the dial. A non-NULL return is recorded under the mutex on the service thread (`:860`) before any later callback can run. `ci.opaque_user_data` (`:848`) lets callbacks during the call find `conn`; that field exists in both versions.

Dropping `lws_set_timer_usecs(wsi, 0)` is correct: 0 arms a timer, and cancel is `LWS_SET_TIMER_USEC_CANCEL`. lws cancels a wsi's timers when it frees it, and the TIMER handler ignores a wsi that isn't the connection's.

**Evidence:** pre-fix 10/10 crashes, post-fix 0/20 (Linux ASan, lws 4.3.3); macOS lws 4.5.8 passes; TSan (slots on) reports 0.

### I2 — Info — `priv->wsi` accessed without the lock in the close-request path

`lws_service_loop` reads and clears `priv->wsi` without `priv->mutex` (`:909-911`, "callbacks run on this same thread"). `nostr_connection_write_message` reads it under the mutex on writer threads. That is a formal data race, pre-existing and benign on this platform. The new connect path does take the mutex, so taking it here too would make the protocol uniform.

---

## nostrc-ig0z — should it block?

**L2 — Low, not blocking.**
- The root `CMakeLists.txt:355` tests for `testing/CMakeLists.txt`, but git tracks `Testing/`. So on case-sensitive Linux `nostr_testing` and every test gated on it, including **`test_nostr_gobject_relay_teardown`**, silently drop out of the Linux gate and hosted CI.
- This change's core fix is still covered on Linux by `libnostr_relay_callback_lifetime` and `libnostr_connection_teardown_stress`, which are libnostr-level and not gated.
- The GNostrRelay-level test was verified on macOS and in a Linux container (via the case-insensitive bind mount).
- The mismatch predates this branch and hides other tests too. Holding a crash fix on it would keep a known SIGSEGV on master.

It is already P1. Land it next, and triage whatever starts running on Linux.

---

## I1 — TSan notes (all pre-existing or test-only)

- **Use `-DGO_TSAN_MPMC_SLOTS=ON` for any workspace TSan run.** With plain `-DGNOSTR_ENABLE_TSAN=ON`, libgo's single-lock ring can't carry a capacity-1 channel (nostrc-ecz3), so no relay ever connects: `connection.c:1145` `req->result` is capacity 1.
- **`test_relay_teardown_race.c:165`** `only_our_core_ref()` reads `NostrRelay.refcount` with `__atomic_load_n`, but libnostr mutates it as a plain `int` under `priv->mutex` (`relay.c` `nostr_relay_unref`). TSan reports it twice. It's harmless for the test's "becomes true and stays true" poll, but any future workspace TSan job will flag it. Read it under the relay mutex, or expose a test accessor.
- **A pre-existing lazy-init race** on `shutdown_dbg_enabled.inited` (`nostr_relay_new`), reported once by the stress test under the MPMC-off config.
- **A mock-server stop-flag race** in `nostr_testing` (`nostr_mock_server_stop` against `service_thread_func`).

## I3 — Info — a shared relay keeps a finalized pool's auth handler

Pool finalize drops only the pool's own reference. Relays shared through the URL registry keep auto-authenticating with that pool's signer until another pool replaces the handler or the relay is finalized. This used to be a dangling pointer; it is now a live, possibly stale, identity. It's latent: in-tree, only Gnostr sets a pool auth handler (`nip42_auth.c:169`, NULL data), and Groundhog never uses GNostrRelay auto-auth. Worth a sentence in the `gnostr_pool_set_auth_handler` docs, or clearing the handlers of this pool's relays in finalize when it still owns them.

## I4 — Info — test seams in the library

`GNOSTR_TESTING` is defined for the library itself whenever `BUILD_TESTING` is on (`nostr-gobject/CMakeLists.txt:173`, since 3920eb70). The new hooks therefore ship in such builds: exported `gnostr_relay_test_set_hook`, and one global G_LOCK per core callback. It's a pre-existing pattern and cheap. Packagers should build with tests off, or the seams should move to a test-only object library.

---

## Versioning

- **libnostr 1.0.11 → 1.1.0 (MINOR):** new `_full` setters and typedefs, no ABI break (private fields only).
- **nostr-gobject 2.0.2 → 2.1.0:** 2.0.3 was never released, so both changes ship in 2.1.0; MINOR for the new `lossless` API. CMake and meson agree, and nostr-gobject now requires `NOSTR >= 1.1.0`.
- **groundhog 0.11.0 → 0.11.1 (PATCH):** the behaviour fix, plus a metainfo entry.
- **gnostr and signet:** no bump, rebuild only; Gnostr's behaviour is unchanged.

All generated version strings match.

## Recommended follow-ups (none blocking)

1. **M1:** a bead for the lossless hard cap (count and bytes), with an explicit in-order CLOSED and a warning, before the Groundhog 0.11.1 release.
2. **L1:** add `apps/gnostr/src/sync/neg-client.c` to nostrc-tw7f, or move it to the `_full` setters.
3. **L2:** fix nostrc-ig0z next, so `test_nostr_gobject_relay_teardown` guards Linux.
4. **I1:** use `GO_TSAN_MPMC_SLOTS=ON` for workspace TSan (until nostrc-ecz3), and make the teardown test read `refcount` under the lock.
5. **I2:** take `priv->mutex` for `priv->wsi` in the close-request path.

## Verdict

**APPROVED.**
- The callback-lifetime protocol is sound and correctly locked. Finalize never blocks, and every destroy thread is documented and safe for in-tree users.
- The lws teardown handles every destroy path on 4.3.3 and 4.5.8, without double closes.
- Gnostr's subscription behaviour is unchanged.
- All three fixes are shown to be caught by their tests: the pre-fix build crashes 10/10, and the mutations restoring the old semantics fail with the original W20 stack and the 200/1000 loss.
- The full suite, Linux ASan in the hosted configuration and TSan with slots on show nothing attributable to this branch.
