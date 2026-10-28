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

This index is stored locally (SQLite or LMDB) and can optionally be
synchronized as a Blossom blob itself.

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
| `read` | Look up Blossom hash from OID index → `GET /<sha256>` → decompress → return |
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

**Source**: `hanami-index-sqlite.c`, `hanami-index.h`

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

**Source**: `hanami-grasp.c`, `hanami-grasp.h`

### 4.6 Nostr Client Integration (`hanami_nostr`)

Keeps NIP-34 event construction/parsing, signer adaptation and tight
kind/author/`#d` filters in Hanami. Delegates connection ownership, reconnect,
subscription dispatch and EOSE/CLOSED observation to libnostr's
`NostrSimplePool`. Snapshot queries reduce replayed IDs idempotently and choose
the newest event across subscribed relays, with the NIP-01 event-id tie-break.
EOSE completes catch-up; the deadline reports an error, not empty history.

The pool has no publish API, so Hanami retains its best-effort policy over
pool-owned relays using `nostr_relay_publish_and_wait()`: success requires at
least one `OK true`, never just a successful websocket send. `OK false` and
`CLOSED` fail visibly. NIP-42 challenges return an authentication error rather
than implicitly authorizing relay login. NIP-11 probing is not added here.

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
   queried EOSE-complete on the discovery relays, then on the announcement's
   `relays` when the discovery relays hold no list.

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
defaults only when none are configured.

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

// Clone from Blossom + Nostr discovery
int hanami_clone(git_repository **out, const char *nostr_uri,
                 const char *local_path, const hanami_config *config);

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
3. Fetch kind 30618 → resolve branch→commit mappings
4. Download Git objects from Blossom by hash
5. Reconstruct repository locally

### Push to Blossom

```c
hanami_push_to_blossom(repo, "https://blossom.example.com",
                       relay_urls, relay_count);
```

1. Walk all reachable objects from refs
2. For each: compute Blossom SHA-256, check existence via HEAD
3. Upload missing objects via PUT /upload with NIP-98 auth
4. Publish updated kind 30618 event with new ref→commit state

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
