#pragma once

#include <stdint.h>
#include <stddef.h>
#include <secure_buf.h>

#ifdef __cplusplus
extern "C" {
#endif

// Security byte as per NIP-49
typedef enum {
  NOSTR_NIP49_SECURITY_INSECURE = 0x00,
  NOSTR_NIP49_SECURITY_SECURE   = 0x01,
  NOSTR_NIP49_SECURITY_UNKNOWN  = 0x02
} NostrNip49SecurityByte;

// 91-byte payload layout (VER|LOG_N|SALT16|NONCE24|AD|CT48)
typedef struct {
  uint8_t version;      // expect 0x02
  uint8_t log_n;        // scrypt exponent byte
  uint8_t salt[16];
  uint8_t nonce[24];
  uint8_t ad;           // security byte
  uint8_t ciphertext[48]; // 32 + 16 tag
} NostrNip49Payload;       // serializes to 91 bytes

// Normalization callback (NFKC) hook
typedef int (*Nip49NormalizeFn)(const char *in_utf8, char **out_nfkc);
void nostr_nip49_set_normalize_cb(Nip49NormalizeFn cb);

// Encrypt 32-byte raw secp256k1 key -> bech32 "ncryptsec..."
int nostr_nip49_encrypt(const uint8_t privkey32[32],
                        NostrNip49SecurityByte security,
                        const char *password_utf8,   // NFKC inside
                        uint8_t log_n,               // e.g. 16,18,20,21,22
                        char **out_ncryptsec);       // caller frees

// Decrypt bech32 -> 32-byte key (+optional metadata)
int nostr_nip49_decrypt(const char *ncryptsec_bech32,
                        const char *password_utf8,   // NFKC inside
                        uint8_t out_privkey32[32],
                        NostrNip49SecurityByte *out_security,  // nullable
                        uint8_t *out_log_n);                     // nullable

// Secure decrypt: returns plaintext key in a secure buffer (mlock + wipe on free)
int nostr_nip49_decrypt_secure(const char *ncryptsec_bech32,
                               const char *password_utf8,
                               nostr_secure_buf *out_privkey,
                               NostrNip49SecurityByte *out_security,  // nullable
                               uint8_t *out_log_n);                   // nullable

// Strict (de)serialization of the 91-byte payload
int nostr_nip49_payload_serialize(const NostrNip49Payload *p, uint8_t out[91]);
int nostr_nip49_payload_deserialize(const uint8_t in[91], NostrNip49Payload *out);

#ifdef __cplusplus
}
#endif
