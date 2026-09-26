# nostr-dav publish live smoke — nostrc-dsls

- **When**: 2026-09-26
- **Where**: aarch64 lab `bizarro@192.168.64.3` (Ubuntu 24.04, kernel 7.0.0)
- **Target relay**: `wss://relay.sharegap.net`
- **Bead**: [nostrc-dsls](../../beads/incoming) (follow-up to `nostrc-tu6y`)
- **Build**: `feat/signer-fuzz-and-dav-live-smoke` at `4379d34f` +
  local edits for the tf3b (fuzz + real-service D-Bus contract test) work.

## What was exercised

The Wave-3 landing (`d6c93d53`) flipped nostr-dav's publish path onto
the real libsoup 3 WebSocket transport and turned `enable_publish` ON by
default. That landing was tested on a macOS dev host where the D-Bus
session-bus signer cannot be driven, so the actual end-to-end round-trip
against a real relay was never observed. This smoke drives it from a
Linux headless lab that speaks the whole story:

1. `nostr-signer-daemon` on the user session bus, key material stored
   via libsecret (real GNOME Keyring, not a mock).
2. `nostr-dav` boots against `~/.config/nostr-dav/nostr-dav.conf` with
   `home_relays=wss://relay.sharegap.net` and `enable_publish=true`.
3. Three DAV writes — a kind-31922 date-based calendar event, a
   kind-31923 time-based occurrence, a kind-30085 contact vcard — all
   over the standard WebDAV endpoints.
4. Publisher tick moves each row from `pending` to `published` after the
   sharegap relay ACKs.
5. Independent NIP-01 REQ from the same host reads the events back off
   sharegap and confirms authorship (pubkey), kind, and `d` tag.
6. A DAV DELETE on the kind-31922 UID stages a kind-5 tombstone.
7. Independent NIP-01 REQ for `{kinds:[5], authors:[pk]}` observes the
   tombstone; the address-replaceable kind-31922 read after the delete
   confirms sharegap surfaces the newest state (replacement-by-kind-5).

## Setup snapshot

Sourced from `~/nostr-dav-smoke.<timestamp>.log` — the raw transcript.
Reproduce with:

```bash
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
/tmp/nostr-dav-smoke.sh
```

`nostr-dav.conf` (written by the smoke, then verified):

```ini
[nostr-dav]
nostr_dav_upstream_mode=direct_only
enable_publish=true
nostr_dav_publish_quorum=1
account_pubkey=<PK_HEX>
home_relays=wss://relay.sharegap.net
```

## Transcript excerpts

Full log: `~/nostr-dav-smoke.20260926070919.log` on the lab (mirrored
into this repo's paste below). Highlights:

### DAV surface is healthy

```
$ curl -sS -u nostr:$TOKEN -X PROPFIND -H 'Depth: 1' \
       http://127.0.0.1:7680/calendars/nostr/
… <?xml version="1.0" encoding="UTF-8"?><D:multistatus …>
HTTP 207

$ curl -sS … -X PUT -H 'Content-Type: text/calendar' \
       --data-binary @smoke-31922.ics …/calendars/nostr/smoke-1790406563.ics
HTTP 201

$ curl -sS … -X PUT … …/calendars/nostr/smoke-occ-1790406563.ics
HTTP 201

$ curl -sS … -X PUT -H 'Content-Type: text/vcard' \
       --data-binary @smoke-30085.vcf …/contacts/nostr/smoke-contact-1790406563.vcf
HTTP 201

$ curl -sS … -X DELETE …/calendars/nostr/smoke-1790406563.ics
HTTP 204
```

`nostr-dav`'s own log confirms it accepted the writes and made a
libsoup TCP+TLS connection to `relay.sharegap.net`:

```
** Message: nostr-dav: created calendar event smoke-1790406563 (nostrc-dsls smoke)
** Message: nostr-dav: created calendar event smoke-occ-1790406563 (nostrc-dsls occurrence)
** Message: nostr-dav: created contact smoke-contact-1790406563 (Smoke Contact)
… GLib-GIO-DEBUG: g_socket_client_connect_async: … url=https://relay.sharegap.net:443 …
… GLib-GIO-DEBUG: GSocketClient: TCP connection successful
… GLib-GIO-DEBUG: GSocketClient: Connection successful!
```

### Publisher tick reaches the signer but the D-Bus call rejects

```
sqlite> SELECT uid, kind, publish_state, publish_attempts, publish_targets FROM events;
smoke-1790406563      31922  pending  1  ["wss://relay.sharegap.net"]
smoke-occ-1790406563  31923  pending  1  ["wss://relay.sharegap.net"]

sqlite> SELECT id, target_kind, target_uid, http_status, error FROM publish_log;
1  0  smoke-1790406563           0  SignEventJson: GDBus.Error:org.freedesktop.DBus.Error.UnknownMethod: No such method “SignEventJson”
2  0  smoke-occ-1790406563       0  SignEventJson: GDBus.Error:org.freedesktop.DBus.Error.UnknownMethod: No such method “SignEventJson”
3  1  smoke-contact-1790406563   0  SignEventJson: GDBus.Error:org.freedesktop.DBus.Error.UnknownMethod: No such method “SignEventJson”
```

Root cause is in-tree: `gnome/nostr-dav/src/nd-signer-dbus.c:20` sends
`org.nostr.Signer.SignEventJson (in s, out s)` but the daemon —
generated from `nips/nip55l/dbus/org.nostr.Signer.xml` — exports
`SignEvent (in s eventJson, in s identity, in s app_id, out s
signed_event)`. Both the method name and the argument tuple diverge, so
the publisher hits `Error.UnknownMethod` on every attempt.

### Independent NIP-01 REQ on the relay confirms nothing landed

```
kind-31922: got 0 events for kinds=[31922]
kind-31923: got 0 events for kinds=[31923]
kind-30085: got 0 events for kinds=[30085]
kind-31922 post-tombstone: 0 events remain
```

### DAV DELETE removes local row but stages no kind-5 publish

After the DELETE the row is gone from `events`, and there is no new
entry in `publish_log` for a kind-5 tombstone — the DELETE handler
(`gnome/nostr-dav/src/nd-dav-server.c:697-712`) calls
`nd_calendar_store_remove` and then simply responds `204`, without a
matching `nd_publisher_stage_*_delete`. Remote copies would be
orphaned even if the SignEventJson bug were fixed.

### Aside: signer GetPublicKey against a just-stored key

The smoke stored a key via `nostr-signer-cli store-key` (returned
`ok npub1qvdm4j0…`), then immediately called `get-pubkey` — which
returned `Error.NoKeyConfigured`. The `nostr-signer-cli store-key`
path writes through libsecret with the same identity the daemon's
`resolve_seckey_hex(NULL, …)` searches (`owner_uid=<uid>`), so the
absence is unexpected. Not in the scope of this smoke (it's an
independent nip55l lane, tracked separately below).

## Outcome

The Wave-3 landing (`nostrc-tu6y`) declared "publish default ON" ready
for real relays. On this lab against `wss://relay.sharegap.net`:

- DAV read/write layer: **green** end-to-end (PROPFIND, PUT, DELETE).
- Publisher outbox lifecycle: **red** — every event fails at the
  D-Bus signer call because nostr-dav asks for a method the signer
  daemon does not export.
- Relay-side observation: **red** — nothing publishes, so nothing
  lands. Confirmed by an out-of-band `websockets` REQ query.
- Tombstone-on-DELETE: **red** (independent of the signer bug) — the
  DELETE handler never stages a kind-5 publish.

The smoke did what it was set up to do — it turned a green Wave-3
declaration into concrete evidence that end-to-end publish against a
real relay is currently non-functional. Two blocker beads filed
below; `nostrc-dsls` itself closes with the evidence linked.

## Follow-ups filed

- **`nostrc-qqkw`** (P1) — `nostr-dav publisher calls SignEventJson but
  signer exports SignEvent`. Blocks all outbound publish. One-line
  fix in `gnome/nostr-dav/src/nd-signer-dbus.c` + arg tuple update,
  or (nicer) generate the client from the same `org.nostr.Signer.xml`
  the daemon does.
- **`nostrc-ls2c`** (P2) — `DAV DELETE removes local row but never stages
  a kind-5 tombstone publish`. `handle_delete_event` /
  `handle_delete_contact` / `handle_delete_file` need to call a new
  `nd_publisher_stage_*_delete` alongside the local removal.
- **`nostrc-7g9d`** (P2) — `nostr-signer-daemon GetPublicKey returns
  NoKeyConfigured for a key it just stored via StoreKey` (observed as
  an aside; separate lane from Wave-3 publish).

