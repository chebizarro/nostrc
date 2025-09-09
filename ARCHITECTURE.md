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
- Relay applications and core policy helpers: `apps/`

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
| context, waitgrp, |
| threads           |
+-------------------+

NIP modules (nips/*) extend libnostr capabilities and are compiled as submodules.
```

## Data Flow

- Event creation and signing flows through `libnostr` APIs (`event.h`, `keys.h`) and uses OpenSSL and libsecp256k1 underneath for cryptography (linked in tests and applications).
- JSON serialization/deserialization is performed by `libjson` via `jansson` and synchronized using `nsync` primitives when needed.
- Concurrency and coordination in C are provided by `libgo` (channels, contexts, wait groups), used by higher-level modules for non-blocking operations.
- Relay connections and subscriptions (`nostr-relay.h`, `nostr-subscription.h`) orchestrate message flow to/from Nostr relays. WebSocket linkage is prepared in `libnostr/CMakeLists.txt` for libwebsockets.

## Security Posture (High-level)

- Event integrity: canonical NIP-01 preimage is used for id/signature; verification recomputes hash and never trusts caller-supplied `id`.
- Encryption: NIP-04 uses AEAD v2 (`v=2:` with AES-256-GCM) for all encryption. Legacy `?iv=` decrypt is supported for interop. Unified decrypt error messages reduce leakage.
- Optional hardening: a build-time switch `-DNIP04_STRICT_AEAD_ONLY=ON` disables legacy decrypt entirely.
- Ingress mitigations: replay TTL + timestamp skew checks, with metrics. The policy decider is pure (no websockets) and unit-testable.
- A reusable, websocket-free ingress decision function is provided for tests and reuse:
  - `apps/relayd/include/protocol_nip01.h` exports `relayd_nip01_ingress_decide_json(...)`, plus `nostr_relay_set_replay_ttl(...)`, `nostr_relay_set_skew(...)`, and getters.
- The ingress policy core is implemented in `apps/relayd/src/policy_decider.c` with a fixed-size duplicate cache and skew checks.
- Relay metrics live in `apps/relayd/src/metrics.c` and include counters such as `duplicate_drops` and `skew_rejects`.
- On startup, the relay prints a one-line security banner, for example: `nostrc-relayd: security AEAD=v2 replayTTL=900s skew=+600/-86400`.

## Relay Daemon and Policy Core

- The relay daemon lives under `apps/relayd/`. Runtime mitigations (replay TTL and timestamp skew) are configured via environment variables and set at startup.

## Technology Choices & Rationale

- C + CMake: portability across embedded/IoT and desktop environments.
- `nsync`: lightweight synchronization primitives compatible with C.
- `jansson`: mature C JSON parser/serializer with permissive license.
- OpenSSL + libsecp256k1: widely adopted crypto libraries for hashing/ECDSA.
- libwebsockets: efficient WebSocket client/server in C.
- OpenSSL + libsecp256k1 for cryptography; stable, widely used.

## Build & Link Boundaries

- `libnostr` is a static library by default, built with `-fPIC` to support shared linkage.
- `libjson` is currently a shared library (`SHARED`) that links against `jansson`, `libnostr`, and `nsync`.
- `libgo` is a static library providing concurrency utilities with `nsync`.

- Tests link against `libnostr`, `nostr_json`, OpenSSL, libsecp256k1, and `nsync`.
- Relay helpers for tests are provided by the `relayd_core` static library built from `apps/relayd/src/policy_decider.c` and `apps/relayd/src/metrics.c`.

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
└── apps/           # Relay daemon and core relay helpers
```
