# Groundhog W6 partial integration review — 2026-09-27

## Context / scope

Independent review of **`origin/master..8a8488b1`** (`4b476147` as the remote base; seven local commits, `9a50aea3` through `8a8488b1`), followed by the exact direct-child fix **`cd253017`** on local master. This is a judgment on the partial integration with that follow-up, **not** Groundhog release or messaging readiness. No code, bead, merge, or remote state was changed for this review.

## Findings

### F1 — Relay `CLOSED` discovery state: resolved by `cd253017`

The original range forwarded a relay `CLOSED` (`gh-relay-gnostr.c:64-70`) but left that source pending in the account reducer. **`cd253017` adds an explicit terminal `GH_RELAY_NOTICE_CLOSED` case** (`gh-account-relays.c:169-184`): before EOSE it no longer traps discovery in `DISCOVERING`; after EOSE it removes that source from the answered set while retaining already admitted lists. The deterministic test checks both cases and an EOSE-driven recovery transition (`test_account_relays.c:398-434`). Without the new case, its first `COMPLETE` assertion would fail. The follow-up changes only the reducer, state documentation, and test; `git diff --check cd253017^ cd253017` passes. **The original REQUEST CHANGES is resolved.**

The recorder test proves the state transition, not socket-level automatic recovery or relay-side CLOSE delivery. `COMPLETE` should not be read as continuous socket liveness across a transient disconnect: the reducer still retains an answered source on `DISCONNECTED` (`gh-account-relays.c:169-180`). Those broader lifecycle/protocol questions are outside this bounded approval.

## Correctness, security, lifecycle, and test evidence

- The libnostr cursor-ID cache is bounded to 1,024 entries per active/boundary cache, with FIFO eviction and no ID-based rejection on allocation failure (`subscription-private.h:57-76`; `subscription.c:20-67,421-467`). The signed loopback test exercises overflow, retained duplicate suppression, evicted redelivery, unseen same-second delivery, and the EOSE boundary (`test_reconnect_same_second.c:312-329,376-394`). Evicted-ID redelivery is an explicit at-least-once tradeoff, not event loss.
- Groundhog signer operations now have separate private D-Bus senders and asynchronous address/connection setup (`gh-signer.c:25-39,81-94,274-371,411-428`). Generation and bus-GUID checks precede dispatch; concurrent-cancel/switch tests check that one cancellation does not revoke a same-account peer (`test_identity.c:344-436`). The real-service test defers `NameOwnerChanged` cleanup, confirms `NameHasOwner=false` while the request is still pending, and rejects approval without a signed reply or remembered grant (`test_signer_dbus_contract.c:958-1009`; `signer_service_g.c:969-977`). I ran the passing real-service test, but did not rerun the disabled-liveness-guard mutation described in the bead.
- Relay discovery is opt-in (`discovery-relays` defaults empty), account-author filtered, signed-event validated, newest-revision selected, and generation-revoked before switch (`gh-account-relays.c:107-149,183-259`; schema lines 14-17). Per-endpoint `GNostrRelay` instances avoid the global URL-wrapper sharing race (`gh-relay-gnostr.c:22-29,175-185,259-296`); both recording and local WebSocket tests cover account switch and same-URL isolation. These discovered lists are not yet used for messaging.
- Local macOS configure/build of the original range with the pinned nostrdb submodule and the CI-shaped CMake options succeeded. Focused CTest: **13 passed, 1 skipped** (`groundhog-launch` uses the existing macOS skip), including `libnostr_reconnect_same_second`, `nip55l_dbus_contract`, and all available Groundhog tests. `groundhog-relay-wire` and `groundhog-account-relays` each passed five repeated runs. For exact `cd253017`, I built `test-groundhog-account-relays` from the local master checkout into a separate `/tmp` build directory; its CTest passed, then passed five repeated runs. `git diff --check` passed for both inspected ranges. This is not a full-tree test or Ubuntu Actions run.

## Recommendations and existing backlog boundaries

- The new Ubuntu workflow builds the Groundhog app and named focused test executables, checks registration, runs under Xvfb, rejects CTest skips, and uploads logs (`.github/workflows/groundhog-ci.yml:22-89`). **Ubuntu execution is unproven**; `nostrc-qp24.15.1` remains open. The opt-in macOS GUI smoke instability is separately tracked as `nostrc-qp24.8.2`. Current `main.c:197` explicitly calls `g_resources_register(groundhog_get_resource())`; this review assigns no root cause to the macOS observation and makes no claim that Linux launch fails.
- `nostrc-qp24.4.3` already tracks best-effort libnostr relay-side CLOSE/socket teardown after account switch. A separate fix is in progress and needs its own bounded review before push. This approval does **not** approve that fix or prove the old relay observes CLOSE; the privacy/resource gap remains outside this review.
- `nostrc-qp24.9` remains open for Handler2/URI activation, background lifecycle, signer execution, use of discovered lists, and UI/GNOME acceptance. This integration is **not messaging-ready**.

## Version assessment

`libnostr` **1.0.1 → 1.0.2 PATCH** is appropriate for the compatible bounded-cache fix; Groundhog **0.3.0 → 0.4.0 MINOR** is appropriate for its new configuration/network capability. CMake declarations match `VERSION_MANIFEST.md:14,21`; release columns remain unchanged. The `cd253017` correction is part of the same unreleased Groundhog 0.4.0 wave and needs **no additional bump**. The NIP-55L fixture is test-build-only; CI/docs also require no additional component bump.

## Verdict

**APPROVED** for the original partial W6 integration **with `cd253017`**: its sole blocking finding, F1, is resolved. This is not approval of messaging readiness, Ubuntu CI, GNOME acceptance, the separate libnostr CLOSE teardown fix, or closure of the named open beads.
