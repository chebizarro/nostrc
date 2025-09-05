#include "nostr/nip04.h"
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <nostr-utils.h> /* for nostr_hex2bin */
#include <secure_buf.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>

static void secure_bzero(void *p, size_t n) {
    if (!p || n == 0) return;
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

/* forward declaration for ECDH hash callback defined below */
static int ecdh_hash_sha256(unsigned char *out, const unsigned char *x32, const unsigned char *y32, void *data);

/* Derive using binary secret key (32 bytes) from secure memory */
static int ecdh_derive_key_bin(const char *peer_pub_hex, const unsigned char sk_bin[32], unsigned char key_out32[32]) {
    unsigned char pk_bin[65];
    if (!peer_pub_hex || !sk_bin) return -1;
    size_t hexlen = strlen(peer_pub_hex);
    if (hexlen != 66 && hexlen != 130) return -1; /* compressed 33B or uncompressed 65B */
    size_t pk_bin_len = hexlen / 2;
    if (!nostr_hex2bin(pk_bin, peer_pub_hex, pk_bin_len)) return -1;

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return -1;
    if (!secp256k1_ec_seckey_verify(ctx, sk_bin)) { secp256k1_context_destroy(ctx); return -1; }
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) { secp256k1_context_destroy(ctx); return -1; }
    if (!secp256k1_ecdh(ctx, key_out32, &pub, sk_bin, ecdh_hash_sha256, NULL)) { secp256k1_context_destroy(ctx); return -1; }
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

static int evp_sha256(const unsigned char *in, size_t in_len, unsigned char out32[32]) {
    unsigned int mdlen = 0;
    if (EVP_Digest(in, in_len, out32, &mdlen, EVP_sha256(), NULL) != 1) return 0;
    return mdlen == 32 ? 1 : 0;
}

/* Hash callback that computes SHA-256 over the 32-byte X coordinate (for secp256k1_ecdh) */
static int ecdh_hash_sha256(unsigned char *out, const unsigned char *x32, const unsigned char *y32, void *data) {
    (void)y32; (void)data;
    return evp_sha256(x32, 32, out);
}

/* Hash callback that copies X coordinate as-is (for diagnostics helper) */
static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32, const unsigned char *y32, void *data) {
    (void)y32; (void)data;
    memcpy(out, x32, 32);
    return 1;
}

static int ecdh_derive_key(const char *peer_pub_hex, const char *self_sec_hex, unsigned char key_out32[32]) {
    unsigned char sk_bin[32];
    unsigned char pk_bin[65];
    if (!peer_pub_hex || !self_sec_hex) return -1;
    size_t seclen = strlen(self_sec_hex);
    if (seclen != 64) return -1;
    if (!nostr_hex2bin(sk_bin, self_sec_hex, sizeof(sk_bin))) return -1;
    size_t hexlen = strlen(peer_pub_hex);
    if (hexlen != 66 && hexlen != 130) return -1; /* compressed 33B or uncompressed 65B */
    size_t pk_bin_len = hexlen / 2;
    if (!nostr_hex2bin(pk_bin, peer_pub_hex, pk_bin_len)) { secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return -1;
    if (!secp256k1_ec_seckey_verify(ctx, sk_bin)) { secp256k1_context_destroy(ctx); secure_bzero(sk_bin, sizeof(sk_bin)); return -1; }
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, pk_bin, pk_bin_len)) { secp256k1_context_destroy(ctx); return -1; }

    if (!secp256k1_ecdh(ctx, key_out32, &pub, sk_bin, ecdh_hash_sha256, NULL)) { secp256k1_context_destroy(ctx); return -1; }
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
    size_t seclen = strlen(self_seckey_hex);
    if (seclen != 64) { if (out_error) *out_error = strdup("bad seckey len"); return -1; }
    if (!nostr_hex2bin(sk_bin, self_seckey_hex, sizeof(sk_bin))) { if (out_error) *out_error = strdup("bad seckey hex"); return -1; }
    size_t hexlen = strlen(peer_pubkey_hex);
    if (hexlen != 66 && hexlen != 130) { secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("bad pubkey len"); return -1; }
    size_t pk_bin_len = hexlen / 2;
    if (!nostr_hex2bin(pk_bin, peer_pubkey_hex, pk_bin_len)) { secure_bzero(sk_bin, sizeof(sk_bin)); if (out_error) *out_error = strdup("bad pubkey hex"); return -1; }
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

    unsigned char key[32];
    if (ecdh_derive_key(receiver_pubkey_hex, sender_seckey_hex, key) != 0) {
        if (out_error) *out_error = strdup("ecdh failed");
        return -1;
    }

    unsigned char iv[16];
    const char *iv_b64_env = getenv("NIP04_TEST_IV_B64");
    if (iv_b64_env && *iv_b64_env) {
        unsigned char *iv_tmp = NULL; size_t iv_len = 0;
        if (!base64_decode(iv_b64_env, &iv_tmp, &iv_len) || iv_len != sizeof(iv)) {
            if (out_error) *out_error = strdup("bad test iv");
            free(iv_tmp); secure_bzero(key, sizeof(key)); return -1;
        }
        memcpy(iv, iv_tmp, sizeof(iv));
        free(iv_tmp);
    } else if (RAND_bytes(iv, sizeof(iv)) != 1) {
        secure_bzero(key, sizeof(key));
        if (out_error) *out_error = strdup("iv rand failed");
        return -1;
    }

    size_t in_len = strlen(plaintext_utf8);
    int out_len1 = (int)in_len + AES_BLOCK_SIZE; /* worst case */
    unsigned char *cipher = (unsigned char *)malloc(out_len1);
    if (!cipher) { secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("oom"); return -1; }

    int len = 0, total = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("evp ctx"); return -1; }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("enc init"); return -1; }
    if (EVP_EncryptUpdate(ctx, cipher, &len, (const unsigned char *)plaintext_utf8, (int)in_len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("enc update"); return -1; }
    total = len;
    if (EVP_EncryptFinal_ex(ctx, cipher + total, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("enc final"); return -1; }
    total += len;
    EVP_CIPHER_CTX_free(ctx);

    char *b64_ct = NULL; char *b64_iv = NULL;
    if (!base64_encode(cipher, (size_t)total, &b64_ct)) {
        free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("b64 ct"); return -1; }
    if (!base64_encode(iv, sizeof(iv), &b64_iv)) {
        free(cipher); secure_bzero(key, sizeof(key)); free(b64_ct); if (out_error) *out_error = strdup("b64 iv"); return -1; }
    free(cipher);
    secure_bzero(key, sizeof(key));

    size_t out_sz = strlen(b64_ct) + strlen(b64_iv) + 4 + 1; /* ?iv= */
    char *out = (char *)malloc(out_sz);
    if (!out) { free(b64_ct); free(b64_iv); if (out_error) *out_error = strdup("oom"); return -1; }
    snprintf(out, out_sz, "%s?iv=%s", b64_ct, b64_iv);
    free(b64_ct); free(b64_iv);
    *out_content_b64_qiv = out;
    return 0;
}

int nostr_nip04_decrypt(const char *content_b64_qiv,
                        const char *sender_pubkey_hex,
                        const char *receiver_seckey_hex,
                        char **out_plaintext_utf8,
                        char **out_error) {
    if (out_error) *out_error = NULL;
    if (!content_b64_qiv || !sender_pubkey_hex || !receiver_seckey_hex || !out_plaintext_utf8) return -1;
    *out_plaintext_utf8 = NULL;

    const char *q = strstr(content_b64_qiv, "?iv=");
    if (!q) { if (out_error) *out_error = strdup("missing iv"); return -1; }
    size_t ct_len = (size_t)(q - content_b64_qiv);
    char *ct_b64 = (char *)malloc(ct_len + 1);
    if (!ct_b64) { if (out_error) *out_error = strdup("oom"); return -1; }
    memcpy(ct_b64, content_b64_qiv, ct_len); ct_b64[ct_len] = '\0';
    const char *iv_b64 = q + 4;

    unsigned char *ct = NULL; size_t ct_bin_len = 0;
    unsigned char *iv = NULL; size_t iv_len = 0;
    if (!base64_decode(ct_b64, &ct, &ct_bin_len)) { free(ct_b64); if (out_error) *out_error = strdup("b64 ct"); return -1; }
    free(ct_b64);
    if (!base64_decode(iv_b64, &iv, &iv_len)) { free(ct); if (out_error) *out_error = strdup("b64 iv"); return -1; }
    if (iv_len != 16) { free(ct); free(iv); if (out_error) *out_error = strdup("iv len"); return -1; }

    unsigned char key[32];
    if (ecdh_derive_key(sender_pubkey_hex, receiver_seckey_hex, key) != 0) {
        free(ct); free(iv); if (out_error) *out_error = strdup("ecdh failed"); return -1; }

    unsigned char *pt = (unsigned char *)malloc(ct_bin_len + 1);
    if (!pt) { free(ct); free(iv); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("oom"); return -1; }
    int len = 0, total = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("evp ctx"); return -1; }
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("dec init"); return -1; }
    if (EVP_DecryptUpdate(ctx, pt, &len, ct, (int)ct_bin_len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("dec update"); return -1; }
    total = len;
    if (EVP_DecryptFinal_ex(ctx, pt + total, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("dec final"); return -1; }
    total += len;
    EVP_CIPHER_CTX_free(ctx);

    pt[total] = '\0';
    *out_plaintext_utf8 = (char *)pt;
    free(ct); free(iv);
    secure_bzero(key, sizeof(key));
    return 0;
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

    unsigned char key[32];
    if (ecdh_derive_key_bin(receiver_pubkey_hex, (const unsigned char*)sender_seckey->ptr, key) != 0) {
        if (out_error) *out_error = strdup("ecdh failed");
        return -1;
    }

    unsigned char iv[16];
    const char *iv_b64_env = getenv("NIP04_TEST_IV_B64");
    if (iv_b64_env && *iv_b64_env) {
        unsigned char *iv_tmp = NULL; size_t iv_len = 0;
        if (!base64_decode(iv_b64_env, &iv_tmp, &iv_len) || iv_len != sizeof(iv)) {
            if (out_error) *out_error = strdup("bad test iv");
            free(iv_tmp); secure_bzero(key, sizeof(key)); return -1;
        }
        memcpy(iv, iv_tmp, sizeof(iv));
        free(iv_tmp);
    } else if (RAND_bytes(iv, sizeof(iv)) != 1) {
        secure_bzero(key, sizeof(key));
        if (out_error) *out_error = strdup("iv rand failed");
        return -1;
    }

    size_t in_len = strlen(plaintext_utf8);
    int out_len1 = (int)in_len + AES_BLOCK_SIZE; /* worst case */
    unsigned char *cipher = (unsigned char *)malloc(out_len1);
    if (!cipher) { secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("oom"); return -1; }

    int len = 0, total = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("evp ctx"); return -1; }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("enc init"); return -1; }
    if (EVP_EncryptUpdate(ctx, cipher, &len, (const unsigned char *)plaintext_utf8, (int)in_len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("enc update"); return -1; }
    total = len;
    if (EVP_EncryptFinal_ex(ctx, cipher + total, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("enc final"); return -1; }
    total += len;
    EVP_CIPHER_CTX_free(ctx);

    char *b64_ct = NULL; char *b64_iv = NULL;
    if (!base64_encode(cipher, (size_t)total, &b64_ct)) {
        free(cipher); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("b64 ct"); return -1; }
    if (!base64_encode(iv, sizeof(iv), &b64_iv)) {
        free(cipher); secure_bzero(key, sizeof(key)); free(b64_ct); if (out_error) *out_error = strdup("b64 iv"); return -1; }
    free(cipher);
    secure_bzero(key, sizeof(key));

    size_t out_sz = strlen(b64_ct) + strlen(b64_iv) + 4 + 1; /* ?iv= */
    char *out = (char *)malloc(out_sz);
    if (!out) { free(b64_ct); free(b64_iv); if (out_error) *out_error = strdup("oom"); return -1; }
    snprintf(out, out_sz, "%s?iv=%s", b64_ct, b64_iv);
    free(b64_ct); free(b64_iv);
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
    if (!content_b64_qiv || !sender_pubkey_hex || !receiver_seckey || !receiver_seckey->ptr || receiver_seckey->len < 32 || !out_plaintext_utf8) return -1;
    *out_plaintext_utf8 = NULL;

    const char *q = strstr(content_b64_qiv, "?iv=");
    if (!q) { if (out_error) *out_error = strdup("missing iv"); return -1; }
    size_t ct_len = (size_t)(q - content_b64_qiv);
    char *ct_b64 = (char *)malloc(ct_len + 1);
    if (!ct_b64) { if (out_error) *out_error = strdup("oom"); return -1; }
    memcpy(ct_b64, content_b64_qiv, ct_len); ct_b64[ct_len] = '\0';
    const char *iv_b64 = q + 4;

    unsigned char *ct = NULL; size_t ct_bin_len = 0;
    unsigned char *iv = NULL; size_t iv_len = 0;
    if (!base64_decode(ct_b64, &ct, &ct_bin_len)) { free(ct_b64); if (out_error) *out_error = strdup("b64 ct"); return -1; }
    free(ct_b64);
    if (!base64_decode(iv_b64, &iv, &iv_len)) { free(ct); if (out_error) *out_error = strdup("b64 iv"); return -1; }
    if (iv_len != 16) { free(ct); free(iv); if (out_error) *out_error = strdup("iv len"); return -1; }

    unsigned char key[32];
    if (ecdh_derive_key_bin(sender_pubkey_hex, (const unsigned char*)receiver_seckey->ptr, key) != 0) {
        free(ct); free(iv); if (out_error) *out_error = strdup("ecdh failed"); return -1; }

    unsigned char *pt = (unsigned char *)malloc(ct_bin_len + 1);
    if (!pt) { free(ct); free(iv); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("oom"); return -1; }
    int len = 0, total = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("evp ctx"); return -1; }
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("dec init"); return -1; }
    if (EVP_DecryptUpdate(ctx, pt, &len, ct, (int)ct_bin_len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("dec update"); return -1; }
    total = len;
    if (EVP_DecryptFinal_ex(ctx, pt + total, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx); free(ct); free(iv); free(pt); secure_bzero(key, sizeof(key)); if (out_error) *out_error = strdup("dec final"); return -1; }
    total += len;
    EVP_CIPHER_CTX_free(ctx);

    pt[total] = '\0';
    *out_plaintext_utf8 = (char *)pt;
    free(ct); free(iv);
    secure_bzero(key, sizeof(key));
    return 0;
}
