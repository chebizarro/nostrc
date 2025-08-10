# ADR-0001: NIP-19 API and Encoding Decisions

Spec: docs/nips/19.md

- Section: "Bare keys and ids" (lines 13–25)
  - Use bech32 (not m) for `npub`, `nsec`, `note`.
- Section: "Shareable identifiers with extra metadata" (lines 27–56)
  - TLV format: list of T(1 byte), L(1 byte), V(L bytes)
  - Prefixes: `nprofile`, `nevent`, `naddr`, `nrelay` (deprecated)
  - TLV types:
    - 0: special (pubkey/event id/identifier per prefix)
    - 1: relay (ASCII URL), repeatable
    - 2: author (32-byte pubkey). Optional for `nevent`, required for `naddr`.
    - 3: kind (uint32 big-endian). Optional for `nevent`, required for `naddr`.
- Section: "Examples" (lines 57–65)
  - Provide deterministic vectors for tests.
- Section: "Notes" (lines 66–70)
  - `npub`/`nsec`/`note` for display only; ignore unknown TLVs when decoding.

Decisions:
- Core exposes:
  - Bech32 helpers: `nostr_b32_encode/decode`, `nostr_b32_to_5bit/to_8bit`.
  - Typed enc/dec for `npub`, `nsec`, `note` now; TLV entities later.
- Errors: add NOSTR_ERROR_BECH32 family (checksum, charset, HRP mismatch, TLV invalid).
- Casing: lower-case strings for encode per bech32 rules (spec lines 13–25 imply standard casing; mixed-case invalid).
- No extra limits beyond spec.

TODOs:
- Implement TLV enc/dec for `nprofile`, `nevent`, `naddr`, `nrelay` with exact TLV rules (spec lines 31–56).
- GLib wrappers with GI annotations.
- Fuzz/property tests for TLV.
