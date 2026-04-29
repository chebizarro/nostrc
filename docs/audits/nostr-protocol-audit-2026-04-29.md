# Nostr Protocol Audit Report

**Date:** 2026-04-29
**Scope:** nostrc codebase — relay client, protocol NIPs, service integration, and test harness
**Auditor:** Automated protocol-smell analysis with manual triage

---

## Executive Summary

The nostrc codebase implements a substantial portion of the Nostr protocol stack in C, including relay connections, subscription management, NIP-42 authentication, NIP-46 remote signing, NIP-47 wallet connect, NIP-65 relay lists, NIP-11 relay metadata, and negentropy-based sync. The architecture shows genuine protocol awareness in several areas — notably the filter builder, EventBus patterns, and negentropy cleanup discipline.

However, this audit identified **47 protocol smells** across 10 categories, including **2 critical**, **13 high**, **24 medium**, and **8 low** severity findings. The issues cluster around several architectural themes:

### Top 5 Highest-Risk Smells

1. **F01 — Optimized relay path silently drops all control frames** (Critical): The `relay_optimized.c` fast path discards `OK`, `CLOSED`, `AUTH`, and `COUNT` frames, breaking protocol semantics for any code path using the optimized relay.
2. **F02 — NIP-42 auth event is structurally invalid** (Critical): The `nip42_Event` struct omits the `sig` field while validation code dereferences it, making NIP-42 authentication fundamentally broken.
3. **F04 — Publish acknowledgement semantics are wrong** (High): Publish reports success at transport level only, `ok_callbacks` infrastructure is dead code, and auth marks `authenticated=TRUE` before receiving `OK`.
4. **F07 — NIP-42 is stubbed end-to-end** (High): No challenge parser, no signing path, no relay-client integration — authentication cannot function.
5. **F14 — Relay negative paths are untested** (High): The fake relay never emits `AUTH` or `OK=false`, mocks bypass the real relay, and "E2E" tests aren't actually end-to-end.

### Architectural Themes

- **Control frame blindness:** Multiple code paths ignore or discard `OK`, `CLOSED`, `AUTH`, and `COUNT` relay responses, undermining the bidirectional protocol contract.
- **Polling over event-driven patterns:** Several components use timer loops (1ms, 200ms, 300ms) where existing callback/channel infrastructure could be used instead.
- **Auth is structurally broken:** NIP-42 has no working end-to-end path; NIP-47 requests are unsigned; auth nonces are predictable; negentropy discards AUTH frames.
- **Test harness doesn't model the protocol:** The fake relay fixture omits AUTH, OK=false, CLOSED reasons, and accurate limit handling, so protocol-level bugs ship undetected.
- **Legacy request/response patterns:** Several modules treat Nostr relays as HTTP endpoints — one connection per call, hardcoded timeouts, tear-down after first event.

---

## Findings

Findings are grouped by severity (Critical → Low). Each finding is tagged with one of the official smell categories A–J.

---

### Critical

---

### [F01] Optimized relay path drops all non-EVENT control frames

- **Severity:** Critical
- **Confidence:** High
- **Category:** C — Ignoring relay response frames
- **Location:** `relay_optimized.c` → `process_envelope()` ~L200–212

- **Smell:**
  The optimized relay message processing path in `process_envelope()` only handles `EVENT` frames. All other relay response types — `OK`, `CLOSED`, `AUTH`, `COUNT`, and `NOTICE` — are silently discarded.

- **Why this is a Nostr protocol smell:**
  The Nostr relay protocol is bidirectional. Relays communicate publish success/failure via `OK`, subscription termination via `CLOSED`, authentication challenges via `AUTH`, and aggregate queries via `COUNT`. Dropping these frames means the client has no feedback channel from the relay on the optimized path.

- **Likely runtime consequence:**
  - Publishes silently fail with no error propagation.
  - Authentication challenges are never received, so auth-required relays appear to work but reject all writes.
  - `CLOSED` reasons (rate limits, policy violations) are lost.
  - `COUNT` responses never arrive, causing `nostr_relay_count()` to block forever on this path.

- **Recommended fix:**
  Route all frame types through a shared dispatch function used by both `relay.c` and `relay_optimized.c`. The optimized path should only optimize parsing/allocation, not skip protocol semantics.

- **Minimal example of better shape:**
  ```c
  // In process_envelope():
  switch (frame_type) {
      case FRAME_EVENT:  dispatch_event(sub_id, event); break;
      case FRAME_OK:     dispatch_ok(event_id, success, message); break;
      case FRAME_CLOSED: dispatch_closed(sub_id, reason); break;
      case FRAME_AUTH:   dispatch_auth(challenge); break;
      case FRAME_COUNT:  dispatch_count(sub_id, count); break;
      case FRAME_NOTICE: dispatch_notice(message); break;
  }
  ```

---

### [F02] NIP-42 auth event struct is structurally invalid — missing `sig` field

- **Severity:** Critical
- **Confidence:** High
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `nip42.h` / `nip42.c`

- **Smell:**
  The `nip42_Event` structure definition omits the `sig` field entirely. However, validation code in `nip42.c` dereferences `event->sig` for signature verification.

- **Why this is a Nostr protocol smell:**
  NIP-42 AUTH events are standard Nostr events (kind 22242) that must be signed. Without a `sig` field in the struct, the auth event cannot carry a valid signature, and validation code that dereferences the missing field invokes undefined behavior.

- **Likely runtime consequence:**
  - NIP-42 authentication is fundamentally non-functional.
  - Validation reads from an incorrect memory offset, potentially accepting invalid events or crashing.
  - Any relay requiring NIP-42 auth will reject connections from this client.

- **Recommended fix:**
  Add `sig` to the `nip42_Event` struct (or use the standard `nostr_event` type). Wire the signing path so AUTH events are properly signed before transmission.

- **Minimal example of better shape:**
  ```c
  typedef struct {
      // ... existing fields ...
      char sig[128];  // hex-encoded schnorr signature
  } nip42_Event;
  ```

---

### High

---

### [F03] `nostr_relay_count()` blocks forever when relay doesn't support COUNT

- **Severity:** High
- **Confidence:** High
- **Category:** B — Timeout-based close instead of protocol-driven close
- **Location:** `relay.c` → `nostr_relay_count()` ~L1607

- **Smell:**
  `nostr_relay_count()` calls `go_channel_receive()` with no timeout. If the connected relay does not implement NIP-45 `COUNT`, no response frame ever arrives.

- **Why this is a Nostr protocol smell:**
  NIP-45 is optional. A correct client must handle the case where a relay simply ignores the `COUNT` request or responds with `NOTICE`. Blocking indefinitely violates the principle that protocol interactions should be bounded.

- **Likely runtime consequence:**
  The calling thread hangs permanently when connected to any relay that does not support NIP-45. This can deadlock higher-level operations that depend on count results.

- **Recommended fix:**
  Use `go_select` with a timeout channel, or detect the relay's NIP-45 support via NIP-11 before issuing COUNT. On timeout or unsupported relay, return an error or sentinel value.

- **Minimal example of better shape:**
  ```c
  go_channel_t timeout_ch = go_after(5000); // 5s
  int which = go_select(count_ch, timeout_ch, NULL);
  if (which == 1) return NOSTR_ERR_UNSUPPORTED;
  ```

---

### [F04] Publish reports transport-level success only; `ok_callbacks` is dead code; auth state set prematurely

- **Severity:** High
- **Confidence:** High
- **Category:** C — Ignoring relay response frames
- **Location:** `relay.c` → `nostr_relay_publish()` ~L1347–1372; `gnostr_relay_authenticate()`

- **Smell:**
  `nostr_relay_publish()` returns success as soon as the event is written to the WebSocket. The `ok_callbacks` hash table exists but is never populated or invoked. Separately, `gnostr_relay_authenticate()` sets `authenticated=TRUE` immediately after sending the AUTH event, before the relay's `OK` response confirms acceptance.

- **Why this is a Nostr protocol smell:**
  The Nostr protocol requires clients to check `OK` responses to know whether a publish was accepted or rejected (and why — duplicate, blocked, rate-limited, invalid). Ignoring `OK` means the client cannot distinguish successful publishes from silent failures.

- **Likely runtime consequence:**
  - Events silently rejected by relays appear as published to the application.
  - Auth failures are invisible — the client believes it is authenticated when the relay rejected the AUTH event.
  - No retry logic can be built because failure is never detected.

- **Recommended fix:**
  Register a pending-publish record keyed by event ID before sending. When `OK` arrives, resolve the pending record with the success/failure status. For auth, transition to `authenticated` only on `OK true`.

- **Minimal example of better shape:**
  ```c
  // On publish:
  pending_publishes[event_id] = callback;
  ws_send(relay, event_json);

  // On OK frame:
  if (pending_publishes[event_id]) {
      pending_publishes[event_id](success, message);
      remove(pending_publishes, event_id);
  }
  ```

---

### [F05] Negentropy `NEG-OPEN` omits `authors` filter, causing global-scope sync

- **Severity:** High
- **Confidence:** High
- **Category:** E — Weak filter design
- **Location:** `neg-client.c` Phase 4 ~L490–506

- **Smell:**
  The negentropy sync `NEG-OPEN` request constructs a filter with `kinds` and `since`/`until` but omits the `authors` tag. This causes the relay to scan its entire database for matching kinds rather than scoping to the intended author set.

- **Why this is a Nostr protocol smell:**
  Nostr filters support `authors` precisely to scope queries. Omitting it for replaceable-event sync (where the author is known) forces the relay to return a superset of events, wasting bandwidth and relay resources.

- **Likely runtime consequence:**
  - Sync fetches events from all authors on the relay, not just the target.
  - Relay may rate-limit or reject overly broad queries.
  - Bandwidth and processing costs scale with total relay size rather than user's event set.

- **Recommended fix:**
  Include the target author pubkey(s) in the `NEG-OPEN` filter's `authors` array.

---

### [F06] Negentropy client discards `AUTH` frames — auth-required relays silently fail

- **Severity:** High
- **Confidence:** High
- **Category:** C — Ignoring relay response frames
- **Location:** `neg-client.c` → `neg_handler` ~L176–213

- **Smell:**
  The negentropy message handler (`neg_handler`) processes `NEG-MSG`, `NEG-ERR`, and `EVENT` frames but does not handle `AUTH`. Any `AUTH` challenge from the relay is silently dropped.

- **Why this is a Nostr protocol smell:**
  NIP-42 `AUTH` challenges can arrive at any time during a connection, including during negentropy sync. A client that ignores them will fail to authenticate and have its sync requests rejected without any error surfacing.

- **Likely runtime consequence:**
  Negentropy sync silently produces empty or incomplete results against auth-required relays. No error is reported to the caller.

- **Recommended fix:**
  Add `AUTH` frame handling in `neg_handler`. Either perform the auth handshake inline or surface the auth requirement to the caller for handling.

---

### [F07] NIP-42 implementation is stubbed — no working end-to-end auth path

- **Severity:** High
- **Confidence:** High
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `nip42/*`

- **Smell:**
  The NIP-42 module has struct definitions and partial validation code but lacks: a challenge string parser, a signing path that produces valid kind-22242 events, and integration with the relay client connection lifecycle.

- **Why this is a Nostr protocol smell:**
  NIP-42 is the standard Nostr authentication mechanism. Without a working implementation, the client cannot authenticate to any relay that requires it, which is an increasing number as the protocol matures.

- **Likely runtime consequence:**
  - All auth-required relays are inaccessible.
  - Features gated behind auth (restricted writes, paid relays, private content) silently fail.
  - The partial implementation creates a false sense of auth support.

- **Recommended fix:**
  Implement the full NIP-42 flow: parse `AUTH` challenge → construct kind 22242 event with relay URL and challenge tags → sign → send → await `OK`. Integrate this into the relay connection state machine.

---

### [F08] `relay_fetch.c` uses static global channel pointers — concurrent fetches are unsafe

- **Severity:** High
- **Confidence:** High
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `relay_fetch.c` → `g_manifest_ch`, `g_secrets_ch`, `g_relays_ch` globals

- **Smell:**
  Fetch helpers use static global `go_channel_t` pointers for result delivery. Multiple concurrent calls to any fetch function will race on the same channel, corrupting results.

- **Why this is a Nostr protocol smell:**
  Nostr clients commonly connect to multiple relays concurrently. Using global state for what should be per-request context reimplements a single-threaded request/response pattern over what is inherently a concurrent subscription-based protocol.

- **Likely runtime consequence:**
  - Concurrent fetches produce interleaved or lost results.
  - Race conditions may cause use-after-free if one fetch completes while another is in progress.

- **Recommended fix:**
  Allocate a per-request context struct containing the result channel. Pass it through the subscription callback's user_data pointer.

---

### [F09] Relay auth challenge nonce is predictable — uses `time(NULL)` instead of CSPRNG

- **Severity:** High
- **Confidence:** High
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `relayd_main.c` ~L196–200

- **Smell:**
  The relay server generates auth challenge nonces using `time(NULL)` (seconds since epoch). A dedicated `gen_nonce()` function exists in the codebase but is not called.

- **Why this is a Nostr protocol smell:**
  NIP-42 challenge strings must be unpredictable to prevent replay attacks. A timestamp-based nonce is trivially guessable — an attacker can precompute valid auth responses.

- **Likely runtime consequence:**
  Auth challenges can be replayed or predicted, allowing unauthorized clients to authenticate to the relay server.

- **Recommended fix:**
  Replace `time(NULL)` with the existing `gen_nonce()` function or use a CSPRNG (e.g., `getrandom()`, `/dev/urandom`).

---

### [F10] `nostr_relay_store.c` performs undefined-behavior cast from GObject interface to C vtable

- **Severity:** High
- **Confidence:** Medium
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `nostr_relay_store.c` ~L41–43

- **Smell:**
  The relay store implementation casts a GObject interface bridge to a plain C vtable struct with an incompatible memory layout. This is undefined behavior under C aliasing rules.

- **Why this is a Nostr protocol smell:**
  This creates an unreliable foundation for relay-backed data stores. The pattern reimplements OOP dispatch incorrectly instead of using the existing GObject interface mechanism properly.

- **Likely runtime consequence:**
  - Vtable calls may invoke wrong functions or crash depending on compiler optimization and struct layout.
  - Bugs manifest unpredictably across compiler versions and platforms.

- **Recommended fix:**
  Use GObject's `G_DEFINE_INTERFACE` / `g_type_interface_peek` properly, or use a clean C function-pointer struct with explicit initialization.

---

### [F11] NIP-47 client cannot receive wallet responses — no subscription layer

- **Severity:** High
- **Confidence:** High
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `nwc_client.c` + `nwc_envelope.c`

- **Smell:**
  The NIP-47 (Nostr Wallet Connect) client can construct and send request events but has no mechanism to subscribe to the relay for response events. The response-receiving side is simply missing.

- **Why this is a Nostr protocol smell:**
  NIP-47 is a request/response protocol layered over Nostr subscriptions. The client must hold a subscription filtering for kind 23195 responses tagged with the request event ID. Without this, NIP-47 is send-only.

- **Likely runtime consequence:**
  All NIP-47 wallet operations (pay invoice, get balance, etc.) send the request but never receive the response. The feature is non-functional.

- **Recommended fix:**
  Add a relay subscription for kind 23195 events filtered by `p` tag (client pubkey) and `e` tag (request event ID). Use the existing subscription infrastructure to receive and dispatch responses.

---

### [F12] NIP-47 requests are built unsigned — relays will reject them

- **Severity:** High
- **Confidence:** High
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `nwc_envelope.c` → `nostr_nwc_request_build`

- **Smell:**
  `nostr_nwc_request_build` constructs NIP-47 request events (kind 23194) but does not sign them. The events have no valid `sig` field.

- **Why this is a Nostr protocol smell:**
  All Nostr events must be signed. Relays validate signatures on ingestion and reject unsigned events. NIP-47 requests are standard Nostr events with encrypted content — they require valid signatures like any other event.

- **Likely runtime consequence:**
  Every NIP-47 request is rejected by every relay. Combined with F11, NIP-47 is completely non-functional.

- **Recommended fix:**
  Sign the event after construction using the client's private key. Ensure `id` is computed from the canonical serialization before signing.

---

### [F13] `gnostr-thread-subscription` declares `SIGNAL_EOSE` but never emits it

- **Severity:** High
- **Confidence:** High
- **Category:** D — Missing EOSE-aware backfill logic
- **Location:** `gnostr-thread-subscription.c` → `class_init`

- **Smell:**
  The thread subscription GObject class registers an `eose` signal in `class_init` but no code path ever calls `g_signal_emit()` for that signal.

- **Why this is a Nostr protocol smell:**
  EOSE (End of Stored Events) is the protocol's mechanism for distinguishing historical backfill from live events. Declaring but never emitting the signal means consumers cannot know when backfill is complete.

- **Likely runtime consequence:**
  - UI code waiting for EOSE to show a "loading complete" state will wait forever.
  - Consumers cannot distinguish between "still loading" and "no more historical events."
  - Any backfill-then-live processing pipeline stalls.

- **Recommended fix:**
  Emit `SIGNAL_EOSE` when the underlying subscription receives EOSE from the relay. Wire the relay subscription's EOSE callback to `g_signal_emit(self, signals[SIGNAL_EOSE], 0)`.

---

### [F14] Relay-level negative protocol paths are effectively untested

- **Severity:** High
- **Confidence:** High
- **Category:** J — Test smells
- **Location:** `test_nip46_e2e.c`; `test_rpc_flow_mock.c`; `fake_relay_fixture.py`

- **Smell:**
  The fake relay fixture never emits `AUTH` challenges or `OK=false` responses. Mock-based tests bypass the real relay message loop entirely. The NIP-46 "E2E" test uses mocks and never exercises the actual relay protocol path.

- **Why this is a Nostr protocol smell:**
  Relay negative responses (`OK false`, `AUTH`, `CLOSED` with reason) are critical protocol paths. If tests never exercise them, bugs like F01 (dropped control frames) and F04 (ignored OK) ship undetected.

- **Likely runtime consequence:**
  Protocol-breaking bugs pass CI. Regressions in frame handling are never caught until production.

- **Recommended fix:**
  Extend `fake_relay_fixture.py` to support configurable `AUTH` challenges, `OK false` responses, and `CLOSED` with reasons. Convert mock-based "E2E" tests to use the fake relay with full protocol semantics.

---

### [F15] Note-counts regression test is permanently skipped despite known defect

- **Severity:** High
- **Confidence:** High
- **Category:** J — Test smells
- **Location:** `test_event_flow_ingest_subscribe.c` L214

- **Smell:**
  A test for note counts is unconditionally skipped (`g_test_skip`) with a comment referencing a known write/read defect. The defect remains unfixed and unguarded.

- **Why this is a Nostr protocol smell:**
  Skipping a test for a known bug removes the regression guard. The bug is now invisible to CI and can worsen without detection.

- **Likely runtime consequence:**
  The note-count defect persists indefinitely. Related code changes may deepen the bug with no CI signal.

- **Recommended fix:**
  Either fix the underlying defect or convert the skip to an expected-failure test that tracks the known behavior and alerts if it changes.

---

### Medium

---

### [F16] `reconnect_now()` is a no-op — full backoff timer still elapses

- **Severity:** Medium
- **Confidence:** High
- **Category:** I — Misuse of heartbeats and timers
- **Location:** `relay.c` → `message_loop()` + `nostr_relay_reconnect_now()` ~L1828–1838

- **Smell:**
  `nostr_relay_reconnect_now()` sets a flag, but the reconnect path in `message_loop()` still waits for the full backoff duration regardless of the flag. The "immediate reconnect" request has no effect.

- **Why this is a Nostr protocol smell:**
  Timely reconnection is essential for subscription continuity. A reconnect-now API that doesn't actually reconnect immediately creates a false contract — callers believe they've triggered recovery, but the delay persists.

- **Likely runtime consequence:**
  After detecting a relay disconnect, recovery takes the full backoff period (potentially seconds to minutes) even when immediate reconnection is requested.

- **Recommended fix:**
  Have `reconnect_now()` signal the sleep/wait in the message loop (e.g., write to a wake-up pipe or channel) so the backoff is interrupted immediately.

---

### [F17] Subscription monitor uses 1ms polling loop instead of blocking wait

- **Severity:** Medium
- **Confidence:** High
- **Category:** A — Polling instead of subscriptions
- **Location:** `nostr_subscription.c` → `subscription_monitor_thread()` L307–327

- **Smell:**
  `subscription_monitor_thread()` runs a tight loop with a 1ms sleep, polling for subscription state changes instead of blocking on a condition variable or channel.

- **Why this is a Nostr protocol smell:**
  Polling for state changes at 1ms intervals is the antithesis of the event-driven subscription model. The channel/callback infrastructure already exists in the codebase.

- **Likely runtime consequence:**
  Unnecessary CPU consumption. On embedded or battery-constrained devices, this is a significant power drain. With many active subscriptions, the overhead multiplies.

- **Recommended fix:**
  Replace the polling loop with `go_channel_receive()` or a condition variable wait on subscription state changes.

---

### [F18] Simplepool dedup is disabled by default and uses O(n) scan

- **Severity:** Medium
- **Confidence:** High
- **Category:** F — Missing idempotency and deduplication
- **Location:** `simplepool.c` → `pool_seen()` + `nostr_simple_pool_subscribe()`

- **Smell:**
  Event deduplication in the simple pool is off by default (must be explicitly enabled). When enabled, the seen-set check is O(n) linear scan up to ~65K entries with no hash-based lookup.

- **Why this is a Nostr protocol smell:**
  Nostr clients connected to multiple relays will receive the same event from multiple sources. Deduplication should be on by default and efficient.

- **Likely runtime consequence:**
  - Without dedup: duplicate events delivered to application, causing duplicate UI entries, duplicate side effects.
  - With dedup enabled: performance degrades linearly as the seen set grows, eventually becoming a bottleneck.

- **Recommended fix:**
  Enable dedup by default. Replace the linear scan with a hash set keyed by event ID. Consider a bounded LRU or time-windowed set to prevent unbounded growth.

---

### [F19] `CLOSED` reasons can be dropped when channel is full — uses `try_send`

- **Severity:** Medium
- **Confidence:** High
- **Category:** C — Ignoring relay response frames
- **Location:** `subscription.c` → `nostr_subscription_dispatch_closed()`

- **Smell:**
  `dispatch_closed()` uses a non-blocking `try_send` to deliver CLOSED reasons to the subscription's channel. If the channel buffer is full, the CLOSED reason is silently discarded.

- **Why this is a Nostr protocol smell:**
  `CLOSED` frames carry the reason a relay terminated a subscription (rate limit, policy, auth required, etc.). Dropping this information prevents the client from understanding why a subscription was terminated and taking corrective action.

- **Likely runtime consequence:**
  Under high event volume (when channels are likely full), subscription termination reasons are lost. The client may attempt to resubscribe without addressing the underlying issue.

- **Recommended fix:**
  Use a dedicated, separate channel for control frames (`CLOSED`, `EOSE`) that is not shared with the event delivery channel, or use a blocking send for control frames.

---

### [F20] Re-REQ on reconnect reuses original `since` — causes event re-delivery

- **Severity:** Medium
- **Confidence:** High
- **Category:** D — Missing EOSE-aware backfill logic
- **Location:** `relay.c` → `relay_refire_subscriptions()`

- **Smell:**
  When a relay connection is re-established, `relay_refire_subscriptions()` re-sends the original subscription filters with the original `since` timestamp. Events received between the original subscription and the reconnect are re-delivered.

- **Why this is a Nostr protocol smell:**
  Proper reconnect-aware backfill should update the `since` filter to the timestamp of the most recently received event, avoiding re-delivery of already-processed events.

- **Likely runtime consequence:**
  - Duplicate events after every reconnect.
  - Side effects triggered by event processing (notifications, state updates) fire again.
  - Combined with F18 (dedup off by default), duplicates propagate to the application.

- **Recommended fix:**
  Track the latest `created_at` timestamp per subscription. On reconnect, update the `since` filter to this timestamp before re-sending.

---

### [F21] `subscription_destroy()` double-closes the events channel

- **Severity:** Medium
- **Confidence:** Medium
- **Category:** F — Missing idempotency and deduplication
- **Location:** `subscription.c` → `subscription_destroy()`

- **Smell:**
  The subscription destruction path closes the events channel, but the channel may already have been closed by an earlier cleanup path. The code relies on undocumented idempotency of `go_channel_close()`.

- **Why this is a Nostr protocol smell:**
  Cleanup must be idempotent. If the channel close is not actually idempotent (or becomes non-idempotent in a future library update), this causes use-after-free or double-free.

- **Likely runtime consequence:**
  Potential crash or memory corruption if the channel library's close semantics change or if the close races with another thread.

- **Recommended fix:**
  Track channel state with a boolean flag or use `g_once` / atomic compare-and-swap to ensure single close.

---

### [F22] `nip46_rpc_call()` stores timeout but blocks forever on receive

- **Severity:** Medium
- **Confidence:** High
- **Category:** B — Timeout-based close instead of protocol-driven close
- **Location:** `nip46_session.c` ~L1046–1056

- **Smell:**
  The RPC call function accepts and stores a `timeout_ms` parameter but then calls `go_channel_receive()` with no timeout. The timeout infrastructure exists but is not wired.

- **Why this is a Nostr protocol smell:**
  NIP-46 RPC calls depend on a remote signer responding via a relay. If the signer is offline or the relay drops the response, the client blocks forever. The timeout parameter's existence creates a false sense of bounded execution.

- **Likely runtime consequence:**
  RPC calls to unavailable signers hang the calling thread indefinitely.

- **Recommended fix:**
  Use `go_select` with a timeout channel derived from `timeout_ms`, returning an error on timeout.

---

### [F23] NIP-11 capability data exists but is never consulted before relay operations

- **Severity:** Medium
- **Confidence:** Medium
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `neg-client.c` + `nip11/*`

- **Smell:**
  The codebase has a complete NIP-11 parser and limitation struct, but no code path queries NIP-11 before initiating negentropy sync, COUNT requests, or other optional-NIP operations.

- **Why this is a Nostr protocol smell:**
  NIP-11 exists so clients can discover relay capabilities before making requests. Ignoring it means the client blindly sends requests that the relay may not support, leading to silent failures (e.g., F03).

- **Likely runtime consequence:**
  Wasted requests to relays that don't support the required NIPs. Silent failures or hangs when expected responses never arrive.

- **Recommended fix:**
  Query NIP-11 on first connect. Cache the result. Check capabilities before issuing COUNT, NEG-OPEN, or other optional-NIP requests.

---

### [F24] Negentropy session state uses global mutex — prevents concurrent sync sessions

- **Severity:** Medium
- **Confidence:** High
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `neg-client.c` ~L167–184

- **Smell:**
  All negentropy session state is protected by a single global mutex. Only one negentropy sync session can run at a time, regardless of how many relays or sync targets exist.

- **Why this is a Nostr protocol smell:**
  Nostr clients typically sync across multiple relays concurrently. Serializing all sync through a single mutex recreates a single-threaded queue pattern over what should be independent concurrent operations.

- **Likely runtime consequence:**
  Sync performance is artificially limited. Multi-relay sync runs sequentially instead of in parallel.

- **Recommended fix:**
  Move session state into a per-session struct. Use per-session locking or lock-free design.

---

### [F25] NIP-46 session polls relay connection state every 200ms

- **Severity:** Medium
- **Confidence:** High
- **Category:** A — Polling instead of subscriptions
- **Location:** `nip46_session.c` → `connect_monitor_thread` ~L619–641

- **Smell:**
  The NIP-46 session monitor thread polls the relay connection state in a 200ms loop, despite the relay having state-change callbacks available.

- **Why this is a Nostr protocol smell:**
  Polling for state changes when callbacks exist is an anti-pattern that wastes CPU and introduces latency (up to 200ms delay in detecting state changes).

- **Likely runtime consequence:**
  Up to 200ms delay in detecting relay disconnection or reconnection. Unnecessary CPU wake-ups.

- **Recommended fix:**
  Register a callback on relay state changes. Use a condition variable or channel to wake the monitor thread only when state actually changes.

---

### [F26] Error detection by string-sniffing instead of typed errors

- **Severity:** Medium
- **Confidence:** High
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `gnostr-filter-set-sync.c` ~L475–485

- **Smell:**
  Error handling checks for specific substrings in error message strings rather than using typed `GError` codes or domains.

- **Why this is a Nostr protocol smell:**
  String-based error detection is fragile and locale-dependent. It reimplements error categorization that the type system (GError domains and codes) already provides, and it will break silently if error message wording changes.

- **Likely runtime consequence:**
  Error handling silently breaks when message strings change. Internationalization or logging changes can alter error detection behavior.

- **Recommended fix:**
  Define proper GError domains and codes for relay/subscription errors. Match on error codes, not message strings.

---

### [F27] `relay_fetch.c` treats Nostr as HTTP — new connection per call, hardcoded 3s timeout, tear-down after first event

- **Severity:** Medium
- **Confidence:** High
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `relay_fetch.c` — all `nh_fetch_*` helpers

- **Smell:**
  Each fetch helper creates a new pool, opens a new relay connection, subscribes, waits for the first event (with a hardcoded 3-second timeout), then tears everything down.

- **Why this is a Nostr protocol smell:**
  This converts the persistent subscription model into an HTTP-style request/response pattern. Connection setup and teardown is expensive (WebSocket handshake, potential AUTH). The relay's EOSE signal is ignored in favor of the arbitrary timeout.

- **Likely runtime consequence:**
  - High latency due to repeated connection setup.
  - Missed events if they arrive after the timeout but before EOSE.
  - Unnecessary relay load from repeated connections.
  - Cannot benefit from connection pooling or persistent subscriptions.

- **Recommended fix:**
  Use a shared long-lived pool. Subscribe, wait for EOSE, collect results, then close the subscription (not the connection). Use EOSE as the completion signal instead of a timer.

---

### [F28] Relay server uses polling ticks as a publish→subscribe race workaround

- **Severity:** Medium
- **Confidence:** Medium
- **Category:** A — Polling instead of subscriptions
- **Location:** `grelay_main.c` ~L349–357

- **Smell:**
  The relay server uses periodic polling ticks to check for new events that should be delivered to subscribers, rather than deterministic fan-out from the `put_event` path.

- **Why this is a Nostr protocol smell:**
  When an event is stored, matching subscriptions should be notified immediately via a direct dispatch (fan-out). Using a poll loop introduces unnecessary latency and creates a race between publish and subscribe.

- **Likely runtime consequence:**
  Events are delivered to subscribers with up to one tick period of latency. Under high load, the polling overhead adds up.

- **Recommended fix:**
  On `put_event`, iterate active subscriptions, check filter match, and dispatch to matching subscribers immediately.

---

### [F29] Relay server defaults `max_subs` to 1

- **Severity:** Medium
- **Confidence:** High
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `grelay_main.c` → `max_subs`

- **Smell:**
  The default maximum subscriptions per client is set to 1. Typical Nostr clients open multiple concurrent subscriptions (timeline, notifications, DMs, profile, etc.).

- **Why this is a Nostr protocol smell:**
  A relay that only allows 1 subscription per client is incompatible with standard Nostr client behavior. This forces clients to serialize their queries, breaking the concurrent subscription model that the protocol is designed around.

- **Likely runtime consequence:**
  Most clients will fail or degrade severely when connecting to this relay. `CLOSED` frames will be sent for excess subscriptions, which the client may not handle (see F01, F19).

- **Recommended fix:**
  Default `max_subs` to at least 20 (a common relay default). Make it configurable via NIP-11 limitations.

---

### [F30] Replay dedup TTL defaults to 0 — deduplication is effectively disabled

- **Severity:** Medium
- **Confidence:** High
- **Category:** F — Missing idempotency and deduplication
- **Location:** `protocol_nip01.c` ~L30

- **Smell:**
  The replay deduplication TTL defaults to 0 seconds, which means the dedup window is empty and every event is treated as new.

- **Why this is a Nostr protocol smell:**
  Nostr events are identified by their `id` hash. Relays should deduplicate events within a reasonable window to prevent replay and unnecessary storage writes.

- **Likely runtime consequence:**
  The same event submitted multiple times is stored and broadcast each time. This wastes storage, bandwidth, and can cause duplicate delivery to subscribers.

- **Recommended fix:**
  Default to a reasonable TTL (e.g., 3600 seconds). Use a hash set with TTL-based expiry.

---

### [F31] `policy_decider.c` duplicates ring-buffer state and API symbols from `protocol_nip01.c`

- **Severity:** Medium
- **Confidence:** Medium
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `policy_decider.c`

- **Smell:**
  `policy_decider.c` re-implements ring-buffer state tracking and exports symbols that duplicate those in `protocol_nip01.c`, creating a One Definition Rule (ODR) violation risk at link time.

- **Why this is a Nostr protocol smell:**
  Duplicated protocol state tracking means the two modules can disagree about event history, leading to inconsistent policy decisions.

- **Likely runtime consequence:**
  Linker may resolve to either copy of duplicate symbols, causing inconsistent behavior. The two ring buffers track different subsets of events.

- **Recommended fix:**
  Extract the shared ring buffer into a common module. Have both `protocol_nip01.c` and `policy_decider.c` use the shared implementation.

---

### [F32] `nostr_relay_store.h` exposes only blocking `query_sync` — encourages main-loop stalls

- **Severity:** Medium
- **Confidence:** Medium
- **Category:** G — Recreating legacy queue/WebSocket patterns
- **Location:** `nostr_relay_store.h`

- **Smell:**
  The relay store interface only provides a synchronous `query_sync` method. There is no async variant. Callers on the GLib main loop must block the UI thread to query the relay store.

- **Why this is a Nostr protocol smell:**
  Relay-backed stores involve network I/O. A synchronous-only API forces callers into a blocking pattern that is incompatible with event-loop-based applications.

- **Likely runtime consequence:**
  UI freezes or main-loop stalls when querying relay-backed stores. Application responsiveness degrades.

- **Recommended fix:**
  Add an async variant using GLib's `GTask` / `g_task_run_in_thread` pattern, or provide a callback-based API.

---

### [F33] NIP-17 pool initialization is not thread-safe

- **Severity:** Medium
- **Confidence:** Medium
- **Category:** F — Missing idempotency and deduplication
- **Location:** `gnostr-relays.c` ~L570

- **Smell:**
  The NIP-17 (gift-wrapped DM) pool is lazily initialized without synchronization. Multiple threads can race to initialize the pool, potentially creating duplicates or corrupting state.

- **Why this is a Nostr protocol smell:**
  DM delivery requires a reliable pool connection. A race in pool initialization can lead to split-brain behavior where different threads use different pool instances.

- **Likely runtime consequence:**
  Race condition on first DM send from multiple threads. Potential double-initialization, leaked resources, or inconsistent relay connections.

- **Recommended fix:**
  Use `g_once_init_enter` / `g_once_init_leave` or `pthread_once` for thread-safe lazy initialization.

---

### [F34] Thread subscription filter only matches root ID — misses monitored IDs

- **Severity:** Medium
- **Confidence:** High
- **Category:** E — Weak filter design
- **Location:** `gnostr-thread-subscription.c` ~L402

- **Smell:**
  The thread subscription constructs a relay filter that only matches the root event ID, ignoring the `monitored_ids` set. Events that are replies to monitored sub-threads are not fetched.

- **Why this is a Nostr protocol smell:**
  Nostr thread trees use `e` tag references. A thread subscription that only watches the root misses branches and deep replies, providing an incomplete view of the conversation.

- **Likely runtime consequence:**
  Thread views are incomplete — replies to non-root events in the thread are not displayed.

- **Recommended fix:**
  Include all `monitored_ids` in the filter's `#e` tag array. Update the filter when new reply targets are discovered.

---

### [F35] Tests use sleep-based timing and weak assertions — masks partial delivery

- **Severity:** Medium
- **Confidence:** High
- **Category:** J — Test smells
- **Location:** `test_event_flow_ingest_subscribe.c` L156, L195, L67

- **Smell:**
  Tests use `g_usleep()` to wait for async operations and then assert on counts that may reflect partial delivery. Pass/fail depends on timing rather than protocol semantics.

- **Why this is a Nostr protocol smell:**
  Sleep-based tests conflate "arrived within timeout" with "protocol behavior is correct." They produce false passes on fast machines and false failures on slow CI.

- **Likely runtime consequence:**
  Flaky tests that occasionally fail in CI. Real bugs masked by generous sleeps.

- **Recommended fix:**
  Use condition variables, semaphores, or channel receives with bounded timeouts as deterministic completion signals.

---

### [F36] Missing error-path test coverage for publish, sync, and timeout scenarios

- **Severity:** Medium
- **Confidence:** High
- **Category:** J — Test smells
- **Location:** `test_sync_bridge_mute_reload.c`; `test_filter_set_sync.c`; `test_nip46_e2e.c`; `test_startup_live_eose.c`

- **Smell:**
  No tests cover: publish callback completion on relay rejection, network error during sync, relay timeout/no-EOSE behavior, or unreachable relay handling.

- **Why this is a Nostr protocol smell:**
  These are the most common failure modes in production Nostr usage. Without test coverage, error handling code (if it exists) cannot be verified.

- **Likely runtime consequence:**
  Error paths may crash, hang, or silently corrupt state in production with no CI guard.

- **Recommended fix:**
  Add test cases for: relay returns `OK false`, relay disconnects mid-sync, relay never sends EOSE, relay is unreachable. Use the fake relay fixture with configurable failure modes.

---

### [F37] Fake relay fixture has incorrect protocol semantics

- **Severity:** Medium
- **Confidence:** High
- **Category:** J — Test smells
- **Location:** `fake_relay_fixture.py` L91, L102

- **Smell:**
  The fake relay fixture: (1) applies `limit` only to the first filter in a multi-filter REQ, and (2) never verifies incoming `EVENT` signatures.

- **Why this is a Nostr protocol smell:**
  Per NIP-01, `limit` applies to each filter independently. Signature verification is mandatory for event acceptance. A test fixture with wrong semantics validates against a non-existent protocol variant.

- **Likely runtime consequence:**
  Tests pass against the fake relay but fail against real relays. Protocol bugs in filter handling and signature verification are invisible.

- **Recommended fix:**
  Apply `limit` per filter. Add optional signature verification (at least structural — check that `sig` exists and is 128 hex chars). Add a strict mode flag for protocol conformance testing.

---

### [F38] NIP-47 tests use dummy correlation and lack relay-level integration

- **Severity:** Medium
- **Confidence:** High
- **Category:** J — Test smells
- **Location:** `test_nwc_session.c`; `nips/nip47/tests/`

- **Smell:**
  NIP-47 tests use hardcoded dummy correlation IDs and test envelope construction in isolation. No test exercises the full NIP-47 request→relay→response flow.

- **Why this is a Nostr protocol smell:**
  NIP-47 depends on relay subscriptions for response delivery (see F11). Testing only envelope construction misses the entire response path. Dummy IDs mean correlation logic is untested.

- **Likely runtime consequence:**
  F11 and F12 (missing response subscription, unsigned requests) were not caught by existing tests precisely because of this gap.

- **Recommended fix:**
  Create an integration test that: builds a signed NIP-47 request, publishes it through the fake relay, has the fake relay deliver a response event, and verifies the client receives and correlates it.

---

### [F39] NIP-65 edge cases untested — stale events, conflicts, write-relay filtering

- **Severity:** Medium
- **Confidence:** Medium
- **Category:** J — Test smells
- **Location:** `test_nip65.c`; `test_relays.c`

- **Smell:**
  Tests cover basic NIP-65 relay list parsing but not: stale/conflicting kind 10002 events (which should replace, not accumulate), or publish-side write-relay filtering.

- **Why this is a Nostr protocol smell:**
  Kind 10002 is a replaceable event (NIP-01). Only the latest version should be used. Without testing this, the client may accumulate conflicting relay lists.

- **Likely runtime consequence:**
  Stale relay lists may persist, causing the client to use outdated relay configurations.

- **Recommended fix:**
  Add tests for: receiving a newer kind 10002 replaces the old one, receiving an older one is ignored, write-relay subset is correctly used for publishing.

---

### Low

---

### [F40] `nostr_relay_publish()` logs full event JSON to stderr

- **Severity:** Low
- **Confidence:** High
- **Category:** C — Ignoring relay response frames
- **Location:** `relay.c` L1334–1336

- **Smell:**
  The publish function logs the complete event JSON (including content) to stderr at a non-debug log level.

- **Why this is a Nostr protocol smell:**
  Nostr events may contain encrypted DMs (NIP-04/NIP-44), private content, or sensitive metadata. Logging full event payloads to stderr creates a data disclosure vector, especially in containerized or shared-log environments.

- **Likely runtime consequence:**
  Sensitive user content appears in system logs, log aggregators, or crash reports.

- **Recommended fix:**
  Log only event metadata (id, kind, pubkey, created_at) at info level. Log full JSON only at trace/debug level with a warning comment.

---

### [F41] `sync_relays` runs on every store query instead of on relay-list change

- **Severity:** Low
- **Confidence:** Medium
- **Category:** A — Polling instead of subscriptions
- **Location:** `gnostr-relays.c` ~L533

- **Smell:**
  The relay synchronization function runs on every query to the relay store, performing redundant relay-list refreshes.

- **Why this is a Nostr protocol smell:**
  Relay lists (NIP-65, kind 10002) change infrequently. Re-syncing on every query is a polling pattern that should be replaced with change-driven updates.

- **Likely runtime consequence:**
  Unnecessary overhead on every store query. Under high query volume, this becomes a performance bottleneck.

- **Recommended fix:**
  Sync relay lists on: (1) receipt of a new kind 10002 event, (2) explicit user action, (3) a periodic timer with reasonable interval (e.g., 5 minutes). Not on every query.

---

### [F42] Profile indexer relay URLs are hardcoded

- **Severity:** Low
- **Confidence:** High
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `gnostr-relays.c` ~L352

- **Smell:**
  The profile indexer uses hardcoded relay URLs rather than discovering relays from the user's relay list or NIP-65.

- **Why this is a Nostr protocol smell:**
  Nostr is a decentralized protocol with no canonical relay set. Hardcoding relay URLs creates a single point of failure and ignores the user's relay preferences.

- **Likely runtime consequence:**
  Profile indexing fails if the hardcoded relays go offline. Users on different relay sets have degraded profile discovery.

- **Recommended fix:**
  Use the user's NIP-65 relay list as the primary source. Fall back to configurable defaults, not hardcoded URLs.

---

### [F43] Verbose mode sends `NOTICE` on every writable callback — can corrupt the stream

- **Severity:** Low
- **Confidence:** High
- **Category:** I — Misuse of heartbeats and timers
- **Location:** `grelay_main.c` ~L464

- **Smell:**
  In verbose/debug mode, the relay server sends a `NOTICE` frame to the client on every writable callback from the event loop, regardless of whether there is meaningful information to convey.

- **Why this is a Nostr protocol smell:**
  `NOTICE` frames are protocol messages that clients may display to users or log. Flooding the stream with debug notices can interfere with protocol parsing and overwhelm client logging.

- **Likely runtime consequence:**
  Clients connected to a verbose-mode relay receive a flood of meaningless NOTICE frames. Client-side log files fill rapidly. Some clients may misinterpret the notices as errors.

- **Recommended fix:**
  Remove NOTICE emission from the writable callback. Use server-side logging for debug output instead of protocol frames.

---

### [F44] 300ms EOSE linger on top of polling delays — ~800ms worst-case latency

- **Severity:** Low
- **Confidence:** Medium
- **Category:** I — Misuse of heartbeats and timers
- **Location:** `grelay_main.c` → EOSE linger

- **Smell:**
  The relay server adds a 300ms delay ("linger") before sending EOSE, on top of the existing polling tick delays from F28. Combined, worst-case completion latency approaches 800ms.

- **Why this is a Nostr protocol smell:**
  EOSE should be sent as soon as all stored events matching the filter have been delivered. Artificial delays slow down client initialization and backfill.

- **Likely runtime consequence:**
  Clients experience unnecessary latency on subscription setup. Interactive applications feel sluggish.

- **Recommended fix:**
  Send EOSE immediately after the last matching stored event is dispatched. If batching is needed for performance, use a much shorter window (e.g., 10ms) or a message-count trigger.

---

### [F45] TCP auth and ACL tests are skipped or bypassable in CI

- **Severity:** Low
- **Confidence:** Medium
- **Category:** J — Test smells
- **Location:** `test_nip5f_tcp.c`; `test_nip5f_loopback.c`

- **Smell:**
  Tests for TCP-level authentication and access control are conditionally skipped in CI environments, leaving those code paths effectively unverified.

- **Why this is a Nostr protocol smell:**
  Access control is a security boundary. If tests for it don't run in CI, regressions can ship.

- **Likely runtime consequence:**
  Auth and ACL bugs go undetected until production. Security-sensitive code has no automated guard.

- **Recommended fix:**
  Ensure TCP auth tests run in CI. If they require special setup (ports, permissions), use CI-specific configuration rather than unconditional skips.

---

### [F46] `nostr_nip65_add_relay()` has no relay-count cap

- **Severity:** Low
- **Confidence:** Medium
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `nip65.c`

- **Smell:**
  There is no upper bound on the number of relays that can be added from a kind 10002 event. A malicious or buggy event can advertise hundreds or thousands of relays.

- **Why this is a Nostr protocol smell:**
  The client will attempt to connect to every relay in the list, consuming sockets, memory, and bandwidth proportional to the attacker-controlled list size.

- **Likely runtime consequence:**
  Resource exhaustion from a single malicious kind 10002 event. Potential denial of service.

- **Recommended fix:**
  Cap the relay list at a reasonable maximum (e.g., 20–50 relays). Log a warning when truncating.

---

### [F47] NIP-42 example uses `https://` instead of `wss://`

- **Severity:** Low
- **Confidence:** High
- **Category:** H — Auth, relay metadata, and relay capability blind spots
- **Location:** `nip42/examples/example.c`

- **Smell:**
  The example code for NIP-42 auth uses an `https://` URL for the relay, which is the wrong scheme. Nostr relays use WebSocket (`wss://` or `ws://`).

- **Why this is a Nostr protocol smell:**
  Example code is often copied by developers. An incorrect URL scheme in an auth example creates confusion and leads to connection failures.

- **Likely runtime consequence:**
  Developers copying the example will get connection errors. Minor documentation bug, but in a security-sensitive context.

- **Recommended fix:**
  Change `https://` to `wss://` in the example.

---

## Good Patterns Found

The following areas demonstrate correct protocol usage and good architectural decisions:

1. **Correct `go_select` on EOSE channels** — `neg-client.c fetch_need_events()` uses `go_select` properly to wait on EOSE with timeout, demonstrating the right way to handle backfill completion.

2. **EventBus-driven callback architecture** — `gnostr-sync-bridge.c` uses an EventBus callback pattern instead of polling, cleanly separating event producers from consumers.

3. **Well-scoped NIP-46 subscription filters** — `nip46_session.c` constructs tight subscription filters and validates the sender's pubkey before attempting decryption, preventing unauthorized events from being processed.

4. **Complete `since`/`until`/`limit` exposure in filter builder** — The NIP-01 filter builder properly exposes all temporal and cardinality controls, enabling efficient relay queries.

5. **Full NIP-11 limitation parsing** — The NIP-11 module correctly parses relay limitation documents, including max message length, max subscriptions, and supported NIPs. (Though this data is not yet consulted at decision time — see F23.)

6. **`NEG-CLOSE` cleanup discipline** — `neg-client.c` sends `NEG-CLOSE` on session cleanup, properly releasing server-side resources.

7. **NIP-65 read/write relay separation** — The relay list implementation correctly distinguishes read and write relays, enabling proper relay routing.

8. **Publish-side NIP-11 pre-filtering** — Before publishing, the code checks NIP-11 relay limits (event size, kind restrictions), avoiding futile publish attempts.

9. **Per-object dedup via `seen_events`** — `gnostr-thread-subscription.c` maintains a per-subscription set of seen event IDs, preventing duplicate delivery within a thread view.

10. **D-Bus local signer integration** — `nip46_client_dbus.c` uses D-Bus appropriately for local signer communication, keeping the signing key in a separate process with proper IPC.

11. **Clean store/cache separation** — `nostr-dav` stores maintain a clean separation between in-memory cache state and persistent storage.

---

## Prioritized Remediation Plan

### 1-Week Fixes

These are the highest-priority items that can be addressed with targeted patches:

| Priority | Findings | Action |
|----------|----------|--------|
| **P0** | F01 | Route all frame types through shared dispatch in both `relay.c` and `relay_optimized.c`. |
| **P0** | F02 | Add `sig` field to `nip42_Event` or switch to standard `nostr_event`. |
| **P0** | F03 | Add timeout to `nostr_relay_count()` via `go_select`. |
| **P0** | F04 | Wire `ok_callbacks`; move auth state transition to `OK` receipt. |
| **P1** | F05 | Add `authors` to negentropy `NEG-OPEN` filter. |
| **P1** | F06 | Handle `AUTH` frames in `neg_handler`. |
| **P1** | F08 | Replace static globals in `relay_fetch.c` with per-request context. |
| **P1** | F09 | Replace `time(NULL)` with `gen_nonce()` in auth challenge generation. |
| **P1** | F11, F12 | Add response subscription and event signing to NIP-47 client. |
| **P1** | F13 | Emit `SIGNAL_EOSE` when relay EOSE is received. |
| **P1** | F16 | Make `reconnect_now()` actually interrupt the backoff sleep. |
| **P1** | F20 | Update `since` filter on reconnect to latest received timestamp. |

### Structural Fixes

These require broader refactoring but address systemic issues:

1. **Unified relay frame dispatch** (F01, F04, F06, F07)
   - Create a single `relay_dispatch_frame()` function used by all relay paths.
   - Ensure `OK`, `CLOSED`, `AUTH`, `COUNT`, `NOTICE` are always handled.

2. **Complete NIP-42 implementation** (F02, F07, F09)
   - Implement full challenge→sign→send→OK flow.
   - Integrate with relay connection state machine.
   - Use CSPRNG for server-side nonce generation.

3. **Replace polling with event-driven signaling** (F17, F25, F28, F44)
   - Replace 1ms subscription monitor poll with channel wait.
   - Replace 200ms NIP-46 state poll with state-change callback.
   - Replace relay server poll ticks with direct fan-out from `put_event`.
   - Remove EOSE linger delay.

4. **Fix API and ownership boundaries** (F10, F26, F31, F32)
   - Fix UB cast in relay store.
   - Replace string-sniffed errors with GError domains.
   - Deduplicate ring-buffer state.
   - Add async query variant to relay store interface.

5. **Improve defaults for production use** (F18, F29, F30, F46)
   - Enable dedup by default with hash-based lookup.
   - Default `max_subs` to ≥20.
   - Default replay dedup TTL to 3600s.
   - Cap NIP-65 relay list size.

### Test Hardening

Dedicated workstream to close assurance gaps:

1. **Upgrade `fake_relay_fixture.py`** (F14, F37)
   - Add configurable `AUTH` challenges.
   - Add `OK false` responses with reason strings.
   - Fix per-filter `limit` handling.
   - Add optional `EVENT` signature verification.
   - Add relay capability toggles (COUNT unsupported, delayed EOSE, CLOSED reasons).

2. **Replace sleep-based tests with deterministic helpers** (F35)
   - Use condition variables or bounded channel receives instead of `g_usleep`.
   - Assert on protocol events, not timing.

3. **Add error-path coverage** (F36)
   - Publish rejection (OK false).
   - Network disconnect mid-sync.
   - Relay never sends EOSE.
   - Unreachable relay timeout.

4. **Add integration tests for NIP-47** (F38)
   - Full request→relay→response flow through fake relay.
   - Signature verification.
   - Correlation ID matching.

5. **Add NIP-65 edge case tests** (F39)
   - Replaceable event semantics (newer replaces older).
   - Write-relay filtering for publishes.

6. **Re-enable skipped tests** (F15, F45)
   - Fix the underlying note-count defect or convert to expected-failure.
   - Ensure TCP auth tests run in CI.

### Observability Improvements

1. **Redact event payload logging** (F40)
   - Log only event metadata (id, kind, pubkey, created_at) at info level.
   - Full JSON at trace/debug only.

2. **Add protocol frame counters** (F01, F04, F19)
   - Count frames by type: EVENT, OK, CLOSED, AUTH, COUNT, NOTICE.
   - Count dropped frames (channel full, unhandled type).
   - Expose as metrics or structured log entries.

3. **Add reconnect and dedup metrics** (F16, F18, F20, F30)
   - Track reconnect attempts and backoff durations.
   - Track dedup hits/misses and seen-set size.
   - Track re-delivered events on reconnect.

4. **Fix verbose-mode stream corruption** (F43)
   - Remove NOTICE emission from writable callback.
   - Use server-side logging for debug output.

---

*Report generated 2026-04-29. Covers the nostrc codebase as of this date. Findings are based on static analysis of protocol patterns; runtime verification is recommended for all Critical and High findings before remediation.*
