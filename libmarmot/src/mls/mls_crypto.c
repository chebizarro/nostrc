/*
 * libmarmot - MLS crypto primitives
 *
 * Wraps libsodium (Ed25519, X25519, random) and OpenSSL (AES-128-GCM,
 * HKDF-SHA256, SHA-256) for MLS ciphersuite 0x0001.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls-internal.h"
#include <string.h>
#include <stdlib.h>

#include <sodium.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Random
 * ══════════════════════════════════════════════════════════════════════════ */

void
mls_crypto_random(uint8_t *out, size_t len)
{
    randombytes_buf(out, len);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Hash (SHA-256)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_crypto_hash(uint8_t out[MLS_HASH_LEN], const uint8_t *data, size_t len)
{
    if (!SHA256(data, len, out))
        return -1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * HKDF (SHA-256)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_crypto_hkdf_extract(uint8_t prk[MLS_KDF_EXTRACT_LEN],
                         const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len)
{
    unsigned int out_len = MLS_KDF_EXTRACT_LEN;
    /* HKDF-Extract = HMAC-SHA256(salt, ikm) */
    const uint8_t default_salt[MLS_HASH_LEN] = {0};
    if (!salt || salt_len == 0) {
        salt = default_salt;
        salt_len = MLS_HASH_LEN;
    }
    if (!HMAC(EVP_sha256(), salt, (int)salt_len, ikm, ikm_len, prk, &out_len))
        return -1;
    return 0;
}

int
mls_crypto_hkdf_expand(uint8_t *out, size_t out_len,
                        const uint8_t prk[MLS_KDF_EXTRACT_LEN],
                        const uint8_t *info, size_t info_len)
{
    /*
     * HKDF-Expand (RFC 5869 §2.3):
     * T(0) = empty
     * T(i) = HMAC-SHA256(PRK, T(i-1) || info || i)
     * Output is T(1) || T(2) || ... truncated to out_len
     */
    /* Check for overflow before arithmetic */
    if (out_len > SIZE_MAX - MLS_HASH_LEN + 1) return -1;
    size_t n = (out_len + MLS_HASH_LEN - 1) / MLS_HASH_LEN;
    if (n > 255) return -1;

    uint8_t t_prev[MLS_HASH_LEN];
    size_t t_prev_len = 0;
    size_t offset = 0;

    for (size_t i = 1; i <= n; i++) {
        unsigned int hmac_len = MLS_HASH_LEN;
        uint8_t t_curr[MLS_HASH_LEN];

        /* Build input: T(i-1) || info || uint8(i) */
        size_t in_len = t_prev_len + info_len + 1;
        uint8_t *in_buf = malloc(in_len);
        if (!in_buf) return -1;

        size_t pos = 0;
        if (t_prev_len > 0) {
            memcpy(in_buf, t_prev, t_prev_len);
            pos += t_prev_len;
        }
        if (info_len > 0) {
            memcpy(in_buf + pos, info, info_len);
            pos += info_len;
        }
        in_buf[pos] = (uint8_t)i;

        if (!HMAC(EVP_sha256(), prk, MLS_KDF_EXTRACT_LEN, in_buf, in_len, t_curr, &hmac_len)) {
            free(in_buf);
            return -1;
        }
        free(in_buf);

        size_t copy_len = out_len - offset;
        if (copy_len > MLS_HASH_LEN) copy_len = MLS_HASH_LEN;
        memcpy(out + offset, t_curr, copy_len);
        offset += copy_len;

        memcpy(t_prev, t_curr, MLS_HASH_LEN);
        t_prev_len = MLS_HASH_LEN;
    }

    return 0;
}

int
mls_crypto_expand_with_label(uint8_t *out, size_t out_len,
                              const uint8_t secret[MLS_HASH_LEN],
                              const char *label,
                              const uint8_t *context, size_t ctx_len)
{
    return mls_crypto_expand_with_label_raw(out, out_len, secret,
                                             (const uint8_t *)label,
                                             label ? strlen(label) : 0,
                                             context, ctx_len);
}

int
mls_crypto_derive_secret(uint8_t out[MLS_HASH_LEN],
                          const uint8_t secret[MLS_HASH_LEN],
                          const char *label)
{
    return mls_crypto_expand_with_label(out, MLS_HASH_LEN,
                                         secret, label, NULL, 0);
}

int
mls_crypto_derive_tree_secret(uint8_t *out, size_t out_len,
                               const uint8_t secret[MLS_HASH_LEN],
                               const char *label,
                               uint32_t generation)
{
    uint8_t generation_be[4];
    generation_be[0] = (uint8_t)(generation >> 24);
    generation_be[1] = (uint8_t)(generation >> 16);
    generation_be[2] = (uint8_t)(generation >> 8);
    generation_be[3] = (uint8_t)generation;

    return mls_crypto_expand_with_label(out, out_len, secret, label,
                                         generation_be, sizeof(generation_be));
}

/**
 * Encode a QUIC variable-length integer (RFC 9000 §16).
 * Returns number of bytes written (1, 2, 4, or 8).
 */
static size_t
quic_varint_encode(uint64_t value, uint8_t *out)
{
    if (value < 64) {
        out[0] = (uint8_t)value;
        return 1;
    } else if (value < 16384) {
        out[0] = (uint8_t)(0x40 | (value >> 8));
        out[1] = (uint8_t)(value & 0xFF);
        return 2;
    } else if (value < 1073741824ULL) {
        out[0] = (uint8_t)(0x80 | (value >> 24));
        out[1] = (uint8_t)((value >> 16) & 0xFF);
        out[2] = (uint8_t)((value >> 8) & 0xFF);
        out[3] = (uint8_t)(value & 0xFF);
        return 4;
    } else {
        out[0] = (uint8_t)(0xC0 | (value >> 56));
        out[1] = (uint8_t)((value >> 48) & 0xFF);
        out[2] = (uint8_t)((value >> 40) & 0xFF);
        out[3] = (uint8_t)((value >> 32) & 0xFF);
        out[4] = (uint8_t)((value >> 24) & 0xFF);
        out[5] = (uint8_t)((value >> 16) & 0xFF);
        out[6] = (uint8_t)((value >> 8) & 0xFF);
        out[7] = (uint8_t)(value & 0xFF);
        return 8;
    }
}

int
mls_crypto_expand_with_label_raw(uint8_t *out, size_t out_len,
                                  const uint8_t secret[MLS_HASH_LEN],
                                  const uint8_t *label, size_t label_len,
                                  const uint8_t *context, size_t ctx_len)
{
    /*
     * MLS §5.1 ExpandWithLabel — KDFLabel uses QUIC variable-length
     * integer encoding (RFC 9000 §16) for the <V> length prefixes:
     *
     * struct {
     *   uint16 length = out_len;
     *   opaque label<V> = "MLS 1.0 " + Label;
     *   opaque context<V> = Context;
     * } KDFLabel;
     */
    const char prefix[] = "MLS 1.0 ";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t full_label_len = prefix_len + label_len;

    /* Max info size: 2 (length) + 8 (varint) + label + 8 (varint) + context */
    size_t info_size = 2 + 8 + full_label_len + 8 + ctx_len;
    uint8_t *info = malloc(info_size);
    if (!info) return -1;

    size_t pos = 0;
    /* uint16 length (big-endian) */
    info[pos++] = (uint8_t)(out_len >> 8);
    info[pos++] = (uint8_t)(out_len & 0xff);
    /* label<V>: QUIC varint length prefix */
    pos += quic_varint_encode(full_label_len, info + pos);
    memcpy(info + pos, prefix, prefix_len);
    pos += prefix_len;
    if (label_len > 0) {
        memcpy(info + pos, label, label_len);
        pos += label_len;
    }
    /* context<V>: QUIC varint length prefix */
    pos += quic_varint_encode(ctx_len, info + pos);
    if (ctx_len > 0) {
        memcpy(info + pos, context, ctx_len);
        pos += ctx_len;
    }

    int rc = mls_crypto_hkdf_expand(out, out_len, secret, info, pos);
    free(info);
    return rc;
}

int
mls_crypto_derive_secret_raw(uint8_t out[MLS_HASH_LEN],
                              const uint8_t secret[MLS_HASH_LEN],
                              const uint8_t *label, size_t label_len)
{
    return mls_crypto_expand_with_label_raw(out, MLS_HASH_LEN,
                                             secret, label, label_len,
                                             NULL, 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * AEAD (AES-128-GCM)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_crypto_aead_encrypt(uint8_t *out, size_t *out_len,
                         const uint8_t key[MLS_AEAD_KEY_LEN],
                         const uint8_t nonce[MLS_AEAD_NONCE_LEN],
                         const uint8_t *pt, size_t pt_len,
                         const uint8_t *aad, size_t aad_len)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int len;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1)
        goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, MLS_AEAD_NONCE_LEN, NULL) != 1)
        goto cleanup;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto cleanup;

    /* AAD */
    if (aad && aad_len > 0) {
        if (EVP_EncryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1)
            goto cleanup;
    }

    /* Plaintext */
    if (EVP_EncryptUpdate(ctx, out, &len, pt, (int)pt_len) != 1)
        goto cleanup;
    size_t written = (size_t)len;

    if (EVP_EncryptFinal_ex(ctx, out + written, &len) != 1)
        goto cleanup;
    written += (size_t)len;

    /* Append tag */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, MLS_AEAD_TAG_LEN, out + written) != 1)
        goto cleanup;
    written += MLS_AEAD_TAG_LEN;

    *out_len = written;
    rc = 0;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int
mls_crypto_aead_decrypt(uint8_t *out, size_t *out_len,
                         const uint8_t key[MLS_AEAD_KEY_LEN],
                         const uint8_t nonce[MLS_AEAD_NONCE_LEN],
                         const uint8_t *ct, size_t ct_len,
                         const uint8_t *aad, size_t aad_len)
{
    if (ct_len < MLS_AEAD_TAG_LEN) return -1;

    size_t enc_len = ct_len - MLS_AEAD_TAG_LEN;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int len;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1)
        goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, MLS_AEAD_NONCE_LEN, NULL) != 1)
        goto cleanup;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto cleanup;

    /* AAD */
    if (aad && aad_len > 0) {
        if (EVP_DecryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1)
            goto cleanup;
    }

    /* Ciphertext (without tag) */
    if (EVP_DecryptUpdate(ctx, out, &len, ct, (int)enc_len) != 1)
        goto cleanup;
    size_t written = (size_t)len;

    /* Set expected tag */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, MLS_AEAD_TAG_LEN,
                             (void *)(ct + enc_len)) != 1)
        goto cleanup;

    /* Verify tag */
    if (EVP_DecryptFinal_ex(ctx, out + written, &len) != 1)
        goto cleanup;
    written += (size_t)len;

    *out_len = written;
    rc = 0;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * HPKE / DHKEM (X25519)
 * ══════════════════════════════════════════════════════════════════════════ */

#define MLS_HPKE_KEM_ID        0x0020 /* DHKEM(X25519, HKDF-SHA256) */
#define MLS_HPKE_KDF_ID        0x0001 /* HKDF-SHA256 */
#define MLS_HPKE_AEAD_ID       0x0001 /* AES-128-GCM */
#define MLS_HPKE_MODE_BASE     0x00

static const uint8_t MLS_HPKE_LABEL_PREFIX[] = {'H','P','K','E','-','v','1'};
static const uint8_t MLS_HPKE_KEM_SUITE_ID[] = {'K','E','M', 0x00, 0x20};
static const uint8_t MLS_HPKE_SUITE_ID[] = {
    'H','P','K','E',
    0x00, 0x20, /* KEM  DHKEM(X25519, HKDF-SHA256) */
    0x00, 0x01, /* KDF  HKDF-SHA256 */
    0x00, 0x01  /* AEAD AES-128-GCM */
};

static int
hpke_labeled_extract(uint8_t prk[MLS_KDF_EXTRACT_LEN],
                     const uint8_t *suite_id, size_t suite_id_len,
                     const uint8_t *salt, size_t salt_len,
                     const char *label,
                     const uint8_t *ikm, size_t ikm_len)
{
    size_t label_len = label ? strlen(label) : 0;
    size_t prefix_len = sizeof(MLS_HPKE_LABEL_PREFIX);
    if (ikm_len > SIZE_MAX - prefix_len - suite_id_len - label_len)
        return -1;
    size_t labeled_ikm_len = prefix_len + suite_id_len + label_len + ikm_len;

    uint8_t *labeled_ikm = malloc(labeled_ikm_len ? labeled_ikm_len : 1);
    if (!labeled_ikm) return -1;

    size_t pos = 0;
    memcpy(labeled_ikm + pos, MLS_HPKE_LABEL_PREFIX, prefix_len);
    pos += prefix_len;
    memcpy(labeled_ikm + pos, suite_id, suite_id_len);
    pos += suite_id_len;
    if (label_len > 0) {
        memcpy(labeled_ikm + pos, label, label_len);
        pos += label_len;
    }
    if (ikm_len > 0) {
        memcpy(labeled_ikm + pos, ikm, ikm_len);
        pos += ikm_len;
    }

    int rc = mls_crypto_hkdf_extract(prk, salt, salt_len, labeled_ikm, pos);
    sodium_memzero(labeled_ikm, labeled_ikm_len);
    free(labeled_ikm);
    return rc;
}

static int
hpke_labeled_expand(uint8_t *out, size_t out_len,
                    const uint8_t prk[MLS_KDF_EXTRACT_LEN],
                    const uint8_t *suite_id, size_t suite_id_len,
                    const char *label,
                    const uint8_t *info, size_t info_len)
{
    if (out_len > 0xffff) return -1;

    size_t label_len = label ? strlen(label) : 0;
    size_t prefix_len = sizeof(MLS_HPKE_LABEL_PREFIX);
    if (info_len > SIZE_MAX - 2 - prefix_len - suite_id_len - label_len)
        return -1;
    size_t labeled_info_len = 2 + prefix_len + suite_id_len + label_len + info_len;

    uint8_t *labeled_info = malloc(labeled_info_len);
    if (!labeled_info) return -1;

    size_t pos = 0;
    labeled_info[pos++] = (uint8_t)(out_len >> 8);
    labeled_info[pos++] = (uint8_t)(out_len & 0xff);
    memcpy(labeled_info + pos, MLS_HPKE_LABEL_PREFIX, prefix_len);
    pos += prefix_len;
    memcpy(labeled_info + pos, suite_id, suite_id_len);
    pos += suite_id_len;
    if (label_len > 0) {
        memcpy(labeled_info + pos, label, label_len);
        pos += label_len;
    }
    if (info_len > 0) {
        memcpy(labeled_info + pos, info, info_len);
        pos += info_len;
    }

    int rc = mls_crypto_hkdf_expand(out, out_len, prk, labeled_info, pos);
    sodium_memzero(labeled_info, labeled_info_len);
    free(labeled_info);
    return rc;
}

static int
dhkem_extract_and_expand(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                         const uint8_t dh[MLS_KEM_SECRET_LEN],
                         const uint8_t *kem_context, size_t kem_context_len)
{
    uint8_t eae_prk[MLS_KDF_EXTRACT_LEN];
    if (hpke_labeled_extract(eae_prk,
                             MLS_HPKE_KEM_SUITE_ID, sizeof(MLS_HPKE_KEM_SUITE_ID),
                             NULL, 0, "eae_prk", dh, MLS_KEM_SECRET_LEN) != 0)
        return -1;

    int rc = hpke_labeled_expand(shared_secret, MLS_KEM_SECRET_LEN, eae_prk,
                                  MLS_HPKE_KEM_SUITE_ID, sizeof(MLS_HPKE_KEM_SUITE_ID),
                                  "shared_secret", kem_context, kem_context_len);
    sodium_memzero(eae_prk, sizeof(eae_prk));
    return rc;
}

static int
kem_encap_from_keypair(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                       uint8_t enc[MLS_KEM_ENC_LEN],
                       const uint8_t sk_e[MLS_KEM_SK_LEN],
                       const uint8_t pk_e[MLS_KEM_PK_LEN],
                       const uint8_t pk_r[MLS_KEM_PK_LEN])
{
    uint8_t dh[MLS_KEM_SECRET_LEN];
    if (mls_crypto_dh(dh, sk_e, pk_r) != 0)
        return -1;

    memcpy(enc, pk_e, MLS_KEM_ENC_LEN);

    uint8_t kem_context[MLS_KEM_ENC_LEN + MLS_KEM_PK_LEN];
    memcpy(kem_context, enc, MLS_KEM_ENC_LEN);
    memcpy(kem_context + MLS_KEM_ENC_LEN, pk_r, MLS_KEM_PK_LEN);

    int rc = dhkem_extract_and_expand(shared_secret, dh,
                                      kem_context, sizeof(kem_context));
    sodium_memzero(dh, sizeof(dh));
    return rc;
}

int
mls_crypto_dh(uint8_t out[MLS_KEM_SECRET_LEN],
              const uint8_t sk[MLS_KEM_SK_LEN],
              const uint8_t pk[MLS_KEM_PK_LEN])
{
    return crypto_scalarmult_curve25519(out, sk, pk) == 0 ? 0 : -1;
}

int
mls_crypto_kem_keygen(uint8_t sk[MLS_KEM_SK_LEN],
                       uint8_t pk[MLS_KEM_PK_LEN])
{
    randombytes_buf(sk, MLS_KEM_SK_LEN);
    return crypto_scalarmult_curve25519_base(pk, sk) == 0 ? 0 : -1;
}

int
mls_crypto_kem_derive_keypair(const uint8_t *ikm, size_t ikm_len,
                               uint8_t sk[MLS_KEM_SK_LEN],
                               uint8_t pk[MLS_KEM_PK_LEN])
{
    if (!ikm || !sk || !pk) return -1;

    uint8_t dkp_prk[MLS_KDF_EXTRACT_LEN];
    if (hpke_labeled_extract(dkp_prk,
                             MLS_HPKE_KEM_SUITE_ID, sizeof(MLS_HPKE_KEM_SUITE_ID),
                             NULL, 0, "dkp_prk", ikm, ikm_len) != 0)
        return -1;

    int rc = hpke_labeled_expand(sk, MLS_KEM_SK_LEN, dkp_prk,
                                  MLS_HPKE_KEM_SUITE_ID, sizeof(MLS_HPKE_KEM_SUITE_ID),
                                  "sk", NULL, 0);
    sodium_memzero(dkp_prk, sizeof(dkp_prk));
    if (rc != 0) return -1;

    if (crypto_scalarmult_curve25519_base(pk, sk) != 0) {
        sodium_memzero(sk, MLS_KEM_SK_LEN);
        return -1;
    }
    return 0;
}

int
mls_crypto_kem_encap(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                      uint8_t enc[MLS_KEM_ENC_LEN],
                      const uint8_t pk[MLS_KEM_PK_LEN])
{
    if (!shared_secret || !enc || !pk) return -1;

    uint8_t sk_e[MLS_KEM_SK_LEN];
    uint8_t pk_e[MLS_KEM_PK_LEN];
    if (mls_crypto_kem_keygen(sk_e, pk_e) != 0)
        return -1;

    int rc = kem_encap_from_keypair(shared_secret, enc, sk_e, pk_e, pk);
    sodium_memzero(sk_e, sizeof(sk_e));
    return rc;
}

int
mls_crypto_kem_encap_derand(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                             uint8_t enc[MLS_KEM_ENC_LEN],
                             const uint8_t pk[MLS_KEM_PK_LEN],
                             const uint8_t *ikm_e, size_t ikm_e_len)
{
    if (!shared_secret || !enc || !pk || !ikm_e) return -1;

    uint8_t sk_e[MLS_KEM_SK_LEN];
    uint8_t pk_e[MLS_KEM_PK_LEN];
    if (mls_crypto_kem_derive_keypair(ikm_e, ikm_e_len, sk_e, pk_e) != 0)
        return -1;

    int rc = kem_encap_from_keypair(shared_secret, enc, sk_e, pk_e, pk);
    sodium_memzero(sk_e, sizeof(sk_e));
    return rc;
}

int
mls_crypto_kem_decap(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                      const uint8_t enc[MLS_KEM_ENC_LEN],
                      const uint8_t sk[MLS_KEM_SK_LEN],
                      const uint8_t pk[MLS_KEM_PK_LEN])
{
    if (!shared_secret || !enc || !sk || !pk) return -1;

    uint8_t dh[MLS_KEM_SECRET_LEN];
    if (mls_crypto_dh(dh, sk, enc) != 0)
        return -1;

    uint8_t kem_context[MLS_KEM_ENC_LEN + MLS_KEM_PK_LEN];
    memcpy(kem_context, enc, MLS_KEM_ENC_LEN);
    memcpy(kem_context + MLS_KEM_ENC_LEN, pk, MLS_KEM_PK_LEN);

    int rc = dhkem_extract_and_expand(shared_secret, dh,
                                      kem_context, sizeof(kem_context));
    sodium_memzero(dh, sizeof(dh));
    return rc;
}

int
mls_crypto_hpke_key_schedule_base(MlsHpkeContext *ctx,
                                   const uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                                   const uint8_t *info, size_t info_len)
{
    if (!ctx || !shared_secret || (info_len > 0 && !info)) return -1;

    uint8_t psk_id_hash[MLS_HASH_LEN];
    uint8_t info_hash[MLS_HASH_LEN];
    uint8_t secret[MLS_KDF_EXTRACT_LEN];
    uint8_t key_schedule_context[1 + MLS_HASH_LEN + MLS_HASH_LEN];
    int rc = -1;

    if (hpke_labeled_extract(psk_id_hash,
                             MLS_HPKE_SUITE_ID, sizeof(MLS_HPKE_SUITE_ID),
                             NULL, 0, "psk_id_hash", NULL, 0) != 0)
        goto cleanup;
    if (hpke_labeled_extract(info_hash,
                             MLS_HPKE_SUITE_ID, sizeof(MLS_HPKE_SUITE_ID),
                             NULL, 0, "info_hash", info, info_len) != 0)
        goto cleanup;

    key_schedule_context[0] = MLS_HPKE_MODE_BASE;
    memcpy(key_schedule_context + 1, psk_id_hash, MLS_HASH_LEN);
    memcpy(key_schedule_context + 1 + MLS_HASH_LEN, info_hash, MLS_HASH_LEN);

    if (hpke_labeled_extract(secret,
                             MLS_HPKE_SUITE_ID, sizeof(MLS_HPKE_SUITE_ID),
                             shared_secret, MLS_KEM_SECRET_LEN,
                             "secret", NULL, 0) != 0)
        goto cleanup;

    if (hpke_labeled_expand(ctx->key, MLS_AEAD_KEY_LEN, secret,
                            MLS_HPKE_SUITE_ID, sizeof(MLS_HPKE_SUITE_ID),
                            "key", key_schedule_context,
                            sizeof(key_schedule_context)) != 0)
        goto cleanup;
    if (hpke_labeled_expand(ctx->base_nonce, MLS_AEAD_NONCE_LEN, secret,
                            MLS_HPKE_SUITE_ID, sizeof(MLS_HPKE_SUITE_ID),
                            "base_nonce", key_schedule_context,
                            sizeof(key_schedule_context)) != 0)
        goto cleanup;
    if (hpke_labeled_expand(ctx->exporter_secret, MLS_HASH_LEN, secret,
                            MLS_HPKE_SUITE_ID, sizeof(MLS_HPKE_SUITE_ID),
                            "exp", key_schedule_context,
                            sizeof(key_schedule_context)) != 0)
        goto cleanup;

    rc = 0;

cleanup:
    sodium_memzero(psk_id_hash, sizeof(psk_id_hash));
    sodium_memzero(info_hash, sizeof(info_hash));
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(key_schedule_context, sizeof(key_schedule_context));
    if (rc != 0)
        sodium_memzero(ctx, sizeof(*ctx));
    return rc;
}

int
mls_crypto_hpke_setup_base_s(uint8_t enc[MLS_KEM_ENC_LEN],
                              MlsHpkeContext *ctx,
                              const uint8_t pk_r[MLS_KEM_PK_LEN],
                              const uint8_t *info, size_t info_len)
{
    if (!enc || !ctx || !pk_r) return -1;

    uint8_t shared_secret[MLS_KEM_SECRET_LEN];
    if (mls_crypto_kem_encap(shared_secret, enc, pk_r) != 0)
        return -1;

    int rc = mls_crypto_hpke_key_schedule_base(ctx, shared_secret, info, info_len);
    sodium_memzero(shared_secret, sizeof(shared_secret));
    return rc;
}

int
mls_crypto_hpke_setup_base_r(MlsHpkeContext *ctx,
                              const uint8_t enc[MLS_KEM_ENC_LEN],
                              const uint8_t sk_r[MLS_KEM_SK_LEN],
                              const uint8_t pk_r[MLS_KEM_PK_LEN],
                              const uint8_t *info, size_t info_len)
{
    if (!ctx || !enc || !sk_r || !pk_r) return -1;

    uint8_t shared_secret[MLS_KEM_SECRET_LEN];
    if (mls_crypto_kem_decap(shared_secret, enc, sk_r, pk_r) != 0)
        return -1;

    int rc = mls_crypto_hpke_key_schedule_base(ctx, shared_secret, info, info_len);
    sodium_memzero(shared_secret, sizeof(shared_secret));
    return rc;
}

int
mls_crypto_hpke_seal_base(uint8_t enc[MLS_KEM_ENC_LEN],
                           uint8_t *ct, size_t *ct_len,
                           const uint8_t pk_r[MLS_KEM_PK_LEN],
                           const uint8_t *info, size_t info_len,
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *pt, size_t pt_len)
{
    if (!enc || !ct || !ct_len || !pk_r || (pt_len > 0 && !pt) ||
        (aad_len > 0 && !aad))
        return -1;

    MlsHpkeContext ctx;
    if (mls_crypto_hpke_setup_base_s(enc, &ctx, pk_r, info, info_len) != 0)
        return -1;

    int rc = mls_crypto_aead_encrypt(ct, ct_len, ctx.key, ctx.base_nonce,
                                      pt, pt_len, aad, aad_len);
    sodium_memzero(&ctx, sizeof(ctx));
    return rc;
}

int
mls_crypto_hpke_open_base(uint8_t *pt, size_t *pt_len,
                           const uint8_t enc[MLS_KEM_ENC_LEN],
                           const uint8_t sk_r[MLS_KEM_SK_LEN],
                           const uint8_t pk_r[MLS_KEM_PK_LEN],
                           const uint8_t *info, size_t info_len,
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *ct, size_t ct_len)
{
    if (!pt || !pt_len || !enc || !sk_r || !pk_r || !ct ||
        (aad_len > 0 && !aad))
        return -1;

    MlsHpkeContext ctx;
    if (mls_crypto_hpke_setup_base_r(&ctx, enc, sk_r, pk_r, info, info_len) != 0)
        return -1;

    int rc = mls_crypto_aead_decrypt(pt, pt_len, ctx.key, ctx.base_nonce,
                                      ct, ct_len, aad, aad_len);
    sodium_memzero(&ctx, sizeof(ctx));
    return rc;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Signing (Ed25519)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_crypto_sign_keygen(uint8_t sk[MLS_SIG_SK_LEN], uint8_t pk[MLS_SIG_PK_LEN])
{
    return crypto_sign_ed25519_keypair(pk, sk) == 0 ? 0 : -1;
}

int
mls_crypto_sign(uint8_t sig[MLS_SIG_LEN],
                const uint8_t sk[MLS_SIG_SK_LEN],
                const uint8_t *msg, size_t msg_len)
{
    return crypto_sign_ed25519_detached(sig, NULL, msg, msg_len, sk) == 0 ? 0 : -1;
}

int
mls_crypto_verify(const uint8_t sig[MLS_SIG_LEN],
                  const uint8_t pk[MLS_SIG_PK_LEN],
                  const uint8_t *msg, size_t msg_len)
{
    return crypto_sign_ed25519_verify_detached(sig, msg, msg_len, pk) == 0 ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Ref Hash (MLS §5.3.1)
 * ══════════════════════════════════════════════════════════════════════════ */

int
mls_crypto_ref_hash(uint8_t out[MLS_HASH_LEN],
                    const char *label,
                    const uint8_t *value, size_t value_len)
{
    /*
     * RefHash(label, value) = H(RefHashInput)
     * struct {
     *   opaque label<V>;
     *   opaque value<V>;
     * } RefHashInput;
     *
     * Uses mls_tls_write_vli for QUIC-style VLI length prefixes.
     */
    size_t label_len = label ? strlen(label) : 0;

    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, label_len + value_len + 8) != 0) return -1;

    if (mls_tls_write_opaque16(&buf, (const uint8_t *)label, label_len) != 0 ||
        mls_tls_write_opaque32(&buf, value, value_len) != 0) {
        mls_tls_buf_free(&buf);
        return -1;
    }

    int rc = mls_crypto_hash(out, buf.data, buf.len);
    mls_tls_buf_free(&buf);
    return rc;
}
