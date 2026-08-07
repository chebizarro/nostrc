#ifndef NOSTR_NIP44_H
#define NOSTR_NIP44_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NIP-44 version byte (v2) */
typedef enum {
  NOSTR_NIP44_V_UNKNOWN = 0x00,
  NOSTR_NIP44_V2        = 0x02
} NostrNip44Version;

typedef struct {
  NostrNip44Version version; /* 0x02 */
} NostrNip44Params;

/* Derive a 32-byte conversation key from sender sk (32) and receiver
 * x-only pubkey (32). Returns 0 on success. On failure the output is zeroed. */
int nostr_nip44_convkey(const uint8_t sender_sk[32],
                        const uint8_t receiver_pk_xonly[32],
                        uint8_t out_convkey[32]);

/* Encrypt content with standard NIP-44 v2.
 * Returns canonical base64 of version || nonce || ciphertext || MAC.
 * Caller frees *out_base64 with free(). On failure *out_base64 is NULL. */
int nostr_nip44_encrypt_v2(const uint8_t sender_sk[32],
                           const uint8_t receiver_pk_xonly[32],
                           const uint8_t *plaintext_utf8, size_t plaintext_len,
                           char **out_base64);

/* Strictly decode and decrypt a standard NIP-44 v2 payload.
 * The complete base64 input must be canonical; MAC is verified before decrypting.
 * Caller frees *out_plaintext with free(). On failure output is NULL/zero length.
 *
 * NIP-44 does not bind an application kind, sender/recipient tag, role, or
 * session identifier. Protocol users such as NIP-46 must validate those
 * outer-envelope invariants before accepting decrypted content. */
int nostr_nip44_decrypt_v2(const uint8_t receiver_sk[32],
                           const uint8_t sender_pk_xonly[32],
                           const char *base64_payload,
                           uint8_t **out_plaintext, size_t *out_plaintext_len);

/* Lower-level helpers: bring-your-own conversation key */
int nostr_nip44_encrypt_v2_with_convkey(const uint8_t convkey[32],
                                        const uint8_t *plaintext_utf8, size_t plaintext_len,
                                        char **out_base64);
int nostr_nip44_decrypt_v2_with_convkey(const uint8_t convkey[32],
                                        const char *base64_payload,
                                        uint8_t **out_plaintext, size_t *out_plaintext_len);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP44_H */
