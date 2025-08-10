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

/* Derive 32-byte conversation key from sender sk (32) and receiver pk (x-only 32) */
int nostr_nip44_convkey(const uint8_t sender_sk[32],
                        const uint8_t receiver_pk_xonly[32],
                        uint8_t out_convkey[32]);

/* Encrypt UTF-8 content with NIP-44 v2.
   Returns base64 string of concat(version,nonce,ciphertext,mac). Caller frees *out_base64 via free(). */
int nostr_nip44_encrypt_v2(const uint8_t sender_sk[32],
                           const uint8_t receiver_pk_xonly[32],
                           const uint8_t *plaintext_utf8, size_t plaintext_len,
                           char **out_base64);

/* Decrypt base64 payload; validates MAC & padding; outputs UTF-8 content. Caller frees *out_plaintext via free(). */
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
