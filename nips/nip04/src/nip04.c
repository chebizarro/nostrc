#include "nostr/nip04.h"
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>

#include <nostr-utils.h> /* for nostr_hex2bin */
#include <secure_buf.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>

static void secure_bzero(void *p, size_t n) {
    if (!p || n == 0) return;
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

/* (moved AEAD KDF helpers below) */

/* Forward declaration for the standard NIP-04 raw-X ECDH callback. */
static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32, const unsigned char *y32, void *data);

/* === Minimal HKDF-SHA256 helpers (per NIP-04 key separation) === */
static int hmac_sha256_once(const unsigned char *key, size_t key_len,
                            const unsigned char *data, size_t data_len,
                            unsigned char out[32]) {
    unsigned int digest_len = 0;
    if (!key || !out || (data_len && !data) || key_len > INT_MAX) return -1;
    if (!HMAC(EVP_sha256(), key, (int)key_len, data, data_len,
              out, &digest_len) || digest_len != 32) {
        OPENSSL_cleanse(out, 32);
        return -1;
    }
    return 0;
}

static int hkdf_extract(const unsigned char *salt, size_t salt_len,
                        const unsigned char *ikm, size_t ikm_len,
                        unsigned char prk_out[32]) {
    unsigned char null_salt[32] = {0};
    const unsigned char *actual_salt = salt ? salt : null_salt;
    const size_t actual_salt_len = salt ? salt_len : sizeof(null_salt);
    if (!ikm || !prk_out) return -1;
    memset(prk_out, 0, 32);
    return hmac_sha256_once(actual_salt, actual_salt_len,
                            ikm, ikm_len, prk_out);
}

static int hkdf_expand(const unsigned char prk[32],
                       const unsigned char *info, size_t info_len,
                       unsigned char *okm_out, size_t okm_len) {
    unsigned char t[32] = {0};
    size_t pos = 0;
    size_t t_len = 0;
    int rc = -1;

    if (!prk || !okm_out || (info_len && !info) || okm_len == 0 ||
        okm_len > 255u * 32u) {
        return -1;
    }
    memset(okm_out, 0, okm_len);

    const size_t blocks = (okm_len + 31u) / 32u;
    for (size_t i = 1; i <= blocks; ++i) {
        const size_t msg_len = t_len + info_len + 1;
        unsigned char *msg = (unsigned char *)OPENSSL_malloc(msg_len);
        if (!msg) goto done;
        size_t off = 0;
        if (t_len) {
            memcpy(msg + off, t, t_len);
            off += t_len;
        }
        if (info_len) {
            memcpy(msg + off, info, info_len);
            off += info_len;
        }
        msg[off] = (unsigned char)i;
        if (hmac_sha256_once(prk, 32, msg, msg_len, t) != 0) {
            OPENSSL_clear_free(msg, msg_len);
            goto done;
        }
        OPENSSL_clear_free(msg, msg_len);

        const size_t take = okm_len - pos < sizeof(t)
                                ? okm_len - pos : sizeof(t);
        memcpy(okm_out + pos, t, take);
        pos += take;
        t_len = sizeof(t);
    }
    rc = 0;

done:
    OPENSSL_cleanse(t, sizeof(t));
    if (rc != 0) OPENSSL_cleanse(okm_out, okm_len);
    return rc;
}

/* Convert x-only pubkey (32 bytes) to compressed format (33 bytes with 02 prefix).
 * For ECDH, using 02 prefix (even y) works because the shared secret is the same
 * regardless of y parity - ECDH only uses the x coordinate of the result. */
static int xonly_to_compressed(const unsigned char x32[32], unsigned char out33[33]) {
    out33[0] = 0x02; /* even y parity */
    memcpy(out33 + 1, x32, 32);
    return 0;
}

/* Derive using binary secret key (32 bytes) from secure memory.
 * Supports x-only (64 hex / 32 bytes), compressed (66 hex / 33 bytes),
 * and uncompressed (130 hex / 65 bytes) public keys. */
static int ecdh_derive_key_bin(const char *peer_pub_hex, const unsigned char sk_bin[32], unsigned char key_out32[32]) {
    unsigned char pk_bin[65];
    size_t pk_bin_len;
    if (!peer_pub_hex || !sk_bin) return -1;
    size_t hexlen = strlen(peer_pub_hex);

    if (hexlen == 64) {
        /* x-only pubkey (Nostr format): convert to compressed */
        unsigned char x32[32];
        if (!nostr_hex2bin(x32, peer_pub_hex, 32)) return -1;
        xonly_to_compressed(x32, pk_bin);
        pk_bin_len = 33;
    } else if (hexlen == 66 || hexlen == 130) {
        /* compressed (33B) or uncompressed (65B) */
        pk_bin_len = hexlen / 2;
        if (!nostr_hex2bin(pk_bin, peer_pub_hex, pk_bin_len)) return -1;
    } else {
        return -1; /* invalid pubkey length */
    }

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return -1;
    if (!secp256k1_ec_seckey_verify(ctx, sk_bin)) { secp256k1_context_destroy(ctx); return -1; }
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) { secp256k1_context_destroy(ctx); return -1; }
    /* Standard NIP-04 uses the raw ECDH X coordinate as its AES key. */
    if (!secp256k1_ecdh(ctx, key_out32, &pub, sk_bin, ecdh_hash_xcopy, NULL)) { secp256k1_context_destroy(ctx); return -1; }
    secp256k1_context_destroy(ctx);
    return 0;
}

static bool base64_encode(const unsigned char *in, size_t in_len, char **out_str) {
    *out_str = NULL;
    bool ok = false;
    BIO *b64 = BIO_new(BIO_f_base64());
    if (!b64) return false;
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO *mem = BIO_new(BIO_s_mem());
    if (!mem) { BIO_free(b64); return false; }
    BIO *chain = BIO_push(b64, mem);
    if (BIO_write(chain, in, (int)in_len) != (int)in_len) goto out;
    if (BIO_flush(chain) != 1) goto out;
    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(chain, &bptr);
    if (!bptr || !bptr->data) goto out;
    *out_str = (char *)malloc(bptr->length + 1);
    if (!*out_str) goto out;
    memcpy(*out_str, bptr->data, bptr->length);
    (*out_str)[bptr->length] = '\0';
    ok = true;
out:
    BIO_free_all(chain);
    return ok;
}

static bool base64_decode(const char *in, unsigned char **out_buf, size_t *out_len) {
    *out_buf = NULL; *out_len = 0;
    bool ok = false;
    BIO *b64 = BIO_new(BIO_f_base64());
    if (!b64) return false;
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO *mem = BIO_new_mem_buf(in, -1);
    if (!mem) { BIO_free(b64); return false; }
    BIO *chain = BIO_push(b64, mem);
    size_t cap = strlen(in); /* upper bound */
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { BIO_free_all(chain); return false; }
    int n = BIO_read(chain, buf, (int)cap);
    if (n <= 0) { free(buf); BIO_free_all(chain); return false; }
    *out_buf = buf;
    *out_len = (size_t)n;
    ok = true;
    BIO_free_all(chain);
    return ok;
}

/* Hash callback that copies the ECDH X coordinate for standard NIP-04. */
static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32, const unsigned char *y32, void *data) {
    (void)y32; (void)data;
    memcpy(out, x32, 32);
    return 1;
}

/* === AEAD key derivation (HKDF with info="NIP04") === */
static int nip04_kdf_aead_from_x(const unsigned char x[32],
                                    unsigned char key32[32]) {
    static const unsigned char info[] = { 'N','I','P','0','4' };
    unsigned char prk[32] = {0};
    int rc = -1;

    if (!x || !key32) return -1;
    memset(key32, 0, 32);
    if (hkdf_extract(NULL, 0, x, 32, prk) != 0) goto done;
    if (hkdf_expand(prk, info, sizeof(info), key32, 32) != 0) goto done;
    rc = 0;

done:
    OPENSSL_cleanse(prk, sizeof(prk));
    if (rc != 0) OPENSSL_cleanse(key32, 32);
    return rc;
}

static int nip04_kdf_aead(const char *peer_pub_hex, const char *self_seckey_hex,
                          unsigned char key32[32]) {
    unsigned char sk_bin[32]; unsigned char pk_bin[65];
    size_t pk_bin_len;
    if (!peer_pub_hex || !self_seckey_hex) return -1;
    size_t seclen = strlen(self_seckey_hex);
    if (seclen != 64) return -1;
    if (!nostr_hex2bin(sk_bin, self_seckey_hex, sizeof(sk_bin))) return -1;
    size_t hexlen = strlen(peer_pub_hex);
    if (hexlen == 64) {
        /* x-only pubkey (Nostr format): convert to compressed */
        unsigned char x32[32];
        if (!nostr_hex2bin(x32, peer_pub_hex, 32)) { secure_bzero(sk_bin, sizeof sk_bin); return -1; }
        xonly_to_compressed(x32, pk_bin);
        pk_bin_len = 33;
    } else if (hexlen == 66 || hexlen == 130) {
        pk_bin_len = hexlen / 2;
        if (!nostr_hex2bin(pk_bin, peer_pub_hex, pk_bin_len)) { secure_bzero(sk_bin, sizeof sk_bin); return -1; }
    } else {
        secure_bzero(sk_bin, sizeof sk_bin); return -1;
    }
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) { secure_bzero(sk_bin, sizeof sk_bin); return -1; }
    if (!secp256k1_ec_seckey_verify(ctx, sk_bin)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof sk_bin); return -1; }
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof sk_bin); return -1; }
    unsigned char x[32];
    if (!secp256k1_ecdh(ctx, x, &pub, sk_bin, ecdh_hash_xcopy, NULL)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof sk_bin); return -1; }
    secp256k1_context_destroy(ctx);
    secure_bzero(sk_bin, sizeof sk_bin);
    int rc = nip04_kdf_aead_from_x(x, key32);
    secure_bzero((void*)x, sizeof x);
    return rc;
}

static int nip04_kdf_aead_bin(const char *peer_pub_hex,
                              const unsigned char sk_bin[32],
                              unsigned char key32[32]) {
    unsigned char pk_bin[65] = {0};
    unsigned char shared_x[32] = {0};
    size_t pk_bin_len = 0;
    secp256k1_context *ctx = NULL;
    int rc = -1;

    if (!peer_pub_hex || !sk_bin || !key32) return -1;
    memset(key32, 0, 32);

    const size_t hex_len = strlen(peer_pub_hex);
    if (hex_len == 64) {
        unsigned char x32[32];
        if (!nostr_hex2bin(x32, peer_pub_hex, sizeof(x32))) goto done;
        xonly_to_compressed(x32, pk_bin);
        OPENSSL_cleanse(x32, sizeof(x32));
        pk_bin_len = 33;
    } else if (hex_len == 66 || hex_len == 130) {
        pk_bin_len = hex_len / 2;
        if (!nostr_hex2bin(pk_bin, peer_pub_hex, pk_bin_len)) goto done;
    } else {
        goto done;
    }

    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx || !secp256k1_ec_seckey_verify(ctx, sk_bin)) goto done;
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) goto done;
    if (!secp256k1_ecdh(ctx, shared_x, &pub, sk_bin,
                        ecdh_hash_xcopy, NULL)) {
        goto done;
    }
    rc = nip04_kdf_aead_from_x(shared_x, key32);

done:
    secp256k1_context_destroy(ctx);
    OPENSSL_cleanse(shared_x, sizeof(shared_x));
    OPENSSL_cleanse(pk_bin, sizeof(pk_bin));
    if (rc != 0) OPENSSL_cleanse(key32, 32);
    return rc;
}

static int ecdh_derive_key(const char *peer_pub_hex, const char *self_sec_hex, unsigned char key_out32[32]) {
    unsigned char sk_bin[32];
    unsigned char pk_bin[65];
    size_t pk_bin_len;
    if (!peer_pub_hex || !self_sec_hex) return -1;
    size_t seclen = strlen(self_sec_hex);
    if (seclen != 64) return -1;
    if (!nostr_hex2bin(sk_bin, self_sec_hex, sizeof(sk_bin))) return -1;
    size_t hexlen = strlen(peer_pub_hex);
    if (hexlen == 64) {
        /* x-only pubkey (Nostr format): convert to compressed */
        unsigned char x32[32];
        if (!nostr_hex2bin(x32, peer_pub_hex, 32)) { secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }
        xonly_to_compressed(x32, pk_bin);
        pk_bin_len = 33;
    } else if (hexlen == 66 || hexlen == 130) {
        pk_bin_len = hexlen / 2;
        if (!nostr_hex2bin(pk_bin, peer_pub_hex, pk_bin_len)) { secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }
    } else {
        secure_bzero(sk_bin, sizeof(sk_bin)); return -1;
    }

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) { secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }
    if (!secp256k1_ec_seckey_verify(ctx, sk_bin)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }

    if (!secp256k1_ecdh(ctx, key_out32, &pub, sk_bin, ecdh_hash_xcopy, NULL)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }
    secp256k1_context_destroy(ctx);
    secure_bzero(sk_bin, sizeof(sk_bin));
    return 0;
}

int nostr_nip04_shared_secret_hex(const char *peer_pubkey_hex,
                                  const char *self_seckey_hex,
                                  char **out_shared_hex,
                                  char **out_error) {
    if (!out_shared_hex) return -1;
    *out_shared_hex = NULL;
    /* Compute raw X coordinate via secp256k1_ecdh with identity hash (copy x32) */
    unsigned char sk_bin[32];
    unsigned char pk_bin[65];
    size_t pk_bin_len;
    size_t seclen = strlen(self_seckey_hex);
    if (seclen != 64) { if (out_error) *out_error = strdup("bad seckey len"); return -1; }
    if (!nostr_hex2bin(sk_bin, self_seckey_hex, sizeof(sk_bin))) { if (out_error) *out_error = strdup("bad seckey hex"); return -1; }
    size_t hexlen = strlen(peer_pubkey_hex);
    if (hexlen == 64) {
        /* x-only pubkey (Nostr format): convert to compressed */
        unsigned char x32[32];
        if (!nostr_hex2bin(x32, peer_pubkey_hex, 32)) { secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("bad pubkey hex"); return -1; }
        xonly_to_compressed(x32, pk_bin);
        pk_bin_len = 33;
    } else if (hexlen == 66 || hexlen == 130) {
        pk_bin_len = hexlen / 2;
        if (!nostr_hex2bin(pk_bin, peer_pubkey_hex, pk_bin_len)) { secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("bad pubkey hex"); return -1; }
    } else {
        secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("bad pubkey len"); return -1;
    }
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) { secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("ctx"); return -1; }
    if (!secp256k1_ec_seckey_verify(ctx, sk_bin)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("bad seckey"); return -1; }
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("pub parse"); return -1; }
    unsigned char x[32];
    if (!secp256k1_ecdh(ctx, x, &pub, sk_bin, ecdh_hash_xcopy, NULL)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("ecdh"); return -1; }
    secp256k1_context_destroy(ctx);
    secure_bzero(sk_bin, sizeof(sk_bin));
    static const char *hexd = "0123456789abcdef";
    char *hexstr = (char *)malloc(65);
    if (!hexstr) { secure_bzero(x, sizeof(x)); if (out_error) *out_error = strdup("oom"); return -1; }
    for (int i = 0; i < 32; i++) { hexstr[2*i] = hexd[(x[i] >> 4) & 0xF]; hexstr[2*i+1] = hexd[x[i] & 0xF]; }
    hexstr[64] = '\0';
    secure_bzero(x, sizeof(x));
    *out_shared_hex = hexstr;
    return 0;
}

int nostr_nip04_encrypt(const char *plaintext_utf8,
                        const char *receiver_pubkey_hex,
                        const char *sender_seckey_hex,
                        char **out_content_b64_qiv,
                        char **out_error) {
    if (out_error) *out_error = NULL;
    if (!plaintext_utf8 || !receiver_pubkey_hex || !sender_seckey_hex || !out_content_b64_qiv)
        return -1;
    *out_content_b64_qiv = NULL;

    nostr_secure_buf sender_seckey = secure_alloc(32);
    if (!sender_seckey.ptr) {
        if (out_error) *out_error = strdup("oom");
        return -1;
    }
    if (!nostr_hex2bin((unsigned char *)sender_seckey.ptr, sender_seckey_hex, 32)) {
        secure_free(&sender_seckey);
        if (out_error) *out_error = strdup("invalid secret key");
        return -1;
    }

    /* Generic encryption always emits the authenticated AEAD extension.
     * Legacy CBC emission requires the explicit legacy API. */
    int rc = nostr_nip04_encrypt_secure(plaintext_utf8, receiver_pubkey_hex,
                                        &sender_seckey, out_content_b64_qiv,
                                        out_error);
    secure_free(&sender_seckey);
    return rc;
}

int nostr_nip04_decrypt(const char *content_b64_qiv,
                        const char *sender_pubkey_hex,
                        const char *receiver_seckey_hex,
                        char **out_plaintext_utf8,
                        char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_plaintext_utf8) *out_plaintext_utf8 = NULL;
    if (!content_b64_qiv || !sender_pubkey_hex || !receiver_seckey_hex ||
        !out_plaintext_utf8) {
        if (out_error) *out_error = strdup("decrypt failed");
        return -1;
    }

    /* New format: v=2:base64(nonce||cipher||tag). If not present, fall back to legacy unless STRICT is enabled. */
    if (strncmp(content_b64_qiv, "v=2:", 4) == 0) {
        const char *b64 = content_b64_qiv + 4;
        unsigned char *payload = NULL; size_t payload_len = 0;
        if (!base64_decode(b64, &payload, &payload_len)) { if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        if (payload_len < 12 + 16) { free(payload); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        const unsigned char *nonce = payload;
        const unsigned char *tag = payload + payload_len - 16;
        const unsigned char *ct = payload + 12;
        size_t ct_len = payload_len - 12 - 16;
        unsigned char key[32];
        if (nip04_kdf_aead(sender_pubkey_hex, receiver_seckey_hex, key) != 0) { OPENSSL_cleanse(payload, payload_len); free(payload); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        /* Note: nonce from payload, not derived */
        unsigned char *pt = (unsigned char*)malloc(ct_len + 1);
        if (!pt) { secure_bzero(key, sizeof key); OPENSSL_cleanse(payload, payload_len); free(payload); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int ok = 0; int len = 0, total = 0;
        if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
            EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
            EVP_DecryptUpdate(ctx, pt, &len, ct, (int)ct_len) == 1) {
            total = len;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) == 1 &&
                EVP_DecryptFinal_ex(ctx, pt + total, &len) == 1) {
                total += len; ok = 1;
            }
        }
        if (ctx) EVP_CIPHER_CTX_free(ctx);
        OPENSSL_cleanse(payload, payload_len); free(payload);
        secure_bzero(key, sizeof key);
        if (!ok) { OPENSSL_cleanse(pt, ct_len + 1); free(pt); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        pt[total] = '\0';
        *out_plaintext_utf8 = (char*)pt;
        return 0;
    }

#ifdef NIP04_STRICT_AEAD_ONLY
    /* Strict mode: legacy decrypt disabled */
    if (out_error) *out_error = strdup("decrypt failed");
    return -1;
#else
    /* Legacy fallback: AES-CBC ?iv= */
    const char *q = strstr(content_b64_qiv, "?iv=");
    if (!q) { if (out_error) *out_error = strdup("decrypt failed"); return -1; }
    size_t ct_len = (size_t)(q - content_b64_qiv);
    char *ct_b64 = (char *)malloc(ct_len + 1);
    if (!ct_b64) { if (out_error) *out_error = strdup("decrypt failed"); return -1; }
    memcpy(ct_b64, content_b64_qiv, ct_len); ct_b64[ct_len] = '\0';
    const char *iv_b64 = q + 4;

    unsigned char *ct = NULL; size_t ct_bin_len = 0;
    unsigned char *iv = NULL; size_t iv_len = 0;
    if (!base64_decode(ct_b64, &ct, &ct_bin_len)) { free(ct_b64); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
    free(ct_b64);
    if (!base64_decode(iv_b64, &iv, &iv_len)) { free(ct); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
    if (iv_len != 16) { free(ct); free(iv); if (out_error) *out_error = strdup("decrypt failed"); return -1; }

    unsigned char key[32];
    if (ecdh_derive_key(sender_pubkey_hex, receiver_seckey_hex, key) != 0) {
        free(ct); free(iv); if (out_error) *out_error = strdup("decrypt failed"); return -1; }

    unsigned char *pt = (unsigned char *)malloc(ct_bin_len + 1);
    if (!pt) { free(ct); free(iv); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
    int len = 0, total = 0; int ok = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
        EVP_DecryptUpdate(ctx, pt, &len, ct, (int)ct_bin_len) == 1) {
        total = len;
        if (EVP_DecryptFinal_ex(ctx, pt + total, &len) == 1) { total += len; ok = 1; }
    }
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    free(ct); free(iv); secure_bzero(key, sizeof key);
    if (!ok) { OPENSSL_cleanse(pt, ct_bin_len + 1); free(pt); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
    pt[total] = '\0';
    *out_plaintext_utf8 = (char *)pt;
    return 0;
#endif
}

int nostr_nip04_encrypt_secure(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const nostr_secure_buf *sender_seckey,
    char **out_content_b64_qiv,
    char **out_error)
{
    if (out_error) *out_error = NULL;
    if (!plaintext_utf8 || !receiver_pubkey_hex || !sender_seckey || !sender_seckey->ptr || sender_seckey->len < 32 || !out_content_b64_qiv)
        return -1;
    *out_content_b64_qiv = NULL;

    unsigned char key[32], nonce[12];
    if (nip04_kdf_aead_bin(receiver_pubkey_hex, (const unsigned char*)sender_seckey->ptr, key) != 0) {
        if (out_error) *out_error = strdup("ecdh failed");
        return -1;
    }
    /* The nonce is carried in the v2 envelope and must be unique for every
     * AES-GCM encryption under the long-lived ECDH-derived key. */
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        secure_bzero(key, sizeof key);
        if (out_error) *out_error = strdup("random nonce failed");
        return -1;
    }

    size_t in_len = strlen(plaintext_utf8);
    unsigned char *cipher = (unsigned char *)malloc(in_len + 16);
    if (!cipher) { secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("oom"); return -1; }

    int len = 0, total = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(cipher); secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("evp ctx"); return -1; }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) { EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("encrypt failed"); return -1; }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)sizeof(nonce), NULL) != 1) { EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("encrypt failed"); return -1; }
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) { EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("encrypt failed"); return -1; }
    if (EVP_EncryptUpdate(ctx, cipher, &len, (const unsigned char *)plaintext_utf8, (int)in_len) != 1) { EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("encrypt failed"); return -1; }
    total = len;
    if (EVP_EncryptFinal_ex(ctx, cipher + total, &len) != 1) { EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("encrypt failed"); return -1; }
    total += len;
    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) { EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof key); if (out_error) *out_error = strdup("encrypt failed"); return -1; }
    EVP_CIPHER_CTX_free(ctx);

    size_t payload_len = sizeof(nonce) + (size_t)total + sizeof(tag);
    unsigned char *payload = (unsigned char*)malloc(payload_len);
    if (!payload) { secure_bzero(key, sizeof key); OPENSSL_cleanse(tag, sizeof tag); free(cipher); if (out_error) *out_error = strdup("oom"); return -1; }
    size_t off = 0; memcpy(payload + off, nonce, sizeof(nonce)); off += sizeof(nonce);
    memcpy(payload + off, cipher, (size_t)total); off += (size_t)total;
    memcpy(payload + off, tag, sizeof(tag));
    char *b64 = NULL; int rc_b64 = base64_encode(payload, payload_len, &b64) ? 0 : -1;
    OPENSSL_cleanse(tag, sizeof tag); free(cipher); secure_bzero(key, sizeof key); OPENSSL_cleanse(payload, payload_len); free(payload);
    if (rc_b64 != 0) { if (out_error) *out_error = strdup("encrypt failed"); return -1; }
    size_t out_sz = 4 + strlen(b64) + 1;
    char *out = (char*)malloc(out_sz);
    if (!out) { free(b64); if (out_error) *out_error = strdup("oom"); return -1; }
    snprintf(out, out_sz, "v=2:%s", b64);
    free(b64);
    *out_content_b64_qiv = out;
    return 0;
}

/* Legacy NIP-04 encryption using AES-256-CBC with ?iv= format.
 * This is required for compatibility with NIP-46 signers like nsec.app
 * that expect the original NIP-04 format, not the AEAD v2 format. */
int nostr_nip04_encrypt_legacy_secure(
    const char *plaintext_utf8,
    const char *receiver_pubkey_hex,
    const nostr_secure_buf *sender_seckey,
    char **out_content_b64_qiv,
    char **out_error)
{
    if (out_error) *out_error = NULL;
    if (!plaintext_utf8 || !receiver_pubkey_hex || !sender_seckey || !sender_seckey->ptr || sender_seckey->len < 32 || !out_content_b64_qiv)
        return -1;
    *out_content_b64_qiv = NULL;

    /* Original NIP-04 uses the raw ECDH X coordinate as the AES key. */
    unsigned char pk_bin[65];
    size_t pk_bin_len;
    size_t hexlen = strlen(receiver_pubkey_hex);
    if (hexlen == 64) {
        unsigned char x32[32];
        if (!nostr_hex2bin(x32, receiver_pubkey_hex, 32)) {
            if (out_error) *out_error = strdup("invalid pubkey");
            return -1;
        }
        xonly_to_compressed(x32, pk_bin);
        pk_bin_len = 33;
    } else if (hexlen == 66 || hexlen == 130) {
        pk_bin_len = hexlen / 2;
        if (!nostr_hex2bin(pk_bin, receiver_pubkey_hex, pk_bin_len)) {
            if (out_error) *out_error = strdup("invalid pubkey");
            return -1;
        }
    } else {
        if (out_error) *out_error = strdup("invalid pubkey length");
        return -1;
    }

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) { if (out_error) *out_error = strdup("secp256k1 context failed"); return -1; }

    if (!secp256k1_ec_seckey_verify(ctx, (const unsigned char*)sender_seckey->ptr)) {
        secp256k1_context_destroy(ctx);
        if (out_error) *out_error = strdup("invalid secret key");
        return -1;
    }

    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) {
        secp256k1_context_destroy(ctx);
        if (out_error) *out_error = strdup("invalid pubkey parse");
        return -1;
    }

    /* ECDH raw X coordinate is the AES key for standard NIP-04. */
    unsigned char key[32];
    if (!secp256k1_ecdh(ctx, key, &pub, (const unsigned char*)sender_seckey->ptr, ecdh_hash_xcopy, NULL)) {
        secp256k1_context_destroy(ctx);
        if (out_error) *out_error = strdup("ecdh failed");
        return -1;
    }
    secp256k1_context_destroy(ctx);

    /* Generate random 16-byte IV */
    unsigned char iv[16];
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        secure_bzero(key, sizeof key);
        if (out_error) *out_error = strdup("random iv failed");
        return -1;
    }

    /* AES-256-CBC encryption with PKCS#7 padding */
    size_t in_len = strlen(plaintext_utf8);
    size_t max_cipher_len = in_len + 16; /* padding can add up to 16 bytes */
    unsigned char *cipher = (unsigned char *)malloc(max_cipher_len);
    if (!cipher) {
        secure_bzero(key, sizeof key);
        if (out_error) *out_error = strdup("oom");
        return -1;
    }

    EVP_CIPHER_CTX *evp_ctx = EVP_CIPHER_CTX_new();
    if (!evp_ctx) {
        free(cipher);
        secure_bzero(key, sizeof key);
        if (out_error) *out_error = strdup("evp ctx failed");
        return -1;
    }

    int len = 0, total = 0;
    if (EVP_EncryptInit_ex(evp_ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1 ||
        EVP_EncryptUpdate(evp_ctx, cipher, &len, (const unsigned char *)plaintext_utf8, (int)in_len) != 1) {
        EVP_CIPHER_CTX_free(evp_ctx);
        free(cipher);
        secure_bzero(key, sizeof key);
        if (out_error) *out_error = strdup("encrypt failed");
        return -1;
    }
    total = len;
    if (EVP_EncryptFinal_ex(evp_ctx, cipher + total, &len) != 1) {
        EVP_CIPHER_CTX_free(evp_ctx);
        free(cipher);
        secure_bzero(key, sizeof key);
        if (out_error) *out_error = strdup("encrypt final failed");
        return -1;
    }
    total += len;
    EVP_CIPHER_CTX_free(evp_ctx);
    secure_bzero(key, sizeof key);

    /* Base64 encode ciphertext and IV */
    char *ct_b64 = NULL;
    char *iv_b64 = NULL;
    if (!base64_encode(cipher, (size_t)total, &ct_b64) ||
        !base64_encode(iv, sizeof(iv), &iv_b64)) {
        free(cipher);
        free(ct_b64);
        free(iv_b64);
        if (out_error) *out_error = strdup("base64 encode failed");
        return -1;
    }
    free(cipher);

    /* Format: base64(ciphertext)?iv=base64(iv) */
    size_t out_len = strlen(ct_b64) + 4 + strlen(iv_b64) + 1; /* "?iv=" = 4 */
    char *out = (char *)malloc(out_len);
    if (!out) {
        free(ct_b64);
        free(iv_b64);
        if (out_error) *out_error = strdup("oom");
        return -1;
    }
    snprintf(out, out_len, "%s?iv=%s", ct_b64, iv_b64);
    free(ct_b64);
    free(iv_b64);

    *out_content_b64_qiv = out;
    return 0;
}

int nostr_nip04_decrypt_secure(
    const char *content_b64_qiv,
    const char *sender_pubkey_hex,
    const nostr_secure_buf *receiver_seckey,
    char **out_plaintext_utf8,
    char **out_error)
{
    if (out_error) *out_error = NULL;
    if (out_plaintext_utf8) *out_plaintext_utf8 = NULL;
    if (!content_b64_qiv || !sender_pubkey_hex || !receiver_seckey ||
        !receiver_seckey->ptr || receiver_seckey->len < 32 ||
        !out_plaintext_utf8) {
        if (out_error) *out_error = strdup("decrypt failed");
        return -1;
    }

    /* AEAD v2 path */
    if (strncmp(content_b64_qiv, "v=2:", 4) == 0) {
        const char *b64 = content_b64_qiv + 4;
        unsigned char *payload = NULL; size_t payload_len = 0;
        if (!base64_decode(b64, &payload, &payload_len)) { if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        if (payload_len < 12 + 16) { OPENSSL_cleanse(payload, payload_len); free(payload); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        const unsigned char *nonce = payload;
        const unsigned char *tag = payload + payload_len - 16;
        const unsigned char *ct = payload + 12; size_t ct_len = payload_len - 12 - 16;
        unsigned char key[32];
        if (nip04_kdf_aead_bin(sender_pubkey_hex, (const unsigned char*)receiver_seckey->ptr, key) != 0) { OPENSSL_cleanse(payload, payload_len); free(payload); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        unsigned char *pt = (unsigned char*)malloc(ct_len + 1);
        if (!pt) { secure_bzero(key, sizeof key); OPENSSL_cleanse(payload, payload_len); free(payload); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int ok = 0, len = 0, total = 0;
        if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
            EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
            EVP_DecryptUpdate(ctx, pt, &len, ct, (int)ct_len) == 1) {
            total = len;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) == 1 &&
                EVP_DecryptFinal_ex(ctx, pt + total, &len) == 1) { total += len; ok = 1; }
        }
        if (ctx) EVP_CIPHER_CTX_free(ctx);
        OPENSSL_cleanse(payload, payload_len); free(payload); secure_bzero(key, sizeof key);
        if (!ok) { OPENSSL_cleanse(pt, ct_len + 1); free(pt); if (out_error) *out_error = strdup("decrypt failed"); return -1; }
        pt[total] = '\0'; *out_plaintext_utf8 = (char*)pt; return 0;
    }

#ifdef NIP04_STRICT_AEAD_ONLY
    /* Strict mode: legacy decrypt disabled */
    if (out_error) *out_error = strdup("decrypt failed"); return -1;
#else
    const char *q = strstr(content_b64_qiv, "?iv=");
    if (!q) {
        if (out_error) *out_error = strdup("decrypt failed");
        return -1;
    }

    const size_t ct_b64_len = (size_t)(q - content_b64_qiv);
    char *ct_b64 = (char *)malloc(ct_b64_len + 1);
    unsigned char *ct = NULL;
    unsigned char *iv = NULL;
    unsigned char *pt = NULL;
    size_t ct_len = 0;
    size_t iv_len = 0;
    unsigned char key[32] = {0};
    EVP_CIPHER_CTX *ctx = NULL;
    int ok = 0;
    int len = 0;
    int total = 0;

    if (!ct_b64) goto legacy_done;
    memcpy(ct_b64, content_b64_qiv, ct_b64_len);
    ct_b64[ct_b64_len] = '\0';
    if (!base64_decode(ct_b64, &ct, &ct_len) ||
        !base64_decode(q + 4, &iv, &iv_len) ||
        iv_len != 16 ||
        ecdh_derive_key_bin(sender_pubkey_hex,
                            (const unsigned char *)receiver_seckey->ptr,
                            key) != 0) {
        goto legacy_done;
    }

    pt = (unsigned char *)malloc(ct_len + 1);
    ctx = EVP_CIPHER_CTX_new();
    if (!pt || !ctx) goto legacy_done;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) == 1 &&
        EVP_DecryptUpdate(ctx, pt, &len, ct, (int)ct_len) == 1) {
        total = len;
        if (EVP_DecryptFinal_ex(ctx, pt + total, &len) == 1) {
            total += len;
            ok = 1;
        }
    }

legacy_done:
    EVP_CIPHER_CTX_free(ctx);
    free(ct_b64);
    free(ct);
    free(iv);
    secure_bzero(key, sizeof(key));
    if (!ok) {
        if (pt) {
            OPENSSL_cleanse(pt, ct_len + 1);
            free(pt);
        }
        if (out_error) *out_error = strdup("decrypt failed");
        return -1;
    }

    pt[total] = '\0';
    *out_plaintext_utf8 = (char *)pt;
    return 0;
#endif
}
