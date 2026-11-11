# Groundhog call signalling

`GhAppServices` starts `GhCallService` independently of the DM inbox and durable
outbox. `gh_app_services_get_calls()` exposes it to a later UI. No call payload,
replay key or missed-call record is written to the message store. Missed-call
requests are session-memory objects/signals, not conversation timeline entries.

## Media adapter

Implement `src/app/gh-call-media-backend.h` (under `gnome/groundhog/`) and register
a factory with `gh_call_service_set_media_factory()`. The service ships without a
production media factory; place/answer fail with a named unsupported error until
one is installed. The fake backend is linked only into the conformance test.
The contract does not expose GStreamer types or dependencies.

Local SDP creation includes installation. Remote SDP completion is the boundary
before buffered ICE is applied. Local ICE arriving during SDP creation is held
until that SDP is queued. Implementations must enforce the local-network setting
for both accepted and emitted candidates, honor cancellation, emit on the owning
main context and tear down synchronously/idempotently. Tor mode is fail-closed.

## Wire and routing

Signed inner 25050–25055 events go directly into fresh ephemeral 21059 wraps;
there is no kind-13 seal. Reception validates the outer wrap, uses the active
account-controller NIP-44 decrypt operation (no account secret escapes), then
validates the signed inner canonical id/signature, correlation and freshness.

The receive `GhRelayScope` requests only 21059 addressed to the active account,
with a 60-second freshness window. It remains live after EOSE. Publication uses
`GhRelayPublish`, the relay layer's one-event write companion to scopes, so an
actual OK is distinguished from a socket write and bounded retry is available.
It never uses the durable outbox. Both paths explicitly check the current
kind-10006 deny snapshot before adding URLs; installed relay network transports
also enforce updates at their socket boundary. NIP-42 identities are assigned
only by `GhAuthPolicy`: account for current own inbox/self-copy, ephemeral for
peer delivery and discovery. An ephemeral refusal never escalates to account.

Recipient resolution uses `GhInboxResolver`; only NOT_FOUND (not EMPTY or an
unreachable lookup) permits discovery of the peer's signed NIP-65 read list.
Self-wraps use current own 10050 relays only. Optional extra NIP-65 receive
subscriptions for Noscall are not enabled.

The in-memory FIFO serializes signer work. It does not wait for relay OK before
signing the next event. Answer/reject self-wraps reuse the same signed inner,
without an extra signer round trip. Jobs expire after 60 seconds; each relay
gets an 8-second deadline and at most one retry. At most 128 jobs are pending;
ending a call cancels obsolete work before queuing its terminal signal. Slow
NIP-46 signers still cost one round trip per trickled event: a large candidate
set can exceed the freshness window, fails explicitly, and is never persisted
for replay. This is a latency bound, not a claim of measured NIP-46 throughput.

## Conformance evidence

The test imports Nospeak's wire-parity JSON unchanged from revision
`518b7711d397d8d62bcbe9774c34300b4e33d9d4`, at
[`tests/fixtures/nip-ac-wire/inner-events.json`](https://github.com/psic4t/nospeak/blob/518b7711d397d8d62bcbe9774c34300b4e33d9d4/tests/fixtures/nip-ac-wire/inner-events.json); see its
`src/lib/core/voiceCall/{VoiceCallService,nipAcGiftWrap,wireParity.test}.ts`.
The Amethyst vector in `nips/nip59/tests/nipac_wire_vector.h` is independently
hand-derived and pinned to [quartz `ce558d90e70d432157cc0672762b2d4119d7f904`](https://github.com/vitorpamplona/quartz/tree/ce558d90e70d432157cc0672762b2d4119d7f904/quartz/src/commonMain/kotlin/com/vitorpamplona/quartz/nipACWebRtcCalls).
These are source-derived vectors, **not captured live calls**.

Receive compatibility includes Nospeak's unhyphenated 128-bit call ids,
nullable ICE mid/index (mapped to the bundled first media section) and missing
call-type (voice). Groundhog sends UUIDs, typed ICE and an explicit call-type.
Group extensions are rejected: this service implements only 1:1 calls.

`test-groundhog-calls` uses a real account controller and private D-Bus mock
signer performing real crypto, plus recording relay transports and fake media.
It proves no live relay, media interoperability, native GStreamer behaviour,
Flatpak artifact or release readiness. Run via CTest to preserve private-profile
and private-bus isolation. Packaging/media gates are separate work.
