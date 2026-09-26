# Native Evolution Data Server backends for Nostr calendar and contacts

**Status:** Design. **No implementation in this document.**
**Bead:** `nostrc-00i0` (child of epic `nostrc-prqu`). Implementation beads are the W-items in §16; none are filed until the §17 answers land.
**Reads:** `docs/investigations/gnome-gtk-nostr-planned-not-implemented-2026-09-26.md` (§3 item 4), `docs/plans/gnome-integration-and-samba-server-2026-09-25.md` (Calendar + Contacts track, "Path A confirmed", D7), `gnome/nostr-dav/` (all sources, headers, `docs/QUICKSTART.md`, `SECURITY.md`), `gnome/libnostr-publish/include/nostr-publish/*.h`, `nips/nip55l/dbus/org.nostr.Signer.xml`, `docs/reviews/nostr-dav-publish-live-2026-09-26.md`, `docs/designs/packaging-plan-debian-fedora.md`.
**Sibling:** `docs/designs/gvfs-nostr-backends.md` (`nostrc-jaxi`, written concurrently) owns the NIP-94 / Blossom *files* story. This document cross-references it and does not touch it.
**Date:** 2026-09-26 · **Repo state:** `master` at `28e82491`

> Decision numbering in this document (**D1…D24**) is local. The plan's earlier
> Path-A decision is written **PLAN-D7** where referenced. Sizes in §16 use
> S ≤ 1 day, M = 2–4 days, L = 1–2 weeks, XL > 2 weeks of one engineer.

---

## 0. Executive summary

`nostr-dav` is a loopback CalDAV/CardDAV bridge: GNOME Calendar and Contacts
talk HTTP to `127.0.0.1:7680`, `nostr-dav` translates to NIP-52 events and a
custom contact kind, and a SQLite store + relay-sync + outbox publisher behind
it does the Nostr work. That back half — `nd-store-db`, `nd-calendar-store`,
`nd-contact-store`, `nd-ical`, `nd-vcard`, `nd-relay-sync`, `nd-publisher`, and
`libnostr-publish` underneath — is the part worth keeping. The front half — a
libsoup server, libxml2 DAV, a bearer token on disk, a stock WebDAV account
that the user must paste a password into — exists only because GNOME Online
Accounts cannot load out-of-tree providers (`2cf193a7` removed ours for exactly
that reason) and because CalDAV/CardDAV was the only account type available
without one.

This design replaces the front half with **three Evolution Data Server
modules** that run inside EDS's own host processes:

| Module | Process | Class | Does |
| --- | --- | --- | --- |
| `module-nostr-backend.so` | `evolution-source-registry` | `ECollectionBackend` | Creates the "Nostr" account source from the signer's identity, discovers kind-31924 calendars and materialises one child calendar source per calendar plus the address book. |
| `libecalbackendnostr.so` | `evolution-calendar-factory-subprocess` | `ECalMetaBackend` | NIP-52 kinds 31922/31923 ↔ VEVENT, 31924 ↔ calendar membership, 31925 ↔ `ATTENDEE;PARTSTAT` with iTIP semantics, kind 5 ↔ deletion. |
| `libebookbackendnostr.so` | `evolution-addressbook-factory-subprocess` | `EBookMetaBackend` | kind 3 follows as the contact set, kind 0 profiles as the card, kind 30000 follow sets as categories, ↔ vCard 4.0. |

Each module embeds the same core (`libnostr-dav-core`, split out of today's
static `nostr-dav-core`) on a private owner thread, and publishes through
`libnostr-publish` with the NIP-65 all-ACK outbox commit that `nostr-dav` uses
today. There is no daemon, no HTTP, no token, no password, and no nsec:
identity comes from `org.nostr.Signer.GetPublicKey`, signatures from
`SignEvent`, approval prompts from the signer's own UI. Offline cache, change
notification, refresh scheduling, and offline-edit replay come from
`ECalMetaBackend`/`EBookMetaBackend` for free, which is the concrete reason to
prefer the meta-backend base classes over raw `ECalBackend`.

What the user gets that the bridge cannot give: invitations and RSVPs that
Evolution renders as meetings with the attendee list and each attendee's
status (§5.4 — whether Evolution's Accept/Decline chrome appears without an
iTIP message is the first thing W5 verifies), local alarms that survive sync
(§5.6, `evolution-alarm-notify` fires), a follow list that *is* the address
book with follow/unfollow as add/remove (§6), push-style updates instead of a
PROPFIND poll, no per-user loopback port (two users on one host cannot both run
`nostr-dav` on `7680` today), and an account that appears by itself and is
enabled with one switch (D5).

The honest caveats, stated up front: EDS backends bind to the distro's EDS ABI
and rebuild on every EDS soname bump (§14); they cannot ship in a Flatpak
(§10); NIP-52 has no recurrence, no invitation delivery, and no version counter
(§12), so recurring events are refused rather than faked (D13) and invitations
only reach participants whose read relays we publish to (D19 follow-up). The
bridge's one real advantage — non-GNOME DAV clients — is lost when it is
deleted, and §17 asks the maintainer whether that matters (question 4).

Reading the current tree also surfaced three defects the shared core must fix
before either consumer can rely on it (§1, findings 1–3): `nd-relay-sync` never
sends a `REQ`, local writes are stored with `created_at = 0`, and `nd-ical`
emits an RFC-5545-invalid `TZID` + `Z` combination. They are W1 work, and they
fix `nostr-dav` too.

---

## 1. Ground truth — what exists today

Read 2026-09-26 on `master` (`28e82491`).

| Area | File | Reality |
| --- | --- | --- |
| Daemon | `gnome/nostr-dav/src/nd-application.c` (480) | `GApplication` service `org.nostr.Dav`, `G_APPLICATION_IS_SERVICE`. Loopback `127.0.0.1:7680` fixed at compile time. Startup order lock → config → token → store → account → listen. Builds the signer proxy as `nd_signer_new_dbus(bus, "nostr-dav")`, then `NdPublisher` **before** `NdRelaySync` so `session_transport_factory` can bind each transport into the publisher's send cache and route OK frames back. NIP-42 AUTH answered via `nostr_publish_signer_sign_auth_event`. Outbox tick every 30 s on the default main context. |
| DAV front | `src/nd-dav-server.c` (1753) | libsoup 3 `SoupServer` + libxml2. `OPTIONS`/`PROPFIND`/`REPORT`/`GET`/`PUT`/`DELETE` for `/calendars/nostr/`, `/contacts/nostr/`, `/files/nostr/`, `.well-known` redirects, principal. HTTP Basic whose password is the bearer token (constant-time compare). `PUT` stages `nd_publisher_stage_*_put`; `DELETE` stages a kind-5 tombstone (`nostrc-ls2c`). |
| Token | `src/nd-token-store.c` (426) | 256-bit token at `$XDG_CONFIG_HOME/nostr-dav/token`, 0600 in 0700, refuses to start on looser modes; optional libsecret mirror. Exists only to authenticate the loopback HTTP hop. |
| Store | `src/nd-store-db.c` (725) | SQLite WAL, 0600, `$XDG_DATA_HOME/nostr-dav/store.sqlite`, `SQLITE_OPEN_FULLMUTEX`, integrity check + quarantine-and-restart. Schema v4: `events`, `contacts`, `files` (each with `publish_state ∈ {idle,pending,published,failed_permanent,superseded}`, `publish_attempts`, `publish_next_ts`, `publish_targets` JSON, `signed_event_json`), `publish_log`, `relay_cursor(relay_url, since_ts)`, `tombstones`, `store_generation` (ctag). |
| Stores | `src/nd-calendar-store.c` (218), `src/nd-contact-store.c` (225) | put/get/remove/list_all/count/ctag over the tables above. **Finding 2:** `nd_calendar_store_put` writes `created_at = event->created_at`, which is **0 for every DAV write** (nothing stamps it). Consequences: the relay-sync fold guard `excluded.created_at > events.created_at` lets *any* inbound copy overwrite a local non-pending row, and the ETag hash `(kind, pubkey, uid, created_at)` never changes on edit (`nostrc-ir7c`). |
| iCal mapping | `src/nd-ical.c` (672) | Hand-rolled VEVENT subset (UID, SUMMARY, DESCRIPTION, DTSTART/DTEND, LOCATION, CATEGORIES, URL, GEO). Rejects RRULE/RDATE/EXDATE. NIP-52 tags `d title start end start_tzid end_tzid location t r g`; **no `p` participants, `summary`, `image`, labels; no 31924, no 31925.** **Finding 3:** generator emits `DTSTART;TZID=<tzid>:<YYYYMMDDTHHMMSS>Z` — RFC 5545 §3.3.5 forbids `TZID` on the UTC form; libical-based clients treat it as malformed or ignore the TZID. `nd_ical_event_to_nip52_json` stamps `created_at = time(NULL)` at sign time rather than the row's value. |
| vCard mapping | `src/nd-vcard.c` (445) | vCard 4.0 subset ↔ **kind 30085, an application-specific kind**; `content` is the full raw vCard **in plaintext**, tags `d name t=contact p`. Personal contact data (TEL, ADR, NOTE) is published as a public relay event. `nd_vcard_to_nostr_json` stamps `created_at = time(NULL)` at sign time — the same class of defect as finding 2; W1's fix covers both generators. |
| Relay sync | `src/nd-relay-sync.c` (754) | Fold for 31922/31923/30085/5: last-writer-wins by `created_at`, `pending` rows protected, LRU dedup of 4096 ids, kind-5 accepted only from `account_pubkey` (`fold_deletion` — the gate W1 item 4 replaces; the `a`-tag kind is parsed with `atoi`, silently 0 for non-numeric input, so W1 uses a strict parse), `e`- and `a`-tag targets, cursor stamped on every ingest (not on EOSE; `nostrc-tlp2`). **Finding 1: it never sends a `REQ`.** `nd_relay_sync_start()` only calls `connect_async`; no state callback is installed and no `["REQ",…]` frame exists anywhere in `gnome/nostr-dav/src/` or `gnome/libnostr-publish/src/`. The header's `{kinds:[31922,31923,30085,5], authors:[…]}` filter is documentation. Inbound sync is exercised only via the fixture transport in `test_relay_sync`. |
| Publisher | `src/nd-publisher.c` (1090) | Drains `pending` rows: builds the unsigned event from the stored iCal/vCard, signs **before** persisting (`signed_event_json`, so a retry never re-prompts), hands the signed JSON to `NostrPublisher` with `home_relays` as the target set and `quorum.all` → policy `quorum = 0` (NIP-65 all-ACK). Verdicts: PUBLISHED → `published`; FAILED_PERMANENT (signer denied, `invalid:`/`blocked:`/`banned:`/`restricted:`/`auth-required:`) → `failed_permanent` + one notification; RETRY → `min(60·2^n, 3600)` s. Tombstone outbox shares the code path. Target set is config `home_relays`; NIP-65 discovery is `nostrc-lyvz`; `upstream_mode` is stored but not enforced (`nostrc-862u`). |
| Publish core | `gnome/libnostr-publish/` → `libnostr-publish.so.0` (Debian `libnostr-publish0`) | `NostrPublishSigner` (D-Bus `org.nostr.Signer.SignEvent(eventJson, identity, app_id)` on `/org/nostr/signer`, or a vtable double), `NostrPublishTransport` (libsoup 3 WebSocket; `_new_websocket_unix(url, path)` for the session relay at `$XDG_RUNTIME_DIR/nostr/relay.sock`; in-process fixture with `deliver_frame`/`take_sent`), `NostrPublishPolicy` (quorum, OK-reason classification, `nostr_publish_nip65_relays()` for kind 10002, `nostr_publish_policy_select_targets()` for upstream mode), `NostrPublisher` engine. Not thread-safe: one main context. |
| Signer | `nips/nip55l/dbus/org.nostr.Signer.xml` | `GetPublicKey() → npub`, `SignEvent(sss) → s`, `NIP44*`, `NIP44DeriveConversationKey`, `GetRelays() → json` (user's configured relays, never network-fetched), `StoreKey`/`ClearKey`, `ApproveRequest`, signals `ApprovalRequested(app_id, identity, kind, preview, request_id)`/`ApprovalCompleted`. ACL keyed on `app_id`. `nostr-share` (`gnome/nostr-share/src/ns-net.c:134`) already uses `GetPublicKey` as the signer liveness probe and `GetRelays` as a relay-list fallback. |
| GOA | `2cf193a7` | Deleted `gnome/goa/`: `goa-daemon` registers providers statically in `ensure_builtins_loaded()`; an out-of-tree `GoaProvider` can never load. Today's only account UX is the stock WebDAV account plus a token paste, and the signer's `sheet-online-accounts.c` wizard that automates it (`nostrc-0e7k`). |
| Build | `gnome/nostr-dav/CMakeLists.txt` | `nostr-dav-core` is a **STATIC** library that includes the DAV server and token store alongside the store/sync/publish core. `ENABLE_NOSTR_DAV` OFF by default; implies `ENABLE_LIBNOSTR_PUBLISH`. Deps: glib ≥ 2.60, libsoup-3.0, libxml-2.0, json-glib, sqlite3 ≥ 3.24, optional libsecret. |
| Packaging | `debian/control`, `debian/nostr-dav.install`, `packaging/rpm/nostr-login.spec` | Debian `nostr-dav` (Section `admin`, Recommends `gnostr-signer-daemon`, `nostrc-session-relay`) ships the daemon, two user units, the D-Bus activation file, the conf sample. `libnostr-publish0` ships the `.so.0`. The RPM spec is the headless set only; GNOME packages are packaging-plan Phase 4. |
| Tests | `gnome/nostr-dav/tests/` | `test_relay_sync` (6: ingest, dedup, last-writer-wins, tombstone-e, tombstone-a, tombstone-stranger), `test_publish_rollback` (5: permanent-reject-notifies-once, transient-then-success, signer-denial, ok-wait-timeout, dav-delete-stages-kind5), `test_store_sqlite` (5, one of which drives the HTTP harness), `test_dav_propfind` (DAV + `/ical/*` ×6 + `/vcard/*` ×5 + `/nip94/*` ×3), `test_auth_required`, `test_relay_transport_libsoup`. Harness `nd-test-harness.[ch]` spawns a real `NdDavServer` on a thread. |
| Reusable fixtures | `gnome/nostr-homed/tests/integ/fake_relay_fixture.py`, `gnome/nostr-homed/tests/integration/mock_signer.c` | Python NIP-01 relay: `REQ`/`CLOSE`/`EVENT`, `EOSE`, `OK` with `blocked:`/`invalid:`/`auth-required:` reasons, optional NIP-42 `AUTH`, filters on `kinds`/`authors`/`#x`, pre-seeded from JSON; exits 77 when `websockets` is missing. C mock of `org.nostr.Signer` exporting `GetPublicKey`, `SignEvent` (real BIP-340 with a fixed public test key), `NIP44Encrypt/Decrypt` (marker transform). Neither exports `GetRelays`. |
| NIP libs | `nips/nip52`, `nip02`, `nip51`, `nip05`, `nip19` | `nip52.h`: `CalendarEvent{kind, identifier, title, image, start, end, locations[], geohashes[], participants[]{pub_key, relay, role}, references[], hashtags[], start_tzid, end_tzid}` — no 31924/31925. `nip02.h`: build/parse/append kind-3 follow lists with petnames. `nip51.h`: kind-30000 people lists (`identifier`, `title`). `nip05.h`: parse, fetch (own HTTPS), `resolve_from_json`, `validate`. `nip19.h`: `nostr_nip19_decode_npub`. All on the GLib-free `libnostr` ABI. |
| EDS | (nothing in-tree references EDS) | Target versions: Ubuntu 24.04 → 3.52, Fedora 40 → 3.52, Fedora 41 → 3.54, Debian 13 → 3.56. Modules: `libedata-cal-2.0` (`pkg-config --variable=backenddir` → `<libdir>/evolution-data-server/calendar-backends`), `libedata-book-1.2` (`…/addressbook-backends`), `libebackend-1.2` (`--variable=moduledir` → `…/registry-modules`). `ECalMetaBackend`/`EBookMetaBackend` exist since 3.26; the `libecal-2.0`/`ICalComponent` API landed well before 3.52 (3.34–3.36; irrelevant at our floor). |

Four consequences shape everything below:

1. **The core is real and tested; the front is the part with the bugs and the
   friction.** Everything that touches a relay or the signer lives in files that
   have no libsoup-server or libxml2 dependency. The split in D2 is a file move,
   not a rewrite.
2. **Findings 1–3 mean the inbound path has never run against a relay.** The
   design cannot assume "relay sync works, just reuse it". W1 specifies the
   `REQ`/EOSE/cursor state machine explicitly (D9) and it lands in the shared
   core so `nostr-dav` inherits it.
3. **Kind 30085 is not something to carry forward.** It publishes plaintext
   personal data. The EDS book is the *follow graph* (kind 3 ∩ kind 0), not a
   personal address book, and private contacts are deferred to an encrypted
   design (D18).
4. **Identity plumbing already exists** (`GetPublicKey`, `GetRelays`, session
   relay socket, app_id-keyed ACL). The registry module reuses it rather than
   adding any account store of its own (D5).

---

## 2. Why native EDS vs the loopback DAV bridge

PLAN-D7 chose the bridge on 2026-09-25 for five reasons: any DAV client works,
crash isolation, no EDS ABI bind, a `curl`-able boundary, and GNOME 46's stock
WebDAV account. All five are still true. What changed in one day of reading:
the bridge's front is where the release blockers were (auth bypass, port drift,
token wizard — all fixed since, all front-half), the back half was extracted
into `libnostr-publish` and proved on a live relay, GOA is confirmed gone, and
the bead itself (`nostrc-00i0`) exists because the investigation found that
"invitations as real invitations" and "follows as contacts" are not expressible
over CalDAV/CardDAV at all.

| Dimension | Loopback DAV bridge (`nostr-dav`) | Native EDS backends (this design) |
| --- | --- | --- |
| Clients | Any CalDAV/CardDAV client: GNOME (via EDS's `caldav`/`carddav`), Thunderbird, KDE, macOS, DAVx⁵ over a TLS proxy | GNOME/EDS consumers only: Evolution, GNOME Calendar, GNOME Contacts, `evolution-alarm-notify`, Shell search over EDS, libfolks |
| Account setup | Start unit → `nostr-dav --show-credentials` → Settings → Online Accounts → WebDAV → paste URL/user/token (or the signer wizard does it) | Signer has a key ⇒ a *disabled* "Nostr" account appears (D5); one switch enables it — the signer's Online Accounts sheet, `nostr-eds-account enable`, Evolution's account editor, or `org.nostr.Settings` (`nostrc-janr`) |
| Credentials on disk | 256-bit bearer token file + libsecret mirror; anyone with the token has full R/W | Nothing. Signer approval gates every signature (D7) |
| Change propagation | Client polls ctag on the `ESourceRefresh` interval (EDS default 30 min), then PROPFIND depth 1, then multiget | Backend folds relay `EVENT`, calls `schedule_refresh`; UI updates within seconds (D8) |
| Invitations / RSVP (31925) | Impossible. CalDAV scheduling would need `schedule-inbox`/`outbox`, iMIP or auto-schedule on the server; `nd-dav-server` implements none; `p` tags are dropped | `ATTENDEE;PARTSTAT` on the VEVENT, `ORGANIZER`, `save-schedules` capability, RSVP publishes 31925 (D12). Evolution renders a meeting; accept/decline works |
| Follows as contacts | Not expressible: CardDAV `PUT` of a vCard cannot mean "follow npub"; kind 3 is a whole-list replaceable event | `EBookMetaBackend::save_contact_sync` = follow (kind 3 RMW), `remove_contact_sync` = unfollow (D17) |
| Local-only data (VALARM, X-props) | Stripped by `nd-ical`'s parser; alarms set in GNOME Calendar are lost on next sync | Kept in a local overlay, spliced on load, never published (D14). `evolution-alarm-notify` fires |
| Multi-user host | Fixed port `7680` per user ⇒ second logged-in user's unit fails to bind | Per-user EDS processes, per-user store dirs |
| Crash isolation | A crash kills `nostr-dav`; EDS unaffected | A crash kills `evolution-calendar-factory-subprocess` (EDS restarts it; other backends in other subprocesses unaffected since 3.16's per-backend subprocess split). The registry module's blast radius is every account's source registry (§4.3), which is why it does the least work |
| ABI coupling | libsoup 3 only | `libedata-cal-2.0`, `libedata-book-1.2`, `libebackend-1.2`, `libecal-2.0`, `libebook-1.2`, libical-glib. Rebuild on EDS soname bump; distro packaging with a versioned dependency (§14) |
| Flatpak | Daemon is host-side; Flatpak Calendar reaches it over loopback with `--share=network` | Host-side only; Flatpak Calendar/Contacts reach host EDS over the session bus (§10) |
| Debuggability | `curl -u nostr:$TOKEN -X PROPFIND` | `e-cal-client`/`e-book-client` test tools, `EDS_DEBUG`, `CALDAV_DEBUG`-style env, `journalctl --user -u evolution-calendar-factory` |
| Code | 1753 lines of DAV server + 426 token + 480 app + 1696 lines of DAV tests | Three modules (estimated 2.5–3.5 kLOC) + shared core; DAV/token/app deleted at the end (§11) |
| Files (NIP-94) | `/files/nostr/` WebDAV collection | Out of scope here; GVfs backend (`docs/designs/gvfs-nostr-backends.md`) |
| Status | Shipped (Debian `nostr-dav`), live-smoked, findings 1–3 open | This document |

**Verdict.** Build the EDS backends on the shared core, coexist for one release,
then delete the DAV front. The bridge's *only* unrecoverable advantage is
non-GNOME DAV clients; §17 question 4 asks whether that is worth keeping a
second front alive for.

---

## 3. Architecture

### 3.1 Process map

```
 evolution-source-registry            evolution-calendar-factory-subprocess     evolution-addressbook-factory-subprocess
 ┌──────────────────────────┐         ┌──────────────────────────────┐          ┌──────────────────────────────┐
 │ module-nostr-backend.so  │         │ libecalbackendnostr.so       │          │ libebookbackendnostr.so      │
 │ ENostrCollectionBackend  │         │ ECalBackendNostr             │          │ EBookBackendNostr            │
 │  : ECollectionBackend    │         │  : ECalMetaBackend           │          │  : EBookMetaBackend          │
 │ - watches org.nostr.Signer│        │ - NeCore thread (D3)         │          │ - NeCore thread (D3)         │
 │ - GetPublicKey → account │         │ - libnostr-dav-core          │          │ - libnostr-dav-core          │
 │ - 31924 → child sources  │         │   store · relay-sync · outbox│          │   store · relay-sync · outbox│
 │ - registry.sqlite        │         │ - calendar.sqlite            │          │ - book.sqlite                │
 └───────────┬──────────────┘         └───────────────┬──────────────┘          └───────────────┬──────────────┘
             │  ESource keyfiles (~/.config/evolution/sources/*.source) ◄── children created by the registry module
             │
             └──────────── session bus: org.nostr.Signer  (GetPublicKey · SignEvent · GetRelays · ApprovalRequested) ────┘
                           relays: wss://<home relays>  ·  ws://session (AF_UNIX $XDG_RUNTIME_DIR/nostr/relay.sock)
```

**D1 — Three EDS modules, no daemon.** One registry module and two backend
modules, each hosting its own core instance. We ship no service of our own, no
D-Bus name, no unit file. Rationale: EDS already runs one process per backend
type, restarts it on crash, decides online/offline, schedules refreshes, and
brokers every client; a fourth process would only add a second IPC hop and a
second lifecycle to reason about. The cost is three store files instead of one
(§3.3), which the relay-is-truth model makes harmless.

### 3.2 D2 — The shared core: `libnostr-dav-core`

Split today's static `nostr-dav-core` into:

| Target | Type | Contents | Consumers |
| --- | --- | --- | --- |
| `libnostr-dav-core` (`libnostr-dav-core.so.0`, `gnome/libnostr-dav-core/`) | shared, `-fvisibility=hidden`, `NDC_API` export macro like `NOSTR_PUBLISH_API` | `nd-store-db`, `nd-calendar-store`, `nd-contact-store`, `nd-ical`, `nd-vcard`, `nd-relay-sync`, `nd-publisher`, `nd-config` (parsing helpers only), plus the additions below | `nostr-dav`, the three EDS modules |
| `nostr-dav` | executable | `nd-application`, `nd-main`, `nd-dav-server`, `nd-token-store`, `nd-file-store`, `nd-file-entry` (NIP-94 stays with the DAV front until the GVfs design takes it) | — |

Additions to the core in W1 (each fixes a finding or is a prerequisite):

1. **`REQ` subscription state machine** (finding 1, D9): `nd_relay_sync` installs
   a state callback per endpoint, sends the configured filter set on connect,
   handles `EOSE` and `CLOSED`, commits the cursor on `EOSE`, re-subscribes
   after reconnect. Filters become a configurable list
   (`nd_relay_sync_set_filters(self, NdRelayFilter *filters, n)`) instead of a
   hard-coded kind list, because the calendar and book modules subscribe to
   different kinds. Subscription lifecycle is explicit: one fixed sub id per
   filter, `CLOSE` sent on stop and on reconfigure, cursors keyed per
   `(relay_url, sub_id)` — the existing `relay_cursor` table (PK `relay_url`)
   migrates to that composite key in schema v5.
2. **`created_at` stamping** (finding 2): `nd_calendar_store_put` /
   `nd_contact_store_put` stamp `created_at = max(now, prev_created_at + 1)`
   when the caller passes 0; `nd_ical_event_to_nip52_json` /
   `nd_vcard_to_nostr_json` emit the row's `created_at` (the nip55l 0.2.0
   signer preserves a non-zero `created_at`). The LWW guard and the ETag then
   work; `nostrc-ir7c` closes.
3. **Correct `TZID` emission** (finding 3): `nd-ical` emits local time under
   `TZID`, UTC with `Z` only when there is no tzid. Kept for `nostr-dav`; the
   EDS calendar backend does not use `nd-ical`'s generator at all (D10) but
   *does* use its NIP-52 JSON model (`NdCalendarEvent`), which gains
   `participants[]`, `summary`, `image`, `labels[]`, multiple `location`.
4. **Kind-5 authorisation by target author** (D9): a deletion is honoured when
   its `pubkey` equals the *target row's* `pubkey`, not only when it equals the
   account. Needed for invitations (foreign-authored events).
5. **Schema v5**: new tables `calendars` (31924), `rsvps` (31925, folded
   values only), `my_rsvp_ids(coord, d)` (the local, stable `d` I publish per
   coordinate — D12), `profiles` (kind 0), `follows` (kind 3, one row per
   `p`), `follow_sets` (30000), `relay_lists` (10002),
   `local_overlay(collection, uid, props, hidden)`,
   `nip05_cache(identifier, pubkey, verified, checked_at)`; new column
   `events.prev_signed_event_json` (last `published` copy, for the D15
   revert); `relay_cursor` re-keyed to `(relay_url, sub_id)`. Same
   `publish_state` columns on the rows that can be published (`calendars`,
   `rsvps`, `follows`-as-a-list, `follow_sets`). Strict `a`-tag parsing
   (`<kind>:<pubkey>:<d>` with a numeric kind check; `d` may contain `:`).
6. **Refresh hook**: `nd_relay_sync_set_changed_callback(cb, data)` fired on
   the core's context after any fold that wrote a row; the EDS modules debounce
   it into `e_cal_meta_backend_schedule_refresh()` /
   `e_book_meta_backend_schedule_refresh()`.

Nothing in the core links libsoup's *server* API, libxml2, or EDS. The name
`libnostr-dav-core` is the task's; it will outlive `nostr-dav`, so §17
question 1 offers `libnostr-pim-core` as the alternative.

### 3.3 D3 — Threading: one owner thread per core instance

EDS calls meta-backend vfuncs from its worker threads. `libnostr-publish`'s
transport and engine are "not thread-safe: use from one main context", and
`nd-store-db` opens SQLite with `FULLMUTEX` but its transactions
(`BEGIN IMMEDIATE … COMMIT`) span several calls and would interleave across
threads.

**Decision:** each module owns a `NeCore` object: a `GThread` running a private
`GMainContext` on which *everything* Nostr-side lives — the `NdStoreDb`,
stores, `NdRelaySync`, `NdPublisher`, transports, the signer proxy, and the
30 s outbox tick. Every EDS vfunc marshals its work onto that context with
`g_main_context_invoke_full()` and blocks on a `GCond` until the closure
completes or the `GCancellable` fires. Vfuncs therefore never touch SQLite or a
transport directly. The registry module's core is the same class with a
smaller filter set.

`NeCore` public surface (in `gnome/nostr-eds/src/common/ne-core.h`):

```c
typedef struct _NeCore NeCore;

typedef struct {
  const gchar   *pubkey_hex;        /* from ESourceNostrAccount              */
  const gchar   *npub;
  const gchar   *store_path;        /* calendar.sqlite | book.sqlite | registry.sqlite */
  const GStrv    home_relays;       /* [Nostr Account] HomeRelays, may be NULL */
  NostrPublishUpstream upstream;    /* [Nostr Account] UpstreamMode           */
  const gchar   *signer_app_id;     /* "nostr-eds"                            */
  const NdRelayFilter *filters;     /* module-specific REQ filters            */
  gsize          n_filters;
  void         (*on_changed)(NeCore *core, gpointer user_data);  /* core ctx */
  gpointer       on_changed_data;
} NeCoreConfig;

NeCore  *ne_core_new   (const NeCoreConfig *cfg, GError **error);   /* opens store, no I/O */
gboolean ne_core_start (NeCore *core, GError **error);              /* signer probe + relays */
void     ne_core_stop  (NeCore *core);
void     ne_core_free  (NeCore *core);

/* Run @func on the core thread; blocks until it returns or @cancellable fires.
 * @func receives the NdStoreDb / stores / publisher through @ctx. */
gboolean ne_core_run_sync (NeCore *core, NeCoreFunc func, gpointer data,
                           GCancellable *cancellable, GError **error);

gboolean ne_core_signer_available (NeCore *core);   /* last GetPublicKey probe */
```

A slow signer prompt (up to 30 s in `SignEvent`) never blocks a vfunc: signing
happens on the outbox tick, not in `save_*_sync` (D15).

Three consequences of this model, stated so nobody rediscovers them:

- **Cancellation is abandonment, not interruption.**
  `g_main_context_invoke_full()` cannot be cancelled; when the EDS
  `GCancellable` fires, `ne_core_run_sync` stops waiting and returns
  `G_IO_ERROR_CANCELLED`, but the closure runs to completion on the core
  thread. For reads that is harmless. For `save_*_sync` the local write (and
  outbox row) may land *after* the client saw a cancellation — the same shape
  as a cancelled DAV `PUT` today, and the next refresh reconciles it.
- **Head-of-line blocking is real.** The read-modify-write in D11/D17 waits
  up to 5 s for `EOSE` on the owner thread; every other vfunc of that backend
  waits behind it. Accepted for v1 (one user, one backend per process); the
  documented escape is a second `GMainContext` for subscriptions, the same
  three-step path the porthome design's D13 records.
- **Cross-process reads are read-only and best-effort.** The calendar
  process opens `book.sqlite` read-only for `CN` lookups only, tolerates
  `SQLITE_BUSY` and a missing/quarantined file, and falls back to the
  shortened npub. It never writes to it.

### 3.4 Store layout

`$XDG_DATA_HOME/nostr-eds/<pubkey_hex[0..16]>/{registry,calendar,book}.sqlite`,
directory 0700, files 0600 (`nd-store-db` already enforces both and quarantines
corruption). One file per module because the modules are separate processes
and SQLite WAL across processes is fine but a shared outbox would need a shared
ticker. The relay is the durable copy; a quarantined store is re-filled from
`REQ` on next start.

---

## 4. Account plumbing (registry module)

### 4.1 D4 — The account source

One `ESource` collection per Nostr identity, written to
`~/.config/evolution/sources/nostr-<pubkey_hex[0..16]>.source`:

```ini
[Data Source]
DisplayName=Nostr (npub1abc…xyz)
Enabled=true
Parent=

[Collection]
BackendName=nostr
CalendarEnabled=false
ContactsEnabled=false
MailEnabled=false
Identity=npub1…

[Nostr Account]
Npub=npub1…
PubkeyHex=<64 hex>
HomeRelays=wss://relay.example;wss://relay2.example
UpstreamMode=session_relay_or_direct
PublishQuorum=0
SignerAppId=nostr-eds
Nip05AsEmail=true
SyntheticAttendeeEmail=false

[Refresh]
Enabled=true
IntervalMinutes=30

[Offline]
StaySynchronized=true
```

`[Nostr Account]` is a custom `ESourceExtension` subclass
(`ESourceNostrAccount`, `E_SOURCE_EXTENSION_NOSTR_ACCOUNT = "Nostr Account"`)
registered by all three modules (each process registers the GType once; they
are separate processes). There is deliberately **no `[Authentication]`
group**: EDS's credentials prompter is never involved (D7).

`HomeRelays` is optional. Precedence for the relay set is defined once in D19
and applies to both subscribe and publish: cached kind 10002 → signer
`GetRelays()` → `HomeRelays` → session relay only. `UpstreamMode` values are
`nd-config`'s three strings. Parsing rules, in one place: unknown keys in
`[Nostr Account]` are ignored (forward compatibility); an unknown
`UpstreamMode` is fatal for the collection (privacy control; no silent
widening — the `nd-config` rule); `PublishQuorum=0` means every target must
ACK (D19), `n > 0` is the operator override. The two `Enabled` toggles start
`false` (D5). This keyfile block is the contract W2 freezes; W3, W4 and W6
implement against it and W4/W6 can be developed against hand-written
`.source` files before W3 lands.

### 4.2 D5 — Identity selection and creation

**Decision:** the registry module watches the bus name `org.nostr.Signer`
(`g_bus_watch_name`, `G_BUS_NAME_WATCHER_FLAGS_AUTO_START` off — we never
auto-activate the signer just to look). When the name appears it calls
`GetPublicKey()`. On success and if no collection source for that pubkey
exists, it creates one as in §4.1 **with `CalendarEnabled=false` and
`ContactsEnabled=false`** and populates the children (§4.3). A disabled
collection opens no relay connection and writes nothing but the `.source`
file. On `Error.NoKeyConfigured` or any error it does nothing and re-probes on
the next name appearance.

Enabling is one switch, in any of: the signer's Online Accounts sheet
(`apps/gnostr-signer/src/ui/sheets/sheet-online-accounts.c`, today the WebDAV
wizard, re-pointed in W9a), `nostr-eds-account enable`, Evolution's account
editor, or `org.nostr.Settings` (`nostrc-janr`). GNOME Calendar and Contacts
have no per-collection UI, which is why the sheet and the CLI exist.

Why materialise at all rather than wait for an explicit "add account": it is
what the removed GOA provider would have done (GOA accounts appear in EDS
automatically through `module-gnome-online-accounts`), and it gives the
enabling UIs something to toggle instead of something to create. Why
*disabled*: the signer being installed is a `Recommends:` of half the stack;
a key existing is not consent to open relay subscriptions and write a cache.

Removal is respected: the module connects to the registry server's
`source-removed` signal and, when the removed source is one of its
collections, writes `~/.config/evolution/sources/.nostr-<pk16>.removed`;
`nostr-eds-account disable --forget` writes the same marker. While the marker
exists the collection is never re-created; `enable` deletes the marker. §17
question 2 asks the maintainer to confirm disabled-by-default over
enabled-by-default (the previous draft) or explicit-create-only.

Identity = the signer's default identity: `GetPublicKey()` with no selector,
and every `SignEvent` is called with `identity = ""`, exactly as `nostr-dav`
and `nostr-share` do today. If the signer's default identity changes, the
module sees a different npub, creates a second collection, and leaves the first
enabled (its data is still that identity's data); the `nostr-eds-account`
CLI lists and disables. Multi-identity selection UI is out of scope.

`nostr-eds-account` (installed in `bindir`, links only `libedataserver` +
`nostr-eds-common`):

```
nostr-eds-account status                  # collections, children, outbox counts, signer/relay state
nostr-eds-account enable [--npub N]       # create/enable the collection for the signer identity (or N)
nostr-eds-account disable [--forget]      # disable; --forget also writes the .removed marker
nostr-eds-account calendar create <title> # publish a kind-31924 and materialise its child source
nostr-eds-account migrate-from-dav        # §11 phase 3
```

### 4.3 D6 — Child sources

`ECollectionBackend::populate` creates, via `e_collection_backend_new_child()`
with a stable resource id, the following children of the collection:

| Child (resource id) | Extension | `[Nostr Calendar]` / `[Nostr Book]` keys | Content |
| --- | --- | --- | --- |
| `events` | `[Calendar] BackendName=nostr` | `Scope=unfiled` | Own 31922/31923 events that are in no kind-31924 calendar |
| `calendar:<d>` (one per 31924) | `[Calendar] BackendName=nostr` | `Scope=calendar; Coordinate=31924:<pk>:<d>` | Own events listed in that calendar's `a` tags; `DisplayName` = the 31924 `title`, colour from `X-` overlay |
| `invitations` | `[Calendar] BackendName=nostr` | `Scope=invitations` | Foreign-authored events whose `p` tags include me (§5.3) |
| `follows` | `[Address Book] BackendName=nostr` | — | The follow graph (§6) |

`populate` returns immediately with the three static children (`events`,
`invitations`, `follows`) and never blocks on the network. When the collection
is enabled, the registry module starts a small core with one filter,
`{"kinds":[31924,5],"authors":[me]}`, folds into `registry.sqlite`, and on
every fold — on its owner thread, asynchronously to the registry server —
diffs the 31924 set against existing `calendar:*` children: new `d` →
`e_collection_backend_new_child()` + commit, deleted (kind 5 on the
coordinate) → remove the child (`e_source_remove_sync`), title change →
`DisplayName` update. Discovery happening in `evolution-source-registry`
mirrors what `EWebDAVCollectionBackend` does with PROPFIND there. The calendar
backend does not create sources.

The risk this buys: `evolution-source-registry` owns *every* account's
sources on the machine; a crash in our relay code there is restarted by EDS
but takes every Google/CalDAV/IMAP source offline for a moment. That is why
the registry core is the smallest of the three (one filter, one table), does
no publishing except `create/delete_resource_sync`, and why the blocking
`populate` path touches no socket.

`ECollectionBackend::create_resource_sync` (Evolution's "New Calendar" under a
collection) publishes a 31924 with the requested title and creates the child;
`delete_resource_sync` tombstones the 31924 (events themselves are untouched —
they become unfiled). GNOME Calendar's "New Calendar" dialog cannot target a
collection backend (it offers local/WebDAV only), hence the CLI verb.

### 4.4 D7 — Credentials: none in EDS; the signer is the credential

- No `[Authentication]` extension, so `ESourceRegistry` never prompts and
  `EBackend::authenticate_sync` is never called.
  `ECalMetaBackend::connect_sync` returns `E_SOURCE_AUTHENTICATION_ACCEPTED`
  unconditionally after the store opens.
- The bearer token, `nd-token-store`, the libsecret mirror, and the
  `--show-credentials` flow have no equivalent. Nothing is written to the
  keyring.
- The signer proxy is `nostr_publish_signer_new_dbus(bus, "nostr-eds", …)`.
  `app_id = "nostr-eds"` is a **new** ACL key, distinct from `"nostr-dav"`, so
  a migrating user sees one fresh approval prompt (with "remember") rather than
  inheriting an ACL granted to a process that no longer exists.
- No EDS process ever sees an nsec: the modules link `libnostr-publish`'s
  signer proxy and nothing that can hold key material. `nips/nip19` is used
  only for `nostr_nip19_decode_npub` (public key).
- Signer down ⇒ reads keep working from cache and relays; writes stage into
  the outbox and retry (`NOSTR_PUBLISH_SIGNER_ERROR_TRANSIENT` path) — the same
  rule as `nostr-dav` (D15).

---

## 5. Calendar backend (`ECalBackendNostr : ECalMetaBackend`)

### 5.1 Factory and capabilities

`ECalBackendNostrFactory : ECalBackendFactory` with `factory_name = "nostr"`,
`component_kind = I_CAL_VEVENT_COMPONENT`, `backend_type =
E_TYPE_CAL_BACKEND_NOSTR`. VTODO/VJOURNAL factories are not registered (NIP-52
has no tasks or memos). Module entry `e_module_load()` registers the factory
type and `ESourceNostrAccount`.

`get_backend_property(CLIENT_BACKEND_PROPERTY_CAPABILITIES)` advertises (names
to be verified against `libecal-2.0/libecal/e-cal-util.h` at W4):
`refresh-supported`, `save-schedules`, `organizer-not-email-address`,
`no-conv-to-recur`, `no-email-alarms`, `no-procedure-alarms`,
`no-thisandfuture`, `no-thisandprior`, `no-task-assignment`,
`no-transparency`, `no-gen-options`, `organizer-must-attend` **not** set,
`itip-suppress-on-remove-supported`. `CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS`
returns `nostr:<npub>` so Evolution's "is this attendee me?" logic
(`itip_strip_mailto` + string compare) recognises our own `ATTENDEE` line (D12).

### 5.2 D10 — 31922 / 31923 ↔ VEVENT

The backend uses libical-glib (`ICalComponent`, `ICalTime`, `ICalTimezone`)
through `ECalComponent`; `nd-ical`'s hand-rolled ICS parser/generator is not
used here (it stays for `nostr-dav`). The NIP-52 JSON side stays in the core's
`NdCalendarEvent` model.

| NIP-52 | iCalendar (VEVENT) | Direction | Rule |
| --- | --- | --- | --- |
| kind 31922 | `DTSTART;VALUE=DATE`, `DTEND;VALUE=DATE` | both | All-day. `end` is exclusive on both sides; missing `end` ⇒ `DTEND = start + 1 day` |
| kind 31923 | `DTSTART`/`DTEND` DATE-TIME | both | Missing `end` ⇒ `DTEND = DTSTART` emitted explicitly (zero duration — RFC 5545's default when `DTEND` is absent, but Evolution wants a `DTEND` to lay the event out) plus `X-NOSTR-NO-END=1`; on save, `DTEND == DTSTART` together with that marker publishes no `end` |
| `d` | `UID` | both | Own calendars: `UID = d`. Invitations source: `UID = <author_pubkey_hex>:<d>` (unique across authors). Evolution/Calendar-generated UIDs are accepted as `d` unchanged |
| `title` | `SUMMARY` | both | |
| `content` | `DESCRIPTION` | both | |
| `summary` | `X-NOSTR-SUMMARY` | both | iCal has no second short text; kept lossless |
| `image` | `ATTACH;VALUE=URI` (first) | both | Standard property; `X-NOSTR-IMAGE` mirror for clients that drop ATTACH |
| `start`/`end` + `start_tzid`/`end_tzid` (31923) | `DTSTART;TZID=<tzid>:<local>` via `i_cal_time_new_from_timet_with_zone(ts, FALSE, i_cal_timezone_get_builtin_timezone(tzid))`; no tzid ⇒ UTC `Z` form | both | Unknown tzid ⇒ UTC + `X-NOSTR-TZID-UNKNOWN=<tzid>`; the string is round-tripped on save. Never `TZID` + `Z` (finding 3) |
| `location` (repeatable) | `LOCATION` = first; `X-NOSTR-LOCATION` for the rest | both | |
| `g` (geohash) | `GEO:<lat>;<lon>` decoded from the geohash + `X-NOSTR-GEOHASH` | both | GEO is lossy; on save the geohash is regenerated only if `GEO` changed and `X-NOSTR-GEOHASH` is absent |
| `p` (`pubkey`, `relay`, `role`) | `ATTENDEE;CN=<profile name>;ROLE=<map>;X-NOSTR-RELAY=<relay>;PARTSTAT=<from rsvps>:nostr:<npub>` | both | `role` ↔ `ROLE`: `chair`↔`CHAIR`, `required`↔`REQ-PARTICIPANT` (default), `optional`↔`OPT-PARTICIPANT`, anything else ↔ `X-NOSTR-ROLE=<text>` on `NON-PARTICIPANT` |
| author pubkey | `ORGANIZER;CN=<profile name>:nostr:<npub>` | in | Own events: `ORGANIZER` = me |
| `t` | `CATEGORIES` | both | |
| `r` | `URL` (first) + `X-NOSTR-REFERENCE` | both | |
| `l` / `L` | `X-NOSTR-LABEL;X-NOSTR-NAMESPACE=<L>:<l>` | both | Pass-through |
| `created_at` | `DTSTAMP`, `LAST-MODIFIED` | in | `CREATED` = first-seen |
| event id / coordinate | `X-NOSTR-EVENT-ID`, `X-NOSTR-COORD` | in | Read-only; ignored on save |
| relay of origin | `X-NOSTR-RELAY-SEEN` | in | Diagnostic |
| — | `VALARM`, `CLASS`, `TRANSP`, `PRIORITY`, `STATUS`, `SEQUENCE`, `X-*` not listed | local | Overlay (D14) |
| — | `RRULE`, `RDATE`, `EXDATE`, `RECURRENCE-ID`, detached instances | rejected | D13 |

`CN` values come from the book module's `profiles` table when the same
identity's `book.sqlite` exists (read-only open of a second SQLite file is
cheap and safe; the calendar never writes it); otherwise the shortened npub.

**Kind flip.** Turning an all-day event into a timed one (or back) changes the
kind and therefore the addressable coordinate. `save_component_sync` stages,
in one store transaction: the new-kind row with the same `d`, a tombstone for
the old coordinate (`["a","31922:<pk>:<d>"]`), and an RMW of every 31924 that
listed the old coordinate (D11). The publisher dispatches the new event before
the tombstone (rows are ordered by `publish_next_ts` and the tombstone is
staged 1 s later).

### 5.3 D11 — 31924 calendars and membership

A kind-31924 event is a list of `a` tags; membership is owned by the calendar,
not the event. The backend keeps `calendars(d, title, description, a_list
JSON, created_at, event_id, publish_state…)` and answers `list_existing_sync`
for a `Scope=calendar` source with exactly the own events whose coordinate is
in that calendar's `a_list`; `Scope=unfiled` with own events in no calendar.

- **Create in calendar X** ⇒ stage the event; stage RMW of X: re-fold the
  latest 31924 for X from the store, append the coordinate, re-sign the whole
  list (replaceable). If a newer 31924 arrives from a relay while the RMW is
  pending, the fold guard keeps the pending row; on `published` the next fold
  merges normally. There is no compare-and-swap in Nostr; §12 gap 10.
- **Delete from calendar X** ⇒ RMW X removing the coordinate. **Only if the
  coordinate is then in no other 31924** is a kind-5 tombstone staged for the
  event. This makes Evolution's "move to calendar" — implemented as create in
  target, then remove from source — safe: the event is in Y before it leaves
  X, so no tombstone is published.
- **Delete from `unfiled`** ⇒ tombstone.
- **Delete from `invitations`** ⇒ never tombstones (not our event); stages a
  31925 `status=declined` if we had not declined already, and hides the row
  locally (`local_overlay.hidden=1`) so it does not reappear on the next fold.

### 5.4 D12 — 31925 RSVPs ↔ `ATTENDEE;PARTSTAT` and iTIP semantics

**Inbound.** The core folds every 31925 into `rsvps(coord, author_pubkey,
status, fb, d, event_id, target_event_id, created_at)` keyed by
`(coord, author)` with LWW. When a VEVENT is loaded, each `p` participant
becomes an `ATTENDEE` whose `PARTSTAT` is `ACCEPTED`/`DECLINED`/`TENTATIVE`
from the participant's newest RSVP for that coordinate, else `NEEDS-ACTION`.
An RSVP from a pubkey that is *not* in `p` is still shown (as a
`NON-PARTICIPANT` attendee with `X-NOSTR-UNINVITED=1`): Nostr lets anyone RSVP
to a public event and hiding that would misrepresent the relay state. Extra
params: `X-NOSTR-RSVP-EVENT-ID`, `X-NOSTR-FB=<free|busy>`,
`X-NOSTR-RSVP-NOTE=<content>`, and `X-NOSTR-RSVP-STALE=1` when the RSVP's
`e` tag names an event id older than the current version of the event (NIP-52
has no `SEQUENCE`; §12 gap 3).

**Invitations.** Filter `{"kinds":[31922,31923],"#p":[me]}` folds
foreign-authored events into the `invitations` source with
`ORGANIZER:nostr:<author>` and my own `ATTENDEE` line whose `PARTSTAT` comes
from *my* newest 31925 for that coordinate. Evolution shows the organizer and
attendee list with each attendee's status. **Whether Evolution offers
Accept/Decline/Tentative on such an event is not guaranteed**: that chrome is
produced by its iTIP formatter for *received scheduling messages*, and an
event that merely sits in a calendar with `ORGANIZER` ≠ me and `ATTENDEE` =
me may render read-only. W5's first task is to test exactly this on 3.52 and
3.54 before writing any RSVP code. If the chrome is absent, the fallback is
in scope for W5: the backend hands the invitation to EDS through
`receive_objects_sync` with a generated `METHOD:REQUEST` object (the path the
formatter drives), so Evolution treats it as a received invitation. GNOME
Calendar renders attendees read-only and has no RSVP UI either way.

**Outbound (attendee side).** With `save-schedules` advertised, Evolution does
not try to send iMIP mail; changing my `PARTSTAT` is a plain modify. In
`save_component_sync` on a foreign-authored event the backend diffs the
incoming component against the cached one and accepts **only**: my own
`ATTENDEE` `PARTSTAT` (and `X-NOSTR-FB`), and overlay properties (D14). Any
other change ⇒ `E_CLIENT_ERROR_PERMISSION_DENIED` with "Only the organizer can
change this event on Nostr". A `PARTSTAT` change stages a 31925:

```
kind 31925, content = X-NOSTR-RSVP-NOTE or "",
tags: ["a", "<kind>:<author>:<d>", "<relay hint>"], ["e", "<current event id>"],
      ["d", "<my stable id for this coordinate, from my_rsvp_ids>"],
      ["status", accepted|declined|tentative], ["fb", busy|free]   (omitted when declined),
      ["p", "<author pubkey>"]
```

`d` is required (31925 is addressable). It comes from the local
`my_rsvp_ids(coord, d)` table, **never** from `rsvps.d`, which holds folded
values — including my own RSVPs echoed back from relays or published from
another of my machines, possibly under a different `d`. On first RSVP for a
coordinate, `my_rsvp_ids` is seeded from the newest folded RSVP authored by me
for that coordinate if one exists, so a second machine replaces rather than
forks; otherwise a fresh UUID. `PARTSTAT` mapping is a policy choice, not
spec: `ACCEPTED→accepted,fb=busy`; `TENTATIVE→tentative,fb=free`;
`DECLINED→declined` (no `fb`); back to `NEEDS-ACTION` after a previous RSVP ⇒
kind 5 on my 31925 coordinate.

**Outbound (organizer side).** On my own events, `ATTENDEE` lines added or
removed in the editor become `p` tags (`nostr:<npub>` directly;
`mailto:<npub>` with no `@` — what Evolution's attendee entry produces when a
user types an npub — is decoded as an npub; `mailto:<verified nip05>` and
`mailto:<npub>@nostr.invalid` are resolved through the book's
`profiles`/`nip05_cache`; anything else is rejected with
`E_CLIENT_ERROR_INVALID_ARG` naming the address — §6.2 explains where those
`mailto:` forms come from). `PARTSTAT` values the
organizer sets on *other* attendees are ignored with a debug log: on Nostr
only the attendee's own 31925 asserts status.

**iTIP vfuncs.** Advertising `save-schedules` is a deliberate trade:
Evolution will **never** send iMIP mail for this calendar and hands every
scheduling object to the backend instead. So `send_objects_sync` must not be
a silent no-op. It inspects the recipient list: if every recipient resolves
to a pubkey (`nostr:`, bare `mailto:<npub>`, verified NIP-05, or
`@nostr.invalid`), it returns success and the delivery *is* the relay fan-out
of the 31922/31923 with those `p` tags (discoverable by `#p`; delivery to the
invitee's read relays is §12 gap 2); if any recipient is an unresolvable
`mailto:`, it returns `E_CLIENT_ERROR_INVALID_ARG` naming that address, so the
organizer learns at send time that a non-Nostr invitee would get nothing.
`receive_objects_sync` (e-mailed `METHOD:REQUEST/REPLY/CANCEL` via the
`itip-formatter`) returns `E_CLIENT_ERROR_NOT_SUPPORTED` for external
objects; if W5's verification (above) needs the formatter path for relay
invitations, the backend feeds its *own* generated `REQUEST` objects through
it and still refuses external ones. `get_free_busy_sync` returns
`E_CLIENT_ERROR_NOT_SUPPORTED`.

### 5.5 D13 — Recurrence: refuse, do not fake

NIP-52 has no `RRULE`, `RDATE`, `EXDATE`, or `RECURRENCE-ID`. Three options:

| Option | Verdict |
| --- | --- |
| (a) Reject at save (`E_CLIENT_ERROR_NOT_SUPPORTED`, message "Nostr calendar events cannot repeat; create separate events") | **Chosen.** Matches `nd-ical` today; honest; nothing on a relay misrepresents what the user typed. Evolution shows the error and keeps the editor open; GNOME Calendar shows a toast |
| (b) Expand into N discrete 31923 events linked by `X-NOSTR-SERIES`/`t` tag, bounded (e.g. 52 occurrences) | Rejected. Lossy (`UNTIL`/`COUNT` semantics vanish), spammy on relays, edits to "the series" become N republishes, and other Nostr clients see N unrelated events |
| (c) Keep `RRULE` in the local overlay and publish only the master as one event | Rejected. The user's calendar shows repeats that no other client — including their own phone — sees |

`no-conv-to-recur` is advertised so that Evolution versions honouring it hide
the Recurrence page; the save-time rejection is the guarantee regardless.
Detached instances (`RECURRENCE-ID`) and `instances` lists longer than one in
`save_component_sync` are rejected with the same error. If NIP-52 gains a
recurrence tag upstream (a proposal has been discussed on `nostr-protocol/nips`;
no number to cite), the overlay in D14 is the natural place to stage support.

### 5.6 D14 — Local-only overlay

Properties iCal has and NIP-52 does not: `VALARM` (the thing
`evolution-alarm-notify` needs), `CLASS`, `TRANSP`, `PRIORITY`, `STATUS`,
`SEQUENCE`, and every `X-` the backend did not itself define.

**Decision:** `local_overlay(collection='events', uid, props TEXT)` stores the
serialised `VALARM` components and listed properties. `load_component_sync`
builds the VEVENT from the NIP-52 row, then splices the overlay in.
`save_component_sync` splits the incoming component: NIP-52-mapped fields go
to the row and the outbox; overlay fields go to `local_overlay`; overlay-only
changes stage **nothing** on the relay (no republish for an alarm edit).
`STATUS:CANCELLED` is overlay-only — the only cancellation Nostr understands is
a kind 5, and deleting is an explicit user act. The overlay is per
`(identity, uid)` and per machine; it is not synced (that is the definition).
Losing it (store quarantine) loses alarms, which the store's warning line says.

### 5.7 D15 — Publish failure semantics

`save_component_sync` returns as soon as the row and outbox entry are
committed locally (optimistic, exactly as the DAV `PUT` today). Signing and
fan-out happen on the core tick. Terminal outcomes:

| Outcome | Create | Modify | Remove |
| --- | --- | --- | --- |
| `published` | revision flips from `local:<updated_at>` to the event id; next `get_changes` reports a no-content "modified" (harmless cache rewrite) | same | tombstone row `published`; nothing to do |
| `failed_permanent` (signer denied, relay `invalid:`/`blocked:`/`banned:`) | delete the local row, `schedule_refresh` so the cache drops it, `e_cal_backend_notify_error(backend, "Nostr: publishing '<title>' was refused: <reason>")` | re-fetch the last published copy (`REQ {"#d":[d],"kinds":[kind],"authors":[me],"limit":1}` on the core; fallback to the stored `signed_event_json` of the previous `published` state kept in `events.prev_signed_event_json`, new column), re-fold it, refresh, notify | leave the tombstone `failed_permanent`; the row is already gone locally; notify once |
| signer transient / no relay reachable | stays `pending`, retries on backoff; no UI noise. `nostr-eds-account status` shows the outbox depth; `org.nostr.Settings` (`nostrc-janr`) is the intended surface for "3 events waiting for the signer" | same | same |

This is stricter than `nostr-dav`, which leaves a `failed_permanent` row
visible and divergent from the relay forever. The `restricted:`/
`auth-required:` cases stay permanent until `nostrc-lq12` lands the
AUTH-then-retry flow in `libnostr-publish`; the backend inherits that fix.

### 5.8 D8 — Sync loop and the meta-backend fit

`ECalMetaBackend` gives, without backend code: the on-disk `ECalCache`
(SQLite under `~/.cache/evolution/calendar/<source uid>/cache.db`), client
notifications, `ESourceRefresh`-driven periodic refresh, offline-edit queueing
(edits made while `e_backend_get_online()` is `FALSE` are stored in the cache
with an `EOfflineState` of `E_OFFLINE_STATE_LOCALLY_CREATED/MODIFIED/DELETED`
and replayed through `save_component_sync`/`remove_component_sync` on the
first refresh after reconnect; whether replay runs before or after the server
diff is verified in §13 case j — either order converges here, because the
replayed save re-reads the newest relay copy before staging), `get_changes_sync`'s
default implementation (calls `list_existing_sync` and diffs revisions against
the cache), and `search_sync` over the cache. The backend implements exactly:

| Vfunc | Implementation (all via `ne_core_run_sync`) |
| --- | --- |
| `connect_sync` | `ne_core_start` if not started; returns `ACCEPTED`. Never fails for "no relay": the cache serves and writes stage |
| `disconnect_sync` | `ne_core_stop` |
| `list_existing_sync` | `(uid, revision)` for the source's scope (§5.3). `revision` = `nostr_event_id` when `published`/folded, `local:<updated_at>` while `pending` |
| `load_component_sync` | row → VEVENT (D10) + ATTENDEE join (D12) + overlay (D14). `out_extra` = JSON `{"coord":…,"event_id":…,"state":…}` |
| `save_component_sync` | D10/D11/D12 rules; stages outbox; returns `out_new_uid` |
| `remove_component_sync` | D11 rules |
| `requires_reconnect` | `TRUE` when `[Nostr Account]` relay/upstream keys changed (vfunc on `ECalMetaBackendClass`, used the same way by the CalDAV backend; verify at W4 — the fallback is reacting to `ESource::changed` on the extension and reopening the core) |
| `get_changes_sync` | default (not overridden) |

**Refresh push:** the core's `on_changed` callback is debounced 2 s and calls
`e_cal_meta_backend_schedule_refresh()`; `[Refresh] IntervalMinutes=30` is the
fallback for a silent relay. **Online/offline:** EDS's `GNetworkMonitor`
verdict drives `e_backend_get_online`; the core additionally stops reconnect
backoff while offline. The session relay (AF_UNIX) would be reachable during
"offline", but EDS queues writes anyway; they flush on reconnect. Acceptable
and documented.

### 5.9 D9 — Inbound subscriptions (calendar module)

Filters sent as separate `REQ` subscriptions per relay (ids
`ne-cal-own`, `ne-cal-inv`, `ne-cal-rsvp`, `ne-cal-rsvp-a`, `ne-cal-del`):

| Sub | Filter | Notes |
| --- | --- | --- |
| own | `{"kinds":[31922,31923,31924,31925,5],"authors":[me],"since":cursor}` | 31924 folded here too (calendar module keeps its own copy of membership; registry module is the one that creates sources) |
| invitations | `{"kinds":[31922,31923],"#p":[me],"since":cursor}` | foreign events |
| rsvp-p | `{"kinds":[31925],"#p":[me],"since":cursor}` | RSVPs that carry the optional `p` (organizer) tag |
| rsvp-a | `{"kinds":[31925],"#a":[<own coords, ≤ 64 per filter>]}` | rebuilt when the own event set changes; catches RSVPs without `p` |
| deletions | `{"kinds":[5],"authors":[<organizers of my invitations, ≤ 64>]}` | so a cancelled invitation disappears |

Cursor semantics (fixes `nostrc-tlp2` for both consumers): per `(relay,
sub)`, the cursor is committed **on `EOSE`** as `max(created_at)` of the
events received in that backfill; live events after `EOSE` advance it
monotonically; resume uses `since = cursor − 3600` (1 h overlap) and relies on
the id dedup (LRU + `nostr_event_id` columns) to make the overlap free. `CLOSED`
with `auth-required:` triggers the NIP-42 callback and one re-`REQ`; any other
`CLOSED` backs off like a disconnect. Kind-5 authorisation: honoured when the
deletion's pubkey equals the target row's `pubkey` (own or foreign).

---

## 6. Address-book backend (`EBookBackendNostr : EBookMetaBackend`)

### 6.1 D16 — The contact set and the card

**The book is the follow graph.** Its contact set is the `p` tags of my
newest kind 3; each contact's card is that pubkey's newest kind 0 (if we have
seen one) plus the petname from my kind 3 plus the kind-30000 sets that list
it. A follow with no kind 0 yet is still a contact (`FN` = shortened npub) so
the set is always complete.

| Nostr | vCard 4.0 (`EContact`) | Direction |
| --- | --- | --- |
| npub | `UID` | in, immutable |
| npub / hex | `X-NOSTR-NPUB`, `X-NOSTR-PUBKEY` | in |
| npub | `IMPP;X-SERVICE-TYPE=Nostr:nostr:<npub>` | in (Contacts shows it under chat/IM; Evolution under IM) |
| kind 3 petname › kind 0 `display_name` › `name` › npub | `FN` | in; `FN` edits go to overlay (see D17) |
| kind 0 `name` | `NICKNAME` (first value) | in; `NICKNAME[0]` edits → petname (D17) |
| kind 3 petname | `X-NOSTR-PETNAME` | both |
| kind 0 `about` | `NOTE` | in |
| kind 0 `picture` | `PHOTO;VALUE=uri:<https url>` | in |
| kind 0 `banner` | `X-NOSTR-BANNER` | in |
| kind 0 `website` | `URL` | in |
| kind 0 `nip05` | `X-NOSTR-NIP05` always; **`EMAIL;TYPE=x-nip05` only once verified** (§6.2) | in |
| kind 0 `lud16` / `lud06` | `X-NOSTR-LUD16` / `X-NOSTR-LUD06` | in |
| kind 0 `bot` | `X-NOSTR-BOT` | in |
| kind 3 `p` relay hint | `X-NOSTR-RELAY` | in |
| kind 30000 sets (`d`, `title`) | `CATEGORIES` (`title`, else `d`) + `X-NOSTR-SET=<d>` per category | both |
| kind 0 `created_at` | `REV` | in |
| — | `TEL`, `ADR`, `ORG`, `TITLE`, `BDAY`, other `EMAIL`, `N`, anything else | overlay (local, never published) |
| (no verified NIP-05) | `EMAIL;TYPE=x-nostr-synthetic:<npub>@nostr.invalid` | in, only when `SyntheticAttendeeEmail=true` (default **false**, §6.2) |

`PHOTO` as a URI is honest and cheap; whether GNOME Contacts renders remote
`https:` photos through libfolks' `Edsf.Persona` avatar loading is to be
verified at W6, and an inline-cache option (download once, cap 64 KiB,
`PHOTO;ENCODING=b`) is a listed follow-up rather than v1 scope.

Book capabilities: `net`, `do-initial-query`, `refresh-supported`; **not**
`contact-lists` (kind-30000 sets are categories, not `KIND:group` cards, in
v1 — GNOME Contacts has no list UI either way).

### 6.2 NIP-05 and the `mailto:` problem

Two places need an e-mail-shaped address: GNOME Contacts' search/display, and
Evolution's attendee picker, which selects contacts by `EMAIL` and writes
`ATTENDEE:mailto:<email>` (the picker cannot produce a `nostr:` URI).

**Decision:**
- `X-NOSTR-NIP05` is always present when kind 0 has `nip05`.
- `EMAIL;TYPE=x-nip05:<name@domain>` is emitted **only after verification**
  (`nostr_nip05_resolve_from_json` over a libsoup 3 fetch of
  `https://<domain>/.well-known/nostr.json?name=<name>`, on the core thread,
  ≤ 1 request/s, result cached in `nip05_cache` for 7 days, failures for
  24 h). An unverified identifier never becomes an `EMAIL`. `[Nostr Account]
  Nip05AsEmail=false` turns the `EMAIL` form off entirely.
- `SyntheticAttendeeEmail` (default **false**) emits, for follows without a
  verified NIP-05, `EMAIL;TYPE=x-nostr-synthetic:<npub>@nostr.invalid`. The
  forcing reason it exists at all: Evolution's attendee picker
  (`ENameSelector`) lists contacts by `EMAIL` and writes
  `ATTENDEE:mailto:<email>`; a contact with no `EMAIL` cannot be picked. Why
  it defaults off: an `EMAIL` is an `EMAIL` — the mail composer autocompletes
  it, "Send message to contact" tries to mail it and fails at SMTP against
  `.invalid` (RFC 2606's reserved TLD, un-mailable by DNS but not by UX), and
  it leaks into every vCard export. With it off, picker-based invites work for
  NIP-05-verified follows, and any follow can still be invited by typing its
  npub in the attendee entry (`mailto:<npub>` is decoded, D12). The calendar
  backend resolves all three `mailto:` forms back to a pubkey.

§17 question 3 asks the maintainer to confirm the default.

### 6.3 D17 — Writes: follow, unfollow, petname, sets, overlay

| Operation (EDS) | Nostr effect |
| --- | --- |
| `save_contact_sync`, new UID | Resolve identity from, in order: `X-NOSTR-NPUB`, `IMPP nostr:`, `EMAIL` `@nostr.invalid`, `EMAIL` NIP-05 (verified lookup, may take a network round-trip; bounded 5 s). None ⇒ `E_CLIENT_ERROR_INVALID_ARG` "A Nostr contact needs an npub, a nostr: address, or a NIP-05 identifier". Then **follow**: kind-3 read-modify-write |
| `save_contact_sync`, existing UID | `NICKNAME[0]` changed ⇒ petname RMW on kind 3 — the tag is `["p", <pubkey>, <relay>, <petname>]`, petname at index 3, and index 2 (relay hint) is preserved verbatim (`nips/nip02`'s `NostrFollowEntry` already carries `relay` + `petname`; W6 checks its serializer emits that order). `CATEGORIES` changed ⇒ RMW of each affected 30000 set (add/remove `p`); a new category name ⇒ new 30000 with `d = slug(title)`, `title`. Everything else ⇒ overlay only, nothing published |
| `remove_contact_sync` | **Unfollow**: kind-3 RMW removing the `p`; also removed from every 30000 set that lists it (RMW each) |

**Kind-3 read-modify-write.** Kind 3 is one replaceable event; publishing it
from a stale base silently drops follows added elsewhere. Rule: before staging
a kind-3 publish, the core re-issues `{"kinds":[3],"authors":[me],"limit":1}`
to the target relays, waits for `EOSE` (bounded 5 s, on the owner thread —
every other vfunc of the backend waits behind it, D3), folds whatever is
newer, and then applies the **delta** (this add / this remove / this petname)
on top of the newest known list. The staged event carries `created_at =
max(now, base_created_at + 1)`. The same procedure applies to every 30000 set
and to 31924 calendars. It narrows the race to the 5 s window; it does not
close it (§12 gap 10).

Overlay for contacts uses the same `local_overlay` table with
`collection='contacts'`. Overlay values shadow kind-0 values on load, so a
user who renames a contact sees their name, and a profile update from the
relay does not clobber it. `nostr-eds-account status` reports how many
contacts carry overlay data.

### 6.4 D18 — Kind 30085 private contacts: not carried forward

`nostr-dav`'s `/contacts/nostr/` publishes the whole vCard in plaintext as
kind 30085. Phone numbers and postal addresses of third parties on public
relays is not a feature to migrate. **Decision:** the EDS book does not read or
write 30085. A "Private Contacts" address book is a follow-up design that would
store the vCard NIP-44-encrypted to self (`NIP44EncryptB64` via the signer,
which exists for exactly this) — the `nostr-seal`/porthome precedent for
self-encrypted content applies. Until then, users who relied on 30085 keep
`nostr-dav` for that one collection (§11) or export to a local book.

### 6.5 Inbound subscriptions (book module)

| Sub | Filter | Notes |
| --- | --- | --- |
| own lists | `{"kinds":[3,30000,10002],"authors":[me],"since":cursor}` | 10002 feeds D19's target set |
| profiles | `{"kinds":[0],"authors":[<follows chunk of ≤ 100>]}` × N | Issued after the kind-3 fold and on every follow-set change, one chunk at a time, next chunk on `EOSE` (relays cap filter size). Persistent for the first 5 chunks (live profile updates for the first 500 follows), one-shot for the rest, re-run on the `[Refresh]` interval. > 5 000 follows ⇒ profile fetch stops there with a warning; the contacts still exist as npubs |

No relay is hard-coded. Follows' kind 0s often live on relays that are not
mine; with only home relays + session relay, some contacts stay bare npubs.
Per-author NIP-65 outbox resolution (each follow's kind 10002) is the fix and
is a follow-up bead that also serves `nostrc-lyvz`; §17 question 8.

---

## 7. Offline, cache, and why the meta-backends fit

Two caches exist by construction: EDS's `ECalCache`/`EBookCache` (what
applications read; survives our process) and the core's store (relay mirror +
outbox + overlay). They are not redundant:

| Concern | Owner |
| --- | --- |
| What GNOME Calendar/Contacts see, search, and edit offline | EDS cache |
| Offline edits made while EDS is offline | EDS cache (`E_OFFLINE_STATE_*`), replayed into `save_*_sync` on reconnect |
| The exact relay state (`nostr_event_id`, `created_at`, `pubkey`, raw tags), LWW, dedup | core store |
| Edits accepted while online but not yet on a relay (signer down, relay down) | core outbox, retried by the tick |
| Alarms, `X-` props, contact annotations | core `local_overlay` |
| Signed event JSON kept so a retry never re-prompts | core outbox |

A backend written on raw `ECalBackend` would have to implement the EDS cache,
offline queue, refresh timer, and change notification itself (`ECalBackendCalDAV`
before 3.26 did, at several thousand lines). `ECalMetaBackend` reduces the EDS
side to the seven vfuncs in §5.8, which is why the module code estimate in §2
is small and why the risk lives in the mapping, not the plumbing.

---

## 8. D19 — Publishing: the same NIP-65 outbox semantics

Unchanged from `nostr-dav`, because the code is the same code (`nd-publisher`
in the shared core over `libnostr-publish`):

- **Sign before persist.** The unsigned event is built from the row, signed via
  `org.nostr.Signer.SignEvent(json, "", "nostr-eds")`, and the signed JSON is
  stored before the first send. Retries never re-prompt.
- **Target set** is resolved at staging and frozen into `publish_targets` so a
  relay-list change mid-retry never retargets. Resolution order: cached kind
  10002 write relays (`nostr_publish_nip65_relays(…, WRITE)`) → signer
  `GetRelays()` → `[Nostr Account] HomeRelays` → none. Then
  `nostr_publish_policy_select_targets(policy, write_relays,
  session_relay_url)` applies `UpstreamMode`: `session_relay_only` ⇒ the
  session relay alone (error `NO_RELAYS` if its socket is absent, surfaced as
  `failed_permanent` + notify); `session_relay_or_direct` ⇒ session relay when
  present else direct; `direct_only` ⇒ write relays. This is the enforcement
  `nostrc-862u` asks for, done for this consumer; `nostr-dav` gets it when it
  adopts the same core call (W1 does both).
- **Session relay transport**: URL `ws://session` mapped to
  `nostr_publish_transport_new_websocket_unix(url, $XDG_RUNTIME_DIR/nostr/relay.sock)`
  exactly as `ns-net.c` does. Caveat inherited from `nostrc-862u`'s comment:
  the session relay does not federate upstream yet, so `session_relay_only`
  keeps events local.
- **Verdict**: `quorum = 0` (all targets must ACK) by default; `[Nostr Account]
  PublishQuorum=<n>` is the operator override, clamped as today. 120 s OK
  wait; backoff `min(60·2^n, 3600)` s; permanent classes as in
  `nostr_publish_classify_ok`.
- **Tombstones** (kind 5 with `a` coordinates) share the outbox; deletions of
  own 31925s and 31924s use it too.
- **NIP-42 AUTH**: the transport's auth callback signs kind 22242 through the
  same signer (`nostr_publish_signer_sign_auth_event`).
- **Subscribe-side relays** are the READ set from kind 10002 (`…_READ`) plus
  the session relay, resolved by the same precedence — inbox and outbox are
  finally distinct, which the DAV bridge never did.

What is *not* done here: routing an invitation to the invitee's read relays
(NIP-65 inbox model). Publishing to our write relays is what the spec's
`#p` discovery assumes on the reader side but does not guarantee delivery
(§12 gap 2). Follow-up bead alongside `nostrc-lyvz`.

---

## 9. Credentials and security

**D24 — Security posture.**

| Threat | Bridge today | EDS design |
| --- | --- | --- |
| nsec exposure | Never in `nostr-dav` (signer-only) | Never: modules link only the signer proxy; no key APIs |
| Credential on disk | Bearer token 0600 + keyring mirror; theft = full R/W | None. No token, no password, nothing in the keyring |
| Local process injects calendar/contact writes | Needs the token (file read) | Any session process can call EDS's D-Bus (`org.gnome.evolution.dataserver.Calendar8`); the write only reaches a relay if the signer approves. A remembered ACL for `nostr-eds` makes that silent |
| Mitigation for the above | — | Follow-up in the signer track: kind-scoped ACL entries (`nostr-eds` may be remembered only for kinds `{31922,31923,31924,31925,3,30000,5,22242}`), so a remembered approval cannot be abused to sign a kind-1 note or a kind-10002 relay list. Filed as W11(b); §17 question 7 |
| Store readable by other users | 0600/0700 enforced by `nd-store-db` | Same code, same guarantees, under `nostr-eds/` |
| Process hardening | systemd unit: `ProtectSystem=strict`, `ProtectHome=read-only`, `NoNewPrivileges`, … | None: EDS factories are plain user processes launched by D-Bus activation. We inherit whatever hardening the distro applies to EDS (currently none). The token's removal is the larger net gain |
| Relay-supplied data | JSON parsed with json-glib; signature verification is *not* done in `nd-relay-sync` (test comment: "out of scope for the fold logic") | Same gap. W1 adds `nostr_event_verify`-equivalent id+sig verification on ingest (the fake relay's `FAKE_RELAY_VERIFY_SIGS` shows the shape); an event that fails verification is dropped and logged. Without it a malicious relay can plant events under the account's pubkey |
| Malformed iCal/vCard from relays | Hand-rolled parsers | libical / `EVCard` parsers (hardened, fuzzed upstream) |
| NIP-05 fetch | n/a | HTTPS only via libsoup 3, 5 s timeout, 64 KiB cap, no redirects off-host, rate-limited |

Approval prompts: the signer's `ApprovalRequested` preview shows the event
kind and the app id `nostr-eds`; the first publish after migration prompts
once. Recommended `remember` TTL is the signer's default; not our decision.

---

## 10. D20 — Flatpak and sandboxing

- EDS backends are dlopen'd by `evolution-source-registry`,
  `evolution-calendar-factory[-subprocess]` and
  `evolution-addressbook-factory[-subprocess]`. Those are **host** processes;
  there is no Flatpak extension point for EDS backends. `evolution-nostr` is a
  distro package only (§14). A Flatpak of Evolution (`org.gnome.Evolution`)
  bundles its own EDS and will not see the host module either — documented
  limitation; Evolution-from-Flatpak users are pointed at the distro package.
- Flatpak **GNOME Calendar** and **GNOME Contacts** (Flathub `org.gnome.Calendar`,
  `org.gnome.Contacts`) talk to the *host* EDS over the session bus with
  `--talk-name=org.gnome.evolution.dataserver.*` (to be re-verified against
  the current manifests at W8). Host module ⇒ visible to them. No network
  permission is needed on their side — the relay traffic is the backend's.
- **Signer in a Flatpak** (`apps/gnostr-signer/flatpak/org.gnostr.Signer.yaml`
  owns `org.nostr.Signer` on the session bus): host EDS processes reach it
  over the same bus. Approval prompts appear from the Flatpak. Works today for
  `nostr-dav`; unchanged.
- **Session relay socket** `$XDG_RUNTIME_DIR/nostr/relay.sock` is host-side;
  the backends are host-side; no sandbox boundary.
- Contrast with the bridge: Flatpak Calendar reached `127.0.0.1:7680` only
  with `--share=network` (Flathub grants it to Calendar; Contacts did not need
  it); the EDS path removes that dependency.

---

## 11. D21 — Migration from `nostr-dav`

**Coexistence rules.** Different store directories (`nostr-dav/` vs
`nostr-eds/<pk16>/`), different signer `app_id`, different EDS sources (the
bridge's `webdav` collection vs our `nostr` collection). Both may run; the
user then sees two "Nostr" calendars in GNOME Calendar with the same events.
`nostr-eds-account status` warns when `nostr-dav.service` is active.
Divergence is bounded by the relay: both fold the same events.

| Phase | Work item | What changes | `nostr-dav` |
| --- | --- | --- | --- |
| 0 | this doc | — | unchanged |
| 1 | W1 | `libnostr-dav-core.so.0` split; findings 1–3 fixed in the core; `nostr-dav` links the shared lib and gains a working `REQ`, correct TZID, stamped `created_at`, enforced upstream mode | behaviour improves; package gains `Depends: libnostr-dav-core0` |
| 2 | W2–W8 | EDS modules ship as `evolution-nostr`; coexistence | unchanged; QUICKSTART gains "prefer evolution-nostr on GNOME" |
| 3 | W9 | `nostr-eds-account migrate-from-dav`: (1) refuse unless every `publish_state='pending'` count in `~/.local/share/nostr-dav/store.sqlite` (events, contacts, files, tombstones) is 0, or `--force`; (2) `systemctl --user disable --now nostr-dav.service`; (3) remove the EDS `webdav` collection source(s) whose `[Authentication] Host=127.0.0.1` and `Port=7680` via `ESourceRegistry`; (4) `enable`; (5) leave `~/.local/share/nostr-dav` and `~/.config/nostr-dav` in place (relays are truth; the user deletes them per the QUICKSTART uninstall recipe). The signer's `sheet-online-accounts.c` WebDAV wizard becomes "Enable Nostr in Calendar & Contacts" (W9a; runs `enable`) | marked legacy: `debian/control` description, `README.md` banner, `Recommends` dropped from `gnostr-signer-daemon` if any |
| 4 | W10 | Deletion, gated on: `evolution-nostr` in a tagged release **and** packaged for Debian + Fedora **and** live smoke (§13) green **and** one release cycle elapsed **and** §17 question 4 answered "delete" | deleted |

**What is deleted in phase 4:** `gnome/nostr-dav/src/{nd-dav-server,
nd-token-store, nd-application, nd-main, nd-file-store, nd-file-entry}.c` and
headers; `systemd/*`; `config/`; `docs/QUICKSTART.md`; `SECURITY.md`;
`README.md`; `tests/{nd-test-harness.[ch], test_dav_propfind.c (DAV cases —
the `/ical`, `/vcard`, `/nip94` cases move in phase 1), test_auth_required.c}`;
`debian/nostr-dav.*`; the `ENABLE_NOSTR_DAV` option; the WebDAV branch of the
signer's Online Accounts sheet; `nd-vcard`'s kind-30085 JSON functions (the
vCard parser stays as a fallback for the overlay). NIP-94 `/files/` ownership
transfers to the GVfs design (`docs/designs/gvfs-nostr-backends.md`) — if that
lands first, `nd-file-*` goes with it. `test_relay_transport_libsoup.c` moves
to `libnostr-publish/tests/` in phase 1 (it tests the transport, not the DAV
front).

**What is not migrated:** the SQLite store (nothing in it that the relay does
not have, except `failed_permanent` rows, which the tool lists before
disabling), the token, kind-30085 contacts (D18).

---

## 12. NIP-52 spec gaps (and what the design does about each)

1. **No recurrence.** Refused at save (D13).
2. **No invitation delivery.** Participants discover events by `#p` on relays
   the organizer wrote to; the spec does not say organizers must write to
   participants' read relays. We publish to our write relays (D19);
   NIP-65 inbox routing is a follow-up. Until then "invited but never saw it"
   is possible and documented.
3. **No version counter.** RSVPs reference a coordinate and optionally an
   `e`; after the organizer edits the event there is no `SEQUENCE` to
   re-request. We keep the RSVP and mark `X-NOSTR-RSVP-STALE=1` when `e` is
   older than the current event id (D12).
4. **Attendee status lives with the attendee.** `p` tags carry no PARTSTAT;
   only the attendee's 31925 asserts it. Organizer-set PARTSTAT is ignored
   (D12). Reverse of iCal, where the organizer's copy is authoritative.
5. **No alarms.** Overlay (D14).
6. **No `STATUS`/`TRANSP`/`CLASS`/`PRIORITY`.** Overlay; cancellation is
   deletion.
7. **Timezones by name only.** `start_tzid` is an IANA string; no VTIMEZONE.
   Unknown names fall back to UTC with a preserved `X-NOSTR-TZID-UNKNOWN`.
8. **Optional `end`.** 31923 without `end` ⇒ no `DTEND`; 31922 ⇒ `start + 1`.
9. **Geohash vs lat/lon.** `GEO` is derived and lossy; the geohash is kept.
10. **No compare-and-swap on replaceable lists.** 31924 membership, kind 3,
    30000: read-modify-write on the newest copy narrows but cannot close the
    lost-update window (D11, D17).
11. **Multiple `location` tags.** iCal has one `LOCATION`; extras are `X-`.
12. **Attachments.** Only `image`; iCal `ATTACH` beyond the first URI is
    overlay-only.
13. **Deletion is advisory.** Relays may ignore kind 5; other relays never
    see it. The local cache always removes; `nostr-eds-account status` lists
    tombstones that no relay ACK'd.
14. **Everything is public.** NIP-52 has no private events. A private
    calendar (NIP-44 to self, or gift-wrapped invitations) is a separate
    design; the overlay and the signer's `NIP44EncryptB64` are the hooks.
15. **`fb` ≠ `TRANSP`/`FREEBUSY`.** Mapped to `X-NOSTR-FB` on the attendee
    line; `get_free_busy` is unsupported.
16. **`created_at` is the version.** Clock skew between two of the user's
    devices lets an older edit win LWW. Mitigation: `created_at =
    max(now, last_seen + 1)` on every publish (W1 item 2).
17. **Kind 31924 has no colour, order, or visibility.** Calendar colour is
    an overlay on the child source (`[Calendar] Color`), local only.

---

## 13. D22 — Testing strategy

Four layers plus a live smoke, all skip-if-absent in the tree's convention.

**L0 — core unit tests (ported).** Move to `gnome/libnostr-dav-core/tests/`
unchanged in intent: `/nostr-dav/relay-sync/*` (6), `/nostr-dav/publish/*` (5),
`/store/*` minus `daemon-restart-http`, and the `/ical/*`, `/vcard/*`,
`/nip94/*` cases out of `test_dav_propfind.c`. New in W1: `REQ` frame
assertions (filters, `since`, resubscribe after reconnect), EOSE-committed
cursor with overlap, `created_at` stamping, TZID emission, signature
verification drop, kind-5 foreign-author authorisation, `select_targets`
enforcement per upstream mode. All on the fixture transport + vtable signer.

**L1 — mapping unit tests (libical / libebook-contacts, no EDS daemon).**
`test_nip52_ecal.c` (31922/31923 ↔ `ECalComponent`: all-day exclusivity,
TZID local time, unknown tzid, kind flip staging, RRULE/RECURRENCE-ID
rejection, overlay split/splice), `test_rsvp_join.c` (31925 → `ATTENDEE`
PARTSTAT, stale marking, uninvited RSVP, PARTSTAT → 31925 JSON, un-RSVP →
kind 5), `test_calendar_membership.c` (31924 RMW, move-safe delete,
tombstone-if-orphan), `test_profile_econtact.c` (kind 0/3/30000 → `EContact`,
FN precedence, NIP-05 gating, synthetic address, categories), `test_follow_rmw.c`
(kind-3 delta on newest base, petname, set membership).

**L2 — backend glue in-process.** `NeCore` driven with
`nostr_publish_transport_new_fixture` + vtable signer: `ne_core_run_sync`
marshalling (cancellation mid-call), refresh debounce, `list_existing` scope
answers, outbox staging order for kind flip.

**L3 — EDS integration.** Vendored `e-test-server-utils.[ch]` (from EDS
`tests/test-server-utils/` — confirm the path at the 3.52 tag, it has moved
before; LGPL-2.1-or-later, ~900 lines, kept in
`gnome/nostr-eds/tests/eds-test-utils/` with its licence header, the EDS
version it was taken from, and a matching `debian/copyright` stanza; §17
question 9). The harness spawns the real
`evolution-source-registry`/factories on a private `GTestDBus` with
`EDS_REGISTRY_MODULES`, `EDS_CALENDAR_MODULES`, `EDS_ADDRESS_BOOK_MODULES`
pointing at the build dir, `EDS_TESTING=1`, `GSETTINGS_BACKEND=memory`, and
isolated `XDG_*`. Relay = `fake_relay_fixture.py` (moved to
`tests/fixtures/` at repo top level with a compat symlink from
`gnome/nostr-homed/tests/integ/`), signer = `mock_signer.c` (same move; gains
`GetRelays` returning `Error.NotFound` so our fallback path is exercised).
Distro package needed on CI: `evolution-data-server` (binaries) + the dev
packages; skipped when `evolution-calendar-factory` is not found.

| # | Case | Asserts |
| --- | --- | --- |
| a | account appears | mock signer up ⇒ registry module creates `nostr-<pk16>.source` + `events`, `invitations`, `follows` children; no `[Authentication]` group; signer down at start ⇒ nothing, then appears ⇒ created |
| b | calendar discovery | relay seeded with two 31924 ⇒ two `calendar:<d>` children with titles; kind 5 on one ⇒ child removed |
| c | inbound fold → client | seed 31922 + 31923 ⇒ `e_cal_client_get_object_list` shows both with correct `DTSTART` forms and `TZID`; a newer version on the relay ⇒ modified notification within 5 s (refresh push) |
| d | create → publish | `e_cal_client_create_object` ⇒ relay receives `EVENT` kind 31923 with `d`, `title`, `start`, `start_tzid`; `OK true` ⇒ revision becomes the event id; `FAKE_RELAY_REJECT_PUBKEYS` ⇒ object vanishes from the client + error notified |
| e | recurrence refused | `create_object` with `RRULE` ⇒ `E_CLIENT_ERROR_NOT_SUPPORTED`; relay receives nothing |
| f | alarm survives | add `VALARM` via `modify_object` ⇒ no `EVENT` sent; relay pushes a newer copy of the event ⇒ client still sees the alarm |
| g | invitation + RSVP | seed a foreign 31923 with `p=me` ⇒ appears in `invitations` with `ORGANIZER nostr:` and my `ATTENDEE NEEDS-ACTION`; `modify_object` setting `PARTSTAT=ACCEPTED` ⇒ relay receives 31925 `status=accepted`, `a`, `e`, `p`; a foreign 31925 for my own event ⇒ that attendee's PARTSTAT updates; editing `SUMMARY` on the invitation ⇒ `PERMISSION_DENIED` |
| h | calendar membership | create in `calendar:<d>` ⇒ event + 31924 RMW published; "move" (create in Y, remove from X) ⇒ no kind 5; remove from the last calendar ⇒ kind 5 with `a` |
| i | follows book | seed kind 3 (3 follows) + two kind 0 ⇒ 3 contacts, one bare npub; `add_contact` with `IMPP nostr:` ⇒ kind 3 republished with 4 `p`, RMW base = relay's newest; `remove_contact` ⇒ 3; NIP-05 verified via a local HTTPS stub ⇒ `EMAIL;TYPE=x-nip05` appears; unverified ⇒ absent |
| j | offline replay | `e_backend_set_online(FALSE)` ⇒ `create_object` queued in the EDS cache; online ⇒ `save_component_sync` runs, relay receives the `EVENT`. Variant: relay copy changes while offline ⇒ record whether EDS replays before or after the server diff and assert the RMW-on-newest rule makes the result identical either way |
| k | upstream mode | `UpstreamMode=session_relay_only` with the socket absent ⇒ publish `failed_permanent` + notify; with a fake AF_UNIX relay ⇒ only the session transport receives the frame |
| l | feature-OFF closure | with `ENABLE_NOSTR_EDS=OFF`: no `libecalbackendnostr.so`, no EDS symbols in `nostr-dav`, `libnostr-dav-core.so.0` has no libsoup-server/libxml2 `NEEDED` |
| m | regression: `nostr-dav` | existing six suites green against the shared core; `test_dav_propfind` DAV cases unchanged |

**Live smoke (maintainer, manual)**, on the aarch64 lab
`bizarro@192.168.64.3` per the pattern of
`docs/reviews/nostr-dav-publish-live-2026-09-26.md`: real
`gnostr-signer-daemon`, `wss://relay.sharegap.net`, `gnome-calendar` and
`evolution` from the distro: (1) account appears with no configuration;
(2) create an event in Calendar, observe it via an independent `REQ`;
(3) publish a 31924 from another client, observe the new calendar appear;
(4) RSVP from Evolution to an event another key invited us to, observe the
31925; (5) set an alarm, wait for `evolution-alarm-notify`; (6) follow an
npub from Contacts, observe kind 3; (7) `migrate-from-dav` on a machine that
had the bridge. Record wall-clock for fold → UI in the bead.

---

## 14. D23 — Packaging

**Source tree:** `gnome/nostr-eds/` (`src/common/`, `src/registry/`,
`src/calendar/`, `src/book/`, `src/cli/`, `tests/`, `data/`, `README.md`),
CMake option `ENABLE_NOSTR_EDS` (OFF; implies `ENABLE_LIBNOSTR_PUBLISH` and the
new `ENABLE_LIBNOSTR_DAV_CORE`). `pkg_check_modules` for `libedata-cal-2.0`,
`libedata-book-1.2`, `libebackend-1.2`, `libecal-2.0`, `libebook-1.2`,
`libebook-contacts-1.2`, `libical-glib`, `json-glib-1.0`, `libsoup-3.0`,
`sqlite3`. Install destinations come from pkg-config, never hard-coded:

```
$(pkg-config --variable=backenddir libedata-cal-2.0)/libecalbackendnostr.so
$(pkg-config --variable=backenddir libedata-book-1.2)/libebookbackendnostr.so
$(pkg-config --variable=moduledir  libebackend-1.2)/module-nostr-backend.so
${bindir}/nostr-eds-account
${datadir}/doc/evolution-nostr/README.md
```

Names follow EDS's in-tree pattern (`libecalbackendcaldav.so`,
`libebookbackendcarddav.so`, `module-webdav-backend.so`). Modules are
`MODULE` libraries with no SONAME, `-fvisibility=hidden`, exporting only
`e_module_load`/`e_module_unload`.

**Debian.** One new binary from `src:nostrc`, **`evolution-nostr`** —
the established name shape for out-of-tree EDS backend bundles
(`evolution-ews`, `evolution-mapi`, `evolution-kolab`) even though, unlike
those, it ships no Evolution UI module and must **not** depend on `evolution`:

```
Package: evolution-nostr
Section: gnome
Architecture: any
Depends: ${shlibs:Depends}, ${misc:Depends},
         evolution-data-server (>= ${eds:Version}),
         evolution-data-server (<< ${eds:NextMinor}),
         libnostr-dav-core0 (= ${binary:Version}),
         libnostr-publish0 (= ${binary:Version})
Recommends: gnostr-signer-daemon, nostrc-session-relay (>= ${source:Version})
Enhances: evolution, gnome-calendar, gnome-contacts
Description: Evolution Data Server backends for Nostr calendars and contacts
```

`${eds:Version}` and `${eds:NextMinor}` (e.g. `3.52` → `3.53`) are computed
in `debian/rules` from `pkg-config --modversion libedata-cal-2.0` at build
time. The upper bound is the evolution-ews pattern and it is not optional:
EDS backend vfunc tables grow between *minor* releases, so a module built
against 3.52 can dlopen into 3.54 and misbehave without any soname change;
the `libedata-cal-2.0-2`/`libedata-book-1.2-*`/`libebackend-1.2-*` shlibs
dependencies alone would not catch that. `debian/copyright` gains a stanza
for the vendored LGPL-2.1+ test utility (§13). `evolution-nostr.install` lists the four
paths above with `usr/lib/*/evolution-data-server/…` globs. New
`libnostr-dav-core0` (Section `libs`, Multi-Arch same) mirrors
`libnostr-publish0`. The EDS `-dev` build-dependencies pull GNOME into the
build closure, so they go behind the desktop build profile the packaging plan
defers to (D-10); the headless `nostr-login` closure is untouched (dependency
purity gate unaffected).

**Fedora/RPM.** Not in `nostr-login.spec` (headless). When the Phase-4 desktop
spec exists: `%package -n evolution-nostr`, `BuildRequires:
pkgconfig(libedata-cal-2.0) pkgconfig(libedata-book-1.2)
pkgconfig(libebackend-1.2)`, `Requires: evolution-data-server%{?_isa} >=
%{eds_version}` with `%global eds_version %(pkg-config --modversion
libedata-cal-2.0)`, `%files` under `%{_libdir}/evolution-data-server/
{calendar-backends,addressbook-backends,registry-modules}/`. Fedora rebuilds
EDS-backend packages on every EDS soname bump via the usual
side-tag; the package must be on that list (note in `packaging/rpm/README.md`).

**Version support:** build and test against 3.52 (Ubuntu 24.04 / Fedora 40)
as the floor; CI adds 3.54/3.56 when the desktop packaging job exists. No API
used is newer than 3.36.

---

## 15. Decisions

| # | Decision | Options | Recommendation |
| --- | --- | --- | --- |
| **D1** | Process shape | (a) three EDS modules, no daemon; (b) one Nostr daemon + thin EDS proxies over D-Bus; (c) keep `nostr-dav` and add EDS modules that talk DAV to it | **(a).** EDS already provides process lifecycle, crash restart, online state and refresh scheduling; (b) adds an IPC hop and a second lifecycle; (c) keeps the token and the port |
| **D2** | Shared core | (a) split `nostr-dav-core` into `libnostr-dav-core.so.0`; (b) copy files into the EDS tree; (c) link the static lib | **(a).** One copy of store/sync/outbox for both consumers and fixes land once; static linking into three modules plus a daemon triples the surface |
| **D3** | Threading | (a) one owner thread per core with marshalled vfuncs; (b) call the core from EDS worker threads with a big lock | **(a).** `libnostr-publish` and the store's multi-statement transactions are single-context by contract; a lock around a blocking signer call would stall EDS |
| **D4** | Account source | (a) `ECollectionBackend` collection with a `[Nostr Account]` extension; (b) standalone calendar + book sources with no collection | **(a).** One enable/disable point, standard Calendar/Contacts toggles, children created from relay state |
| **D5** | Identity & creation | (a) materialise a *disabled* collection when `org.nostr.Signer` reports a key, one switch enables it; (b) materialise it enabled; (c) explicit `nostr-eds-account enable` creates it | **(a).** Something to toggle without opening a relay connection on the strength of an installed signer; `.removed` marker honours deletion from Settings. §17 q2 |
| **D6** | Child sources | (a) per-31924 child calendars + unfiled + invitations + follows; (b) one calendar for everything | **(a).** Calendars are user-visible on other clients; invitations need a read-mostly home |
| **D7** | Credentials | (a) none; signer is the credential, app_id `nostr-eds`; (b) reuse app_id `nostr-dav` | **(a).** No `[Authentication]`, no token, one honest re-approval |
| **D8** | Cache roles | (a) EDS cache app-facing, core store relay mirror + outbox; (b) core store only, raw `ECalBackend` | **(a).** Meta-backends give offline queue, refresh, notification for free |
| **D9** | Inbound subscriptions | (a) persistent `REQ`s with EOSE-committed cursor + 1 h overlap; (b) periodic one-shot `REQ` on the refresh interval | **(a).** Push latency is the point; fixes finding 1 and `nostrc-tlp2` for `nostr-dav` too |
| **D10** | VEVENT mapping | (a) libical-based, `nd-ical` model only; (b) reuse `nd-ical`'s text generator | **(a).** Fixes finding 3 by construction; hand-rolled ICS has no place under EDS |
| **D11** | 31924 membership | (a) calendar-owned `a` list, RMW, tombstone only when orphaned; (b) tombstone on any delete | **(a).** Makes Evolution's create-then-remove "move" safe |
| **D12** | RSVP / iTIP | (a) `ATTENDEE;PARTSTAT` from 31925, `save-schedules` with a `send_objects_sync` that refuses unresolvable recipients, PARTSTAT change ⇒ 31925 with a local stable `d`, `nostr:` CAL-ADDRESS + `CAL_EMAIL_ADDRESS`, `receive_objects_sync` self-fed only if W5 finds Evolution needs it; (b) RSVPs as separate read-only items | **(a).** The one thing the bridge can never do; the RSVP chrome is verified before it is promised |
| **D13** | Recurrence | (a) refuse; (b) bounded expansion; (c) local-only RRULE | **(a).** Only honest option |
| **D14** | Local-only properties | (a) overlay table spliced on load; (b) drop; (c) publish as `X-` tags | **(a).** Alarms must survive; publishing local noise to relays is wrong |
| **D15** | Publish failure | (a) revert + notify on permanent, silent retry on transient; (b) leave divergent rows visible (today) | **(a).** A calendar that shows what the relay refused is lying |
| **D16** | Contact set | (a) kind 3 ∩ kind 0 + 30000 categories, UID = npub; (b) a personal address book (kind 30085) | **(a).** The follow graph is the thing Nostr actually has; (b) publishes third-party PII (D18) |
| **D17** | Book writes | (a) follow/unfollow/petname/sets via RMW on newest; other fields overlay; (b) reject non-Nostr edits | **(a).** GNOME Contacts has no read-only field notion; silent rejection is worse than a local annotation |
| **D18** | Kind 30085 | (a) drop; (b) carry forward plaintext; (c) NIP-44-encrypted follow-up | **(a) now, (c) filed.** Plaintext third-party PII on public relays is not migrated |
| **D19** | Publishing | (a) same `nd-publisher` + `libnostr-publish`, NIP-65 all-ACK, `select_targets` enforced, targets 10002 › `GetRelays` › `HomeRelays`; (b) a second publisher in the EDS modules | **(a).** One outbox implementation; closes `nostrc-862u` for this consumer |
| **D20** | Flatpak | (a) host-only distro package, document the talk-names Flatpak clients need; (b) attempt a Flatpak extension | **(a).** EDS has no backend extension point; (b) cannot work |
| **D21** | Migration | (a) coexist, then `migrate-from-dav`, then gated deletion; (b) hard cut-over in one release | **(a).** The outbox must drain and the packages must exist before the bridge goes |
| **D22** | Testing | (a) L0–L3 + live smoke, vendored `e-test-server-utils`, shared fixtures under `tests/fixtures/`; (b) unit tests only | **(a).** The mapping is where the risk is and only a real EDS factory exercises it |
| **D23** | Packaging | (a) `evolution-nostr` + `libnostr-dav-core0`, pkg-config install dirs, EDS version substvar; (b) `nostr-eds` (repo naming) | **(a).** Distro maintainers recognise the `evolution-<provider>` shape; §17 q1 |
| **D24** | Security | (a) no nsec, no token, signature verification on ingest, kind-scoped signer ACL as follow-up; (b) trust relays and the ACL as-is | **(a).** Verification on ingest is the missing half of "relays are truth" |

---

## 16. Work items

Bead-ready titles; sizes per the scale in the header. Dependencies are listed;
W2–W3 can start once W1's header is agreed, W4/W6 in parallel after W2.

| # | Title (bead) | Size | Depends | Scope |
| --- | --- | --- | --- | --- |
| **W0** | `eds: maintainer decisions for docs/designs/eds-nostr-backends.md §17` | S | — | Answer the nine questions; file W1–W10 beads with the answers folded in |
| **W1** | `libnostr-dav-core: split shared store/sync/outbox core out of nostr-dav (SONAME 0); REQ/EOSE state machine; created_at stamping; TZID fix; sig verification; upstream-mode enforcement` | L | W0 | §3.2 items 1–6 in full: `REQ`/`CLOSE`/`EOSE`/`CLOSED` lifecycle with fixed sub ids, `relay_cursor` re-keyed `(relay_url, sub_id)`, `created_at` stamping in both generators (`nd-ical`, `nd-vcard`), TZID fix, id+sig verification on ingest, kind-5 target-author rule + strict `a` parse, schema v5 incl. `my_rsvp_ids` and `events.prev_signed_event_json`, `nd_relay_sync_set_changed_callback`, `select_targets` in `nd_publisher_configure`; tests moved (L0); `test_relay_transport_libsoup` → libnostr-publish; `nostr-dav` relinked; Debian `libnostr-dav-core0`. Closes `nostrc-tlp2`, `nostrc-ir7c`, `nostrc-862u` (nostr-dav part) |
| **W2** | `nostr-eds: common module — ESourceNostrAccount extension, NeCore owner thread, identity + relay-set resolution` | M | W1 | §3.3, §4.1, D19 precedence, `nostr_nip19_decode_npub`, NIP-05 fetch/cache helper (libsoup 3) |
| **W3** | `nostr-eds: registry module (ECollectionBackend "nostr") + nostr-eds-account CLI` | L | W2 | §4.2–4.3: signer name watch, disabled-by-default materialisation, `source-removed` → `.removed` marker, non-blocking `populate`, async 31924 discovery → children on the owner thread, `create/delete_resource_sync`, CLI verbs `status/enable/disable/calendar create`. Contract with W4/W6 is the §4.1 keyfile, frozen at W2 |
| **W4** | `nostr-eds: calendar backend (ECalMetaBackend) — 31922/31923 ↔ VEVENT, 31924 membership, overlay, capabilities, publish-failure revert` | XL | W2 | §5.1–5.3, 5.5–5.8, D9 filters own/deletions; L1 tests `test_nip52_ecal`, `test_calendar_membership` |
| **W5** | `nostr-eds: invitations + RSVP — 31925 ↔ ATTENDEE/PARTSTAT, invitations source, save-schedules, foreign-edit rules` | L | W4 | **Step 0: verify on 3.52/3.54 whether Evolution offers Accept/Decline on a folded foreign-organizer event; if not, the self-fed `receive_objects_sync` path is in scope.** Then §5.4: `rsvps` + `my_rsvp_ids`, `send_objects_sync` recipient check, D9 filters invitations/rsvp-p/rsvp-a/deletions; L1 `test_rsvp_join` |
| **W6** | `nostr-eds: address-book backend (EBookMetaBackend) — kind 3/0/30000 ↔ vCard 4.0, NIP-05 verification, follow/unfollow RMW, overlay` | XL | W2 | §6; L1 `test_profile_econtact`, `test_follow_rmw`; PHOTO URI rendering check |
| **W7** | `nostr-eds: test harness — vendored e-test-server-utils, shared fake relay + mock signer under tests/fixtures/, L2/L3 suites, CI job with EDS installed` | L | W3, W4, W6 | §13 cases a–m; `mock_signer.c` gains `GetRelays → NotFound`; `debian/copyright` stanza for the vendored file; feature-OFF closure job |
| **W8** | `packaging: evolution-nostr (Debian) + libnostr-dav-core0; RPM subpackage stanza for the desktop spec` | M | W7 | §14; `${eds:Version}` substvar; lintian clean; Flatpak talk-name verification note |
| **W9** | `nostr-dav → evolution-nostr migration: migrate-from-dav verb, QUICKSTART/README rewrite, nostr-dav marked legacy` | M | W8 | §11 phase 3 CLI verb; `gnome/nostr-eds/README.md` + QUICKSTART for `evolution-nostr`; `gnome/nostr-dav/docs/QUICKSTART.md` "prefer evolution-nostr on GNOME"; `debian/control` legacy wording |
| **W9a** | `gnostr-signer: Online Accounts sheet becomes "Enable Nostr in Calendar & Contacts" (toggles the EDS collection; WebDAV wizard kept only while nostr-dav ships)` | S | W3 | `apps/gnostr-signer/src/ui/sheets/sheet-online-accounts.c` (+ `.blp/.ui`); calls `nostr-eds-account enable` or writes the toggles through `ESourceRegistry`; signer-track file scope, separate bead |
| **W10** | `Delete nostr-dav DAV front, token store, units, DAV tests and packaging after the §11 phase-4 gate` | M | W9 + gate | §11 phase 4 file list |
| **W11** | Follow-ups, separate beads, not blocking: (a) `NIP-65 inbox routing for invitations + per-follow outbox resolution` (extends `nostrc-lyvz`); (b) `signer: kind-scoped remembered ACL entries` (signer track); (c) `Evolution config module module-nostr-config.so (New Calendar/Address Book under the Nostr collection)`; (d) `Private Contacts book: kind 30085 → NIP-44-to-self`; (e) `avatar inline cache option`; (f) `NIP-52 recurrence: track/propose upstream tag`; (g) `AUTH-then-retry publish` is `nostrc-lq12` | — | — | — |

Critical path: W1 → W2 → W4 → W5 → W7 → W8 → W9 → W10 (W3, W6 and W9a in
parallel with W4; W3 and W4 share only the §4.1 keyfile contract, frozen at
W2, so W4 can test against hand-written `.source` files). Rough total for one
engineer: 11–15 weeks to W8; W9–W10 are gated by release cadence, not effort.

---

## 17. Open questions for the maintainer

1. **Names.** Debian/Fedora binary `evolution-nostr` (distro convention, no
   dependency on `evolution`) vs `nostr-eds` (repo convention: `nostr-dav`,
   `nostr-share`, `nostr-seal`)? And does the shared core keep the task's
   `libnostr-dav-core` after `nostr-dav` is deleted, or become
   `libnostr-pim-core` in W1 while the rename is free?
2. **Materialise disabled (D5).** The registry module creates the Nostr
   collection, *disabled*, as soon as the signer reports a key; one switch
   (signer sheet, CLI, Evolution, `org.nostr.Settings`) enables it. Confirm
   this over (b) enabled-by-default (no switch, but relay subscriptions start
   on the strength of an installed signer) or (c) nothing until
   `nostr-eds-account enable` creates it.
3. **`EMAIL` for verified NIP-05 (default on) and the `@nostr.invalid`
   synthetic address (default off) (§6.2).** Both exist so Evolution's
   attendee picker can invite Nostr contacts; the synthetic form pollutes the
   composer and vCard exports, so it ships off. Confirm both defaults, or drop
   the synthetic option entirely (typed npubs still work through
   `mailto:<npub>`).
4. **Fate of `nostr-dav` for non-GNOME clients.** Deleting the DAV front (W10)
   removes the only path for Thunderbird/KDE/macOS. Delete (recommended — no
   bead asks for those clients and the maintenance cost is a second front),
   or keep it buildable, `OFF`, unrecommended, for one more release?
5. **Kind 30085 (D18).** Confirm it is dropped from the EDS book and that the
   encrypted "Private Contacts" design is a follow-up bead, not v1.
6. **Recurrence (D13).** Confirm "refuse" over "bounded expansion".
7. **Kind-scoped signer ACL (D24).** Without it, a remembered approval for
   `nostr-eds` lets any local process publish any kind through EDS. File in
   the signer track before W8, or accept for v1?
8. **No hard-coded profile relays (§6.5).** With only home + session relays,
   a fresh account's follows may show as bare npubs until the NIP-65
   follow-up lands. Accept, or allow an optional `ProfileRelays=` key that
   ships empty?
9. **Vendoring `e-test-server-utils` (LGPL-2.1+) into `tests/` of an
   MIT-licensed tree.** Fine for test-only code with its header intact, or
   should L3 be written against a minimal in-house harness (`GTestDBus` +
   `EDS_*_MODULES` env, ~300 lines)?

---

## 18. References

- `docs/investigations/gnome-gtk-nostr-planned-not-implemented-2026-09-26.md` — §3 item 4 (`nostrc-00i0`), Cluster 5/7 context, GOA removal `2cf193a7`.
- `docs/plans/gnome-integration-and-samba-server-2026-09-25.md` — "Calendar + Contacts track (Path A confirmed)", PLAN-D7 functional trade-offs.
- `docs/reviews/nostr-dav-publish-live-2026-09-26.md` — live smoke method and lab.
- `docs/designs/packaging-plan-debian-fedora.md` — D-10 build profiles, Phase-4 desktop packaging, dependency-purity gate.
- `docs/designs/gvfs-nostr-backends.md` — sibling design (`nostrc-jaxi`), owner of NIP-94 files.
- In-tree: `gnome/nostr-dav/{src,include,tests,docs,CMakeLists.txt,SECURITY.md}`, `gnome/libnostr-publish/include/nostr-publish/*.h`, `gnome/nostr-share/src/ns-net.c` (session relay + `GetPublicKey` probe + target resolution), `nips/nip55l/dbus/org.nostr.Signer.xml`, `nips/nip52/include/nip52.h`, `nips/nip02`, `nips/nip51`, `nips/nip05`, `nips/nip19`, `gnome/nostr-homed/tests/integ/fake_relay_fixture.py`, `gnome/nostr-homed/tests/integration/mock_signer.c`, `debian/control`, `debian/nostr-dav.install`, `packaging/rpm/nostr-login.spec`.
- Beads: `nostrc-00i0`, `nostrc-prqu` (epic), `nostrc-jaxi`, `nostrc-tmsc` (closed; libnostr-publish), `nostrc-862u` (upstream mode), `nostrc-tlp2` (EOSE cursor), `nostrc-lyvz` (NIP-65 discovery), `nostrc-ir7c` (ETag), `nostrc-lq12` (NIP-42 retry), `nostrc-ls2c` (closed; tombstones), `nostrc-0e7k` (closed; signer wizard), `nostrc-janr` (settings app).
- EDS: `libedata-cal-2.0` (`ECalMetaBackend`, `ECalBackendFactory`, `ECalCache`), `libedata-book-1.2` (`EBookMetaBackend`, `EBookBackendFactory`, `EBookCache`), `libebackend-1.2` (`ECollectionBackend`, `EModule`, `ESourceRegistryServer`), `libedataserver-1.2` (`ESource`, `ESourceExtension`, `ESourceRefresh`, `ESourceOffline`), EDS `tests/test-server-utils/e-test-server-utils.[ch]`.
- Specs: NIP-01, NIP-02 (kind 3), NIP-05, NIP-09 (kind 5), NIP-19, NIP-42, NIP-44, NIP-51 (30000), NIP-52 (31922/31923/31924/31925), NIP-65 (10002); RFC 5545 (§3.3.5 DATE-TIME forms, §3.8.4.1 ATTENDEE, §3.8.4.3 ORGANIZER), RFC 5546 (iTIP), RFC 6350 (vCard 4.0: IMPP, PHOTO, CATEGORIES, X-), RFC 2606 (`.invalid`).
