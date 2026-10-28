# libhanami — Blossom Filesystem Extension & Tools for libgit2

> **Library Name**: libhanami
> **Language**: C
> **Dependencies**: libgit2, libblossom, libporthome, libnostr, nip34, SQLite, OpenSSL
> **Date**: 2026-04-20
> **Status**: Design

## 1. Overview

libhanami is a C library that implements custom libgit2 backends enabling Git
repositories to use **Blossom servers** as a decentralized, content-addressed
object store and **Nostr events** (NIP-34) as the coordination/ref layer.

The name "hanami" (花見, cherry blossom viewing) reflects its role as the
interface through which Git views Blossom-hosted data.

### Core Thesis

Git is already content-addressed. Blossom is content-addressed. Nostr provides
signed, distributed state announcements. By wiring these together through
libgit2's pluggable backend interfaces, we get **fully decentralized Git
hosting** where:

- **Blossom** = Object store (blobs, trees, commits, tags, packfiles)
- **Nostr relays** = Coordination layer (repo announcements, ref state, patches, PRs)
- **libgit2** = Git semantics engine

No centralized Git server is required.

## 2. Architecture

```
┌─────────────────────────────────────────────────┐
│                   Application                    │
│         (gnostr, CLI tools, other apps)          │
├─────────────────────────────────────────────────┤
│                   libgit2 API                    │
│         (clone, commit, push, fetch, etc.)       │
├──────────┬──────────┬───────────────────────────┤
│  ODB     │  RefDB   │  Transport                │
│ Backend  │ Backend  │  (blossom://, nostr://)    │
├──────────┴──────────┴───────────────────────────┤
│                  libhanami                        │
│  ┌────────────┐ ┌──────────┐ ┌────────────────┐ │
│  │ GRASP      │ │ NIP-34   │ │ SQLite index   │ │
│  │ Client     │ │ Helpers  │ │ implementation │ │
│  └────────────┘ └──────────┘ └────────────────┘ │
├─────────────────────────────────────────────────┤
│           nostrc primitives                      │
│  libblossom · libporthome · libnostr · nip34     │
└─────────────────────────────────────────────────┘
```

## 3. Key Design Constraints

### 3.1 Hash Algorithm Alignment

| System | Hash | Input |
|--------|------|-------|
| Git (classic) | SHA-1 | `"blob {len}\0" + content` |
| Git (SHA-256 mode) | SHA-256 | `"blob {len}\0" + content` |
| Blossom | SHA-256 | Raw content bytes |

**Git OID ≠ Blossom hash** because Git prepends a type+length header before
hashing. libhanami must maintain a **bidirectional index** mapping:

```
Git OID (SHA-1 or SHA-256) ←→ Blossom SHA-256 (raw content)
```

A reader cannot recompute this mapping. Blossom addresses the raw payload (or
sealed ciphertext), and distinct objects can share one address (the empty blob
and the empty tree). The mapping is therefore **published discovery
metadata**: the manifest chain of §4.4b in plaintext mode, or the sealed
ref-state node in private mode (§4.4a). The local SQLite index is a cache of
the published map.

### 3.2 Composite Backend Layering

libgit2 supports stacking ODB backends by priority:

```
[ memory cache (priority 3) ]
        ↓ miss
[ local disk cache (priority 2) ]
        ↓ miss
[ blossom remote (priority 1) ]
```

This enables offline work with transparent remote fallback.

### 3.3 NIP-34 Event Kinds Used

| Kind | Name | libhanami Usage |
|------|------|-----------------|
| 30617 | Repository Announcement | Advertise repo existence + Git clone URLs |
| 30618 | Repository State | Branch → commit mappings (ref state) |
| 1617 | Patch | `git format-patch` content |
| 1618 | Pull Request | PR with commit tip + clone URLs |
| 1619 | PR Update | Update PR tip |
| 1621 | Issue | Bug reports, feature requests |
| 1630–1633 | Status | Open/Applied/Closed/Draft |
| 10317 | User GRASP List | Preferred GRASP servers |
| 10063 | BUD-03 User Server List | Owner's Blossom servers (clone discovery) |

### 3.4 Authentication

Blossom uploads require NIP-98-style authorization:
- Create a kind 24242 event with `["method", "PUT"]`, `["u", "<url>"]` tags
- Sign with user's Nostr key
- Base64-encode the signed event JSON
- Send as `Authorization: Nostr <base64>` header

nostrc's `nip98` module provides `nostr_nip98_create_auth_event()` and
`nostr_nip98_create_auth_header()` which can be adapted for BUD-02 auth
(kind 24242 vs kind 27235).

## 4. Components

Hanami owns Git-specific behavior only. Blossom HTTP, shim and capability
probing belong to **libblossom**; BUD-02 authorization belongs to
**nips/bud02**. The storage-independent OID-to-sealed-address index interface
belongs to **libporthome** (`<porthome/index.h>`). Hanami retains its SQLite
implementation and Git-facing row adapter, not a second shared index interface.

### 4.1 Blossom ODB Backend (`hanami_odb_backend`)

Implements `git_odb_backend` vtable, delegating HTTP operations to libblossom:

| Method | Behavior |
|--------|----------|
| `read` | Look up Blossom hash from OID index → `GET /<sha256>` (wire hash verified) → re-hash with the row's type and require the requested Git OID (plaintext and sealed) → return |
| `read_header` | `HEAD /<sha256>` → return size and type from index |
| `write` | Compute Blossom hash of raw content → `PUT /upload` with auth → store OID mapping |
| `exists` | Check OID index, fallback to `HEAD /<sha256>` |
| `writepack` | Receive packfile → upload as single Blossom blob → unpack index |
| `free` | Clean up connections and index handle |

**Source**: `hanami-odb-backend.c`, `hanami-odb-backend.h`

### 4.2 Nostr RefDB Backend (`hanami_refdb_backend`)

Implements `git_refdb_backend` vtable:

| Method | Behavior |
|--------|----------|
| `exists` | Query local cache of kind 30618 state event |
| `lookup` | Parse ref from cached 30618 event tags |
| `iterator` | Iterate all refs from 30618 event |
| `write` | Update local state → publish new 30618 event to relays |
| `rename` | Update ref name in state → republish |
| `del` | Remove ref from state → republish |
| `free` | Clean up relay connections |

Refs are derived from the **latest kind 30618 event** signed by the repo
maintainer. The backend subscribes to relay updates and caches locally.

**Source**: `hanami-refdb-backend.c`, `hanami-refdb-backend.h`

### 4.3 Blossom Transport (`hanami_transport`)

Registers `blossom://` and `nostr://` URL schemes with libgit2:

```c
git_transport_register("blossom", hanami_transport_new);
```

Handles:
- **Fetch**: Download packfile or individual objects from Blossom by hash
- **Push**: Upload new objects/packfiles to Blossom, publish 30618 state update
- **Ref advertisement**: Derived from kind 30618 events on Nostr relays

**Source**: `hanami-transport.c`, `hanami-transport.h`

### 4.4 SQLite Index Implementation (`hanami_index`)

Persists Git OID mappings in SQLite. `hanami_index_porthome()` exposes a
borrowed `porthome_index` view implementing libporthome's storage-independent
interface. The legacy `hanami_index_entry_t` is a Git-specific row adapter;
the shared OID/sealed-address/key-id type and vtable live in libporthome.
Raw-mode rows use key id zero. In sealed mode the ODB seals each object's
content with PCS v2 under the latest root-ring key and verifies its Git OID
again after decryption. Private snapshots carry the shared index row plus
Git-only `type` and exact plaintext `size`, required to decode padded chunks.

The schema is versioned in a `hanami_meta` table (`schema_version` 1, stamped
onto older unversioned databases on open; newer versions are refused). A
cloned repository keeps its cache in `.git/hanami/index.sqlite`, with the
newest fully imported manifest segment recorded as `manifest_head`, its chain
length as `manifest_depth`, and OIDs covered by the published chain in
`manifest_rows`; other rows are local uploads not yet published. Rows are
keyed by Git OID. `hanami_index_get_by_blossom` refuses (`HANAMI_ERR_INDEX`) a
hash mapped from more than one OID instead of returning an arbitrary row.

**Source**: `hanami-index-sqlite.c`, `hanami-index.h`

### 4.4b Object manifest chain (hanami extension to NIP-34 30618, 0.6.0)

Plaintext repositories publish the OID map as a chain of manifest segments on
Blossom. Each segment is a JSON blob (canonical form: compact, sorted keys):

```json
{"prev":null,"repo":"<30618 d>","rows":[{"git_oid":"<40 hex>","sha256":"<64 hex>","size":12,"type":3}],"v":1}
```

- `rows` are strictly ascending by `git_oid`, with at most 65,536 rows and
  16 MiB per segment (the sealed snapshot bounds). `type` is the libgit2
  object type (1 commit, 2 tree, 3 blob, 4 tag) and `size` is the payload
  length. `sha256` is the Blossom address of the raw payload.
- `prev` names the previous segment. A segment with `prev: null` is a
  **checkpoint** holding every row; other segments hold only the rows added
  since `prev`.
- The owner's kind-30618 carries the head as a hanami extension tag:
  `["hanami-manifest", "<sha256 of newest segment>"]`. NIP-34 neither
  defines nor forbids extra 30618 tags. The state event signature binds
  refs and objects atomically.

**Readers** (`hanami_clone`, `hanami_fetch`):

- Accept exactly one well-formed tag.
- Walk `prev` from the head back to a checkpoint, or to the cache's
  `manifest_head` (incremental fetch). Chains deeper than **64** segments are
  refused.
- Decode strictly: exact field sets, version 1, matching `repo`, bounds and
  OID order. Rows import in one transaction, and existing rows win.
- Require every 30618 ref and HEAD target to be covered.
- Fetch every object through the verifying ODB `read` (§4.1). A manifest row
  therefore can never produce wrong content, only a visible error.

**Writers** (`hanami_manifest_publish_state`) publish data before state:

1. Every local object is uploaded unless the server already has it, and
   recorded in the cache as an unpublished row.
2. The cache catches up to the signer's published chain. This is a base
   check, not compare-and-swap. Every ref and HEAD target must be covered.
3. One segment is uploaded. It is a **checkpoint** with every row when there
   is no base (first push, or a 30618 republished without the tag) or the
   chain already has **16** segments. Otherwise it is a delta of the
   unpublished rows. A refs-only change reuses the current head.
4. The 30618 carries refs (local refs only, no `refs/remotes/`), HEAD and
   the tag. It goes through the relay-accepted publish (OK true, bounded
   NIP-42 retry), with `created_at` strictly newer than any observed state.

A push succeeds only when the object uploads, the segment upload and the
30618 acceptance all succeed. Rows are marked published (`manifest_rows`)
only after relay OK, so a failed push is completed by the next one. A crash
after OK is repaired by the next sync.

The publishers are:

- `hanami_push_to_blossom` / `_ex`;
- the `blossom://` transport push. It publishes the repository's full local
  ref state, not individual refspecs, because libgit2 exposes no push specs to
  custom transports. The URL owner must be the signer;
- the refdb of `hanami_repo_open`, which publishes rows of objects written
  through the Blossom ODB. It also imports the owner's chain at open, so
  existing remote objects are readable;
- `hanami_publish_state`, which uploads nothing and carries the signer's
  published head forward. It cannot read the chain without a Blossom client,
  so it accepts only targets that already appear in the published 30618.

The writer refuses an author that is not the context's signer
(`HANAMI_ERR_AUTH`): that state would land where readers never look. A sync
that reaches a checkpoint without meeting the cached head prunes published
markers to that chain, so a replaced chain cannot make uncovered refs look
published.

GRASP state publication (`hanami_push_to_grasp`) does not carry the tag: its
Git data lives on the GRASP server.

**Other NIP-34 clients:** a client that republishes the 30618 without the tag
leaves hanami nothing to locate objects with. `hanami_clone` and
`hanami_fetch` then fail with `HANAMI_ERR_NOT_FOUND`, and `git_error_last()`
names the missing tag and suggests the announcement's Git clone URL. They never
guess. The same applies after a GRASP push to the same repository identifier.
The next hanami push writes a fresh checkpoint, because a tagless base forces
one.

**Relation to sealed mode:** both modes share the row validation and bounds.
Sealed mode keeps its index inside the sealed ref-state node referenced from
the private 30078 head (§4.4a), because refs and OIDs must stay secret. Its
wire format is unchanged.

**Source**: `hanami-manifest.c`, `hanami-fetch.c`

### 4.4a Sealed Git assembly (SV-8, libhanami 0.4.0)

`hanami_repo_open(..., config, vault)` selects private mode when `vault` is
non-NULL. The ring is copied; `repo_id` is the **random vault_id rendered as
64 lowercase hex characters**, not a name or key-derived ID. Only an owner
signer can initialize an EOSE-confirmed empty vault. Initialization creates an
empty sealed dir_node with owner writer policy; existing vaults use their
owner-signed root policy. No libvault filesystem fold is applied to Git.

Each object is a PCS v2 chunk. The refdb uploads a v2 manifest-domain sealed
JSON ref-state node containing `refs`, `index`, `prev`, and a durable `public`
lift marker. Ref entries use `refname`/`target_oid` (symbolic targets retain
`ref: `). Index rows use the shared `git_oid`/`sealed_sha256`/`chunk_key_id`
fields plus `type`/`size`. `prev` is the previous sealed ref-state address or
null. This experimental codec rejects duplicate/unknown fields and malformed
rows, and bounds snapshot bytes and row counts. Addresses are the probe-selected
wire hashes, including a PNG prefix if libblossom selects shimming; reads verify
the wire address before unwrapping/decrypting. Epochs above UINT32_MAX are
refused because the shared index key ID is uint32_t. Classic SHA-1 Git OIDs are
supported by this libgit2 build; SHA-256 OIDs are not silently truncated.

The snapshot address is in a signed, NIP-44-sealed kind-30078 head's `ref_state`.
Public 30618 is neither queried nor published for private state. Snapshot reads
require EOSE; invalid signatures, wrong epochs, corrupt/missing objects and
relay refusals fail visibly. A ref mutation becomes committed only after blob
upload and relay `OK true`; failures restore the last committed ref cache.
The writer slot and owner's root policy are checked before publish. This detects
an already-moved base, **not atomic compare-and-swap**: simultaneous writes by
the same identity can still race under Nostr addressable-event semantics.

`hanami_clone_sealed` invokes libgit2 clone with per-operation transport/ring
state and a sealed remote ODB attached to the destination. Checkout files and
local Git refs are plaintext, as expected for a working copy. `hanami_repo_sync`
uploads local objects through the sealed backend before checkpointing local
refs. `hanami_repo_fetch_writer` validates the writer against the owner's signed
root and imports branch tips under `refs/remotes/<writer>/<branch>`, without
advancing local branches. Applications use libgit2 merge APIs and explicitly
sync the result. There is no hidden LWW merge or vault filesystem conflict fold.

Public Blossom/GRASP pushes are refused for private repositories, including a
local repository reopened without its ring (the nonsecret `hanami.vault-id`
marker survives). Only `hanami_repo_lift_privacy` by the owner mirrors all objects
as plaintext, ACKs a public 30618 projection, and checkpoints the irreversible
lift marker in private state; sealed state remains authoritative. Failed lifts
can already have disclosed objects and never authorize public pushes. Privacy
cannot be restored by this API. Pushing with raw libgit2 HTTP APIs is outside
Hanami's guard; callers must use the Hanami entrypoints for this policy.

The ordinary Blossom transport guards stay enabled. Local fixtures explicitly
opt in with `hanami_config_set_allow_insecure(config, true)`; no URL automatically
weakens them. Signer callback data must outlive returned repositories. Repository
handles/backends are externally serialized, like the borrowed index interface.

**Compatibility:** 0.4.0 changes `hanami_repo_open`'s source/ABI signature: existing
callers append NULL and rebuild to retain raw mode. The low-level raw backend
options remain unchanged. No libporthome or libvault ABI changes are required.

### 4.5 GRASP Client (`hanami_grasp`)

Git repository discovery and publication using NIP-34 announcements, GRASP
server URL parsing, and GRASP-specific announcement tags. Uses the NIP-34
helpers below; it does not implement Blossom HTTP or BUD-02 authentication.

Before any GRASP push publication or fetch transfer/query, perform one HTTP GET
of the clone authority's root relay URL, replacing `ws(s)` with `http(s)` and
sending `Accept: application/nostr+json`. Parse through `nips/nip11` using the
existing curl client dependency. Require an exact `GRASP-01` string in the
`supported_grasps` string array. A reachable document missing that advertisement
returns `HANAMI_ERR_CONFIG`; transport errors, non-2xx HTTP (including redirects)
and the 15-second request deadline return `HANAMI_ERR_NETWORK`. Invalid JSON,
wrong GRASP field types, embedded NUL bytes, documents over 1 MiB or missing
required `repo_acceptance_criteria` return `HANAMI_ERR_NOSTR`. No failure means
capability support or successful completion; none publishes a corrective state.
The deadline bounds a failed HTTP request, not relay backfill completion.

The probe is cached only for that operation: state queries, replica publication,
Git transfer and recovery do not probe again. Plain NIP-34/Blossom clone/fetch
paths do not probe NIP-11. A NULL-repository GRASP fetch now performs capability
preflight only; use `hanami_grasp_parse_clone_url` for offline URL parsing.

`hanami_grasp_push_result_t.server` and the optional server output from
`hanami_grasp_fetch_with_result` retain the advertised `supported_grasps`,
`repo_acceptance_criteria`, optional `curation`, and `supports_grasp05` flag.
Metadata survives missing-GRASP-01 and subsequent operation refusals so callers
can show the server's policy; the caller frees it with
`hanami_grasp_metadata_free`, including before result reuse. Calls without an
output free it internally. Absent `curation` means no extra curation advertised,
not an invented policy. GRASP-05 is informational only: announcement-listing
behavior is unchanged.

Push authorization follows [GRASP-01](https://ngit.dev/protocol/grasp/specification/01):
fetch the signer's previous state through EOSE from the clone authority's root
WebSocket relay, merge the requested destination ref updates into that complete
state, sign, and require that root's event-ID-specific `OK true` before calling
`git_remote_push`. The d-tag identifier must match the percent-decoded clone-path
identifier, and signed state must use the configured signer's key. The signer
may be an authorized maintainer rather than the clone URL's owner; the GRASP
server still enforces its recursive-maintainer push policy. Other configured relays are optional replicas; their ACKs
cannot authorize this endpoint. The existing named remote must target the same
clone URL. Default pushes include only heads; explicit force, wildcard and
removal refspecs use libgit2 parsing. Negotiated OIDs must still match the signed
plan, and per-ref receive-pack rejection is a failure even if transport succeeds.

The historical reversal in `85914cb3d` / `nostrc-dwj` attempted to prevent a
failed push advertising absent refs. It had no server-specific justification.
Restore protocol ordering without losing that concern: before announcement,
prepare a second signed event with the saved prior refs/HEAD (empty on a first
push). If the announcement is unconfirmed or the transfer fails, publish this
corrective event to the same root and require its `OK true`. Its timestamp is
strictly newer than the intended event, itself newer than the previous state;
same-second event-ID tie-breaking cannot leave the failed intent current.
`hanami_push_to_grasp_with_result` reports original and corrective outcomes.
A corrected transfer failure is still a failure, never `HANAMI_OK`.

Correction repairs signed authorization, not partially applied Git refs/objects.
If correction is refused or unconfirmed, the API exposes recovery failure: the
intended state may remain pending and needs explicit caller repair/retry. This
is a synchronous compensation, not a durable transaction or crash recovery.
Callers serialize operations for the same signed coordinate; concurrent writers
cannot safely share a saved baseline. GRASP purgatory is a server visibility
boundary, not evidence of delivered Git data or a client rollback mechanism.

Fetch downloads Smart HTTP objects without updating local refs, then queries the
configured relays for the owner's latest kind-30617 announcement (if present)
and the newest signed kind-30618 state from the owner and its listed maintainers.
Each author query completes at EOSE; replayed event IDs have no effect, and
states are ordered by `created_at`, then event ID. The clone URL binds the owner
and repository identifier. Every advertised Git ref tip must exactly match a
state ref, and every state ref must be advertised. `HEAD` and peeled-tag aliases
are not independent refs. Only after the complete comparison may libgit2 update
`refs/remotes/grasp/*` (for example `refs/heads/main` maps to
`refs/remotes/grasp/heads/main`). A missing state, CLOSED, AUTH without a fetch
signer, timeout, extra ref or mismatched tip is an error; downloaded objects
alone do not authorize a local ref update. `hanami_grasp_fetch_with_signer`
allows a bounded NIP-42 retry: the relay must acknowledge the signed AUTH with
`OK true` before the state query is retried. The original entrypoint has no
signer and fails visibly on an AUTH-gated read. Fetch accepts at most eight
relays, 16 listed maintainers, 2048 advertised refs and 64 KiB serialized
state events.

[GRASP-08](https://ngit.dev/protocol/grasp/specification/08) additionally needs
NIP-42 and repository-scoped HTTP NIP-98. This helper does not claim that profile:
the private/sealed repository guard remains in force before network activity.

**Source**: `hanami-grasp.c`, `hanami-grasp.h`

### 4.6 Nostr Client Integration (`hanami_nostr`)

Keeps NIP-34 event construction/parsing, signer adaptation and tight
kind/author/`#d` filters in Hanami. Delegates connection ownership, reconnect,
subscription dispatch and EOSE/CLOSED observation to libnostr's
`NostrSimplePool`. Snapshot queries reduce replayed IDs idempotently and choose
the newest event across subscribed relays, with the NIP-01 event-id tie-break.
EOSE completes catch-up; the deadline reports an error, not empty history.

The pool has no publish API, so Hanami applies an explicit any-one acceptance
policy over pool-owned relays using `nostr_relay_publish_and_wait()`: success
requires at least one application event's `OK true`, never just a websocket send
or an AUTH acknowledgment. GRASP's root-only context makes that specific relay
mandatory. `OK false`, missing ACK, no connected relay and subscription `CLOSED`
are errors when the operation's policy is not met; a refusal from an optional
replica cannot veto another accepting relay in the generic publisher.

A signer supplied to a context authorizes NIP-42 on that context's configured
relays. AUTH callbacks only wake the caller; the retained current-connection
challenge also covers AUTH received during pool dialing, before callback
installation. A connection/challenge generation records completed authentication;
retaining or repeating the same challenge does not cause another login on every
operation. Reconnects or different challenges invalidate that record. A challenge
arriving during a publication wait is serviced after the failed attempt within
the same one-AUTH retry budget, even when the relay omitted an OK refusal.
The operation thread signs kind 22242 with the exact relay and
challenge tags, sends an `AUTH` frame and waits for its event-ID-specific
`OK true`. Only then may it retry the identical signed application event once.
No signer, refused AUTH, missing AUTH OK, or another auth-required refusal fails
visibly. Authentication never blocks the relay reader. Query retry joins and
removes the old subscriptions, discards partial events and starts a fresh
EOSE-complete snapshot. NIP-11 probing is not added here.

Public refdb write, rename and deletion now operate on a private cache copy.
Only accepted publication commits that copy; any build/sign/auth/relay failure
returns a libgit2 reference error and restores the original cache, including a
force-renamed destination. Private refs retain their authoritative sealed-state
recovery. State timestamps advance beyond prior attempts and fetched state to
avoid same-second replacement ties. Missing ACK may mean the event exists on a
relay: local rollback is not distributed rollback. Future-timestamp rejection
is an error, not a reason to report cache-only success. Backend access remains
externally serialized.

**Source**: `hanami-nostr.c`, `hanami-nostr.h`

### 4.7 Configuration (`hanami_config`)

Configurable via:
- `.gitconfig` keys under `[hanami]` section
- Environment variables
- Programmatic API

```ini
[hanami]
    endpoint = https://blossom.example.com
    relays = wss://nos.lol,wss://relay.nostr.band
    cache-dir = ~/.cache/hanami
    index-backend = sqlite
    upload-threshold = 32768
```

**Source**: `hanami-config.c`, `hanami-config.h`

### 4.7a Blossom server discovery for clone

NIP-34 `clone` tags are URLs "to be given to `git clone`" (for example a GRASP
Smart HTTP endpoint), not Blossom APIs, and NIP-34 defines no Blossom tag for
kinds 30617/30618. `hanami_clone` therefore never derives the Blossom endpoint
from `clone`. Candidates, in order:

1. the explicit `hanami.endpoint` / `HANAMI_ENDPOINT` / programmatic endpoint;
2. the repository owner's BUD-03 kind-10063 `server` tags, in listed order,
   taken from one EOSE-complete snapshot over the discovery relays ∪ the
   announcement's `relays`, so the newest replaceable list wins across both.

Each candidate is classified with libblossom's capability probe. Only positive
Blossom evidence counts (server-info JSON, an accepted BUD-02 upload, or a
BUD-02 401 auth refusal). A bare HTTP answer, such as a Git Smart HTTP host
returning 404, does not. The first Blossom server wins and is recorded as
`hanami.endpoint` in the clone's repository config. When no candidate exists,
clone fails with `HANAMI_ERR_CONFIG`. When no candidate is a Blossom server, it
fails with `HANAMI_ERR_BLOSSOM`. A failed server-list query returns its own
error. In each case clone creates no directory, and the libgit2 last-error
message names the cause.

Discovery relays are the configured `hanami.relays`, falling back to public
defaults only when none are configured. The kind-30618 snapshot uses the same
union, so a state held only by the discovery relays is not mistaken for an
empty repository. The union is recorded as `hanami.relays` in the clone.

**Compatibility (libhanami 0.6.0):** announcements that put a Blossom API URL
in `clone` are no longer treated as Blossom endpoints. Their owners publish a
kind-10063 list, or cloners set `hanami.endpoint`. Keeping the old fallback
would silently misroute every repository with a genuine Git clone URL.

**Source**: `hanami-discovery.c`

### 4.8 Public API (`hanami.h`)

High-level convenience functions:

```c
// Initialize libhanami (call after git_libgit2_init)
int hanami_init(void);

// Shutdown
void hanami_shutdown(void);

// Create a Blossom-backed repository
hanami_error_t hanami_repo_open(git_repository **out, const char *blossom_endpoint,
                     const char **relay_urls, const char *repo_id,
                     const char *owner_pubkey, const hanami_signer_t *signer,
                     const hanami_config_t *config, const porthome_ring_t *vault);

// Register blossom:// transport globally
int hanami_transport_register(void);

// Create and register ODB backend for an existing repo
int hanami_odb_backend_new(git_odb_backend **out,
                           const char *endpoint,
                           const hanami_config *config);

// Create and register RefDB backend for an existing repo
int hanami_refdb_backend_new(git_refdb_backend **out,
                             const char **relay_urls, size_t relay_count,
                             const char *repo_id, const char *owner_pubkey);

// Push all local objects to Blossom + publish NIP-34 state
int hanami_push_to_blossom(git_repository *repo,
                           const char *endpoint,
                           const char **relay_urls, size_t relay_count);

// Clone from Blossom + Nostr discovery (durable local repository)
int hanami_clone(git_repository **out, const char *nostr_uri,
                 const char *local_path, const hanami_config *config);

// Incremental fetch into refs/remotes/<remote>/ of a hanami clone
hanami_error_t hanami_fetch(git_repository *repo, const char *remote_name,
                            const hanami_signer_t *signer,
                            const hanami_config_t *config);

// Publish repository announcement (kind 30617)
int hanami_announce_repo(const char *repo_id, const char *name,
                         const char *description,
                         const char **clone_urls, size_t clone_count,
                         const char **relay_urls, size_t relay_count);

// Publish repository state (kind 30618)
int hanami_publish_state(const char *repo_id,
                         const hanami_ref_state *refs, size_t ref_count,
                         const char **relay_urls, size_t relay_count);
```

## 5. nostrc Primitive Reuse

| nostrc Component | Reuse in libhanami |
|------------------|--------------------|
| `libnostr` (events, keys, relay, filter) | All Nostr operations — event creation, signing, relay connections, subscriptions |
| `nips/nip34` (repository, patch parsing) | Parse/create kind 30617, 30618, 1617, 1618 events |
| `nips/nip98` (HTTP auth) | Adapt for BUD-02 kind 24242 upload authorization |
| `apps/blossom-cache` (blob store, HTTP) | Patterns for SHA-256 verification, blob storage, HTTP endpoint handling |
| `apps/blossom-cache` (upstream client) | Blossom HTTP client patterns (GET/HEAD/PUT via libsoup→libcurl) |
| `apps/blossom-cache` (DB backends) | SQLite and LMDB index storage for OID mapping |
| `apps/gnostr/plugins/nip34-git` | libgit2 integration patterns, git_repository usage |
| `libjson` | JSON serialization for Nostr events and Blossom descriptors |
| `libnostr/crypto` | SHA-256 computation, key handling |

## 6. Distribution

### 6.1 Shared Library Plugin (Primary)

Build as `libhanami.so` / `libhanami.dylib` / `hanami.dll`:

```bash
# Register on load via constructor
__attribute__((constructor))
static void hanami_auto_register(void) {
    hanami_init();
    hanami_transport_register();
}
```

Users load via `LD_PRELOAD` or link directly.

### 6.2 Static Library

For embedding in applications like gnostr:

```cmake
target_link_libraries(gnostr PRIVATE hanami_static)
```

### 6.3 CMake Integration

```cmake
find_package(hanami REQUIRED)
target_link_libraries(myapp PRIVATE hanami::hanami)
```

## 7. Build System

CMake, integrated into nostrc's existing build:

```cmake
# nostrc/libhanami/CMakeLists.txt
add_library(hanami
    src/hanami.c
    src/hanami-sealed.c
    src/hanami-odb-backend.c
    src/hanami-refdb-backend.c
    src/hanami-transport.c
    src/hanami-index-sqlite.c
    src/hanami-grasp.c
    src/hanami-nostr.c
    src/hanami-config.c
)

target_link_libraries(hanami
    PUBLIC  PkgConfig::LIBGIT2 nostrc::blossom nostrc::porthome
    PRIVATE nostrc::vault nostr nip34 PkgConfig::SQLITE3
            PkgConfig::HANAMI_JANSSON OpenSSL::Crypto pthread
)
```

## 8. Test Strategy

- **Unit tests**: Per-component tests (ODB read/write, refdb lookup, index CRUD)
- **Integration tests**: End-to-end clone → commit → push against a local blossom-cache instance
- **Mock server**: Lightweight HTTP server for testing Blossom API interactions
- **Test fixtures**: Pre-computed Git objects with known OID↔Blossom hash mappings

## 9. Workflow Examples

### Clone from Blossom + Nostr

```c
hanami_config config = { .cache_dir = "/tmp/hanami-cache" };
git_repository *repo;
hanami_clone(&repo, "nostr://npub1.../my-repo", "/tmp/my-repo", &config);
```

1. Resolve `nostr://` URI → query relays for kind 30617 event
2. Select the Blossom server from config or the owner's kind-10063 list (§4.7a);
   `clone` URLs are Git remotes and are not used as Blossom APIs
3. Fetch the owner's kind 30618: refs plus the `hanami-manifest` head (§4.4b)
4. Import the manifest chain into `.git/hanami/index.sqlite`; check coverage
5. `git_clone` through a per-operation transport. It downloads each object
   reachable from the wanted tips, verifies it against its Git OID, and writes
   it to the local ODB only after all its children are present. libgit2
   writes `refs/remotes/origin/*` and checks out HEAD; checkout failure is
   fatal
6. The result is a plain Git repository (`origin` = the `nostr://` URI,
   `hanami.endpoint`, `hanami.relays`); no Blossom/Nostr backends stay
   attached

`hanami_fetch(repo, NULL, signer, config)` repeats steps 3–5 incrementally.
It imports only segments newer than the cached head, downloads only objects
missing locally, and updates `refs/remotes/origin/*` without moving local
branches.

### Push to Blossom

```c
hanami_push_to_blossom(repo, "https://blossom.example.com",
                       relay_urls, relay_count);
```

1. For every local object: compute the Blossom SHA-256, check existence via
   HEAD, upload if missing (BUD-02 auth), record in `.git/hanami/index.sqlite`
2. Catch up to the published manifest chain; upload a delta segment of the
   unpublished rows (or a checkpoint, §4.4b)
3. Publish kind 30618 with refs, HEAD and `["hanami-manifest", <segment>]`;
   success requires relay OK true

`hanami_push_to_blossom_ex(..., config)` is the same with
`hanami_config_set_allow_insecure(config, true)` honoured, for loopback/http
fixtures.

### Transparent libgit2 Usage

```c
git_libgit2_init();
hanami_init();

// Open repo with Blossom ODB + Nostr RefDB
git_repository *repo;
hanami_repo_open(&repo, "https://blossom.example.com",
                 relay_urls, "repo-name", owner_pubkey, signer, config, NULL);

// Standard libgit2 operations work transparently
git_reference *head;
git_repository_head(&head, repo);
// ...

hanami_shutdown();
git_libgit2_shutdown();
```
