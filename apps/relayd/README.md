# apps/relayd

Two daemons share one relay core (`libnostr-relay-server`,
`include/nostr-relay-server.h`):

- **`nostrc-relayd`** — the system relay: TCP `host:port` from `relay.toml`
  (`relay.toml.example`), nostrdb storage, `nostr-relayd.service`.
- **`nostr-session-relayd`** — the per-user session relay on
  `$XDG_RUNTIME_DIR/nostr/relay.sock` (0600, SO_PEERCRED uid check),
  socket-activated by `nostr-session-relay.socket`, nostrdb store under
  `$XDG_DATA_HOME/nostr/session-relay/`, config
  `~/.config/nostr/session-relay.conf` (`session-relay.conf.example`),
  read-only status on the session bus as `org.nostr.SessionRelay1`
  (`gnome/dbus/org.nostr.SessionRelay1.xml`).

`nostr-session-relayd --stats` prints the running daemon's statistics;
`--upstream [EVENT_ID]` prints upstream delivery status (below).

## AUTH and private vault service (VSP-01)

The default remains `auth = "off"`. For a dedicated vault namespace:

```toml
auth = "optional"
relay_url = "wss://vault.example.com/"
vault_allowlist = "/etc/nostrc-relayd/vault-allowlist.txt"
```

The allowlist is operator service configuration, not a Nostr event: one lowercase
64-character hex pubkey per line (up to 256), with blank lines and `#` comments.
Restart to reload. An empty file denies all vault access; an unreadable or invalid
file refuses startup. `relay_url` is the exact public URL clients put in AUTH's
`relay` tag, including any trailing slash. Vault mode refuses `auth = "off"`.

This isolated service protects **all** events of kinds 30078, 445 and 1059, rather
than guessing a plaintext vault tag (gift-wrap outer events need not have one).
REQ/COUNT without an explicit kinds restriction, including ids-only queries and
mixed filters, are protected too. Explicit unrelated kinds retain normal behavior.
Unauthenticated access returns `auth-required:`; a valid AUTH from a nonmember
returns `restricted:`. NIP-11 advertises `limitation.auth_required: true` and
`vault: true` only when vault mode is enabled (required-auth mode also advertises
the former). Membership grants access to the service, **not** vault decryption.

AUTH validates the signature, challenge, kind 22242, a ±10 minute timestamp window
and the configured relay URL, and returns a correlated OK. Kind 22242 cannot be
published as EVENT. Any successfully authenticated allowlisted identity on the
connection permits vault operations; failed AUTH does not grant access. Keep
`auth = "optional"` for vaults: legacy `required` mode additionally requires EVENT's
author to equal the authenticated key, which is incompatible with random NIP-59
outer authors. Vault mode authorizes the connection, not the outer author.

Challenges use OpenSSL `RAND_bytes`; if secure randomness is unavailable,
relayd rejects the connection rather than issuing a predictable challenge.
For real-store VSP-01 checks run
`packaging/lab-blossom/run_vsp01_lab.sh /absolute/path/to/nostrc-relayd`.

## NIP-01 catch-up and live delivery

A successful REQ retains its filters after EOSE. EOSE marks only the end of
stored catch-up; the subscription stays live until CLOSE, a new REQ with the
same subscription id, or disconnect. `limit` applies to history, not live
delivery (`limit: 0` requests live-only delivery). Active subscriptions,
including those past EOSE, count toward `max_subs`; `max_filters` applies to
each REQ. Replacing an id cancels its old iterator and buffered deliveries.

Each accepted newly stored EVENT is delivered once per matching subscription,
even when several of its filters match. Duplicate, rejected and unconfirmed
writes do not fan out. Accepted replaceable/addressable events retain their
original signed payload. With nostrdb, fanout and OK follow commit visibility;
a disconnected publisher does not cancel a pending commit's live delivery.
NIP-42 required-auth and VSP-01 authorization are checked for each subscriber,
not inherited from the publisher. Protected kinds and kind-less filters retain
the allowlist gate. Search filters use the storage backend's NIP-50 matcher.

All connection/subscription ownership and WebSocket writes stay on the single
libwebsockets service thread. The nostrdb writer only calls
`lws_cancel_service`; reconciliation on the service thread performs fanout.
Outbound frames share a per-connection buffer bounded to **256 frames and 2 MiB
of serialized payload**, plus bounded frame metadata. Live frames wait for their
subscription's EOSE; control replies can pass live frames still awaiting catch-up.
Buffered delivery alternates with round-robin catch-up progress so a busy live
subscription cannot starve another subscription's EOSE. Writes happen one frame
per writable callback, using libwebsockets' own partial-write buffering. If the
queue cannot accept a frame (including allocation failure), the connection is
**disconnected** with WebSocket policy-violation status rather than silently
losing events or allocating an unbounded queue. Clients must reconnect and
reissue their REQs to catch up.

### Storage confirmation and late commits

`OK true` and live fanout require committed visibility. A definitive enqueue
refusal returns `OK false`; it cannot subsequently commit or fan out. Once a
write is admitted, absence or a lookup error does **not** prove storage failure.
At the **10-second confirmation deadline**, relayd sends
`["NOTICE","error: storage outcome unknown for event <id>"]` and closes the
publisher WebSocket, ending its subscriptions too. It sends **no OK** for that
unknown outcome: NIP-01 has no indeterminate OK, and `OK false` would claim denial.

Storage ownership outlives the publisher. A commit notification reconciles the
reservation and fans out the confirmed event once to matching authorized live
subscribers. Resubmission while unresolved attaches a waiter to the existing
write; it never enqueues another copy. Each waiter has its own confirmation
deadline. Commit yields `OK true` to all remaining waiters (resubmissions use
`duplicate:`). Resubmission after reconciliation returns `OK true duplicate:`
without another live delivery. Failed visibility **reads** retry on a
connection-independent 100 ms–1 s capped backoff, so a commit wake is not lost
to a transient read error. Successful reads reporting an invisible event do
not poll; writes are never retried.

Configure these global bounds in `relay.toml`:

```toml
storage_max_pending_events = 256
storage_max_pending_bytes = 16777216
```

The event limit accepts 1–65536; the byte limit accepts 1024–1073741824.
The byte budget counts retained canonical event JSON including its NUL, not
whole-process RSS or backend scratch memory. Both are reserved **before enqueue**
and include timed-out/disconnected writes. ACK waiters (including ready but
unsent OKs) are bounded to twice the event limit globally and 256 per connection.
If waiter capacity is full for an unresolved resubmission, it gets the same
unknown NOTICE and connection close immediately, never a false rejection.

At saturation, **new writes are refused before enqueue** with
`OK false rate-limited: storage reconciliation capacity exhausted`.
Already-stored duplicates and reads remain available. Reservations have no TTL
and are never evicted to admit new writes. A saturation log records cumulative
refusals, retained events/bytes and waiter count at the first refusal and powers
of two. Confirmed commits release capacity.

nostrdb has four independent ingest workers before its single writer. Their
completion order can differ from submission order: a later committed event is
**not a failure watermark** for an earlier invisible event. The current backend
cannot cancel or report terminal failures after admission. Permanently unknown
writes can therefore pin capacity indefinitely and stop new-write admission;
relayd does not guess failures or retry them. Reservations are process-local,
not a durable crash-recovery/fanout journal; shutdown discards tracking without
claiming that outstanding writes failed. These semantics apply to the async
nostrdb path, not synchronous storage wrappers such as the session outbox tee.

Focused coverage: `ctest --test-dir <build> -R '^relayd_req_contract$'
--output-on-failure` exercises the real WebSocket entrypoint with scripted
storage and (when built) real nostrdb. Delivery/absence assertions use EVENT,
OK, NOTICE/close handshakes, EOSE and same-connection barriers, not sleep-based
quiet periods. Fault cases cover late commit, attached waiter deadlines (with
replay enabled and disabled), waiter/count/byte saturation, non-admission,
out-of-order commits and visibility-read recovery after disconnect. A held
write released through the real nostrdb adapter verifies its actual commit
notification, live delivery, stored query and duplicate-success path. The SV-10
two-npub harness now waits for the already-subscribed owner's live conflict fold;
it no longer restarts owner catch-up as a workaround for missing relay fanout.

## Upstream federation (session relay)

The session relay is a cache *and a queue*: what a local app publishes to
`relay.sock` is stored locally and forwarded to the user's own upstream
relays by a store-and-forward client inside the daemon
(`src/session/session_federation.c`, bead nostrc-7d96). A local app may
therefore publish **only** to the session relay (`session_relay_only`) and
still reach the network.

### Contract

1. **`OK true` from relay.sock means "held locally and queued upstream".**
   nostrdb stores the event, then it is committed to a durable outbox
   (`$XDG_DATA_HOME/nostr/session-relay/outbox.sqlite3`, SQLite WAL,
   `synchronous=FULL`), and only then is the OK sent. Nothing is queued —
   so nothing can leave — for an event the local store refused. If the
   outbox write fails the relay answers `OK false` (`error: store failed`)
   and the app retries (for a replaceable kind that exact event is then
   refused as stale, since the store already holds it: publish a fresh
   version). `OK true` is **not** "accepted upstream": ask
   `GetEventUpstream` (below) for that.

2. **What is never forwarded** (the event stays on this machine):
   - events carrying a NIP-70 `["-"]` tag — the local-only marker for
     apps. A protected event that should reach a remote must be published
     to that remote directly by the author;
   - NIP-17 seals (kind 13) and rumors (kind 14), NIP-42 AUTH (22242) and
     every ephemeral kind (20000–29999), whatever the config says;
   - kinds listed in `federation_local_only_kinds` (relay-side policy);
   - events not authored by a **local account** — caching another
     author's event in the session relay never rebroadcasts it (it is
     stored and answered `OK true`). With `federation_accounts` set it is
     not queued at all (`unknown`; no outbox write, so bulk cache writes
     cost no fsync); with accounts learned from `org.nostr.Signer` it is
     queued and settles `skipped` once the signer has answered (an unknown
     key may still turn out to be an account the signer reports later). Relay lists (10002 / 10050 / 10009) of anyone are
     always remembered for routing (without an fsync: they are read back
     from the store if lost). When `federation_accounts`
     is set it is the complete, authoritative list. When it is empty, the
     local account is whatever `org.nostr.Signer.GetPublicKey` reports,
     asked lazily when an event by an unknown author is waiting; every key
     the signer has reported is remembered in the outbox (to forget one,
     set `federation_accounts` or delete `outbox.sqlite3`). While no
     account is known, queued events wait (`unroutable`,
     `FederationState` `waiting-for-account`) and never expire.
     Kind-1059 gift wraps are signed by throw-away keys and are exempt
     from the author check — so a wrap an app caches from the user's own
     inbox relays is sent back to them (answered `duplicate:`): it cannot
     be told apart from the user's self-copy until clients can mark cache
     writes (nostrc-pw0d).

3. **Where events go** — only relays derived from the user's own data;
   there is no built-in, default or fallback relay:

   | event | targets |
   |---|---|
   | default (kind 1, 0, 3, 30023, …) | the author's kind-10002 **write** relays (`r` tags marked `write` or unmarked) |
   | kind 10002 itself | the write relays it names |
   | NIP-09 kind 5 | the author's write relays, plus every relay that acknowledged a deleted event (`e`/`a` tags; delivery records are kept ~400 days) |
   | NIP-29: kinds 9–12, 9000–9030, 39000–39005, and **any other kind carrying an `h` tag** (NIP-29 "normal user-created events": a kind-1 note, a kind-30023 article, a kind-5 deletion… sent to a group) — except NIP-17 kinds, ephemeral kinds and user-level replaceable state (0, 3, 10000–19999, e.g. the kind-10009 group list), which keep their own row | the group's relay only, never the home relays. A group is **(relay, id)** (NIP-29 forks share the id on other relays): the relay named in the `h` tag (`["h", id, relay]` — a nostrc extension, not a NIP-29 shape; relays ignore the extra element), else the author's kind-10009 `["group", id, relay]` entry when it is the only one for that id. Several entries for the id (the user is in several forks) → `unroutable` until the event names one; no entry → `unroutable`. No relay is derived from a `host'id` value (the `h` tag carries the bare id) |
   | kind 1059 gift wrap | the recipient's (`p` tag) kind-10050 `relay` list; never the sender's relays, never 10002 as a fallback |

   A write for a group is never moved to another relay for the same id:
   if the group relay stays unreachable it is retried until
   `federation_max_age_seconds` (the publisher sees the per-relay errors).
   Finding out whether a group moved or forked is the client's job
   (NIP-29: consult the admins' kind 10009) — then republish with the
   relay named in the `h` tag.

   Relay lists are read from the session relay itself: the relay-list
   events (10002 / 10050 / 10009, anyone's) that apps write to relay.sock
   are remembered, newest `created_at` wins. An event whose targets are not
   known yet stays **unroutable** and is re-routed as soon as the list it
   waits for arrives — the author's 10002, the recipient's 10050, the
   author's 10009 — (and with backoff meanwhile), e.g. write the
   recipient's 10050 before the gift wrap, or the event waits. Pending
   deliveries follow relay-list changes: when a newer 10002 (or a
   recipient's newer 10050) arrives, events routed from the old list stop
   waiting on relays the new one dropped (their target becomes
   `cancelled`, reason `dropped from the relay list`) and gain targets on
   relays it added; acknowledged and failed deliveries stay as they are.
   Group writes keep their relay (a group is (relay, id); see below).
   URLs must be `wss://`, or `ws://` to a loopback host
   (`federation_allow_plaintext_ws = 1` lifts that); at most
   `federation_max_relays_per_event` targets.

4. **Delivery.** Each (event, relay) pair is tracked separately: one
   relay's rejection does not stop the others. NIP-01 OK classes:
   `OK true` / `duplicate:` → acked; `invalid:` `blocked:` `banned:`
   `restricted:` → failed for that relay; anything else, timeouts and
   connection loss → retried with exponential backoff (±20 % jitter,
   `federation_backoff_initial_seconds` … `federation_backoff_max_seconds`)
   until `federation_max_age_seconds` after queueing, then failed
   (`expired: …`). Delivery is at-least-once, keyed by event id; queued
   work survives restarts (an attempt interrupted by a crash is retried
   after its lease). A newer version of a replaceable / addressable event
   supersedes a still-queued older one; a kind 5 by the author cancels
   still-queued events it deletes.

5. **NIP-42 AUTH.** When a relay answers `auth-required:`, the session
   relay signs the kind-22242 challenge response **through
   `org.nostr.Signer`** (app id `nostr-session-relay`; the relay holds no
   keys), then resends. AUTH is lazy (only after an `auth-required:`
   refusal). Signing happens off the delivery path: while the signer
   prompts (up to 30 s), other connections keep delivering, and the
   `GetPublicKey` that learns the local account does not block them
   either. **Gift wraps are never delivered over an authenticated
   connection**: authenticating as the user would tell the inbox relay
   who sent the wrap, which NIP-17's throw-away keys exist to hide; wraps
   use separate, never-authenticated connections, and an inbox relay that
   demands AUTH for writes fails that delivery with the reason prefix
   `auth-refused-for-gift-wrap:` (a deliberate privacy choice, not a
   fault; NIP-17 inbox relays that require AUTH for *writes* cannot receive
   wraps from this relay).

6. **Observability** (`org.nostr.SessionRelay1`, additive):
   - properties `FederationState` (`active`, `waiting-for-account`,
     `disabled`, `unavailable`), `PendingUpstream`, `ForwardedCount`,
     `FailedUpstream`, `LastUpstreamError`;
   - `GetStats` keys `federation_*`, `pending_upstream*`,
     `forwarded_count`, `partially_forwarded_count`, `failed_upstream`,
     `skipped_upstream`, `last_upstream_error`,
     `upstream_relays_connected` (so `--stats` prints them);
   - `GetEventUpstream(id) → (state, detail, [(relay, relay_state,
     reason, attempts, updated_at, acked_at)])` — event states `new`,
     `unroutable`, `pending`, `forwarded`, `partial`, `failed`, `skipped`,
     `superseded`, `cancelled`, or `unknown`. `unknown` means "not in the
     outbox": never queued (local-only by rule 2, or never stored here) *or*
     settled and pruned after `federation_keep_settled_seconds` — the two
     are deliberately not distinguished;
   - `GetUpstreamRelays()` — per relay: connected, authenticated, pending,
     acked, failed, last_error, last_ok_at;
   - signal `UpstreamStatusChanged(event_id, relay_url, relay_state,
     reason, event_state)` on every transition;
   - CLI: `nostr-session-relayd --upstream [EVENT_ID]`.

7. **Retention interplay.** Eviction is not implemented (nostrc-8rxk,
   nostrc-prqu.17). The hook is `nsr_outbox_eviction_eligible()`
   (`src/session/session_outbox.h`): an evictor must keep any event for
   which it is FALSE (still owed upstream). Forwarded-and-acknowledged
   events are the eligible ones; local-only events are never "safe
   elsewhere" and need their own policy.

### For publishers (nostr-dav, nostr-share, nostr-notify, …)

`session_relay_only` is now a real mode: publish the signed event to
relay.sock; `OK true` = durably queued. Then follow the event id with
`UpstreamStatusChanged` (or poll `GetEventUpstream`):

| event state | meaning for the publisher |
|---|---|
| `new`, `pending` | in progress |
| `unroutable` | waiting: no relay list yet (write the author's 10002 / recipient's 10050 / user's 10009 to the relay), or no local account yet (`FederationState` `waiting-for-account`) — see `detail` |
| `forwarded` | every target relay acknowledged — published |
| `partial` | some targets acknowledged, others failed — published, with per-relay failures |
| `failed` | no target accepted (rejected / expired / invalid, e.g. gift wrap without `p`) |
| `skipped` | terminal: held locally, deliberately not forwarded (not a local account's event) |
| `superseded`, `cancelled` | terminal: replaced by a newer version / deleted by a kind 5 before it went out |
| `unknown` | never queued (rule 2 local-only, or not stored here) or pruned |

Check `FederationState` first: `active` / `waiting-for-account` forward;
`disabled` (by the user) or `unavailable` (an older daemon, a build
without libnostr-publish, a cache-less relay) do not, and
`session_relay_only` must then not be used.

### Configuration (`session-relay.conf`)

| key | default | |
|---|---|---|
| `federation` | `1` | `0` turns forwarding off (`FederationState` `disabled`) |
| `federation_accounts` | *(empty)* | comma-separated npub / hex; empty = ask `org.nostr.Signer` |
| `federation_local_only_kinds` | *(empty)* | e.g. `30078, 31000-31999` |
| `federation_allow_plaintext_ws` | `0` | allow `ws://` to non-loopback hosts |
| `federation_backoff_initial_seconds` | `15` | first retry delay |
| `federation_backoff_max_seconds` | `3600` | retry delay cap |
| `federation_ok_timeout_seconds` | `30` | connect + OK deadline per attempt |
| `federation_max_age_seconds` | `604800` | give up after this long |
| `federation_keep_settled_seconds` | `604800` | keep settled outbox rows for status queries |
| `federation_max_relays_per_event` | `16` | target cap |
| `federation_max_inflight_per_relay` | `32` | EVENTs awaiting OK per connection |
| `federation_idle_disconnect_seconds` | `60` | close idle upstream connections |

A bad value for a `federation_*` key (or an unknown `federation_*` key) is
a config error: the daemon refuses to start, like any other invalid key.

### Build

Federation is compiled when the tree builds `gnome/libnostr-publish`
(`ENABLE_LIBNOSTR_PUBLISH`, implied by `ENABLE_NOSTR_DAV` / `…_SHARE` /
`…_WALLET_AGENT`) and gio-2.0 + sqlite3 are found; otherwise the daemon
builds as before and reports `FederationState` `unavailable`. `wss://`
needs a GIO TLS backend at runtime (`glib-networking`). `nostr-authd` /
`pam_nostr.so` gain nothing (`scripts/check-authd-dep-purity.sh`).

### Tests

`tests/test_session_fed_policy.c` (contract, routing per kind/tag, URLs,
config, backoff), `tests/test_session_outbox.c` (state machine,
durability), `tests/test_session_federation.c` (engine vs fake remotes:
accept / reject / AUTH / dropped connection / gift-wrap lane / restart),
`tests/test_session_relay_federation.c` (packaged daemon end to end over
relay.sock and D-Bus with a fake `org.nostr.Signer`).
