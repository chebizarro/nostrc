# How to use NIP-49 (ncryptsec)

This guide shows how to encrypt/decrypt a 32-byte Nostr private key with NIP-49 using the core API and the GLib API.

## Core API

Header: `nips/nip49/include/nostr/nip49/nip49.h`

- Encrypt 32-byte key to Bech32 `ncryptsec...`:
```c
#include "nostr/nip49/nip49.h"

uint8_t sk[32] = { /* 32 bytes */ };
char *ncrypt = NULL;
int rc = nostr_nip49_encrypt(sk, NOSTR_NIP49_SECURITY_SECURE, "password", 18, &ncrypt);
if (rc == 0) {
  // use ncrypt, then free()
  free(ncrypt);
}
```

- Decrypt `ncryptsec...` back to 32 bytes:
```c
uint8_t out_sk[32]; NostrNip49SecurityByte sec = 0; uint8_t logn = 0;
int rc = nostr_nip49_decrypt(ncrypt, "password", out_sk, &sec, &logn);
```

- Normalization (NFKC):
  - By default, ASCII-only passwords are accepted. If the password contains non-ASCII, core returns `NFKC_REQUIRED` unless a NFKC callback was installed.
  - Install a normalizer:
```c
static int nfkc_cb(const char *in, char **out) { /* ICU/GLib ... */ }
nostr_nip49_set_normalize_cb(nfkc_cb);
```

## GLib API

Header: `nips/nip49/include/nostr/nip49/nip49_g.h`

```c
gchar *ncrypt = NULL; GError *err = NULL;
gboolean ok = nostr_nip49_encrypt_g(sk, 1 /* secure */, "password", 18, &ncrypt, &err);

guint8 out_sk[32], sec, logn;
ok = nostr_nip49_decrypt_g(ncrypt, "password", out_sk, &sec, &logn, &err);
```

GLib layer installs NFKC via `g_utf8_normalize` automatically.

## Parameters and recommendations

- log_n: scrypt cost `N = 1 << log_n`. Suggested values: 16..22 for interactive use based on device performance.
- Security byte:
  - `0x00` = insecure tests
  - `0x01` = secure
  - `0x02` = unknown
- Zeroization: We wipe derived key and normalized password in core. You should wipe/handle the original secret key at the call site as needed.

## CLI example

Built if present: `build/nips/nip49/nostr-nip49`

- Encrypt:
```
./nostr-nip49 encrypt --privkey-hex <64hex> --password <pw> --log-n 18 --security 1
```

- Decrypt:
```
./nostr-nip49 decrypt --ncryptsec <code> --password <pw>
```

## Testing

Registered CTest targets:
- `test_nip49_payload`
- `test_nip49_roundtrip`
- `test_nip49_negative`
- `test_nip49_vectors` (placeholder until spec vectors are pinned)

Run only NIP-49 tests:
```
ctest -R nip49 -V --output-on-failure
```

## Implementation notes

- Bech32 uses NIP-19 helpers directly (`nostr_b32_*`) — no wrappers.
- AEAD: XChaCha20-Poly1305 IETF (24-byte nonce), AD is the security byte.
- KDF: scrypt, r=8, p=1, `N=1<<log_n`.
- Version: payload `version=0x02`; 91-byte strict layout.
- libsodium is initialized on first use.

### Validation and compatibility

- The core now validates payload version during `nostr_nip49_payload_deserialize()` and returns an error if it is not `0x02`. This hardens negative tests and avoids silently accepting incompatible payloads.
- Tools use `nips/nip49/SPEC_SOURCE` to locate the spec source (`SPEC_MD=../../docs/nips/49.md`). Keep this file present; tests verify it.
