#ifndef NOSTR_NIP44_KDF_H
#define NOSTR_NIP44_KDF_H

#include <nostr/nip44/export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HKDF-SHA256 (RFC 5869). Return 0 on success, -1 on failure.
 * Extract requires non-NULL salt, ikm and prk_out, even for empty inputs.
 * Expand requires a 32-byte PRK, non-NULL output, and 1..8160 output bytes;
 * info may be NULL only when info_len is zero.
 * Outputs are cleared on crypto failure, but not on invalid arguments. */
NOSTR_NIP44_API int nostr_nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                            const uint8_t *ikm, size_t ikm_len,
                            uint8_t prk_out[32]);
NOSTR_NIP44_API int nostr_nip44_hkdf_expand(const uint8_t prk[32],
                           const uint8_t *info, size_t info_len,
                           uint8_t okm_out[], size_t okm_len);

/* HMAC-SHA256 over data1 || data2. Return 0 on success, -1 on failure.
 * key and mac_out must be non-NULL; each data pointer may be NULL only
 * when its length is zero. Output is cleared on crypto failure, but not
 * on invalid arguments. */
NOSTR_NIP44_API int nostr_nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *data1, size_t len1,
                           const uint8_t *data2, size_t len2,
                           uint8_t mac_out[32]);

#ifdef __cplusplus
}
#endif
#endif /* NOSTR_NIP44_KDF_H */
