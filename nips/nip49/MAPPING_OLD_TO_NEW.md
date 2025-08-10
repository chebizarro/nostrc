# NIP-49: Mapping Old to New

This documents the prior attempt and where code now lives per the guardrails.

- Old: `nips/nip49/include/nip49.h`
  - New: `nips/nip49/include/nostr/nip49/nip49.h` (core API)
  - New: `nips/nip49/include/nostr/nip49/nip49_g.h` (GLib API)

- Old: `nips/nip49/src/nip49.c`
  - New: `nips/nip49/src/core/nip49.c` (public API + payload + pipeline)
  - New: `nips/nip49/src/core/nip49_kdf.c` (scrypt KDF)
  - New: `nips/nip49/src/core/nip49_aead.c` (XChaCha20-Poly1305 AEAD)
  - New: `nips/nip49/src/core/nip49_bech.c` (Bech32 adapter using NIP-19)
  - New: `nips/nip49/src/glib/nip49_g.c` (GLib wrapper; installs NFKC callback)

Notes:
- Old files remain present for reference but are no longer built.
- All public APIs are canonical `nostr_*` with no transitional wrappers.
