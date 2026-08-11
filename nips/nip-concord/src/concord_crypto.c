/* nip-concord: primitives — SHA-256, HKDF-SHA256, secp256k1 helpers. */

#include "concord_internal.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

int concord_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    if (!out || (!data && len)) return -1;
    unsigned int out_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    int rc = -1;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, len) == 1 &&
        EVP_DigestFinal_ex(ctx, out, &out_len) == 1 && out_len == 32) {
        rc = 0;
    }
    EVP_MD_CTX_free(ctx);
    if (rc != 0) OPENSSL_cleanse(out, 32);
    return rc;
}

int concord_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                        const uint8_t *info, size_t info_len,
                        uint8_t out[32]) {
    if (!ikm || !ikm_len || !out || (!info && info_len)) return -1;

    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    if (!kdf) return -1;
    EVP_KDF_CTX *ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx) return -1;

    /* CORD-02 A.1: salt is zero-length. RFC 5869 pads an absent salt to
     * HashLen zeros, and HMAC pads a short key the same way, so a zero-length
     * salt and a 32-zero-byte salt are the same key — no ambiguity here. */
    static const unsigned char salt[1] = { 0 };
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                          (void *)(uintptr_t)ikm, ikm_len),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                          (void *)(uintptr_t)salt, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                          (void *)(uintptr_t)info, info_len),
        OSSL_PARAM_construct_end()
    };

    int rc = (EVP_KDF_derive(ctx, out, 32, params) == 1) ? 0 : -1;
    EVP_KDF_CTX_free(ctx);
    if (rc != 0) OPENSSL_cleanse(out, 32);
    return rc;
}

bool concord_seckey_verify(const uint8_t sk[32]) {
    if (!sk) return false;
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;
    bool ok = secp256k1_ec_seckey_verify(ctx, sk) == 1;
    secp256k1_context_destroy(ctx);
    return ok;
}

int concord_xonly_pubkey(const uint8_t sk[32], uint8_t out_pk[32]) {
    if (!sk || !out_pk) return -1;
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return -1;
    int rc = -1;
    secp256k1_keypair keypair;
    secp256k1_xonly_pubkey xonly;
    if (secp256k1_keypair_create(ctx, &keypair, sk) == 1 &&
        secp256k1_keypair_xonly_pub(ctx, &xonly, NULL, &keypair) == 1 &&
        secp256k1_xonly_pubkey_serialize(ctx, out_pk, &xonly) == 1) {
        rc = 0;
    }
    OPENSSL_cleanse(&keypair, sizeof(keypair));
    secp256k1_context_destroy(ctx);
    if (rc != 0) OPENSSL_cleanse(out_pk, 32);
    return rc;
}

size_t concord_build_info(const char *label, const uint8_t id[32],
                          bool has_epoch, uint64_t epoch, int counter,
                          uint8_t *out, size_t out_cap) {
    if (!label || !id || !out) return 0;
    size_t label_len = strlen(label);
    /* label || 0x00 || id[32] || epoch_be[8]? || counter? */
    size_t need = label_len + 1 + 32 + (has_epoch ? 8 : 0) +
                  (counter >= 0 ? 1 : 0);
    if (need > out_cap) return 0;

    size_t off = 0;
    memcpy(out + off, label, label_len);
    off += label_len;
    out[off++] = 0x00;
    memcpy(out + off, id, 32);
    off += 32;
    if (has_epoch) {
        for (int i = 7; i >= 0; i--) out[off++] = (uint8_t)(epoch >> (i * 8));
    }
    if (counter >= 0) out[off++] = (uint8_t)counter;
    return off;
}

bool concord_memeq_32(const uint8_t a[32], const uint8_t b[32]) {
    if (!a || !b) return false;
    return CRYPTO_memcmp(a, b, 32) == 0;
}

char *concord_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}
