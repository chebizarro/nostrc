/* IETF ChaCha20-Poly1305 backends for aead_primitive.h (nostrc-u19cz.2).
 * Exactly one of FIPS_AEAD_BACKEND_OPENSSL / FIPS_AEAD_BACKEND_SODIUM is
 * defined by CMake (FIPS_AEAD_BACKEND). libsodium stays a dependency of both
 * builds for memzero/HMAC/HKDF/SHA-256/randombytes. */
#if defined(FIPS_AEAD_BACKEND_OPENSSL) && !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* pthread under C_EXTENSIONS NO */
#endif
#include "aead_primitive.h"
#include <sodium.h>

_Static_assert(FIPS_AEAD_KEY_SIZE == crypto_aead_chacha20poly1305_ietf_KEYBYTES,
               "ChaChaPoly key size");
_Static_assert(FIPS_NOISE_NONCE_SIZE == crypto_aead_chacha20poly1305_ietf_NPUBBYTES,
               "ChaChaPoly nonce size");
_Static_assert(FIPS_NOISE_TAG_SIZE == crypto_aead_chacha20poly1305_ietf_ABYTES,
               "ChaChaPoly tag size");

#if defined(FIPS_AEAD_BACKEND_OPENSSL) == defined(FIPS_AEAD_BACKEND_SODIUM)
#error "define exactly one of FIPS_AEAD_BACKEND_OPENSSL / FIPS_AEAD_BACKEND_SODIUM"
#endif

/* libsodium: the whole AEAD for the sodium backend, short payloads for openssl. */
static int sodium_seal_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            out, NULL, in, (unsigned long long)len, ad, (unsigned long long)ad_len,
            NULL, nonce, key->bytes) == 0)
        return 0;
    sodium_memzero(out, len + FIPS_NOISE_TAG_SIZE);
    return FIPS_AEAD_PRIMITIVE_ERR_CRYPTO;
}

static int sodium_open_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{
    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
        out, NULL, NULL, in, (unsigned long long)len, ad,
        (unsigned long long)ad_len, nonce, key->bytes);
    if (rc == 0) return 0;
    /* libsodium already zeroes on tag mismatch; keep the contract local. */
    sodium_memzero(out, len - FIPS_NOISE_TAG_SIZE);
    return FIPS_AEAD_PRIMITIVE_ERR_AUTH;
}

#if defined(FIPS_AEAD_BACKEND_SODIUM)

const char *fips_aead_backend_name(void) { return "sodium"; }

int fips_aead_seal_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{ return sodium_seal_nonce(key, nonce, ad, ad_len, in, len, out); }

int fips_aead_open_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{ return sodium_open_nonce(key, nonce, ad, ad_len, in, len, out); }

#else /* FIPS_AEAD_BACKEND_OPENSSL */

#include <limits.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/provider.h>
#include <pthread.h>
#include <stdlib.h>

/* Design (measured in docs/reviews/libfips-core-noise-audit-2026-11-15.md, B-1):
 *
 * - Payloads (plaintext) shorter than FIPS_AEAD_OPENSSL_MIN_LEN use libsodium:
 *   the fixed EVP cost plus the scrub below (~110 ns) loses to libsodium's
 *   portable ChaCha20 there; above it OpenSSL's NEON/AVX code wins (~2.1x at
 *   1400 B on an M4). Byte-identical either way; the length is public.
 * - One EVP_CIPHER, fetched once from a private OSSL_LIB_CTX holding only the
 *   built-in "default" provider, so a host openssl.cnf (e.g. fips=yes default
 *   properties) cannot make ChaCha20-Poly1305 disappear. Process lifetime.
 * - One EVP_CIPHER_CTX per thread in a pthread key: fips_aead_key and
 *   fips_cipher_state stay plain value-copyable structs with no pointers.
 *   Re-keyed per op with EVP_CipherInit_ex2(ctx, NULL, key, nonce, enc, NULL),
 *   so the hot path allocates nothing. The key destructor frees it at thread
 *   exit (winpthreads on MinGW); an atexit hook frees the exiting thread's
 *   context before OPENSSL_cleanup.
 * - After every op the context is scrubbed: re-keyed with an all-zero key and
 *   nonce and fed one byte of AAD. That overwrites the ChaCha key words and the
 *   64-byte keystream buffer (Poly1305 one-time key or last partial block) with
 *   values derived from the public zero key; Poly1305_Final already cleansed the
 *   MAC state. FipsCoreAeadResidueTest scans OpenSSL's heap to prove no key,
 *   one-time key or keystream byte window survives an op. If scrubbing fails the
 *   context is freed (OPENSSL_clear_free) and recreated on the next op. */

#define FIPS_AEAD_OPENSSL_MIN_LEN 256u /* measured crossover, Apple M4 */

static pthread_once_t backend_once = PTHREAD_ONCE_INIT;
static pthread_key_t ctx_key;
static OSSL_LIB_CTX *backend_libctx;
static EVP_CIPHER *backend_cipher;
static int backend_ready;

static void ctx_free(void *ctx) { EVP_CIPHER_CTX_free(ctx); }

static void thread_ctx_release(void)
{
    EVP_CIPHER_CTX *ctx = pthread_getspecific(ctx_key);
    if (ctx == NULL) return;
    (void)pthread_setspecific(ctx_key, NULL);
    EVP_CIPHER_CTX_free(ctx);
}

static void backend_init(void)
{
    backend_libctx = OSSL_LIB_CTX_new();
    if (backend_libctx == NULL) return;
    /* Built in to libcrypto: no module file or configuration is read. */
    if (OSSL_PROVIDER_load(backend_libctx, "default") == NULL) return;
    backend_cipher = EVP_CIPHER_fetch(backend_libctx, "ChaCha20-Poly1305", NULL);
    if (backend_cipher == NULL) return;
    if (pthread_key_create(&ctx_key, ctx_free) != 0) return;
    /* Registered after OpenSSL's own atexit(OPENSSL_cleanup), so it runs first. */
    (void)atexit(thread_ctx_release);
    backend_ready = 1;
}

static EVP_CIPHER_CTX *thread_ctx(void)
{
    if (pthread_once(&backend_once, backend_init) != 0 || !backend_ready) return NULL;
    EVP_CIPHER_CTX *ctx = pthread_getspecific(ctx_key);
    if (ctx != NULL) return ctx;
    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) return NULL;
    if (EVP_CipherInit_ex2(ctx, backend_cipher, NULL, NULL, 1, NULL) != 1 ||
        pthread_setspecific(ctx_key, ctx) != 0) {
        EVP_CIPHER_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static void scrub(EVP_CIPHER_CTX *ctx)
{
    static const uint8_t zero_key[FIPS_AEAD_KEY_SIZE], zero_nonce[FIPS_NOISE_NONCE_SIZE];
    static const uint8_t zero_ad[1];
    int n;
    if (EVP_CipherInit_ex2(ctx, NULL, zero_key, zero_nonce, 1, NULL) == 1 &&
        EVP_CipherUpdate(ctx, NULL, &n, zero_ad, (int)sizeof(zero_ad)) == 1)
        return;
    (void)pthread_setspecific(ctx_key, NULL);
    EVP_CIPHER_CTX_free(ctx);
}

static int absorb_ad(EVP_CIPHER_CTX *ctx, const uint8_t *ad, size_t ad_len)
{
    while (ad_len > 0) {
        int chunk = ad_len > (size_t)INT_MAX ? INT_MAX : (int)ad_len, n;
        if (EVP_CipherUpdate(ctx, NULL, &n, ad, chunk) != 1 || n != chunk) return 0;
        ad += chunk;
        ad_len -= (size_t)chunk;
    }
    return 1;
}

const char *fips_aead_backend_name(void) { return "openssl"; }

int fips_aead_seal_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{
    if (len < FIPS_AEAD_OPENSSL_MIN_LEN)
        return sodium_seal_nonce(key, nonce, ad, ad_len, in, len, out);
    EVP_CIPHER_CTX *ctx = len <= (size_t)INT_MAX ? thread_ctx() : NULL;
    int ok = ctx != NULL, n;
    uint8_t final_block[FIPS_NOISE_TAG_SIZE]; /* stream cipher: Final emits 0 bytes */
    ok = ok && EVP_CipherInit_ex2(ctx, NULL, key->bytes, nonce, 1, NULL) == 1;
    ok = ok && absorb_ad(ctx, ad, ad_len);
    ok = ok && EVP_CipherUpdate(ctx, out, &n, in, (int)len) == 1 && (size_t)n == len;
    ok = ok && EVP_CipherFinal_ex(ctx, final_block, &n) == 1 && n == 0;
    if (ok) {
        OSSL_PARAM params[2] = {
            OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, out + len,
                                              FIPS_NOISE_TAG_SIZE),
            OSSL_PARAM_construct_end()};
        ok = EVP_CIPHER_CTX_get_params(ctx, params) == 1;
    }
    if (ctx != NULL) scrub(ctx);
    if (ok) return 0;
    sodium_memzero(out, len + FIPS_NOISE_TAG_SIZE);
    return FIPS_AEAD_PRIMITIVE_ERR_CRYPTO;
}

int fips_aead_open_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{
    size_t plain_len = len - FIPS_NOISE_TAG_SIZE;
    if (plain_len < FIPS_AEAD_OPENSSL_MIN_LEN)
        return sodium_open_nonce(key, nonce, ad, ad_len, in, len, out);
    EVP_CIPHER_CTX *ctx = len <= (size_t)INT_MAX ? thread_ctx() : NULL;
    int ok = ctx != NULL, n, auth = 0;
    uint8_t final_block[FIPS_NOISE_TAG_SIZE];
    ok = ok && EVP_CipherInit_ex2(ctx, NULL, key->bytes, nonce, 0, NULL) == 1;
    if (ok) {
        /* The provider copies the tag; in may alias out (exact in-place). */
        OSSL_PARAM params[2] = {
            OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG,
                                              (void *)(in + plain_len), FIPS_NOISE_TAG_SIZE),
            OSSL_PARAM_construct_end()};
        ok = EVP_CIPHER_CTX_set_params(ctx, params) == 1;
    }
    ok = ok && absorb_ad(ctx, ad, ad_len);
    ok = ok && EVP_CipherUpdate(ctx, out, &n, in, (int)plain_len) == 1 &&
         (size_t)n == plain_len;
    /* Only Final verifies the tag: a failure there is authentication. OpenSSL 4
     * raises "bad decrypt" for it (3.x does not); a forged packet must not leave
     * error state for other OpenSSL users on this thread (e.g. TLS), so the
     * queue is restored to its pre-Final mark on mismatch. */
    if (ok) {
        (void)ERR_set_mark();
        ok = EVP_CipherFinal_ex(ctx, final_block, &n) == 1 && n == 0;
        auth = !ok;
        if (auth) (void)ERR_pop_to_mark();
        else (void)ERR_clear_last_mark();
    }
    if (ctx != NULL) scrub(ctx);
    if (ok) return 0;
    /* Plaintext was written before the tag check: wipe it. */
    sodium_memzero(out, plain_len);
    return auth ? FIPS_AEAD_PRIMITIVE_ERR_AUTH : FIPS_AEAD_PRIMITIVE_ERR_CRYPTO;
}

#endif
