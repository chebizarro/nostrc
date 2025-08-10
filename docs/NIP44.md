# NIP-44 (v2) — Implementation Notes and Usage

This module implements NIP-44 v2 encryption primitives and high-level helpers.

- __Conversation key__: `nostr_nip44_convkey(sk1, pub2_xonly32)` uses secp256k1 ECDH with x-only public key (tries 0x02/0x03 parity) and HKDF-Extract with salt "nip44-v2" to derive a 32-byte PRK.
- __Message keys__: HKDF-Expand(PRK=convkey, info=nonce32, L=76) ->
  - chacha_key: bytes 0..31
  - chacha_nonce: bytes 32..43 (12 bytes)
  - hmac_key: bytes 44..75 (32 bytes)
- __Padding__: buffer layout is `[len_be:u16][plaintext][zeros]` where the padded section after the 2-byte length is `calc_padded_len(len)`:
  - if len <= 32 -> 32
  - let x = len-1; let next_power = 1 << (floor(log2(x)) + 1)
  - chunk = (next_power <= 256) ? 32 : next_power/8
  - rounded up to a multiple of chunk
  - total padded buffer size = 2 + calc_padded_len(len)
- __Cipher__: IETF ChaCha20 with 12-byte nonce and initial block counter = 0.
- __MAC__: HMAC-SHA256 over `nonce32 || ciphertext` with `hmac_key`.
- __Payload__: `version(1) || nonce32 || ciphertext || mac32`, base64-encoded.

## Public API

- `int nostr_nip44_convkey(const uint8_t sk1[32], const uint8_t pub2_xonly[32], uint8_t out[32]);`
- `int nostr_nip44_encrypt_v2_with_convkey(const uint8_t convkey[32], const uint8_t *msg, size_t msg_len, char **out_b64);`
- `int nostr_nip44_decrypt_v2_with_convkey(const uint8_t convkey[32], const char *b64, uint8_t **out_msg, size_t *out_msg_len);`

See headers in `nips/nip44/include/` for full signatures including GLib variants.

## Examples

- `nip44_demo_encrypt`: Encrypts/decrypts a message using a provided secret key and peer public x-only key (or peer secret to derive pub). Supports an optional deterministic 32-byte nonce for testing.
- `nip44_vector_demo`: Verifies a known vector end-to-end and prints OK on success.

Build targets are added automatically when building the project.

## Testing

- `test_nip44_vectors` runs the official JSON vectors when Jansson is available; otherwise a built-in single vector.
- All vectors pass under ASAN/UBSAN/TSAN.

## Notes

- Inputs with length 0 or > 65535 are rejected (per vectors invalid cases).
- Ensure peer public key is x-only (32 bytes). If you have a 33-byte compressed pub, strip the first byte and use the 32-byte X.
