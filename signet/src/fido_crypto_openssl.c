/* SPDX-License-Identifier: MIT */
/* signet FIDO crypto — OpenSSL 3.x software backend (Phase 0 spike). */

#include "signet/fido_crypto.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <openssl/err.h>

struct signet_fido_key {
    EVP_PKEY *pkey;
};

signet_fido_key *signet_fido_key_generate(void)
{
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!ctx) return NULL;

    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(ctx);

    signet_fido_key *k = calloc(1, sizeof(*k));
    if (!k) { EVP_PKEY_free(pkey); return NULL; }
    k->pkey = pkey;
    return k;
}

int signet_fido_key_public_xy(const signet_fido_key *k, uint8_t x[32], uint8_t y[32])
{
    if (!k || !k->pkey) return -1;

    /* Fetch the uncompressed public point: 0x04 || X(32) || Y(32). */
    unsigned char pub[65];
    size_t publen = 0;
    if (EVP_PKEY_get_octet_string_param(k->pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                        pub, sizeof(pub), &publen) <= 0)
        return -1;
    if (publen != 65 || pub[0] != 0x04) return -1;

    memcpy(x, pub + 1, 32);
    memcpy(y, pub + 33, 32);
    return 0;
}

int signet_fido_key_export_private(const signet_fido_key *k, uint8_t **out, size_t *out_len)
{
    if (!k || !k->pkey || !out || !out_len) return -1;

    int len = i2d_PrivateKey(k->pkey, NULL);
    if (len <= 0) return -1;

    uint8_t *buf = malloc((size_t)len);
    if (!buf) return -1;
    uint8_t *p = buf;
    if (i2d_PrivateKey(k->pkey, &p) != len) { free(buf); return -1; }

    *out = buf;
    *out_len = (size_t)len;
    return 0;
}

signet_fido_key *signet_fido_key_import_private(const uint8_t *der, size_t der_len)
{
    if (!der || der_len == 0) return NULL;

    const uint8_t *p = der;
    EVP_PKEY *pkey = d2i_PrivateKey(EVP_PKEY_EC, NULL, &p, (long)der_len);
    if (!pkey) return NULL;

    signet_fido_key *k = calloc(1, sizeof(*k));
    if (!k) { EVP_PKEY_free(pkey); return NULL; }
    k->pkey = pkey;
    return k;
}

int signet_fido_key_sign(const signet_fido_key *k, const uint8_t *msg, size_t msg_len,
                         uint8_t **sig, size_t *sig_len)
{
    if (!k || !k->pkey || !sig || !sig_len) return -1;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md) return -1;

    int rc = -1;
    size_t n = 0;
    if (EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, k->pkey) <= 0) goto out;
    if (EVP_DigestSign(md, NULL, &n, msg, msg_len) <= 0) goto out;

    uint8_t *buf = malloc(n);
    if (!buf) goto out;
    if (EVP_DigestSign(md, buf, &n, msg, msg_len) <= 0) { free(buf); goto out; }

    *sig = buf;
    *sig_len = n;
    rc = 0;
out:
    EVP_MD_CTX_free(md);
    return rc;
}

int signet_fido_verify_p256(const uint8_t x[32], const uint8_t y[32],
                            const uint8_t *msg, size_t msg_len,
                            const uint8_t *sig, size_t sig_len)
{
    int rc = -1;
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    OSSL_PARAM_BLD *bld = NULL;
    OSSL_PARAM *params = NULL;
    EVP_MD_CTX *md = NULL;

    /* Reassemble the uncompressed point and build an EC public key. */
    unsigned char pub[65];
    pub[0] = 0x04;
    memcpy(pub + 1, x, 32);
    memcpy(pub + 33, y, 32);

    bld = OSSL_PARAM_BLD_new();
    if (!bld) goto out;
    if (!OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) ||
        !OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub, sizeof(pub)))
        goto out;
    params = OSSL_PARAM_BLD_to_param(bld);
    if (!params) goto out;

    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!ctx) goto out;
    if (EVP_PKEY_fromdata_init(ctx) <= 0 ||
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
        goto out;

    md = EVP_MD_CTX_new();
    if (!md) goto out;
    if (EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pkey) <= 0) goto out;

    rc = EVP_DigestVerify(md, sig, sig_len, msg, msg_len);  /* 1 ok, 0 bad, <0 err */
out:
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    return rc;
}

void signet_fido_key_free(signet_fido_key *k)
{
    if (!k) return;
    EVP_PKEY_free(k->pkey);
    free(k);
}
