# ADR-0001: NIP-04 API and Implementation Decisions

Date: 2025-08-09
Status: Accepted

## Context
We need a clean, spec-compliant implementation of NIP-04 (Encrypted Direct Messages) that:
- Lives outside the libnostr core, toggled via `ENABLE_NIP04` in `NipOptions.cmake`.
- Exposes a small C API for encrypt/decrypt and an optional helper to inspect the shared secret.
- Strictly follows the NIP-04 content format and cryptographic requirements.

## Decision
- Keep code and headers under `nips/nip04/` and do not export headers via `libnostr/include/`.
- Public header path: `nips/nip04/include/nostr/nip04.h`.
- API:
  - `int nostr_nip04_encrypt(const char *plaintext_utf8, const char *receiver_pubkey_hex, const char *sender_seckey_hex, char **out_content_b64_qiv, char **out_error);`
  - `int nostr_nip04_decrypt(const char *content_b64_qiv, const char *sender_pubkey_hex, const char *receiver_seckey_hex, char **out_plaintext_utf8, char **out_error);`
  - `int nostr_nip04_shared_secret_hex(const char *peer_pubkey_hex, const char *self_seckey_hex, char **out_shared_hex, char **out_error);`
- Cryptography:
  - ECDH over secp256k1 using OpenSSL EC APIs to derive the shared X coordinate (32 bytes).
  - Derive AES key as `SHA-256(shared_x)`.
  - Use AES-256-CBC with PKCS#7 padding.
  - 16-byte random IV.
- Content format exactly: `base64(ciphertext)?iv=base64(iv)` as per NIP-04.
- Inputs for keys are hex strings. Internally, hex is decoded via `nostr_hex2bin()` provided by libnostr utilities.
- Error strategy: return 0 on success; non-zero on failure. If provided, `out_error` is set to a short dynamically-allocated message.
- Security: zeroize intermediate key material and ECDH secrets; never leak in error messages.

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
