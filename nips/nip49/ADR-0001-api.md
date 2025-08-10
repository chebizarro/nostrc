# ADR-0001: NIP-49 API, Payload, and Normalization Strategy

Status: Draft
Date: 2025-08-10
Spec: ../../docs/nips/49.md (authoritative)

## Decision

- Provide canonical `nostr_*` APIs for NIP-49 under `nips/nip49/include/nostr/nip49/nip49.h`.
- Structure the 91-byte payload as `VER(1)|LOG_N(1)|SALT(16)|NONCE(24)|AD(1)|CT(48)` with `VER=0x02`.
- Use libsodium for scrypt and XChaCha20-Poly1305 (IETF) AEAD.
- Require password normalization to NFKC prior to KDF. Provide a core callback hook; the GLib layer installs a normalizer using `g_utf8_normalize(..., G_NORMALIZE_ALL_COMPOSE)`.
- When no normalizer is available and password contains non-ASCII, return `NFKC_REQUIRED`.
- Encode to Bech32 HRP `ncryptsec` using libnostr (NIP-19) bech32 helpers.

## Rationale

- Aligns strictly with NIP-49 sections: "Symmetric Encryption Key derivation", "Encrypting a private key", "Decryption".
- Libsodium provides portable, audited crypto primitives.
- Keeping normalization outside of core avoids heavy ICU deps and keeps core portable; GLib layer offers a high-quality default.

## Alternatives Considered
- Embedding ICU in core: rejected due to footprint.
- Using different AEAD: rejected, NIP-49 specifies XChaCha20-Poly1305 IETF.

## Error Taxonomy
- `ARGS`: invalid input pointers/lengths
- `NFKC_REQUIRED`: non-ASCII password without a NFKC callback
- `KDF`: scrypt failure
- `AEAD`: encrypt/decrypt failure (incl. bad password/tag)
- `BECH32`: encode/decode errors or wrong HRP
- `VERSION`: payload version not 0x02

## Notes
- Zeroization: wipe derived key and normalized password after use.
- Future work: add vector tests from spec and CLI.
