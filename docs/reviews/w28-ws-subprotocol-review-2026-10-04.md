# W28 WebSocket subprotocol peer review — 2026-10-04

**Verdict: CHANGES-REQUIRED** on `review/w28-ws-subprotocol` (reviewed `relayd/w28-ws-subprotocol` at `90d69eab`, bead `nostrc-hja5`; cherry-picked as `455653ab` onto `origin/master` `cf616c3d`). The protocol fix itself looks sound, but its new required Groundhog↔relayd CTest fails in a clean macOS build. Do not merge until the acceptance test is reliable and green.

## Finding

### W28-1 — New publish/read acceptance test races nostrdb ingestion

- **Severity: High** (required CI test fails); **location:** `gnome/groundhog/tests/relay/test_relay_publish_wire.c:678-708`.
- **Failure scenario:** `groundhog-relayd-publish` receives `OK true` for the published event, immediately opens a fresh kind-1 `REQ`, receives `EOSE` with zero events, then aborts at line 707 (`read.events == 1`, actual 0). This occurred on two unmodified macOS runs (CTest #385). `components/nostrdb/src/nostrdb_storage.c:92-99` treats `ndb_process_event()` queue acceptance as store success; the relay sends `OK` from `apps/relayd/src/protocol_nip01.c:496-513` before the async ingester necessarily makes the note visible to a new query. As a timing probe **only**, inserting 250 ms between the `OK` and `REQ` made the test pass. That delay was removed; the reviewed source is unmodified.
- **Required change:** make the readback assertion deterministic without a fixed sleep. Prefer a commit/visibility completion signal before relayd sends a success `OK`, or another explicit storage/relay synchronization mechanism suitable for this test. Do not replace the race with an arbitrary timer or consider `EOSE` from a too-early query proof that the event was read back. Keep the test proving both Groundhog publish and read through relayd.

## Protocol and compatibility checks

- [RFC 6455 §§4.1–4.2](https://www.rfc-editor.org/rfc/rfc6455.html#section-4.1) makes `Sec-WebSocket-Protocol` optional and forbids a server from selecting a protocol the client did not offer. [NIP-01](https://github.com/nostr-protocol/nips/blob/db5fe3de8c5d1443b634c9bbf66ecb004f337057/01.md) requires WebSocket transport but specifies no WebSocket subprotocol token. `libnostr/src/connection.c:875-878` now uses `local_protocol_name="wss"` only for its local lws callback and `protocol=NULL` for the offer. This matches Homebrew's `lws-client.h` semantics. The loopback fixture asserts no client protocol header; restoring the old `ci.protocol="wss"` makes `libnostr_subscription_dispatch` fail that assertion.
- Relayd keeps `nostr` as the default no-offer handler and adds only a legacy `wss` alias (`apps/relayd/src/relay_server.c:378-384`). The raw fixture completes no-header and `wss` upgrades and REQ/EVENT replies. Its no-offer response-header assertion passes, so relayd does not echo an unoffered protocol. Restoring the old relayd protocol table makes the legacy-`wss` round fail with libwebsockets' `No supported protocol "wss"` log. Arbitrary unknown offers still fail before the callback; `nostrc-2oxs` accurately tracks that separate limitation.
- Local Docker-only raw upgrades (not CTest/public-relay traffic) returned HTTP 101 with **no** `Sec-WebSocket-Protocol` response from both `scsibug/nostr-rs-relay:latest` and `dockurr/strfry:latest` when none was offered. Groundhog's external-interop test published successfully to the local nostr-rs-relay container.
- TLS selection is independent of `ci.protocol`: `parse_ws_url()` still maps `wss://` to `use_ssl=1` (`libnostr/src/connection.c:1022-1028`), which still sets `LCCSCF_USE_SSL` (`:874`). `test_connection_tls_no_resumption` established two real `wss://` TLS handshakes; `groundhog-tls-resumption` also passed.

## Build and test evidence

| Check | Result |
| --- | --- |
| macOS clean CMake/Ninja build after `source /tmp/nostrc-macos27-env.sh` (Homebrew libwebsockets 4.5.8) | PASS |
| `python3 scripts/check-unsequenced-args.py` | PASS |
| macOS relayd tests, libnostr relay tests, `groundhog-e2e-dm` (20 focused CTests) | PASS |
| New `relayd_session_relay_ws` fixture, client/server reverted independently then restored | RED / RED / GREEN |
| New `groundhog-relayd-publish` CTest | **FAIL twice**: `read.events` 0 after `OK true` and `EOSE` |
| Ubuntu 24.04 CI image, libwebsockets 4.3.3: build `nostrc-relayd`, `nostr-session-relayd`, fixture; run fixture | PASS |
| `scripts/linux-gate.sh --sanitizers /tmp/rv-w28-ws-subprotocol` | PASS, 60 tests |
| Local Docker nostr-rs-relay Groundhog publish | PASS |

The sanitizer job configures `BUILD_RELAYD=OFF`, so the separate Ubuntu-image relayd build above is essential. The failing Groundhog relayd test is not in that sanitizer test set.

## Version assessment

`libnostr` 1.1.2 → 1.1.3 **PATCH** is correct: this repairs a nonstandard wire offer without changing the public API/ABI; `libnostr/CMakeLists.txt` and `VERSION_MANIFEST.md` agree, and the release columns correctly remain unchanged. `relayd` has no component row or authoritative declared version in the manifest, so no new row/bump is required for this compatible handshake fix. Groundhog, shared test fixture, and CI changes are test-only: **no bump**. These decisions are recorded in the manifest and bead.

---

## Re-review addendum — `806fecc2` (2026-10-04)

**Final verdict: CHANGES-REQUIRED** on `review/w28-ws-subprotocol` (author fix `806fecc2`, cherry-picked as `e41be924`). The original W28-1 immediate-REQ failure is **resolved**: relayd now acknowledges only after the note is query-visible, and the retry-free storage regression passes. A new availability/performance finding below blocks approval.

### W28-R2-1 — Synchronous commit wait blocks every relayd client

- **Severity: High; location:** `components/nostrdb/src/nostrdb_storage.c:152-187`, called from `apps/relayd/src/protocol_nip01.c:493-507` through `apps/relayd/src/relay_server.c:318-352,660-665`.
- **Failure scenario:** `ndb_put_event()` subscribes and queues each EVENT, then waits for commit notification (up to 10 seconds) **inside `LWS_CALLBACK_RECEIVE`**. Relayd has one `lws_service()` loop. While this wait runs, it cannot service another client's REQ/EVENT, accept or complete a handshake, send pending frames, or run its normal loop maintenance. If the writer cannot commit (for example, a storage failure), each otherwise valid EVENT can impose a separate 10-second relay-wide stall; a stream of such events serializes those stalls. A no-notification fault injection produced `OK false, "error: store failed"` in 10.3 seconds, not an infinite wait, but the whole loop is blocked until then. Because the condition uses `CLOCK_REALTIME`, a backward wall-clock adjustment can also extend the nominal 10-second bound.
- **Throughput evidence:** the wait is **per EVENT, not batched**; the vendored nostrdb writer can otherwise pop up to 4096 queued items per transaction (`third_party/nostrdb/src/nostrdb.c:7156-7181`). In a local 100-event burst with limits raised, three old queue-only runs returned all OKs in 5.9–9.1 ms; three current runs took 123–183 ms (approximately 14–31× slower acknowledgment completion). All events were accepted. This comparison is directional, not an apples-to-apples durability benchmark—the old OKs were premature—but it demonstrates the single-event serialization cost.
- **Required change:** keep the commit-before-success guarantee while moving the wait off the libwebsockets service thread. Queue writes/completions to a worker or use an upstream durable-commit callback, then schedule the `OK` on the LWS thread; allow the writer to batch multiple events. Add a failure-path test proving an absent notification yields a bounded `OK false` **without** delaying an independent client. Use a monotonic deadline where supported.

### Verification of the author's fix

- **Commit/lock behavior:** `ndb_subscribe()` happens before enqueue; the callback is invoked only after a successful writer transaction commit and merely signals the condition. It does not query or unsubscribe while holding nostrdb's monitor lock. The waiter queries under its own mutex, and unsubscribes only after releasing it; I found no monitor/commit-mutex lock cycle in this path. Normal green runs confirmed immediate read visibility.
- **Missing notification:** the 10-second timed wait returns `-ETIMEDOUT`, propagated to relayd as `OK false` with `error: store failed`; it does not wait forever under a stable realtime clock. The `error:` prefix and nonempty explanatory text conform to [NIP-01's OK message rules](https://github.com/nostr-protocol/nips/blob/master/01.md), although the text does not distinguish timeout from an actual write failure. The fault-injection run confirmed the rejection after 10.3 seconds; its edit was restored and rebuilt.
- **Red/green regression:** replacing only `components/nostrdb/src/nostrdb_storage.c` with its pre-fix version made the new `relayd_session_relay_storage` test fail at its immediate REQ after `OK true` (`test_session_relay_storage.c:209`). Restoring the fix made that test and `groundhog-relayd-publish` pass. The Groundhog test happened to pass in the red run, so the storage test is the decisive regression.
- **Tests/build:** macOS full incremental CMake/Ninja build (Homebrew libwebsockets 4.5.8) and unsequenced-argument check passed. All 21 focused relayd/libnostr/Groundhog CTests passed; 20 **concurrent** `groundhog-relayd-publish` invocations passed 20/20. Ubuntu 24.04 image with libwebsockets 4.3.3 built `nostrc-relayd` and `nostr-session-relayd`; both relayd storage and WebSocket CTests passed. `scripts/linux-gate.sh --sanitizers /tmp/rv-w28-ws-subprotocol` passed all 60 tests. That sanitizer job sets `BUILD_RELAYD=OFF`, so the relayd-specific code is covered by the separate Ubuntu build/tests, not by those sanitizer tests.
- **Version policy:** `relayd` and the nostrdb storage adapter have no declared component versions or rows in the tracked-component table. Their compatible correctness fix requires **no bump and no new component row** under the current policy; the added `VERSION_MANIFEST.md` decision row is appropriate. `libnostr` remains at the justified 1.1.3 PATCH; no further bump is needed for this commit.
