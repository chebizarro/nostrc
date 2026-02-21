# blossom-cache — Local Blossom Cache Server

A standalone, GObject-based local cache for the [Blossom](https://github.com/hzrd149/blossom) media
storage protocol. Implements the [local-blossom-cache specification](https://github.com/hzrd149/blossom/blob/master/implementations/local-blossom-cache.md)
and relevant BUD-01/BUD-02 endpoints. Designed to work as a transparent local cache that integrates
with **gnostr** and other Nostr clients.

## What it does

1. **Runs a local HTTP server** on `http://127.0.0.1:24242` (spec-mandated port)
2. **Caches blobs** on disk (content-addressed by SHA-256) with SQLite metadata
3. **Fetches on miss** — when a blob isn't cached, transparently fetches from configurable upstream Blossom servers
4. **Supports proxy hints** — `?xs=` server hints and `?as=` author hints per BUD-10
5. **Evicts intelligently** — LRU eviction with configurable cache size limits
6. **Verifies integrity** — optionally validates SHA-256 hashes on every store
7. **Range requests** — supports RFC 7233 partial content for efficient video/large file serving

## Quick start

```bash
# Build (from nostrc root)
cmake -B _build && cmake --build _build --target blossom-cache

# Run
./_build/apps/blossom-cache/blossom-cache

# Health check
curl -I http://127.0.0.1:24242/

# Cache statistics
curl http://127.0.0.1:24242/status
```

## HTTP API

| Method   | Path                   | Spec            | Description                                    |
|----------|------------------------|-----------------|-------------------------------------------------|
| HEAD     | `/`                    | local-cache     | Health check — returns 2xx, no body             |
| GET      | `/<sha256>[.ext]`      | BUD-01          | Retrieve a blob (with range request support)    |
| HEAD     | `/<sha256>[.ext]`      | BUD-01          | Check existence + Content-Type/Content-Length    |
| PUT      | `/upload`              | BUD-02          | Store a blob, returns Blob Descriptor JSON      |
| DELETE   | `/<sha256>`            | BUD-02          | Delete a cached blob                            |
| GET      | `/list/<pubkey>`       | BUD-02          | List blobs with `?cursor=`/`?limit=` pagination |
| GET      | `/status`              | extension       | JSON cache statistics                           |

### Proxy hint query parameters (BUD-10)

On `GET /<sha256>`, the server accepts:
- `?xs=<server>` — server hints where the blob may be available (can repeat)
- `?as=<pubkey>` — author hints for kind:10063 server list lookup (TODO: relay lookup)

Example:
```
GET /b1674191a88ec5cdd733e4240a81803105dc412d6c6708d53ab94fc248f4f553.pdf?xs=cdn.example.com
```

### CORS

All responses include `Access-Control-Allow-Origin: *` per BUD-01. Preflight `OPTIONS` requests
return `Access-Control-Allow-Headers: Authorization, *` and `Access-Control-Max-Age: 86400`.

### Access Control

Per the local-blossom-cache spec, no authentication is required. All requests are served without
requiring an `Authorization` header. Access is implicitly restricted by binding to `127.0.0.1`.

## Integration with gnostr

Point gnostr's Blossom server setting to `http://127.0.0.1:24242` and all media
requests will be served from the local cache, with automatic upstream fetching
on cache miss.

## Configuration

Settings are managed via GSettings (`org.gnostr.BlossomCache`):

| Key                 | Type     | Default                | Description                          |
|---------------------|----------|------------------------|--------------------------------------|
| `listen-address`    | string   | `127.0.0.1`            | HTTP bind address                    |
| `listen-port`       | uint     | `24242`                | HTTP port (spec: kind 24242)         |
| `storage-path`      | string   | (XDG data dir)         | Blob storage directory               |
| `max-cache-size-mb` | uint     | `2048`                 | Maximum total cache size (MB)        |
| `max-blob-size-mb`  | uint     | `100`                  | Maximum single blob size (MB)        |
| `upstream-servers`  | [string] | `[blossom.primal.net]` | Upstream Blossom server URLs         |
| `eviction-policy`   | string   | `lru`                  | Eviction strategy                    |
| `verify-sha256`     | bool     | `true`                 | Verify hashes on store               |
| `auto-fetch-on-miss`| bool     | `true`                 | Fetch from upstream on cache miss    |

If the GSettings schema is not installed, the app falls back to built-in defaults.

## Architecture

```
┌─────────────┐     HTTP      ┌──────────────┐
│   gnostr     │──────────────▶│ BcHttpServer │
│ (or any      │               │ (libsoup)    │
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
            │(SQLite + fs) │                │  (libsoup GET)  │
            └──────────────┘                └─────────────────┘
```

## Spec Compliance

### local-blossom-cache spec
- ✅ Server on `127.0.0.1:24242`
- ✅ `HEAD /` health check endpoint
- ✅ No authentication required
- ✅ `GET /<sha256>` and `HEAD /<sha256>` with file extensions
- ✅ Range requests (RFC 7233)
- ✅ `xs=` proxy hints (BUD-10 query parameters)
- ⏳ `as=` author hints (requires kind:10063 relay lookup — not yet implemented)
- ✅ CORS `Access-Control-Allow-Origin: *` on all responses

### BUD-01
- ✅ `GET /<sha256>[.ext]` — blob retrieval with MIME type
- ✅ `HEAD /<sha256>[.ext]` — metadata without body
- ✅ Range requests with `Accept-Ranges: bytes`
- ✅ CORS on all responses
- ✅ `OPTIONS` preflight with `Authorization, *` headers
- ✅ `X-Reason` header on error responses

### BUD-02
- ✅ `PUT /upload` — returns Blob Descriptor (url, sha256, size, type, uploaded)
- ✅ `GET /list/<pubkey>` — with cursor/limit pagination
- ✅ `DELETE /<sha256>` — delete cached blob

## Dependencies

- GLib/GObject/GIO ≥ 2.56
- libsoup 3.0
- json-glib 1.0
- SQLite 3

## License

MIT
