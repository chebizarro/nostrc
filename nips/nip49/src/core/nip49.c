#include "nostr/nip49/nip49.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sodium.h>

// Internal adapters
int nip49_kdf_scrypt(const char *password_nfkc, const uint8_t salt[16], uint8_t log_n, uint8_t out_key32[32]);
int nip49_aead_encrypt_xchacha20poly1305(const uint8_t key32[32], const uint8_t nonce24[24], const uint8_t *ad, size_t ad_len, const uint8_t pt32[32], uint8_t out_ct48[48]);
int nip49_aead_decrypt_xchacha20poly1305(const uint8_t key32[32], const uint8_t nonce24[24], const uint8_t *ad, size_t ad_len, const uint8_t ct48[48], uint8_t out_pt32[32]);
int nip49_bech32_encode_ncryptsec(const uint8_t payload91[91], char **out_bech32);
int nip49_bech32_decode_ncryptsec(const char *bech32, uint8_t out_payload91[91]);

// Error codes (module-local)
#define NOSTR_NIP49_ERR_ARGS          -1
#define NOSTR_NIP49_ERR_NFKC_REQUIRED -2
#define NOSTR_NIP49_ERR_KDF           -3
#define NOSTR_NIP49_ERR_AEAD          -4
#define NOSTR_NIP49_ERR_BECH32        -5
#define NOSTR_NIP49_ERR_VERSION       -6

// Normalization callback holder
static Nip49NormalizeFn g_norm_cb = NULL;

void nostr_nip49_set_normalize_cb(Nip49NormalizeFn cb) { g_norm_cb = cb; }

static void ensure_sodium_init(void) {
  static int inited = 0;
  if (!inited) { if (sodium_init() >= 0) inited = 1; }
}

static int valid_log_n(uint8_t log_n) {
  // Prevent extreme values; typical range 10..22. Allow 10..31 to be permissive but safe for shift.
  return (log_n >= 10 && log_n <= 31);
}

static int ascii_only(const char *s) {
  for (const unsigned char *p = (const unsigned char*)s; *p; ++p) {
    if (*p >= 0x80) return 0;
  }
  return 1;
}

static int normalize_password(const char *in_utf8, char **out_nfkc) {
  if (!in_utf8 || !out_nfkc) return NOSTR_NIP49_ERR_ARGS;
  *out_nfkc = NULL;
  if (g_norm_cb) {
    return g_norm_cb(in_utf8, out_nfkc);
  }
  if (!ascii_only(in_utf8)) {
    return NOSTR_NIP49_ERR_NFKC_REQUIRED;
  }
  // ASCII pass-through (dup)
  size_t len = strlen(in_utf8);
  char *dup = (char*)malloc(len + 1);
  if (!dup) return NOSTR_NIP49_ERR_ARGS;
  memcpy(dup, in_utf8, len + 1);
  *out_nfkc = dup;
  return 0;
}

int nostr_nip49_payload_serialize(const NostrNip49Payload *p, uint8_t out[91]) {
  if (!p || !out) return NOSTR_NIP49_ERR_ARGS;
  out[0] = p->version;
  out[1] = p->log_n;
  memcpy(out + 2, p->salt, 16);
  memcpy(out + 18, p->nonce, 24);
  out[42] = p->ad;
  memcpy(out + 43, p->ciphertext, 48);
  return 0;
}

int nostr_nip49_payload_deserialize(const uint8_t in[91], NostrNip49Payload *outp) {
  if (!in || !outp) return NOSTR_NIP49_ERR_ARGS;
  memset(outp, 0, sizeof(*outp));
  outp->version = in[0];
  if (outp->version != 0x02) {
    return NOSTR_NIP49_ERR_VERSION;
  }
  outp->log_n = in[1];
  memcpy(outp->salt, in + 2, 16);
  memcpy(outp->nonce, in + 18, 24);
  outp->ad = in[42];
  memcpy(outp->ciphertext, in + 43, 48);
  return 0;
}

int nostr_nip49_encrypt(const uint8_t privkey32[32],
                        NostrNip49SecurityByte security,
                        const char *password_utf8,
                        uint8_t log_n,
                        char **out_ncryptsec) {
  if (!privkey32 || !password_utf8 || !out_ncryptsec) return NOSTR_NIP49_ERR_ARGS;
  if (!valid_log_n(log_n)) return NOSTR_NIP49_ERR_ARGS;
  *out_ncryptsec = NULL;
  ensure_sodium_init();

  // Normalize password (NFKC or ASCII pass-through)
  char *pw_nfkc = NULL;
  int rc = normalize_password(password_utf8, &pw_nfkc);
  if (rc != 0) return rc;

  // Build payload
  NostrNip49Payload p;
  memset(&p, 0, sizeof(p));
  p.version = 0x02;
  p.log_n = log_n;
  p.ad = (uint8_t)security;

  // Salt and nonce generation using libsodium
  randombytes_buf(p.salt, sizeof p.salt);
  randombytes_buf(p.nonce, sizeof p.nonce);

  // KDF
  uint8_t key32[32];
  rc = nip49_kdf_scrypt(pw_nfkc, p.salt, p.log_n, key32);
  if (rc != 0) { memset(key32, 0, sizeof key32); memset(pw_nfkc, 0, strlen(pw_nfkc)); free(pw_nfkc); return NOSTR_NIP49_ERR_KDF; }

  // AEAD encrypt
  rc = nip49_aead_encrypt_xchacha20poly1305(key32, p.nonce, &p.ad, 1, privkey32, p.ciphertext);
  if (rc != 0) { memset(key32, 0, sizeof key32); memset(pw_nfkc, 0, strlen(pw_nfkc)); free(pw_nfkc); return NOSTR_NIP49_ERR_AEAD; }

  // Serialize and Bech32
  uint8_t buf[91];
  nostr_nip49_payload_serialize(&p, buf);
  rc = nip49_bech32_encode_ncryptsec(buf, out_ncryptsec);

  // Zeroize secrets
  memset(key32, 0, sizeof key32);
  memset(pw_nfkc, 0, strlen(pw_nfkc));
  free(pw_nfkc);

  if (rc != 0) return NOSTR_NIP49_ERR_BECH32;
  return 0;
}

int nostr_nip49_decrypt(const char *ncryptsec_bech32,
                        const char *password_utf8,
                        uint8_t out_privkey32[32],
                        NostrNip49SecurityByte *out_security,
                        uint8_t *out_log_n) {
  if (!ncryptsec_bech32 || !password_utf8 || !out_privkey32) return NOSTR_NIP49_ERR_ARGS;
  ensure_sodium_init();

  // Decode
  uint8_t buf[91];
  int rc = nip49_bech32_decode_ncryptsec(ncryptsec_bech32, buf);
  if (rc != 0) return NOSTR_NIP49_ERR_BECH32;

  NostrNip49Payload p; nostr_nip49_payload_deserialize(buf, &p);
  if (p.version != 0x02) return NOSTR_NIP49_ERR_VERSION;
  if (!valid_log_n(p.log_n)) return NOSTR_NIP49_ERR_ARGS;

  // Normalize password
  char *pw_nfkc = NULL;
  rc = normalize_password(password_utf8, &pw_nfkc);
  if (rc != 0) return rc;

  // KDF
  uint8_t key32[32];
  rc = nip49_kdf_scrypt(pw_nfkc, p.salt, p.log_n, key32);
  if (rc != 0) { memset(key32, 0, sizeof key32); memset(pw_nfkc, 0, strlen(pw_nfkc)); free(pw_nfkc); return NOSTR_NIP49_ERR_KDF; }

  // AEAD decrypt
  rc = nip49_aead_decrypt_xchacha20poly1305(key32, p.nonce, &p.ad, 1, p.ciphertext, out_privkey32);

  // Zeroize secrets
  memset(key32, 0, sizeof key32);
  memset(pw_nfkc, 0, strlen(pw_nfkc));
  free(pw_nfkc);

  if (rc != 0) return NOSTR_NIP49_ERR_AEAD;
  if (out_security) *out_security = (NostrNip49SecurityByte)p.ad;
  if (out_log_n) *out_log_n = p.log_n;
  return 0;
}
