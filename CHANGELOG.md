# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [0.4.0] - 2025-08-10
### Added
- NIP-47 (Nostr Wallet Connect) envelope helpers and negotiation:
  - `nostr_nwc_request_build/parse`, `nostr_nwc_response_build/parse`.
  - Encryption negotiation with `nostr_nwc_select_encryption()` (prefers `nip44-v2`, falls back to `nip04`).
  - Clear helpers: `nostr_nwc_request_body_clear`, `nostr_nwc_response_body_clear`.
- Session helpers:
  - Client: `nwc_client.h/c` (`NostrNwcClientSession`, init/clear/build_request).
  - Wallet: `nwc_wallet.h/c` (`NostrNwcWalletSession`, init/clear/build_response).
- GLib bindings for sessions and build helpers:
  - Client: `nwc_client_g.h/c`.
  - Wallet: `nwc_wallet_g.h/c`.
  - GLib functions return g_strdup-allocated JSON for allocator safety.
- Examples:
  - C: `nwc_client_example`, `nwc_wallet_example`, `nwc_error_example`.
  - GLib: `glib_client_example`, `glib_wallet_example`.
- Docs:
  - `docs/NIP47.md` covering helpers, sessions, GLib bindings, examples, and invariants.

### Changed
- Internal robustness fixes and tests across libnostr/libgo retained passing under sanitizers.

### Notes
- Public APIs follow canonical `nostr_*` naming (no thin wrappers), per project policy.

[0.4.0]: https://github.com/chebizarro/nostrc/releases/tag/v0.4.0
