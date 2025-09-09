# Public API Overview

This document summarizes the primary public headers and key functions of the `nostrc` C library. Consult the header files in `libnostr/include/`, `libgo/include/`, and `libjson/include/` for authoritative signatures and comments.

## Modules

- libnostr (core): `libnostr/include/*.h`
- libgo (concurrency): `libgo/include/*.h`
- libjson (JSON helpers): `libjson/include/*.h`
 - NIP modules (optional): `nips/*/include/nostr/*.h` (e.g., NIP-04)

## libnostr

### Events — `event.h`
- Types: `NostrEvent`
- Lifecycle: `create_event()`, `free_event()`
- Serialization: `event_serialize(NostrEvent*) -> char*`
- Identification: `event_get_id(NostrEvent*) -> char*`
- Signing: `event_sign(NostrEvent*, const char* priv_hex) -> int`
- Verification: `event_check_signature(NostrEvent*) -> bool`
- Utility: `event_is_regular(NostrEvent*) -> bool`

Ownership: Functions returning `char*` allocate memory; caller must free.

### Tags — `tag.h`
- Types: `Tag`, `Tags`
- Create/manipulate tags collections used by events.

### Keys — `keys.h`
- Key helpers and conversions for Nostr (pubkey/privkey). Typically backed by libsecp256k1.

### Relays & Connections — `nostr-relay.h`, `connection.h`, `nostr-subscription.h`, `nostr-filter.h`, `relay_store.h`, `simplepool.h`
- Manage connections to relays, subscriptions with filters, and pools of relays.
- Typical flow:
  - Build `Filter` and `Subscription`
  - Connect to relay(s)
  - Send/receive events matching filters

#### Filters — ownership and cleanup

- Types: `NostrFilter`, `NostrFilters` (dynamic vector)
- Heap vs stack lifecycle:
  - Use `nostr_filter_free(NostrFilter* heap_filter)` to free filters created with `nostr_filter_new()`.
  - Use `nostr_filter_clear(NostrFilter* stack_filter)` to free internal members of a stack-allocated filter without freeing the struct.
- Builder semantics (`nostr_nip01_filter_builder_*`):
  - `nostr_nip01_filter_build(&builder, &out_filter)` transfers ownership of internal pointers into `out_filter` via shallow copy and sets the builder's internal pointer to NULL.
  - Always call `nostr_nip01_filter_builder_dispose(&builder)`; it is a no-op for filter internals after a successful build.
  - If `out_filter` is stack-allocated, call `nostr_filter_clear(&out_filter)` when done. If heap-allocated, call `nostr_filter_free(out_filter)`.
  - When appending a filter into `NostrFilters` with `nostr_filters_add(fs, &f)`, ownership is moved into the vector, and the source `f` is zeroed; callers must not clear/free `f` afterward.

### JSON — `json.h`
- Abstractions for JSON interface used by the library (implemented by `libjson`).

### NIP-04 — `nips/nip04/include/nostr/nip04.h`
- Encryption uses AEAD v2 envelopes for all encrypt paths (`v=2:base64(nonce||cipher||tag)`).
- Decrypt accepts AEAD v2; legacy `?iv=` decrypt fallback remains for interop unless built with `-DNIP04_STRICT_AEAD_ONLY=ON`.
- Secure variants keep private keys in `nostr_secure_buf`:
  - `nostr_nip04_encrypt_secure(...)`
  - `nostr_nip04_decrypt_secure(...)`
- Deprecated: `nostr_nip04_shared_secret_hex(...)` (avoid exposing raw ECDH shared secrets).
See `docs/NIP04_MIGRATION.md` for details.

### Utility & Types — `utils.h`, `timestamp.h`, `pointer.h`, `kinds.h`, `error_codes.h`
- Helpers for time, pointers, well-known kinds, and common error codes.

## libjson

- `nostr_json` shared library implementing JSON interop using `jansson`.
- Typical calls from `libnostr` go through `json.h` interfaces; applications can call higher-level APIs in `libnostr` and receive JSON strings.
- Helper: `nostr_json_get_raw(const char* json, const char* key, char** out_raw)` returns a compact JSON string for a top-level key's value (quoted for strings; compact JSON for objects/arrays). Caller frees.

## libgo

- Go-like concurrency primitives in C. See detailed docs in `libgo/LIBGO.md`.
- Entry point: `go()` wrapper in `libgo/include/go.h` launches a goroutine-like detached thread.
- Channels: `libgo/include/channel.h` (blocking and non-blocking send/receive, close)
- Contexts: `libgo/include/context.h` (cancellation and deadlines)
- Select: `libgo/include/select.h` (multi-channel select over send/receive)
- Wait groups: `libgo/include/wait_group.h`
- Ticker: `libgo/include/ticker.h`
- Threads: utilities for thread creation/scheduling integrated with the libgo runtime (see `libgo/fiber/` and `libgo/src/` for schedulers and helpers)

Examples:
- `libgo/examples/helloworld.c` — basic goroutines with `go()` and a wait group
- `libgo/examples/channels.c` — producer/consumer using `go()`, channels, and clean shutdown

## Error Codes

- See `libnostr/include/error_codes.h` for canonical error constants.

## Examples

- `examples/` contains `main.c`, `json_glib.c`, `json_cjson.c` showing typical flows.
- NIP-46: response parsing sets `NostrNip46Response.result` to a plain string if the JSON result is a string; otherwise to compact JSON text suitable for direct `nostr_event_deserialize()` or other JSON-consuming APIs.

## ABI and Versioning

- Libraries are built from HEAD; semver tagging recommended in releases.
- Avoid breaking public function signatures and struct layouts without a major version bump.
