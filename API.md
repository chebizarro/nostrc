# Public API Overview

This document summarizes the primary public headers and key functions of the `nostrc` C library. Consult the header files in `libnostr/include/`, `libgo/include/`, and `libjson/include/` for authoritative signatures and comments.

## Modules

- libnostr (core): `libnostr/include/*.h`
- libgo (concurrency): `libgo/include/*.h`
- libjson (JSON helpers): `libjson/include/*.h`

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

### Relays & Connections — `relay.h`, `connection.h`, `subscription.h`, `filter.h`, `relay_store.h`, `simplepool.h`
- Manage connections to relays, subscriptions with filters, and pools of relays.
- Typical flow:
  - Build `Filter` and `Subscription`
  - Connect to relay(s)
  - Send/receive events matching filters

### JSON — `json.h`
- Abstractions for JSON interface used by the library (implemented by `libjson`).

### Utility & Types — `utils.h`, `timestamp.h`, `pointer.h`, `kinds.h`, `error_codes.h`
- Helpers for time, pointers, well-known kinds, and common error codes.

## libjson

- `nostr_json` shared library implementing JSON interop using `jansson`.
- Typical calls from `libnostr` go through `json.h` interfaces; applications can call higher-level APIs in `libnostr` and receive JSON strings.

## libgo

- Go-like concurrency primitives in C.
- Channels: `channel.c`
- Contexts: cancellation and timeouts (`context.c`) with helpers for cooperative cancellation across operations.
- Wait groups, tickers, error and data structures (`hash_map`, arrays).

## Error Codes

- See `libnostr/include/error_codes.h` for canonical error constants.

## Examples

- `examples/` contains `main.c`, `json_glib.c`, `json_cjson.c` showing typical flows.

## ABI and Versioning

- Libraries are built from HEAD; semver tagging recommended in releases.
- Avoid breaking public function signatures and struct layouts without a major version bump.
