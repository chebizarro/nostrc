# NIP-04 and NIP-46 Transport Migration

## Implemented formats

The library supports two NIP-04 wire formats:

| Format | Wire prefix/shape | Key derivation | Integrity | Policy |
|---|---|---|---|---|
| nostrc AEAD v2 extension | `v=2:base64(nonce(12) || ciphertext || tag(16))` | HKDF-SHA256 over ECDH shared X, `info="NIP04"` | AES-256-GCM tag | Generic encrypt default |
| Original NIP-04 | `base64(ciphertext)?iv=base64(iv)` | Raw ECDH shared X | None (AES-256-CBC/PKCS#7) | Explicit legacy emission only |

The `v=2:` format is a project extension. Generic NIP-04 capability does not
imply support for it. Original CBC remains accepted by decrypt for
interoperability, but it is unauthenticated and malleable. Use it only inside a
strictly validated, signed outer event.

## NIP-04 APIs

`nostr_nip04_encrypt()` and `nostr_nip04_encrypt_secure()` always emit AEAD
v2. No environment variable can downgrade them.

Legacy CBC emission is deliberately explicit:

```c
int nostr_nip04_encrypt_legacy_secure(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const nostr_secure_buf *sender_seckey,
    char **out_content,
    char **out_error);
```

Both decrypt APIs recognize AEAD v2 and original CBC unless the library was
built with `NIP04_STRICT_AEAD_ONLY=ON`:

```c
int nostr_nip04_decrypt(
    const char *content,
    const char *sender_pubkey_hex,
    const char *receiver_seckey_hex,
    char **out_plaintext_utf8,
    char **out_error);

int nostr_nip04_decrypt_secure(
    const char *content,
    const char *sender_pubkey_hex,
    const nostr_secure_buf *receiver_seckey,
    char **out_plaintext_utf8,
    char **out_error);
```

Every public decrypt failure returns the same allocated error string,
`"decrypt failed"`, when `out_error` is supplied. Callers must not branch on
format, padding, KDF, or provider failure details.

Accepted peer public keys are x-only (32-byte), compressed SEC1 (33-byte), or
uncompressed SEC1 (65-byte) hex. Secret keys are exactly 32-byte hex or a
secure 32-byte buffer.

## NIP-46 session transport policy

Every `NostrNip46Session` owns one explicit transport mode:

```c
typedef enum {
    NOSTR_NIP46_TRANSPORT_NIP44_V2 = 1,
    NOSTR_NIP46_TRANSPORT_NIP04_LEGACY = 2,
    NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION = 3
} NostrNip46TransportMode;

int nostr_nip46_session_set_transport_mode(
    NostrNip46Session *session,
    NostrNip46TransportMode mode);

int nostr_nip46_transport_encrypt(
    NostrNip46Session *session,
    const char *peer_pubkey_hex,
    const char *plaintext,
    char **out_ciphertext);

int nostr_nip46_transport_decrypt(
    NostrNip46Session *session,
    const char *peer_pubkey_hex,
    const char *ciphertext,
    char **out_plaintext);
```

The default is `NOSTR_NIP46_TRANSPORT_NIP44_V2`. Configure another mode only
after peer capability negotiation and before starting client or bunker
transport. Ciphertext shape never changes the session mode, and a response uses
the same mode as its request.

### Interoperability matrix

| Peer capability | Session mode | Emitted payload |
|---|---|---|
| Standard/current NIP-46 with NIP-44 v2 | `NIP44_V2` | Standard NIP-44 v2 |
| Legacy signer requiring original NIP-04 | `NIP04_LEGACY` | CBC `?iv=` |
| Peer explicitly advertising the nostrc extension | `NIP04_AEAD_V2_EXTENSION` | `v=2:` AEAD |
| Unknown or conflicting capability | Do not start transport | Failure |

The algorithm-specific
`nostr_nip46_client_nip04_*`/`nostr_nip46_client_nip44_*` functions remain
compatibility helpers. New kind-24133 network paths should use the unified
transport APIs.

## Security and operations

- Never log decrypted requests, replies, full RPC parameters, ciphertext, or
  private keys, including in debug builds.
- Treat a legacy CBC decrypt success as transport compatibility, not proof of
  ciphertext integrity. Outer event signature, canonical ID, author, recipient,
  kind, and request/response correlation remain mandatory.
- `nostr_nip04_shared_secret_hex()` is deprecated because exporting raw ECDH
  material into ordinary heap strings increases exposure.
- Builds that can drop legacy interoperability may enable
  `NIP04_STRICT_AEAD_ONLY`; this is a build-time upper bound, not per-peer
  negotiation.
