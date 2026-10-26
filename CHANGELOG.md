# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Fixed
- Static bud02, libblossom, libporthome, libvault and libvault-marmot retain
  default visibility on public API definitions, allowing Groundhog's shared
  GObject layer to resolve those APIs from executable-supplied archives.
  Shared-library export sets and Windows static annotations are unchanged.

### Breaking library ABI changes
- nostr-gobject 3.0.0 hides implementation and embedded-archive symbols
  (SONAME `libnostr-gobject-1.0.so.3`); rebuild dependent binaries. Declared
  functions, GTypes, enum/flags types, error quarks and the signer proxy remain
  exported through `NOSTR_GOBJECT_API`; installed headers include the generated
  `nostr-gobject-export.h`.
- NIP-19, NIP-34, NIP-46 and NIP-59 are now 0.2.0; NIP-55L is 0.8.0;
  Communikeys is 3.0.0. All NIP library targets now compile hidden-by-default
  and export declared public APIs through package-owned generated headers.
  Rebuild consumers with the matching development headers; incidental private
  symbols are no longer an ABI. Unversioned NIP targets adopt the same boundary.
  NIP-55L daemon/Grotto gate consumers link its private static implementation
  closure rather than importing private permission/caller symbols from the
  GLib DSO; generated D-Bus bindings remain exported by that DSO.
- libnostr 2.0.0 and libnostr-json 2.0.0 export their declared public APIs
  instead of incidental implementation and embedded-archive symbols. Their
  shared-library ABI majors change from 1 to 2; rebuild dependent binaries.
- libgo 0.2.0 makes the same incompatible visibility change under pre-1.0
  versioning (SONAME remains 0). Rebuild consumers that depended on internal
  symbols; use the declarations in the installed public headers instead.
  The fiber runtime's weak interposition hooks remain supported in-tree.
- Public function signatures remain compatible. Consumers compiling these
  headers need the accompanying generated export headers from the same build
  or development package. libjson's helper API uses its own DLL import/export
  macro even where declarations remain in the legacy core `json.h` and
  `nostr-event-extra.h` headers.
- nostrdb-enabled consumers can use the now-declared cache-maintenance APIs
  `ln_ndb_invalidate_txn_cache_ext()` and `ln_ndb_force_close_txn_cache()`;
  the latter is shutdown-only and invalidates active queries on that thread.

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
