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
     stored, `OK true`, and settles `skipped`). When `federation_accounts`
     is set it is the complete, authoritative list. When it is empty, the
     local account is whatever `org.nostr.Signer.GetPublicKey` reports,
     asked lazily when an event by an unknown author is waiting; every key
     the signer has reported is remembered in the outbox (to forget one,
     set `federation_accounts` or delete `outbox.sqlite3`). While no
     account is known, queued events wait (`unroutable`,
     `FederationState` `waiting-for-account`) and never expire.
     Kind-1059 gift wraps are signed by throw-away keys and are exempt
     from the author check.

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
