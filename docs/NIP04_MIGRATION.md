# NIP-04 Migration Guide (AEAD v2)

This guide explains migrating from legacy NIP-04 AES-CBC (`base64(cipher)?iv=base64(iv)`) to the new AEAD v2 envelope (`v=2:base64(nonce||cipher||tag)`), which is now the ONLY encryption format emitted by the library.

- AEAD v2 envelope: `v=2:base64(nonce(12) || ciphertext || tag(16))`
- Cipher: AES-256-GCM via OpenSSL EVP
- Key separation: HKDF-SHA256 with `info="NIP04"` over the ECDH shared X
- Decrypt fallback: legacy `?iv=` is still accepted for interop

## Encrypt/Decrypt APIs

Prefer these canonical APIs. The `_secure` variants take the private key in secure memory.

```c
int nostr_nip04_encrypt(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const char *sender_seckey_hex,
    char **out_content_b64_qiv,
    char **out_error);

int nostr_nip04_decrypt(
    const char *content_b64_qiv,
    const char *sender_pubkey_hex,
    const char *receiver_seckey_hex,
    char **out_plaintext_utf8,
    char **out_error);

int nostr_nip04_encrypt_secure(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const nostr_secure_buf *sender_seckey,
    char **out_content_b64_qiv,
    char **out_error);

int nostr_nip04_decrypt_secure(
    const char *content_b64_qiv,
    const char *sender_pubkey_hex,
    const nostr_secure_buf *receiver_seckey,
    char **out_plaintext_utf8,
    char **out_error);
```

## Example

```c
#include <nostr/nip04.h>
#include <secure_buf.h>

const char *msg = "Hello, NIP-04!";
char *cipher = NULL, *err = NULL, *plain = NULL;

nostr_secure_buf sb_sender = secure_alloc(32); // load hex 32B into sb_sender
nostr_secure_buf sb_receiver = secure_alloc(32); // load hex 32B into sb_receiver

if (nostr_nip04_encrypt_secure(msg, receiver_pubkey_hex, &sb_sender, &cipher, &err) != 0) {
    // handle error
}

if (nostr_nip04_decrypt_secure(cipher, sender_pubkey_hex, &sb_receiver, &plain, &err) != 0) {
    // handle error
}

// cipher starts with "v=2:"; decrypt also accepts legacy ?iv= from older peers
```

## Deprecated APIs

- `nostr_nip04_shared_secret_hex(...)` is deprecated.
  - Rationale: exposing raw ECDH shared secrets increases attack surface.
  - Replace diagnostics with end-to-end AEAD encrypt/decrypt or use internal HKDF outputs only.

## Relay Security Posture

On startup the relay logs a one-line banner summarizing posture, e.g.:

```
nostrc-relayd: security AEAD=v2 replayTTL=900s skew=+600/-86400
```

- `replayTTL`: duplicate cache TTL
- `skew`: timestamp skew window (+future / -past)

## Testing and Compatibility

- Tests and consumers should accept `v=2:` envelopes by default.
- Decrypt continues to accept legacy `?iv=` to interoperate with older peers.
- CI no longer sets `NIP04_LEGACY_CBC`.

## FAQs

- Q: Can I still produce `?iv=`?
  - A: No. Encrypt emits AEAD v2 only. Decrypt fallback remains enabled for compatibility.

- Q: How do I handle errors?
  - A: All decrypt failures return "decrypt failed" to minimize information leakage.

- Q: Which pubkey format is accepted?
  - A: SEC1 compressed (33B) and uncompressed (65B) hex encodings are accepted by the APIs.
