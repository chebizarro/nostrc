# AI Context for LLMs

This document provides conventions and context to maximize LLM effectiveness when contributing to the `nostrc` C library.

## Coding Conventions

- Language: C11
- Build: CMake ≥ 3.10
- Headers live under module `include/` directories; public APIs are declared in `libnostr/include/*.h`, `libgo/include/*.h`, `libjson/include/*.h`.
- Always include public headers via relative paths from module include roots, e.g. `#include "nostr-event.h"` once `target_include_directories(... PUBLIC include)` is used.
- Prefer `const` correctness and explicit sizes (`int64_t`, `size_t`).
- Error handling: return error codes (see `libnostr/include/error_codes.h`), avoid `exit()` in library code.
- Memory: document ownership for all returned pointers. Provide `free_*` functions for heap-allocated structs.

## Preferred Libraries

- OpenSSL: hashing and crypto primitives used in tests/examples.
- libsecp256k1: ECDSA over secp256k1 for key operations.
- jansson: JSON parse/serialize in `libjson`.
- nsync: lightweight synchronization primitives used in `libgo` and linked by other modules.
- libwebsockets: optional; wiring present in `libnostr/CMakeLists.txt` (commented) for WebSocket relay connectivity.

## Common Patterns

- Public struct + `create_*` / `free_*` lifecycle, e.g. `NostrEvent` in `libnostr/include/event.h`.
- Serialization helpers return heap-allocated `char*` strings; caller frees.
- `libgo` provides Go-like channels/contexts for cancellation and timeouts; use to avoid blocking I/O in relay code.
- Tests link small executables that validate unit behavior (`tests/tests_nostr.c`, `test_relay.c`, etc.).

## Domain Terminology

- Event: Nostr protocol event (`event.h`), includes `id`, `pubkey`, `created_at`, `kind`, `tags`, `content`, `sig`.
- Relay: Server that transports Nostr events (`relay.h`).
- Subscription: Filtered stream of events (`subscription.h`, `filter.h`).
- NIP: Nostr Improvement Proposal, implemented as modules in `nips/`.

## Code Organization

- `libnostr/`: core protocol types and relay/subscription utilities.
- `libgo/`: concurrency primitives (channels, contexts, wait groups).
- `libjson/`: JSON interop built on `jansson` with `nsync`.
- `nips/`: per-NIP modules compiled as subdirectories.
- `tests/`: CTest-driven unit/integration tests.
- `examples/`: usage examples.

## Error Handling

- Prefer integer error codes; centralize codes in `libnostr/include/error_codes.h`.
- Use `errno`-style negative codes where helpful; document them in function comments and in API.md.
- Validate inputs early; return `NULL`/`-EINVAL`-style codes as appropriate; never crash on malformed JSON or invalid keys.

## Testing Approach

- CTest targets created in `libgo` and `tests/`.
- Link against `libnostr`, `nostr_json`, `nsync`, OpenSSL, and `libsecp256k1` where required.
- Keep tests deterministic; seed randomness explicitly; avoid network in unit tests.

## Security Considerations

- Zero sensitive buffers after use when feasible (private keys).
- Verify signatures with libsecp256k1; check return paths rigorously.
- Validate JSON input strictly; reject unknown or oversized fields to prevent DoS.
- When enabling libwebsockets, ensure TLS configuration is correct and certificate validation is enforced.

## Contribution Guidance for LLMs

- When adding APIs, update headers in `include/` and provide `*_test.c` verifying behavior.
- Keep public API minimal and stable; mark internal helpers `static` in `*.c`.
- Update documentation cross-references: README.md, ARCHITECTURE.md, API.md.
- Prefer small, isolated commits with tests.


# Nostr NIPs integration

You are editing a C library that implements the Nostr protocol with dual-mode APIs:
- Pure C (no GLib deps) and
- GLib/GObject layer (GTK-Doc, GError, refcounting, naming: nostr_* / Nostr* / NOSTR_*)

Authoritative specs live in the repo as a submodule at `docs/nips/` (nostr-protocol/nips).

For any work in `nips/nipXX/*`, FIRST open and read the associated spec:

1) Resolve the spec path from `nips/nipXX/SPEC_SOURCE` (SPEC_MD=...).
2) Use that markdown (e.g., `docs/nips/04.md`) as the primary reference.
3) Align APIs, constants, and behavior with the NIP language, including edge cases.

While editing:
- Keep naming consistent with the project’s GLib/C conventions.
- Add or update GTK-Doc to reference the spec with a relative link:
  Example:
  *See also:* `docs/nips/04.md`
- Prefer object methods for stateful behaviors (GObject) and pure C functions for core primitives.
- Error handling: return gboolean + `GError **` for GLib surface; libgo Error for pure C surface if needed.
- Maintain thread-safety guarantees via libgo and/or GLib async patterns as applicable.

When in doubt: quote the exact NIP section and justify implementation choices.

Deliverables for each NIP refactor:
- Updated headers/impl following naming + error patterns
- Unit tests mirroring normative examples from the NIP spec
- Clear TODOs for ambiguous or draft sections (with spec citations)
- Migration notes if the public API changes
