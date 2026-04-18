#!/usr/bin/env bash
#
# One-shot script to create the GNOME desktop integration bead hierarchy.
# Idempotency: bd create does NOT dedupe on title, so only run this once.
# If interrupted partway, check `bd list | grep -i gnome` before re-running.
#
# Usage:
#   scripts/add-gnome-integration-beads.sh
#
# Prerequisites:
#   - bd on PATH (installed from ~/Documents/Dev/beads)
#   - pwd == repo root
#   - bd onboard completed in this repo

set -euo pipefail

if [[ ! -d .beads ]]; then
  echo "error: .beads/ not found - run from the nostrc repo root" >&2
  exit 1
fi

if ! command -v bd >/dev/null 2>&1; then
  echo "error: bd not on PATH - install from ~/Documents/Dev/beads" >&2
  exit 1
fi

# Create the epic and capture its ID for parent-child linking.
epic_id=$(bd create --type epic --priority 1 --silent \
  --labels gnome,desktop,integration \
  "GNOME desktop integration: bridge Nostr to EDS/GOA/Files" \
  --body-file - <<'EOF'
## Goal

Make NIP-52 calendar events, NIP-94/Blossom files, and a contacts
collection visible through native GNOME surfaces (GNOME Calendar,
GNOME Contacts, Nautilus/Files) via the stock GNOME Online Accounts +
Evolution Data Server + GVfs stack -- without forking goa-daemon.

## Non-goals

- Do not replace or compete with `gnome/nostr-homed/` (roaming-home
  daemon, orthogonal scope: systemd-homed-style user provisioning).
- Do not build a custom GOA provider. This was attempted in Sept 2025
  (commit ab209dd5 "simplify nostr provider to minimal stub for opaque
  GoaProvider") and again Feb 2026 (`gnome/goa/`, commit 312fc3d6) and
  both hit the same wall: GOA providers are statically registered in
  `ensure_builtins_loaded()` and cannot be loaded out-of-tree.

## Strategy

Use the generic WebDAV provider that upstream GOA has shipped since
GNOME 46 (3.46+, April 2024). Build a localhost DAV bridge
(`nostr-dav`) that translates CalDAV/CardDAV/WebDAV to Nostr via the
existing `org.nostr.Signer` D-Bus interface (which is complete and
production-grade -- see `apps/gnostr-signer/data/dbus/org.nostr.Signer.xml`).

All new code stays in C11 to match the rest of the repo. HTTP server
via libsoup-3; DAV XML via libxml2; concurrency via libgo/fiber where
it fits.

## Acceptance

- User adds a "WebDAV" account in GNOME Settings pointed at
  `http://127.0.0.1:<port>`, and a Nostr-backed calendar appears in
  GNOME Calendar, contacts in GNOME Contacts, and files in Nautilus.
- `gnome/nostr-goa-overlay/` is removed from the tree.
- `gnome/goa/` is removed from the tree.
- `docs/gnome-integration.md` publishes an authoritative NIP mapping
  table to prevent NIP-54-as-documents style hallucinations in the future.
EOF
)

echo "Created epic: $epic_id"

# Helper: create a child task and attach it as parent-child to the epic.
make_child () {
  local title="$1" ; shift
  local priority="$1" ; shift
  local labels="$1" ; shift
  # Remaining args: body via heredoc on stdin
  local child_id
  child_id=$(bd create --type task --priority "$priority" --silent \
    --labels "$labels" "$title" --body-file -)
  bd dep add "$child_id" "$epic_id" --type parent-child >/dev/null
  echo "  + $child_id  $title"
}

# --- Housekeeping beads ---------------------------------------------------

make_child "Remove gnome/nostr-goa-overlay/ subtree and vendored GOA submodule" \
  1 "gnome,desktop,cleanup" <<'EOF'
## Context

Non-functional. Provider is a 15-line stub exporting zero types
(commit ab209dd5 Sep 2025: "simplify nostr provider to minimal stub
for opaque GoaProvider"). Vendored GOA submodule under
`vendor/gnome-online-accounts` pins a chebizarro fork
(commit ca94062c). README contradicts itself (claims both "no
vendoring is performed" and "Build vendor GOA with the Nostr provider
patch"). The architectural approach (per-user overlay that replaces
goa-daemon) cannot work because providers register via
`ensure_builtins_loaded()`, not a plugin directory -- this was not
known to the LLM that generated the subtree.

## Tasks

1. Confirm nothing under `gnome/nostr-goa-overlay/` is referenced from
   top-level `CMakeLists.txt`, `meson.build`, or CI workflows.
2. `git rm -r gnome/nostr-goa-overlay/`
3. Remove the submodule entry from `.gitmodules` if present.
4. Add `docs/proposals/goa-overlay-postmortem.md` explaining why (this
   bead's context, linked to parent epic).
5. Reference this bead ID in the commit message.

## Acceptance

- Tree no longer contains `gnome/nostr-goa-overlay/`.
- Full clean build still succeeds: `rm -rf _build && cmake -B _build && cmake --build _build`.
- Postmortem doc exists and is linked from `docs/README.md`.
EOF

make_child "Delete gnome/goa/ custom GoaProvider subproject" \
  1 "gnome,desktop,cleanup" <<'EOF'
## Context

`gnome/goa/` subclasses `GoaProvider` (via `GOA_API_IS_SUBJECT_TO_CHANGE=1`
and `GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE=1`) and installs `goa-gnostr.so`
into `${CMAKE_INSTALL_LIBDIR}/goa-1.0/providers`. Modern goa-daemon
does not `dlopen` anything from that directory -- providers are
statically registered via `g_io_extension_point_implement(...)` inside
a `G_DEFINE_TYPE_WITH_CODE` block and enumerated by a hard-coded
`ensure_builtins_loaded()` in `src/goabackend/goaprovider.c`. The
module will build and install, but never load.

The Feb 2 2026 account-picker UI (commit 312fc3d6) is ~400 lines of
solid GTK4 code. It does not justify keeping dead code in the tree.
If the picker is wanted later, lift it from git history into
`apps/gnostr-signer/src/ui/sheets/` at that point.

## Tasks

1. Confirm nothing else in the repo depends on `goa-gnostr.so` or the
   `Gnostr` GOA provider type string.
2. `git rm -r gnome/goa/`
3. Remove any `add_subdirectory(gnome/goa)` references from the
   top-level `CMakeLists.txt`.
4. Reference this bead ID in the commit message.

## Acceptance

- Tree no longer contains `gnome/goa/`.
- Full clean build still succeeds.
EOF

# --- The actual bridge work ----------------------------------------------

make_child "nostr-dav: local CalDAV/CardDAV/WebDAV bridge daemon (scaffold)" \
  1 "gnome,desktop,nostr-dav" <<'EOF'
## Goal

Implement a localhost DAV server (systemd `--user` unit listening on
`127.0.0.1:7680`) that translates between the DAV wire protocol and
Nostr events via `org.nostr.Signer`.

This bead is scaffold-only: a server that answers `OPTIONS`,
`PROPFIND` depth 0/1, and principal discovery with an empty calendar
and address book. It proves the GOA-WebDAV <-> localhost handshake
without any Nostr wiring yet. Subsequent beads add real content.

## Scope

- New subdir: `gnome/nostr-dav/`
- systemd `--user` unit, D-Bus activated via `org.nostr.Dav`
- Listens on `127.0.0.1:7680` over plain HTTP (localhost only -- no TLS
  for v1; document in SECURITY.md why this is acceptable)
- `OPTIONS /`, `PROPFIND /` (depth 0/1), well-known redirects
- Bearer token auth: one token per account, generated at install
  time, stored in libsecret, **never** persisted to disk. WebDAV
  client sends it as password in HTTP Basic.
- CMake integration: `-DENABLE_NOSTR_DAV=ON` (default OFF)

## Stack (C11 to match the rest of the repo)

- HTTP server: **libsoup-3** (`SoupServer`). Already pulled in
  transitively by the GNOME stack; used elsewhere in the repo.
- DAV XML parse/emit: **libxml2** (`xmlReaderForMemory`,
  `xmlTextWriter`). The DAV protocol's XML is small and bounded --
  no need for a full DAV library.
- Concurrency: evaluate whether `libgo/fiber` (C11 goroutine/channel
  primitives, already in this repo) fits the per-connection handler
  model. Fallback: libsoup-3's native async GTask model.
- Signer client: the existing D-Bus glue at
  `gnome/nostr-homed/src/common/nip46_client_dbus.c` calls
  `org.nostr.Signer` -- extract the reusable parts into a shared
  helper (`libnostr-signer-client` or similar) and link both
  `nostr-homed` and `nostr-dav` against it. Track the refactor as
  an implementation sub-task inside this bead.

## Acceptance

- `systemctl --user start nostr-dav` succeeds on a clean Ubuntu 24.04
  VM with the package installed.
- Open Settings -> Online Accounts -> Add -> WebDAV, enter
  `http://127.0.0.1:7680` and bearer token. Account appears.
- `curl -u user:token -X PROPFIND http://127.0.0.1:7680/` returns a
  well-formed multistatus response with zero collections (v1 is
  empty-backend).
- Integration test in `gnome/nostr-dav/tests/integ/` exercises the
  add-account handshake using a goa-daemon-free test harness.
EOF

make_child "nostr-dav: CalDAV -> NIP-52 mapping (kinds 31922/31923)" \
  1 "gnome,desktop,nostr-dav,nip-52" <<'EOF'
## Goal

On `PUT` of a `text/calendar` payload, parse the ICS and emit a
NIP-52 event signed by the current account via `org.nostr.Signer`:

- VEVENT with date-only DTSTART -> kind **31922** (date-based)
- VEVENT with timestamp DTSTART -> kind **31923** (time-based)

Tag mapping:
- `UID` -> `d` tag (required; parameterized-replaceable identity)
- `SUMMARY` -> `title` tag
- `DTSTART`/`DTEND` -> `start`/`end` tags (ISO 8601 `YYYY-MM-DD` for
  31922, unix-seconds string for 31923)
- `LOCATION` -> `location` tag (repeatable)
- `DESCRIPTION` -> event `content`
- `ATTENDEE` -> `p` tags (pubkey resolved via NIP-05 where possible,
  else skip attendee -- never silently coerce a non-resolvable email)
- `CATEGORIES` -> `t` tags
- `URL` -> `r` tag
- `GEO` -> `g` tag (geohash)
- Timezone: `DTSTART;TZID=` -> `start_tzid` tag, verbatim

## Reverse direction

On `PROPFIND` / `REPORT calendar-query`, synthesize VEVENTs from
cached NIP-52 events. ETag: hash of `(kind, pubkey, d-tag, created_at)`.

## Non-goals (v1)

- **Recurring events**. NIP-52 intentionally omits RRULE. Return
  `501 Not Implemented` for any ICS containing `RRULE`/`RDATE`/
  `EXDATE` and log a structured warning. Document this as a known
  limitation in `QUICKSTART.md`.
- **Timezones beyond the `start_tzid` tag**. Use client-reported TZ
  faithfully; no conversion on the wire.

## Reuse

`apps/gnostr/src/util/nip52_calendar.h` already has the parsing /
emitting layer (`gnostr_nip52_calendar_event_parse()` etc.). Either
link against it or factor it out into `nips/nip52/` proper -- decide
as part of this bead.

ICS parsing: use **libical** (already a transitive dep via
evolution-data-server). Stays in C11.

## Acceptance

- PUT ICS with date-only VEVENT -> signer emits valid kind-31922
  event, receipt seen on at least one relay from the user's NIP-65
  relay list.
- PROPFIND after PUT returns one VEVENT with correct UID/SUMMARY/
  DTSTART; ETag stable across refresh if event not updated.
- GNOME Calendar (gnome-calendar 46+) shows the event within 5 seconds.
EOF

make_child "nostr-dav: CardDAV <-> bespoke contacts kind (spec + wiring)" \
  2 "gnome,desktop,nostr-dav,contacts" <<'EOF'
## Goal

Implement the CardDAV side of `nostr-dav`. There is no NIP for
vCard-style personal contacts (NIP-02 follow lists and kind-0 profiles
cover only self-profiles + follows, not a general address book).

## Proposal

- New parameterized-replaceable kind: **30085** (chosen from the
  unassigned 30000-39999 range -- verify against
  `github.com/nostr-protocol/nips` kinds table at implementation time
  and pick a nearby free number if 30085 has been claimed).
- Tags: `["d", <uuid>]`, `["name", <FN>]`, `["t", "contact"]`,
  optional `["p", <npub-of-contact>]` when the contact has a resolved
  Nostr identity.
- `content`: stamped vCard 4.0 payload (UTF-8), no line-folding.
  Never encrypted at rest on relays (contacts are personal but not
  sensitive enough to justify NIP-44 overhead for v1 -- document
  threat model in `SECURITY.md`).

## Deliverables

- `docs/proposals/nip-contacts-draft.md` with kind reservation,
  field mapping, and test vectors (example vCard -> event -> vCard
  round-trip).
- DAV wiring: PUT vCard -> publish; PROPFIND -> aggregate cached
  events.
- vCard parsing in C via **libebook-contacts** (part of
  evolution-data-server) or a small hand-rolled parser if the
  vCard subset we emit is narrow enough.
- Follow-on work (not this bead): decide whether to submit upstream
  as a NIP or keep application-specific. Default: keep
  application-specific until at least one other client adopts it.

## Acceptance

- `docs/proposals/nip-contacts-draft.md` exists and lists five
  round-trip test vectors (check-in a `testdata/` dir of .vcf files
  and their expected event JSON).
- PUT a vcard via CardDAV -> GNOME Contacts displays the contact.
- Deleting the contact in GNOME Contacts -> NIP-09 deletion request
  emitted (or kind-5 for the d-tag if NIP-09 path is simpler).
EOF

make_child "nostr-dav: WebDAV Files backed by Blossom + NIP-94" \
  2 "gnome,desktop,nostr-dav,nip-94,blossom" <<'EOF'
## Goal

Implement the WebDAV Files service (the `/Files/` root of the GOA
WebDAV account) backed by Blossom (NIP-B7) content-addressed storage,
with NIP-94 (kind 1063) metadata events and kind-10063 user server
lists.

## Flow

- `PUT /Files/<path>`: upload blob to a Blossom server chosen from
  the user's kind-10063 list (fallback to a configured default if the
  user has no list). Publish kind-1063 with tags:
  `["url", <blossom-url>]`, `["x", <sha256-hex>]`, `["m", <mime>]`,
  `["path", <path>]`, `["size", <bytes>]`. Return `201 Created` with
  `ETag: <sha256>`.
- `GET /Files/<path>`: resolve `x` -> probe kind-10063 servers in
  preference order -> stream.
- `PROPFIND /Files/` (depth 1): list from cached kind-1063 events
  filtered by `path` tag prefix.
- `DELETE /Files/<path>`: NIP-09 deletion of the kind-1063 event;
  do not delete the Blossom blob (that is a separate lifecycle owned
  by the Blossom server).

## Reuse

`gnome/nostr-homed/src/common/blossom_client.c` already implements a
Blossom client in C. Extract the reusable bits into a new
`nips/nip-b7/` (or `libblossom/`) library and link both `nostr-homed`
and `nostr-dav` against it. Track the refactor in a follow-up bead.

HTTP uploads use **libsoup-3** (already chosen for the scaffold bead).

## Acceptance

- Drop a file into `~/gvfs/<webdav-account>/Files/` in Nautilus ->
  appears on a configured Blossom server, hash matches, corresponding
  kind-1063 event is signed and broadcast.
- `blossom-cache` (existing component) or another subscriber can
  retrieve the file by its hash alone.
EOF

make_child "nostr-dav: install, activation, and GOA onboarding UX" \
  2 "gnome,desktop,nostr-dav,ux" <<'EOF'
## Goal

End-to-end onboarding so a user who installs the nostrc package ends
up with a Nostr-backed calendar/contacts/files account in GNOME
Settings with no shell commands beyond the install.

## Scope

- systemd `--user` unit for `nostr-dav.service`, with
  `Requires=gnostr-signer.service` / `After=gnostr-signer.service`.
- First-run wizard implemented as a new sheet in
  `apps/gnostr-signer/src/ui/sheets/` ("Add to Online Accounts").
  Consistent with the existing signer UI (GTK4/libadwaita, C11).
- Wizard steps:
  1. Start `nostr-dav.service`.
  2. Provision a bearer token for the selected npub, store in
     libsecret with attribute `{service: "nostr-dav", npub: ...}`.
  3. Open Settings -> Online Accounts -> Add -> WebDAV via
     `xdg-desktop-portal` (Settings has a documented activation URI).
  4. Copy the server URL and token to clipboard with a visible
     "Paste this as password" instruction. (We cannot fill the GOA
     dialog directly.)
- Documentation: `gnome/nostr-dav/docs/QUICKSTART.md`.

## Acceptance

- Clean Ubuntu 24.04 VM + Fedora 40 VM: install the package, launch
  gnostr-signer, click "Add to Online Accounts", click through the
  wizard. Within 60 seconds the WebDAV account is added and GNOME
  Calendar shows at least one test event.
- Uninstall cleanly: `systemctl --user disable --now nostr-dav` plus
  GOA account removal (manual) leaves no stale libsecret entries.
EOF

# --- Optional polish (lower priority) -----------------------------------

make_child "GNOME Shell search provider for Nostr content" \
  3 "gnome,desktop,search-provider" <<'EOF'
## Goal

Add an `org.gnome.Shell.SearchProvider2` implementation for gnostr so
pressing `Super` and typing surfaces people (kind 0), notes (kind 1),
calendar events (NIP-52), and Blossom-backed files.

## Scope

- `.ini` file in `/usr/share/gnome-shell/search-providers/`
  referencing the existing gnostr `.desktop` and a bus path.
- D-Bus object in the gnostr main process implementing the
  SearchProvider2 interface.
- Reuse the existing NDB/relay cache via `nostr-gobject`.
- Launch handlers: people -> open profile view; notes -> thread view;
  calendar events -> calendar view (or GNOME Calendar if
  `nostr-dav` is configured); files -> Nautilus at the Blossom URL.

## Out of scope for v1

- Long-form articles (kind 30023)
- Wiki entries (NIP-54 kind 30818)
- Picture-first feeds (NIP-68 kind 20)

Track those as separate follow-ups if the search provider proves
useful.
EOF

make_child "Docs: authoritative NIP mapping cheatsheet for desktop integration" \
  3 "gnome,desktop,docs" <<'EOF'
## Goal

A single authoritative `docs/gnome-integration.md` that documents the
real NIP mapping used by nostr-dav, the signer bridge, and related
components -- so future contributors (human or LLM) do not
reintroduce the hallucinations that plagued earlier design docs
(notably "NIP-54 as generic document blobs", "NIP-94 as primary file
storage" before Blossom, etc.).

## Table to publish

| Domain | Kind(s) | NIP |
|---|---|---|
| Calendar, date-based | 31922 | NIP-52 |
| Calendar, time-based | 31923 | NIP-52 |
| Calendar collection | 31924 | NIP-52 |
| Calendar RSVP | 31925 | NIP-52 |
| Contacts (personal) | 30085 (proposed) | application-specific |
| Files, metadata | 1063 | NIP-94 |
| Files, storage | (HTTP blobs) | Blossom / NIP-B7 |
| Files, server list | 10063 | NIP-B7 (BUD-03) |
| Signer protocol | 24133 | NIP-46 |
| Secrets encryption | -- | NIP-44 v2 |
| Relay list | 10002 | NIP-65 |
| User metadata | 0 | NIP-01 |

## Additionally

- Document why `gnome/nostr-goa-overlay/` and `gnome/goa/` were
  removed (link to postmortem).
- Document why the WebDAV-bridge-against-stock-GOA path is correct,
  with citation to the upstream GOA change that shipped the generic
  WebDAV provider in 3.46 (April 2024).
- Document the `org.nostr.Signer` D-Bus interface as the stable
  trust boundary and link to
  `apps/gnostr-signer/data/dbus/org.nostr.Signer.xml`.

## Acceptance

- Doc committed at `docs/gnome-integration.md`.
- Linked from `docs/README.md` and from the parent epic's description.
- No open beads still reference "NIP-54" as the calendar/documents NIP.
EOF

echo
echo "Done. Epic: $epic_id"
echo "Run 'bd show $epic_id' to verify, then 'bd ready' to see what's pickable."
