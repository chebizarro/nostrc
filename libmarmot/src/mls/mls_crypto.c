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
    /*
     * MLS §5.1 ExpandWithLabel:
     *
     * struct {
     *   uint16 length = out_len;
     *   opaque label<V> = "MLS 1.0 " + Label;
     *   opaque context<V> = Context;
     * } KDFLabel;
     *
     * ExpandWithLabel(Secret, Label, Context, Length) =
     *   KDF.Expand(Secret, KDFLabel, Length)
     */
    const char prefix[] = "MLS 1.0 ";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t label_len = label ? strlen(label) : 0;
    size_t full_label_len = prefix_len + label_len;

    /* Build KDFLabel serialization:
     * uint16 length
     * uint8  label_len_byte  (TLS opaque<0..255>)
     * opaque full_label[full_label_len]
     * uint8  context_len_byte (or uint16/uint32 for larger)
     * opaque context[ctx_len]
     *
     * For MLS, we use variable-length encoding per TLS presentation.
     */
    size_t info_size = 2 + 1 + full_label_len + 1 + ctx_len;
    uint8_t *info = malloc(info_size);
    if (!info) return -1;

    size_t pos = 0;
    /* uint16 length (big-endian) */
    info[pos++] = (uint8_t)(out_len >> 8);
    info[pos++] = (uint8_t)(out_len & 0xff);
    /* opaque label<0..255> */
    info[pos++] = (uint8_t)full_label_len;
    memcpy(info + pos, prefix, prefix_len);
    pos += prefix_len;
    if (label_len > 0) {
        memcpy(info + pos, label, label_len);
        pos += label_len;
    }
    /* opaque context<0..255> */
    info[pos++] = (uint8_t)ctx_len;
    if (ctx_len > 0) {
        memcpy(info + pos, context, ctx_len);
        pos += ctx_len;
    }

    int rc = mls_crypto_hkdf_expand(out, out_len, secret, info, pos);
    free(info);
    return rc;
}

int
mls_crypto_derive_secret(uint8_t out[MLS_HASH_LEN],
                          const uint8_t secret[MLS_HASH_LEN],
                          const char *label)
{
    return mls_crypto_expand_with_label(out, MLS_HASH_LEN,
                                         secret, label, NULL, 0);
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
mls_crypto_kem_encap(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                      uint8_t enc[MLS_KEM_ENC_LEN],
                      const uint8_t pk[MLS_KEM_PK_LEN])
{
    /*
     * DHKEM.Encap(pkR):
     *   (skE, pkE) = GenerateKeyPair()
     *   dh = DH(skE, pkR)
     *   enc = pkE
     *   kem_context = pkE || pkR
     *   shared_secret = ExtractAndExpand(dh, kem_context)
     */
    uint8_t sk_eph[MLS_KEM_SK_LEN];
    uint8_t pk_eph[MLS_KEM_PK_LEN];

    if (mls_crypto_kem_keygen(sk_eph, pk_eph) != 0)
        return -1;

    uint8_t dh[MLS_KEM_SECRET_LEN];
    if (mls_crypto_dh(dh, sk_eph, pk) != 0) {
        sodium_memzero(sk_eph, sizeof(sk_eph));
        return -1;
    }

    /* enc = pkE */
    memcpy(enc, pk_eph, MLS_KEM_PK_LEN);

    /* kem_context = pkE || pkR */
    uint8_t kem_ctx[MLS_KEM_PK_LEN * 2];
    memcpy(kem_ctx, pk_eph, MLS_KEM_PK_LEN);
    memcpy(kem_ctx + MLS_KEM_PK_LEN, pk, MLS_KEM_PK_LEN);

    /* ExtractAndExpand(dh, kem_context) */
    /* suite_id = "KEM" || I2OSP(0x0020, 2) for DHKEM(X25519) */
    const uint8_t suite_id[] = "KEM\x00\x20";
    size_t suite_id_len = 5;

    /* psk_id_hash = LabeledExtract("", "psk_id_hash", "") — empty for base mode */
    /* Extract: PRK = HKDF-Extract(dh as salt-ish, kem_context as IKM) */
    uint8_t prk[MLS_KDF_EXTRACT_LEN];
    if (mls_crypto_hkdf_extract(prk, dh, MLS_KEM_SECRET_LEN,
                                 kem_ctx, sizeof(kem_ctx)) != 0) {
        sodium_memzero(sk_eph, sizeof(sk_eph));
        sodium_memzero(dh, sizeof(dh));
        return -1;
    }

    /* Expand: shared_secret = HKDF-Expand(PRK, "shared_secret" || suite_id, 32) */
    /* For simplicity, concatenate label */
    const char *label = "shared_secret";
    size_t label_len = strlen(label);
    size_t info_len = label_len + suite_id_len;
    uint8_t info[64]; /* plenty of room */
    memcpy(info, label, label_len);
    memcpy(info + label_len, suite_id, suite_id_len);

    int rc = mls_crypto_hkdf_expand(shared_secret, MLS_KEM_SECRET_LEN,
                                     prk, info, info_len);

    sodium_memzero(sk_eph, sizeof(sk_eph));
    sodium_memzero(dh, sizeof(dh));
    sodium_memzero(prk, sizeof(prk));

    return rc;
}

int
mls_crypto_kem_decap(uint8_t shared_secret[MLS_KEM_SECRET_LEN],
                      const uint8_t enc[MLS_KEM_ENC_LEN],
                      const uint8_t sk[MLS_KEM_SK_LEN],
                      const uint8_t pk[MLS_KEM_PK_LEN])
{
    /* dh = DH(skR, enc) where enc = pkE */
    uint8_t dh[MLS_KEM_SECRET_LEN];
    if (mls_crypto_dh(dh, sk, enc) != 0)
        return -1;

    /* kem_context = enc || pkR */
    uint8_t kem_ctx[MLS_KEM_PK_LEN * 2];
    memcpy(kem_ctx, enc, MLS_KEM_ENC_LEN);
    memcpy(kem_ctx + MLS_KEM_ENC_LEN, pk, MLS_KEM_PK_LEN);

    uint8_t prk[MLS_KDF_EXTRACT_LEN];
    if (mls_crypto_hkdf_extract(prk, dh, MLS_KEM_SECRET_LEN,
                                 kem_ctx, sizeof(kem_ctx)) != 0) {
        sodium_memzero(dh, sizeof(dh));
        return -1;
    }

    const char *label = "shared_secret";
    const uint8_t suite_id[] = "KEM\x00\x20";
    size_t label_len = strlen(label);
    size_t suite_id_len = 5;
    uint8_t info[64];
    memcpy(info, label, label_len);
    memcpy(info + label_len, suite_id, suite_id_len);

    int rc = mls_crypto_hkdf_expand(shared_secret, MLS_KEM_SECRET_LEN,
                                     prk, info, label_len + suite_id_len);

    sodium_memzero(dh, sizeof(dh));
    sodium_memzero(prk, sizeof(prk));
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
     */
    size_t label_len = label ? strlen(label) : 0;
    /* TLS encoding: 1-byte length for label + label + 4-byte length for value + value */
    size_t buf_len = 1 + label_len + 4 + value_len;
    uint8_t *buf = malloc(buf_len);
    if (!buf) return -1;

    size_t pos = 0;
    buf[pos++] = (uint8_t)label_len;
    if (label_len > 0) {
        memcpy(buf + pos, label, label_len);
        pos += label_len;
    }
    buf[pos++] = (uint8_t)((value_len >> 24) & 0xff);
    buf[pos++] = (uint8_t)((value_len >> 16) & 0xff);
    buf[pos++] = (uint8_t)((value_len >> 8) & 0xff);
    buf[pos++] = (uint8_t)(value_len & 0xff);
    if (value_len > 0) {
        memcpy(buf + pos, value, value_len);
        pos += value_len;
    }

    int rc = mls_crypto_hash(out, buf, pos);
    free(buf);
    return rc;
}
