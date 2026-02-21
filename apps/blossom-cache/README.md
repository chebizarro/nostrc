# blossom-cache — Local Blossom Cache Server

A standalone, GObject-based local cache for the [Blossom](https://github.com/hzrd149/blossom) media
storage protocol. Implements the [local-blossom-cache specification](https://github.com/hzrd149/blossom/blob/master/implementations/local-blossom-cache.md)
and relevant BUD endpoints. Designed to work as a transparent local cache that integrates
with **gnostr** and other Nostr clients.

## Features

- **Local HTTP server** on `http://127.0.0.1:24242` (spec-mandated port aligned with Nostr kind 24242)
- **Content-addressed blob storage** — SHA-256 hashed with 2-char prefix directory fanout
- **Transparent upstream fetching** — cache miss triggers automatic fetch from configured Blossom servers
- **BUD-10 proxy hints** — `?xs=` server hints for targeted upstream fetching
- **LRU eviction** — configurable cache size limits with least-recently-used eviction
- **SHA-256 integrity verification** — optional hash validation on every store operation
- **Range requests** — RFC 7233 partial content for efficient video/large file streaming
- **Pluggable metadata backend** — SQLite (default) or LMDB via a vtable abstraction
- **GSettings configuration** — full runtime configurability with sensible defaults
- **CORS on all responses** — `Access-Control-Allow-Origin: *` per BUD-01

## Quick Start

```bash
# Build (from nostrc root)
cmake -B _build -DENABLE_BLOSSOM_CACHE=ON
cmake --build _build --target blossom-cache

# Run
./_build/apps/blossom-cache/blossom-cache

# Health check
curl -I http://127.0.0.1:24242/

# Upload a blob
curl -X PUT --data-binary @photo.jpg -H "Content-Type: image/jpeg" \
  http://127.0.0.1:24242/upload

# Retrieve it
curl http://127.0.0.1:24242/<sha256>.jpg -o photo.jpg

# Cache statistics
curl http://127.0.0.1:24242/status
```

## HTTP API

Implements the [local-blossom-cache spec](https://github.com/hzrd149/blossom/blob/master/implementations/local-blossom-cache.md),
[BUD-01](https://github.com/hzrd149/blossom/blob/master/buds/01.md) (Server Requirement),
[BUD-02](https://github.com/hzrd149/blossom/blob/master/buds/02.md) (Blob Operations),
and [BUD-10](https://github.com/hzrd149/blossom/blob/master/buds/10.md) (Proxy Hints).

| Method   | Path                   | Spec        | Description                                         |
|----------|------------------------|-------------|-----------------------------------------------------|
| `HEAD`   | `/`                    | local-cache | Health check — returns `200 OK`, no body            |
| `GET`    | `/<sha256>[.ext]`      | BUD-01      | Retrieve a blob (supports range requests)           |
| `HEAD`   | `/<sha256>[.ext]`      | BUD-01      | Check existence + `Content-Type`/`Content-Length`    |
| `PUT`    | `/upload`              | BUD-02      | Store a blob, returns Blob Descriptor JSON          |
| `DELETE` | `/<sha256>`            | BUD-02      | Delete a cached blob                                |
| `GET`    | `/list/<pubkey>`       | BUD-02      | List blobs with `?cursor=`/`?limit=` pagination     |
| `GET`    | `/status`              | extension   | JSON cache statistics                               |
| `OPTIONS`| `*`                    | BUD-01      | CORS preflight with `Authorization, *` headers      |

### Blob Descriptor Response

`PUT /upload` returns a JSON Blob Descriptor per BUD-02:

```json
{
  "url": "http://127.0.0.1:24242/b1674191a88ec5cdd733e4240a81803105dc412d6c6708d53ab94fc248f4f553.jpg",
  "sha256": "b1674191a88ec5cdd733e4240a81803105dc412d6c6708d53ab94fc248f4f553",
  "size": 102400,
  "type": "image/jpeg",
  "uploaded": 1708473600
}
```

### Proxy Hint Query Parameters (BUD-10)

On `GET /<sha256>`, the server accepts hint parameters for upstream resolution:

- **`?xs=<server>`** — Server hints where the blob may be available (repeatable).
  Bare domains are normalized to `https://`. These servers are tried first, before
  the configured upstream list.
- **`?as=<pubkey>`** — Author hints for `kind:10063` server list lookup (requires
  Nostr relay client — not yet implemented, parsed but not acted upon).

```bash
# Fetch with server hints
curl "http://127.0.0.1:24242/b167...553.pdf?xs=cdn.example.com&xs=media.nostr.build"
```

### Range Requests (RFC 7233)

All blob responses include `Accept-Ranges: bytes`. Clients can request partial content:

```bash
# First 1MB of a video
curl -H "Range: bytes=0-1048575" http://127.0.0.1:24242/<sha256>.mp4
```

The server responds with `206 Partial Content` and appropriate `Content-Range` headers.

### CORS

All responses include `Access-Control-Allow-Origin: *` per BUD-01. Error responses
include `X-Reason` headers with human-readable explanations.

`OPTIONS` preflight requests return:
- `Access-Control-Allow-Methods: GET, HEAD, PUT, DELETE`
- `Access-Control-Allow-Headers: Authorization, *`
- `Access-Control-Max-Age: 86400`

### Access Control

Per the local-blossom-cache spec, no authentication is required. All requests are served
without an `Authorization` header. Access is implicitly restricted by binding to `127.0.0.1`.

## Architecture

```
┌─────────────┐     HTTP      ┌──────────────┐
│   gnostr     │──────────────▶│ BcHttpServer │
│ (or any      │               │ (libsoup 3)  │
│  Nostr app)  │               └──────┬───────┘
└─────────────┘                       │
                                      ▼
                              ┌──────────────┐
                              │BcCacheManager│
                              │(policy+evict)│
                              └──┬───────┬───┘
                                 │       │
                    ┌────────────┘       └───────────┐
                    ▼                                ▼
            ┌──────────────┐                ┌─────────────────┐
            │ BcBlobStore  │                │BcUpstreamClient │
            │ (backend+fs) │                │  (libsoup 3)    │
            └──────┬───────┘                └─────────────────┘
                   │
            ┌──────┴──────┐
            ▼             ▼
     ┌───────────┐  ┌──────────┐
     │  SQLite   │  │   LMDB   │
     │ (default) │  │(optional)│
     └───────────┘  └──────────┘
```

### Components

| Component           | GObject Type      | Responsibility                                       |
|---------------------|-------------------|------------------------------------------------------|
| **BcApplication**   | `GApplication`    | Lifecycle management, GSettings, component wiring    |
| **BcHttpServer**    | `GObject`         | libsoup 3 HTTP server, BUD-01/02 endpoint routing    |
| **BcCacheManager**  | `GObject`         | Policy enforcement, size limits, eviction triggers    |
| **BcBlobStore**     | `GObject`         | Content-addressed filesystem + metadata backend      |
| **BcUpstreamClient**| `GObject`         | libsoup 3 HTTP client, upstream server fallback      |
| **BcDbBackend**     | C vtable struct   | Abstract metadata storage (SQLite or LMDB)           |

### Metadata Backend Vtable

The `BcDbBackend` interface follows [libmarmot](https://github.com/nickolasgoodman/libmarmot)'s
vtable pattern — a struct of function pointers with a `void *ctx` for backend-private data:

```c
typedef struct BcDbBackend {
    void *ctx;
    gboolean (*contains)(void *ctx, const char *sha256);
    BcDbBlobMeta *(*get_info)(void *ctx, const char *sha256, GError **error);
    gboolean (*put_meta)(void *ctx, const BcDbBlobMeta *meta, GError **error);
    gboolean (*delete_meta)(void *ctx, const char *sha256, GError **error);
    GPtrArray *(*list_blobs)(void *ctx, const char *cursor, uint32_t limit, GError **error);
    GPtrArray *(*evict_candidates)(void *ctx, int64_t bytes_to_free, GError **error);
    int64_t (*get_total_size)(void *ctx);
    uint32_t (*get_blob_count)(void *ctx);
    bool (*is_persistent)(void *ctx);
    void (*destroy)(void *ctx);
} BcDbBackend;
```

Both backends implement this interface. The blob store delegates all metadata operations
through the vtable while keeping blob content on the filesystem regardless of backend.

#### SQLite Backend (default)

- WAL mode for concurrent reads during writes
- Single `blobs` table with indexes on `created_at` and `last_accessed`
- File: `<storage-path>/blobs.db`

#### LMDB Backend (optional)

- Three named databases: `blobs` (primary KV), `by_access` (LRU index), `by_created` (listing index)
- Big-endian timestamp keys for correct sorted iteration
- Shares LMDB with nostrdb when using vendored copy
- Environment directory: `<storage-path>/metadata.lmdb/`
- Configurable map size (default 256 MB)

**Why LMDB?** When running alongside gnostr/nostrdb (which already uses LMDB), using the
same storage engine avoids mixing database technologies and shares the operational expertise.

## Integration with gnostr

Point gnostr's Blossom server setting to `http://127.0.0.1:24242` and all media
requests will be served from the local cache, with automatic upstream fetching on
cache miss. The cache shares nostrdb's vendored LMDB library for build consistency.

## Configuration

Settings are managed via GSettings (`org.gnostr.BlossomCache`). If the schema is not
compiled/installed, the app falls back to built-in defaults.

| Key                  | Type       | Default                              | Description                             |
|----------------------|------------|--------------------------------------|-----------------------------------------|
| `listen-address`     | `string`   | `127.0.0.1`                          | HTTP bind address                       |
| `listen-port`        | `uint`     | `24242`                              | HTTP port (spec: kind 24242)            |
| `storage-path`       | `string`   | `""` (→ XDG data dir)                | Blob storage directory                  |
| `max-cache-size-mb`  | `uint`     | `2048`                               | Maximum total cache size in MB          |
| `max-blob-size-mb`   | `uint`     | `100`                                | Maximum single blob size in MB          |
| `upstream-servers`   | `[string]` | `[blossom.primal.net, cdn.satellite.earth]` | Upstream Blossom servers         |
| `db-backend`         | `string`   | `sqlite`                             | Metadata backend: `sqlite` or `lmdb`   |
| `verify-sha256`      | `bool`     | `true`                               | Verify SHA-256 hashes on store          |

### Configuration Examples

```bash
# Switch to LMDB backend
gsettings set org.gnostr.BlossomCache db-backend 'lmdb'

# Add an upstream server
gsettings set org.gnostr.BlossomCache upstream-servers \
  "['https://blossom.primal.net', 'https://cdn.satellite.earth', 'https://cdn.nostr.build']"

# Increase cache size to 10GB
gsettings set org.gnostr.BlossomCache max-cache-size-mb 10240

# Change storage location
gsettings set org.gnostr.BlossomCache storage-path '/mnt/data/blossom-cache'
```

## Building

```bash
# Full build with LMDB support (default if LMDB is found)
cmake -B _build -DENABLE_BLOSSOM_CACHE=ON
cmake --build _build --target blossom-cache

# Build without LMDB (SQLite only)
cmake -B _build -DENABLE_BLOSSOM_CACHE=ON -DENABLE_LMDB_BACKEND=OFF
cmake --build _build --target blossom-cache

# Run tests
cmake --build _build --target test-bc-blob-store
ctest --test-dir _build -R test-bc-blob-store -V
```

### LMDB Detection

CMake searches for LMDB in this order:

1. **nostrdb vendor** — `third_party/nostrdb/deps/lmdb/` (built as a static library)
2. **System LMDB** — via `pkg-config` (`lmdb`)
3. **Fallback** — if neither found, builds with SQLite only (a warning is printed)

The LMDB backend source uses `__has_include("lmdb.h")` for compile-time detection.
When LMDB headers are not available, `bc_db_backend_lmdb_new()` compiles as a stub
that returns an error, and the application gracefully falls back to SQLite.

## Dependencies

| Library        | Version  | Purpose                    |
|----------------|----------|----------------------------|
| GLib/GObject   | ≥ 2.56   | Core, GSettings, GError    |
| libsoup        | 3.0      | HTTP server and client     |
| json-glib      | 1.0      | JSON serialization         |
| SQLite         | 3        | Default metadata backend   |
| LMDB           | (any)    | Optional metadata backend  |

## Spec Compliance

### local-blossom-cache spec

- ✅ Server on `127.0.0.1:24242`
- ✅ `HEAD /` health check endpoint
- ✅ No authentication required (localhost-only binding)
- ✅ `GET /<sha256>` and `HEAD /<sha256>` with optional file extensions
- ✅ Range requests (RFC 7233) with `Accept-Ranges: bytes`
- ✅ `xs=` proxy hints (BUD-10 server hint query parameters)
- ⏳ `as=` author hints (requires `kind:10063` relay lookup — not yet implemented)
- ✅ CORS `Access-Control-Allow-Origin: *` on all responses

### BUD-01 (Server Requirement)

- ✅ `GET /<sha256>[.ext]` — blob retrieval with MIME type
- ✅ `HEAD /<sha256>[.ext]` — metadata without body
- ✅ Optional file extension in URL (`.pdf`, `.png`, etc.)
- ✅ Range requests with `Accept-Ranges: bytes` and `206 Partial Content`
- ✅ CORS on all responses (including errors)
- ✅ `OPTIONS` preflight with `Access-Control-Allow-Headers: Authorization, *`
- ✅ `Access-Control-Max-Age: 86400` on preflight
- ✅ `X-Reason` header on error responses

### BUD-02 (Blob Operations)

- ✅ `PUT /upload` — returns full Blob Descriptor (`url`, `sha256`, `size`, `type`, `uploaded`)
- ✅ `GET /list/<pubkey>` — cursor-based pagination with `?cursor=`/`?limit=`
- ✅ `DELETE /<sha256>` — delete cached blob

### BUD-10 (Proxy Hints)

- ✅ `?xs=<server>` — server hint query parameters (repeatable, bare domains normalized)
- ⏳ `?as=<pubkey>` — author hint (parsed but not yet acted upon)

## File Layout

```
apps/blossom-cache/
├── CMakeLists.txt                              # Build config with LMDB detection
├── README.md                                   # This file
├── data/schemas/
│   └── org.gnostr.BlossomCache.gschema.xml     # GSettings schema
├── include/
│   ├── bc-application.h                        # GApplication subclass
│   ├── bc-blob-store.h                         # Blob storage API
│   ├── bc-cache-manager.h                      # Cache policy layer
│   ├── bc-db-backend.h                         # Metadata backend vtable
│   ├── bc-http-server.h                        # HTTP server API
│   └── bc-upstream-client.h                    # Upstream fetch API
├── src/
│   ├── main.c                                  # Entry point
│   ├── bc-application.c                        # GApplication lifecycle + GSettings
│   ├── bc-blob-store.c                         # Content-addressed store (backend + fs)
│   ├── bc-cache-manager.c                      # Policy, eviction, proxy hints
│   ├── bc-db-backend.c                         # Common vtable utilities
│   ├── bc-db-backend-sqlite.c                  # SQLite backend implementation
│   ├── bc-db-backend-lmdb.c                    # LMDB backend implementation
│   ├── bc-http-server.c                        # libsoup 3 server + BUD endpoints
│   └── bc-upstream-client.c                    # libsoup 3 client + hint resolution
└── tests/
    └── test_blob_store.c                       # GTest unit tests (7 tests)
```

## License

MIT
