# libhanami — Blossom Filesystem Extension & Tools for libgit2

> **Library Name**: libhanami
> **Language**: C
> **Dependencies**: libgit2, libnostr (nostrc), libcurl, OpenSSL
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
│ Backend  │ Backend  │  (blossom://)              │
├──────────┴──────────┴───────────────────────────┤
│                  libhanami                        │
│  ┌────────────┐ ┌──────────┐ ┌────────────────┐ │
│  │ Blossom    │ │ Nostr    │ │ OID ↔ SHA-256  │ │
│  │ HTTP Client│ │ Ref Mgr  │ │ Index          │ │
│  └────────────┘ └──────────┘ └────────────────┘ │
├─────────────────────────────────────────────────┤
│           nostrc primitives                      │
│  libnostr · nip34 · nip98 · blossom-cache        │
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
| 30617 | Repository Announcement | Advertise repo existence + clone URLs |
| 30618 | Repository State | Branch → commit mappings (ref state) |
| 1617 | Patch | `git format-patch` content |
| 1618 | Pull Request | PR with commit tip + clone URLs |
| 1619 | PR Update | Update PR tip |
| 1621 | Issue | Bug reports, feature requests |
| 1630–1633 | Status | Open/Applied/Closed/Draft |
| 10317 | User GRASP List | Preferred GRASP servers |

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

### 4.1 Blossom ODB Backend (`hanami_odb_backend`)

Implements `git_odb_backend` vtable:

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

Registers `blossom://` URL scheme with libgit2:

```c
git_transport_register("blossom", hanami_transport_new);
```

Handles:
- **Fetch**: Download packfile or individual objects from Blossom by hash
- **Push**: Upload new objects/packfiles to Blossom, publish 30618 state update
- **Ref advertisement**: Derived from kind 30618 events on Nostr relays

**Source**: `hanami-transport.c`, `hanami-transport.h`

### 4.4 OID ↔ Blossom Hash Index (`hanami_index`)

A local database mapping Git object IDs to Blossom SHA-256 hashes:

```c
typedef struct {
    char git_oid[65];       // hex SHA-1 (40) or SHA-256 (64) + null
    char blossom_hash[65];  // hex SHA-256 of raw content + null
    git_object_t type;      // blob, tree, commit, tag
    size_t size;            // object size
    int64_t timestamp;      // last access time
} hanami_index_entry;
```

Storage options (compile-time):
- SQLite (default, via nostrc's existing SQLite integration)
- LMDB (optional, via nostrc's LMDB backend from blossom-cache)

**Source**: `hanami-index.c`, `hanami-index.h`

### 4.5 Blossom HTTP Client (`hanami_blossom_client`)

Thin wrapper around libcurl implementing BUD-01/BUD-02 endpoints:

| Function | Endpoint | Auth |
|----------|----------|------|
| `hanami_blossom_get(hash)` | `GET /<sha256>` | None |
| `hanami_blossom_head(hash)` | `HEAD /<sha256>` | None |
| `hanami_blossom_upload(data, len)` | `PUT /upload` | NIP-98 (kind 24242) |
| `hanami_blossom_delete(hash)` | `DELETE /<sha256>` | NIP-98 (kind 24242) |
| `hanami_blossom_list(pubkey)` | `GET /list/<pubkey>` | Optional |
| `hanami_blossom_mirror(url)` | `PUT /mirror` | NIP-98 (kind 24242) |

Reuses patterns from `BcUpstreamClient` in blossom-cache.

**Source**: `hanami-blossom-client.c`, `hanami-blossom-client.h`

### 4.6 Nostr Client Integration (`hanami_nostr`)

Uses libnostr for:
- Relay connection management (`NostrRelay`, `NostrSimplePool`)
- Event creation and signing (`NostrEvent`)
- Subscription/filter management (`NostrFilter`, `NostrSubscription`)
- NIP-34 event parsing (from `nips/nip34`)

**Source**: `hanami-nostr.c`, `hanami-nostr.h`

### 4.7 Configuration (`hanami_config`)

Configurable via:
- `.gitconfig` keys under `[hanami]` section
- Environment variables
- Programmatic API

```ini
[hanami]
    endpoint = https://blossom.example.com
    relays = wss://relay.damus.io,wss://relay.nostr.band
    cache-dir = ~/.cache/hanami
    index-backend = sqlite
    upload-threshold = 32768
```

**Source**: `hanami-config.c`, `hanami-config.h`

### 4.8 Public API (`hanami.h`)

High-level convenience functions:

```c
// Initialize libhanami (call after git_libgit2_init)
int hanami_init(void);

// Shutdown
void hanami_shutdown(void);

// Create a Blossom-backed repository
int hanami_repo_open(git_repository **out, const char *blossom_endpoint,
                     const char **relay_urls, size_t relay_count,
                     const hanami_config *config);

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
    src/hanami-odb-backend.c
    src/hanami-refdb-backend.c
    src/hanami-transport.c
    src/hanami-index.c
    src/hanami-blossom-client.c
    src/hanami-nostr.c
    src/hanami-config.c
)

target_link_libraries(hanami
    PUBLIC  git2
    PRIVATE nostr nip34 nip98 curl sqlite3 OpenSSL::Crypto
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
2. Extract `clone` URLs (Blossom endpoints)
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
                 relay_urls, 2, &config);

// Standard libgit2 operations work transparently
git_reference *head;
git_repository_head(&head, repo);
// ...

hanami_shutdown();
git_libgit2_shutdown();
```
