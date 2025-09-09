# ADR-0001: NIP-04 API and Implementation Decisions

Date: 2025-08-09
Status: Accepted

## Context
We need a clean, spec-compliant implementation of NIP-04 (Encrypted Direct Messages) that:
- Lives outside the libnostr core, toggled via `ENABLE_NIP04` in `NipOptions.cmake`.
- Exposes a small C API for encrypt/decrypt and an optional helper to inspect the shared secret.
- Strictly follows the NIP-04 content format and cryptographic requirements.

## Decision (superseded by AEAD v2 migration)
- Keep code and headers under `nips/nip04/` and do not export headers via `libnostr/include/`.
- Public header path: `nips/nip04/include/nostr/nip04.h`.
- API (stable):
  - `int nostr_nip04_encrypt(...)`
  - `int nostr_nip04_decrypt(...)`
  - `int nostr_nip04_encrypt_secure(...)`
  - `int nostr_nip04_decrypt_secure(...)`
- API (deprecated):
  - `int nostr_nip04_shared_secret_hex(...)` — deprecated; avoid exposing raw ECDH secrets.
- Cryptography (current):
  - ECDH over secp256k1 to derive X; HKDF-SHA256 with `info="NIP04"` separates key material.
  - AES-256-GCM (AEAD) for encryption; envelope: `v=2:base64(nonce(12)||cipher||tag(16))`.
  - Encryption emits AEAD v2 only. Decrypt supports AEAD v2 and retains a legacy AES-CBC `?iv=` fallback for interop.
- Inputs for keys are hex strings (SEC1 compressed/uncompressed accepted where applicable). Internally, hex decoded via `nostr_hex2bin()`.
- Error strategy: return 0 on success; non-zero on failure. Unified decrypt error messages ("decrypt failed").
- Security: zeroize intermediate materials; avoid detailed error leakage.

## Alternatives Considered
- Using libsecp256k1 ECDH directly. Chosen to keep OpenSSL EC initially for simplicity since OpenSSL is already a dependency; CMake links `secp256k1` for future flexibility.
- Embedding NIP-04 in libnostr core. Rejected per requirement to keep NIPs optional and removable.

## Consequences
- NIP-04 can be compiled in or out without touching libnostr core.
- Consumers include the header from `nips/nip04/include/` when `ENABLE_NIP04` is enabled.
- Implementation adheres to spec, enabling interop with other Nostr clients/libraries.

## References
- Spec: `docs/nips/04.md` (also referenced via `nips/nip04/SPEC_SOURCE`).
- OpenSSL EVP documentation.
