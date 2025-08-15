# Architecture

## Overview

Nostrc is a C library that implements the Nostr protocol with modular components for core protocol primitives, JSON handling, concurrency primitives, and optional NIP modules. The repository is a monorepo with multiple libraries and tests built via CMake.

- Core library: `libnostr/`
- Concurrency primitives: `libgo/`
- JSON and integration helpers: `libjson/`
- NIP implementations: `nips/`
- Optional serialization: `nson/` (when ENABLE_NSON is used)
- Examples: `examples/`
- Tests: `tests/`

## High-level Component Diagram

```
+-------------------+         +--------------------+
|   Applications    |  use    |      Examples      |
| (your programs)   +--------->  examples/*.c      |
+---------+---------+         +--------------------+
          |
          | links
          v
+-------------------+        +---------------------+
|     libnostr      |<------>|       libjson       |
| Core Nostr types  |  uses  | JSON (jansson) +    |
| events, relays,   |        | sync (nsync)        |
| filters, keys     |        +---------------------+
+---------+---------+                 ^
          ^                           |
          | uses                      | uses
+---------+---------+                 |
|       libgo       |-----------------+
| Go-like channels, |
| context, waitgrp  |
+-------------------+

NIP modules (nips/*) extend libnostr capabilities and are compiled as submodules.
```

## Data Flow

- Event creation and signing flows through `libnostr` APIs (`event.h`, `keys.h`) and uses OpenSSL and libsecp256k1 underneath for cryptography (linked in tests and applications).
- JSON serialization/deserialization is performed by `libjson` via `jansson` and synchronized using `nsync` primitives when needed.
- Concurrency and coordination in C are provided by `libgo` (channels, contexts, wait groups), used by higher-level modules for non-blocking operations.
- Relay connections and subscriptions (`nostr-relay.h`, `nostr-subscription.h`) orchestrate message flow to/from Nostr relays. WebSocket linkage is prepared in `libnostr/CMakeLists.txt` for libwebsockets but may be optional depending on your integration.

## Technology Choices & Rationale

- C + CMake: portability across embedded/IoT and desktop environments.
- `nsync`: lightweight synchronization primitives compatible with C.
- `jansson`: mature C JSON parser/serializer with permissive license.
- OpenSSL + libsecp256k1: widely adopted crypto libraries for hashing/ECDSA.
- Optional libwebsockets: efficient WebSocket client/server in C.

## Build & Link Boundaries

- `libnostr` is a static library by default, built with `-fPIC` to support shared linkage.
- `libjson` is currently a shared library (`SHARED`) that links against `jansson`, `libnostr`, and `nsync`.
- `libgo` is a static library providing concurrency utilities with `nsync`.
- Optional `libwally-core` provides BIP39/BIP32 primitives for NIP-06 when enabled. Global init/cleanup paths call `wally_init(0)`/`wally_cleanup(0)` via `nostr_global_init()`/`nostr_global_cleanup()`.
- Tests link against `libnostr`, `nostr_json`, OpenSSL, libsecp256k1, and `nsync`.

## Performance Considerations

- Avoid unnecessary JSON allocations; reuse buffers where possible.
- Prefer stack allocation for small structures (`event`, `tag`) and pass by pointer to avoid copies.
- Use `libgo` channels and contexts to handle IO timeouts and cancellation cleanly.
- Enable `-O2` or higher in Release builds; consider LTO for final binaries.

## Scalability Notes

- For high-connection counts, ensure non-blocking IO and backpressure via channels.
- Batch JSON parsing/encoding when processing large event sets.
- Use `nsync` primitives to avoid contention; shard state by relay or subscription when possible.

## ASCII Module Map

```
/ (root)
├── libgo/          # Concurrency primitives
├── libnostr/       # Core Nostr types and relay/subscription logic
├── libjson/        # JSON integration and helpers
├── nips/           # NIP-specific extensions (nip04, 05, 13, 19, 29, 31, 34, 42, 44, 46, 49, 52, 53, 54, 94)
├── nson/           # Optional serialization format (if enabled)
├── examples/       # Example programs
└── tests/          # Unit/integration tests
```
