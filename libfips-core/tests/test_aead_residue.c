/* OpenSSL AEAD backend residue probe (nostrc-u19cz.2). The cached per-thread
 * EVP_CIPHER_CTX outlives every op, so after each seal/open the backend scrubs
 * it (aead_primitive.c). This test owns OpenSSL's allocator, then scans every
 * live OpenSSL heap block for any 8-byte window of the key, the Poly1305
 * one-time key (keystream block 0) or the message keystream. A positive control
 * proves the scanner sees an unscrubbed context. */
#include <fips/noise/aead.h>
#include "../src/noise/aead_primitive.h"
#include <openssl/crypto.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

/* Single-threaded test: a plain table of live OpenSSL blocks. */
#define MAX_LIVE 8192
static struct { void *p; size_t n; } live[MAX_LIVE];
static size_t live_count;
static int table_full;

static void track(void *p, size_t n)
{
    if (p == NULL) return;
    if (live_count == MAX_LIVE) { table_full = 1; return; }
    live[live_count].p = p; live[live_count].n = n; ++live_count;
}
static void untrack(void *p)
{
    for (size_t i = 0; i < live_count; ++i)
        if (live[i].p == p) { live[i] = live[--live_count]; return; }
}
static void *t_malloc(size_t n, const char *f, int l)
{ (void)f; (void)l; void *p = malloc(n ? n : 1); track(p, n); return p; }
static void *t_realloc(void *p, size_t n, const char *f, int l)
{
    (void)f; (void)l;
    void *q = realloc(p, n ? n : 1);
    if (q != NULL) { if (p != NULL) untrack(p); track(q, n); }
    return q;
}
static void t_free(void *p, const char *f, int l) { (void)f; (void)l; untrack(p); free(p); }

/* 8-byte windows of the secrets, open addressing. */
#define SLOTS (1u << 18) /* > windows of a maximum-length keystream */
static uint64_t windows[SLOTS];
static unsigned char used[SLOTS];
static size_t window_count;

static size_t slot(uint64_t v) { return (size_t)((v * UINT64_C(0x9e3779b97f4a7c15)) >> (64 - 18)); }
static void add_windows(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 8 <= n; ++i) {
        uint64_t v;
        memcpy(&v, b + i, 8);
        if (v == 0 || v == UINT64_MAX) continue;
        size_t s = slot(v);
        while (used[s] && windows[s] != v) s = (s + 1) % SLOTS;
        if (!used[s]) { used[s] = 1; windows[s] = v; ++window_count; }
    }
}
static void reset_windows(void) { memset(used, 0, sizeof(used)); window_count = 0; }
static size_t heap_hits(void)
{
    size_t hits = 0;
    for (size_t b = 0; b < live_count; ++b) {
        const uint8_t *p = live[b].p;
        for (size_t i = 0; i + 8 <= live[b].n; ++i) {
            uint64_t v;
            memcpy(&v, p + i, 8);
            size_t s = slot(v);
            while (used[s]) { if (windows[s] == v) { ++hits; break; } s = (s + 1) % SLOTS; }
        }
    }
    return hits;
}

/* Key, block-0 keystream (Poly1305 r||s and the rest of the block) and the
 * keystream for every payload block, as libsodium computes it. */
static void secret_windows(const uint8_t key[32], const uint8_t nonce[12], size_t len)
{
    static uint8_t ks[64 + FIPS_NOISE_MAX_MESSAGE_SIZE + 64];
    size_t n = 64 + (len + 63) / 64 * 64;
    memset(ks, 0, n);
    crypto_stream_chacha20_ietf_xor_ic(ks, ks, n, nonce, 0, key);
    reset_windows();
    add_windows(key, 32);
    add_windows(ks, n);
}

static int control(void)
{
    uint8_t key[32], nonce[12] = {7}, plain[64] = {1}, out[80];
    int n;
    randombytes_buf(key, sizeof(key));
    secret_windows(key, nonce, sizeof(plain));
    EVP_CIPHER *cipher = EVP_CIPHER_fetch(NULL, "ChaCha20-Poly1305", NULL);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    CHECK(cipher != NULL && ctx != NULL);
    CHECK(EVP_CipherInit_ex2(ctx, cipher, key, nonce, 1, NULL) == 1);
    CHECK(EVP_CipherUpdate(ctx, out, &n, plain, (int)sizeof(plain)) == 1);
    CHECK(EVP_CipherFinal_ex(ctx, out + 64, &n) == 1);
    CHECK(heap_hits() > 0); /* the scanner sees an unscrubbed context */
    EVP_CIPHER_CTX_free(ctx);
    EVP_CIPHER_free(cipher);
    CHECK(heap_hits() == 0);
    return 0;
}

static int probe(size_t len, int forge)
{
    static uint8_t plain[FIPS_NOISE_MAX_MESSAGE_SIZE], wire[FIPS_NOISE_MAX_MESSAGE_SIZE];
    static uint8_t back[FIPS_NOISE_MAX_MESSAGE_SIZE];
    uint8_t kb[32], nonce[12], ad[16];
    randombytes_buf(kb, sizeof(kb));
    randombytes_buf(nonce, sizeof(nonce));
    randombytes_buf(ad, sizeof(ad));
    randombytes_buf(plain, len);
    fips_aead_key key;
    CHECK(fips_aead_key_init(&key, kb) == FIPS_AEAD_OK);
    secret_windows(kb, nonce, len);
    CHECK(window_count > 0);
    CHECK(fips_aead_seal_nonce(&key, nonce, ad, sizeof(ad), plain, len, wire) == 0);
    CHECK(heap_hits() == 0);
    if (forge) wire[len] ^= 1;
    int rc = fips_aead_open_nonce(&key, nonce, ad, sizeof(ad), wire,
                                  len + FIPS_NOISE_TAG_SIZE, back);
    CHECK(rc == (forge ? FIPS_AEAD_PRIMITIVE_ERR_AUTH : 0));
    CHECK(heap_hits() == 0);
    fips_aead_key_destroy(&key);
    return 0;
}

int main(void)
{
    /* Must precede any OpenSSL allocation in this process. */
    CHECK(CRYPTO_set_mem_functions(t_malloc, t_realloc, t_free) == 1);
    CHECK(sodium_init() >= 0);
    CHECK(control() == 0);
    /* Multiples of 64 leave block 0 (the one-time key) in the keystream buffer
     * unless scrubbed; other lengths leave the last partial block. */
    const size_t lens[] = {0, 1, 63, 64, 128, 255, 256, 320, 1400, 1408, 4096,
                           FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); ++i)
        for (int forge = 0; forge < 2; ++forge)
            CHECK(probe(lens[i], forge) == 0);
    CHECK(!table_full && live_count > 0);
    printf("AEAD residue (%s): %zu live OpenSSL blocks hold no key/keystream window\n",
           fips_aead_backend_name(), live_count);
    return 0;
}
