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
