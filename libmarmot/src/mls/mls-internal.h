/*
 * libmarmot - MLS internal header
 *
 * Internal types and functions for the MLS (RFC 9420) implementation.
 * Ciphersuite: MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519 (0x0001)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MLS_INTERNAL_H
#define MLS_INTERNAL_H

#include <marmot/marmot-types.h>
#include <marmot/marmot-error.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * MLS ciphersuite constants (0x0001)
 * ──────────────────────────────────────────────────────────────────────── */

#define MLS_HASH_LEN        32   /* SHA-256 output */
#define MLS_AEAD_KEY_LEN    16   /* AES-128-GCM key */
#define MLS_AEAD_NONCE_LEN  12   /* AES-128-GCM nonce */
#define MLS_AEAD_TAG_LEN    16   /* AES-128-GCM tag */
#define MLS_KEM_SK_LEN      32   /* X25519 private key */
#define MLS_KEM_PK_LEN      32   /* X25519 public key */
#define MLS_KEM_ENC_LEN     32   /* X25519 ephemeral public key */
#define MLS_KEM_SECRET_LEN  32   /* DHKEM shared secret */
#define MLS_SIG_SK_LEN      64   /* Ed25519 secret key (libsodium format: scalar+pk) */
#define MLS_SIG_PK_LEN      32   /* Ed25519 public key */
#define MLS_SIG_LEN         64   /* Ed25519 signature */
#define MLS_KDF_EXTRACT_LEN 32   /* HKDF-SHA256 extract output */

/* ──────────────────────────────────────────────────────────────────────────
 * Crypto primitives (mls_crypto.c)
 *
 * These wrap libsodium + OpenSSL to match the MLS ciphersuite 0x0001
 * exactly. All functions return 0 on success, non-zero on failure.
 * ──────────────────────────────────────────────────────────────────────── */

/* ── HPKE (DHKEM X25519, RFC 9180 §4.1) ──────────────────────────────── */

/** X25519 DH. out must be MLS_KEM_SECRET_LEN bytes. */
int mls_crypto_dh(uint8_t out[MLS_KEM_SECRET_LEN],
                  const uint8_t sk[MLS_KEM_SK_LEN],
                  const uint8_t pk[MLS_KEM_PK_LEN]);

/** Generate X25519 keypair. */
int mls_crypto_kem_keygen(uint8_t sk[MLS_KEM_SK_LEN],
                           uint8_t pk[MLS_KEM_PK_LEN]);

/** DHKEM Encap: produce (shared_secret, enc) for recipient pk. */
int mls_crypto_kem_encap(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                          uint8_t enc[MLS_KEM_ENC_LEN],
                          const uint8_t pk[MLS_KEM_PK_LEN]);

/** DHKEM Decap: recover shared_secret from enc using sk. */
int mls_crypto_kem_decap(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                          const uint8_t enc[MLS_KEM_ENC_LEN],
                          const uint8_t sk[MLS_KEM_SK_LEN],
                          const uint8_t pk[MLS_KEM_PK_LEN]);

/* ── HKDF (SHA-256) ──────────────────────────────────────────────────── */

/** HKDF-Extract: PRK = HMAC-SHA256(salt, ikm). */
int mls_crypto_hkdf_extract(uint8_t prk[MLS_KDF_EXTRACT_LEN],
                             const uint8_t *salt, size_t salt_len,
                             const uint8_t *ikm, size_t ikm_len);

/** HKDF-Expand: derive out_len bytes from PRK + info. */
int mls_crypto_hkdf_expand(uint8_t *out, size_t out_len,
                            const uint8_t prk[MLS_KDF_EXTRACT_LEN],
                            const uint8_t *info, size_t info_len);

/** HKDF-Expand-Label (MLS §5.1): derive from secret using label + context. */
int mls_crypto_expand_with_label(uint8_t *out, size_t out_len,
                                  const uint8_t secret[MLS_HASH_LEN],
                                  const char *label,
                                  const uint8_t *context, size_t ctx_len);

/** Derive-Secret (MLS §5.1): shorthand for expand_with_label with empty context. */
int mls_crypto_derive_secret(uint8_t out[MLS_HASH_LEN],
                              const uint8_t secret[MLS_HASH_LEN],
                              const char *label);

/* ── AEAD (AES-128-GCM) ─────────────────────────────────────────────── */

/** Encrypt. out must have room for pt_len + MLS_AEAD_TAG_LEN bytes. */
int mls_crypto_aead_encrypt(uint8_t *out, size_t *out_len,
                             const uint8_t key[MLS_AEAD_KEY_LEN],
                             const uint8_t nonce[MLS_AEAD_NONCE_LEN],
                             const uint8_t *pt, size_t pt_len,
                             const uint8_t *aad, size_t aad_len);

/** Decrypt. out must have room for ct_len - MLS_AEAD_TAG_LEN bytes. */
int mls_crypto_aead_decrypt(uint8_t *out, size_t *out_len,
                             const uint8_t key[MLS_AEAD_KEY_LEN],
                             const uint8_t nonce[MLS_AEAD_NONCE_LEN],
                             const uint8_t *ct, size_t ct_len,
                             const uint8_t *aad, size_t aad_len);

/* ── Hash (SHA-256) ──────────────────────────────────────────────────── */

/** SHA-256 hash. */
int mls_crypto_hash(uint8_t out[MLS_HASH_LEN],
                    const uint8_t *data, size_t len);

/* ── Signing (Ed25519) ───────────────────────────────────────────────── */

/** Generate Ed25519 keypair. sk is 64 bytes (libsodium format). */
int mls_crypto_sign_keygen(uint8_t sk[MLS_SIG_SK_LEN],
                            uint8_t pk[MLS_SIG_PK_LEN]);

/** Sign message. sig is 64 bytes. */
int mls_crypto_sign(uint8_t sig[MLS_SIG_LEN],
                    const uint8_t sk[MLS_SIG_SK_LEN],
                    const uint8_t *msg, size_t msg_len);

/** Verify signature. Returns 0 on valid signature. */
int mls_crypto_verify(const uint8_t sig[MLS_SIG_LEN],
                      const uint8_t pk[MLS_SIG_PK_LEN],
                      const uint8_t *msg, size_t msg_len);

/* ── Random ──────────────────────────────────────────────────────────── */

/** Fill buffer with cryptographically secure random bytes. */
void mls_crypto_random(uint8_t *out, size_t len);

/* ── Ref Hash (MLS §5.3.1) ──────────────────────────────────────────── */

/** Compute RefHash(label, value) = H(label_len || label || value). */
int mls_crypto_ref_hash(uint8_t out[MLS_HASH_LEN],
                        const char *label,
                        const uint8_t *value, size_t value_len);

/* ──────────────────────────────────────────────────────────────────────────
 * TLS Presentation Language (mls_tls.c)
 *
 * Serialization primitives for MLS wire format.
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MlsTlsBuf:
 *
 * Growable buffer for TLS serialization.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} MlsTlsBuf;

/** Initialize a buffer with initial capacity. */
int mls_tls_buf_init(MlsTlsBuf *buf, size_t initial_cap);

/** Free buffer resources. */
void mls_tls_buf_free(MlsTlsBuf *buf);

/** Append raw bytes. */
int mls_tls_buf_append(MlsTlsBuf *buf, const uint8_t *data, size_t len);

/** Write uint8_t. */
int mls_tls_write_u8(MlsTlsBuf *buf, uint8_t val);

/** Write uint16_t (big-endian). */
int mls_tls_write_u16(MlsTlsBuf *buf, uint16_t val);

/** Write uint32_t (big-endian). */
int mls_tls_write_u32(MlsTlsBuf *buf, uint32_t val);

/** Write uint64_t (big-endian). */
int mls_tls_write_u64(MlsTlsBuf *buf, uint64_t val);

/** Write variable-length opaque data with 1-byte length prefix (max 255). */
int mls_tls_write_opaque8(MlsTlsBuf *buf, const uint8_t *data, size_t len);

/** Write variable-length opaque data with 2-byte length prefix (max 65535). */
int mls_tls_write_opaque16(MlsTlsBuf *buf, const uint8_t *data, size_t len);

/** Write variable-length opaque data with 4-byte length prefix. */
int mls_tls_write_opaque32(MlsTlsBuf *buf, const uint8_t *data, size_t len);

/**
 * MlsTlsReader:
 *
 * Cursor for reading TLS-serialized data.
 */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} MlsTlsReader;

/** Initialize a reader. */
void mls_tls_reader_init(MlsTlsReader *r, const uint8_t *data, size_t len);

/** Read uint8_t. */
int mls_tls_read_u8(MlsTlsReader *r, uint8_t *out);

/** Read uint16_t (big-endian). */
int mls_tls_read_u16(MlsTlsReader *r, uint16_t *out);

/** Read uint32_t (big-endian). */
int mls_tls_read_u32(MlsTlsReader *r, uint32_t *out);

/** Read uint64_t (big-endian). */
int mls_tls_read_u64(MlsTlsReader *r, uint64_t *out);

/** Read opaque data with 1-byte length prefix. Caller frees *out. */
int mls_tls_read_opaque8(MlsTlsReader *r, uint8_t **out, size_t *out_len);

/** Read opaque data with 2-byte length prefix. Caller frees *out. */
int mls_tls_read_opaque16(MlsTlsReader *r, uint8_t **out, size_t *out_len);

/** Read opaque data with 4-byte length prefix. Caller frees *out. */
int mls_tls_read_opaque32(MlsTlsReader *r, uint8_t **out, size_t *out_len);

/** Read exact N bytes (no length prefix). */
int mls_tls_read_fixed(MlsTlsReader *r, uint8_t *out, size_t n);

/** Check if reader has been fully consumed. */
bool mls_tls_reader_done(const MlsTlsReader *r);

/** Bytes remaining in reader. */
size_t mls_tls_reader_remaining(const MlsTlsReader *r);

#ifdef __cplusplus
}
#endif

#endif /* MLS_INTERNAL_H */
