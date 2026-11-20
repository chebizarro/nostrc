# NIP-42 session and pool CLOSED recovery (libnostr)

nostrc-7i6w. Reference for `libnostr/include/nostr-relay.h` (NIP-42 session)
and `libnostr/include/nostr-simple-pool.h` (CLOSED recovery). The headers are
authoritative; this page shows how the pieces fit.

## Relay session (per connection)

```
            challenge (new auth generation)                 reconnect (new connection generation)
 NONE ─────────────────────────────▶ CHALLENGED ◀────────── any state ── forgets everything
                                        │  ON_CHALLENGE: at once
                                        │  ON_DEMAND: after an "auth-required:" CLOSED / OK false
                                        │  explicit: nostr_relay_auth_start()
                                        ▼
                                     SIGNING ── signer on a library thread; cancelled by a new
                                        │       challenge, reconnect, signer change, close/free
                     complete() verified│            │ refused / invalid / write refused
                                        ▼            ▼
                                      SENT        FAILED (held)
                          OK true id ┌──┴──┐ OK false id
                                     ▼     ▼
                               ACCEPTED   REJECTED (held)
```

* Nothing authenticates without `nostr_relay_set_auth_signer_full()` on that
  relay: identity is a per-connection caller choice (no global auto-auth).
* `["AUTH",event]` is the only transport. The OK is correlated by the AUTH
  event id; an OK for any other id is not the AUTH's answer.
* REJECTED/FAILED are held: no new signature until a new challenge or
  `nostr_relay_auth_start()` (which may also supersede a SENT AUTH the relay
  never answered, or authenticate a new identity generation).
* `nostr_relay_send_auth()` is for callers that sign themselves; it gets the
  same verification, correlation, result callback and pool recovery.
* Library signer templates include a per-attempt nonce: preserve it when
  signing so same-second retries have distinct ids. Caller-signed retries
  must likewise use distinct ids to distinguish late OKs.
* AUTH admission and the writer's connection lease validate the attempt token
  and connection generation. Superseded queued attempts are discarded; a write
  already handed to the connection cannot be recalled. A full AUTH write queue
  fails immediately rather than blocking signing or the relay reader.

## Wire order (`NostrRelayFrameInfo`)

Every control frame (AUTH, OK, CLOSED, EOSE, NOTICE, COUNT) is stamped on the
reader thread with a per-relay `sequence` (monotonic, never reused), its
`connection_generation`, the `auth_generation` and the session's
`auth_state` at receipt. Consumers that see results on different threads
(CLOSED on the pool worker, OK on the reader) order them by `sequence`:

* CLOSED `auth-required:` with `frame.auth_state == ACCEPTED`: the relay
  refused a REQ it received after accepting our AUTH (`AUTH_REFUSED`).
* `frame.connection_generation` older than
  `nostr_relay_get_connection_generation()`: a frame of a lost connection.

Available as `NostrRelayAuthResult.frame`, `nostr_relay_get_callback_frame()`
inside OK/AUTH callbacks and `nostr_simple_pool_get_callback_info()` inside the
pool's subscription callback.

## Pool CLOSED recovery

The pool keeps the CLOSED REQ's own filters (they survive the drain) and
reissues them by reason; nothing is re-sent on a timer except BACKOFF.

| CLOSED reason                         | `NostrPoolRecovery` | reissued                                                   |
|---------------------------------------|---------------------|------------------------------------------------------------|
| `auth-required:`, AUTH not accepted   | `AWAIT_AUTH`        | once, on this connection's next OK true (after the CLOSED) |
| `auth-required:`, AUTH accepted       | `AUTH_REFUSED`      | once, on a later OK true (new challenge/identity)          |
| `rate-limited:`, `error:`             | `BACKOFF`           | after the relay's ladder delay (default 1 s → 60 s; EOSE resets) |
| anything else (`blocked:`, `restricted:`, ...) | `RECONNECT` | only on a new connection                                   |

Every recovery also reissues on a new connection (at most one REQ per held
REQ per connection). A new `subscribe()` supersedes held REQs; `stop()` drops
them. The worker wakes on frames, subscribe, relay connect and AUTH results.

## Adopting it (e.g. metadesk md-xcy)

1. For each relay that should authenticate, register the signer once:
   `nostr_relay_set_auth_signer_full(relay, NOSTR_RELAY_AUTH_ON_CHALLENGE,
   identity_generation, pk_hex, sign_fn, ctx, ctx_unref)`. In `sign_fn`, ref the
   request, sign `nostr_relay_auth_request_get_event()` (copy it first) on
   your own worker or signer IPC, then `nostr_relay_auth_request_complete()`;
   use `nostr_relay_auth_request_set_cancel_handler()` to abort IPC.
2. Observe results with `nostr_relay_set_auth_result_callback_full()` instead
   of matching OK ids by hand.
3. Drop hand-written AUTH envelopes and the "pool re-fires every ~200 ms"
   assumption: in the subscription callback read
   `nostr_simple_pool_get_callback_info()`; `RECONNECT` and `AUTH_REFUSED`
   are terminal for this connection, `AWAIT_AUTH`/`BACKOFF` are transient.

## Migration limits

nostr-gobject delegates AUTH transport/results to the core session. Its
`authenticated` query reads current core state, not a cached queued result.
When NIP-11 says authentication is required, publish returns `AUTH_REQUIRED`
until AUTH is accepted (including while signing or awaiting OK); retry from
`auth-result`. It does not silently queue or resend the EVENT.

Groundhog's GNostrRelay-backed scope and publish transports call
`nostr_relay_send_auth()`. Its higher-level GhRelayScope/GhRelayPublish signer
and recovery state machines remain app-owned, because its soup/Tor transport
cannot yet use the core relay session; full migration is `nostrc-090as`.
The legacy synchronous callbacks remain deprecated compatibility adapters.
