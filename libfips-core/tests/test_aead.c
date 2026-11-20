#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* pthread under C_EXTENSIONS NO */
#endif
#include <fips/noise/aead.h>
#include "../src/noise/aead_primitive.h"
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#if defined(FIPS_AEAD_BACKEND_OPENSSL)
#include <openssl/err.h>
#endif

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

/* RFC 8439 https://www.rfc-editor.org/rfc/rfc8439#section-2.8.2
 * and #appendix-A.5, including the literal slashes/UTF-8 quotes in A.5. */
static const char rfc_2_8_2_key[] =
    "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
    ;
static const char rfc_2_8_2_ad[] =
    "50515253c0c1c2c3c4c5c6c7"
    ;
static const char rfc_2_8_2_plaintext[] =
    "4c616469657320616e642047656e746c656d656e206f662074686520636c6173"
    "73206f66202739393a204966204920636f756c64206f6666657220796f75206f"
    "6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"
    "637265656e20776f756c642062652069742e"
    ;
static const char rfc_2_8_2_ciphertext[] =
    "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
    "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
    "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
    "3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd060"
    "0691"
    ;
static const char rfc_2_8_2_nonce[] =
    "070000004041424344454647"
    ;
static const char rfc_a_5_key[] =
    "1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc207075c0"
    ;
static const char rfc_a_5_ad[] =
    "f33388860000000000004e91"
    ;
static const char rfc_a_5_plaintext[] =
    "496e7465726e65742d4472616674732061726520647261667420646f63756d65"
    "6e74732076616c696420666f722061206d6178696d756d206f6620736978206d"
    "6f6e74687320616e64206d617920626520757064617465642c207265706c6163"
    "65642c206f72206f62736f6c65746564206279206f7468657220646f63756d65"
    "6e747320617420616e792074696d652e20497420697320696e617070726f7072"
    "6961746520746f2075736520496e7465726e65742d4472616674732061732072"
    "65666572656e6365206d6174657269616c206f7220746f206369746520746865"
    "6d206f74686572207468616e206173202fe2809c776f726b20696e2070726f67"
    "726573732e2fe2809d"
    ;
static const char rfc_a_5_ciphertext[] =
    "64a0861575861af460f062c79be643bd5e805cfd345cf389f108670ac76c8cb2"
    "4c6cfc18755d43eea09ee94e382d26b0bdb7b73c321b0100d4f03b7f355894cf"
    "332f830e710b97ce98c8a84abd0b948114ad176e008d33bd60f982b1ff37c855"
    "9797a06ef4f0ef61c186324e2b3506383606907b6a7c02b0f9f6157b53c867e4"
    "b9166c767b804d46a59b5216cde7a4e99040c5a40433225ee282a1b0a06c523e"
    "af4534d7f83fa1155b0047718cbc546a0d072b04b3564eea1b422273f548271a"
    "0bb2316053fa76991955ebd63159434ecebb4e466dae5a1073a6727627097a10"
    "49e617d91d361094fa68f0ff77987130305beaba2eda04df997b714d6c6f2c29"
    "a6ad5cb4022b02709beead9d67890cbb22392336fea1851f38"
    ;
static const char rfc_a_5_nonce[] =
    "000000000102030405060708"
    ;

static int filled(const uint8_t *buf, size_t len, uint8_t byte)
{
    for (size_t i = 0; i < len; ++i) if (buf[i] != byte) return 0;
    return 1;
}

static int vector(const char *kh, const char *nh, const char *ah,
                  const char *ph, const char *ch, int noise_nonce)
{
    uint8_t kb[32], nonce[12], ad[12], plain[512], cipher[528], out[528];
    size_t len = strlen(ph) / 2, clen = strlen(ch) / 2;
    CHECK(sodium_hex2bin(kb, sizeof(kb), kh, strlen(kh), NULL, NULL, NULL) == 0);
    CHECK(sodium_hex2bin(nonce, sizeof(nonce), nh, strlen(nh), NULL, NULL, NULL) == 0);
    CHECK(sodium_hex2bin(ad, sizeof(ad), ah, strlen(ah), NULL, NULL, NULL) == 0);
    CHECK(sodium_hex2bin(plain, sizeof(plain), ph, strlen(ph), NULL, NULL, NULL) == 0);
    CHECK(sodium_hex2bin(cipher, sizeof(cipher), ch, strlen(ch), NULL, NULL, NULL) == 0);
    CHECK(clen == len + FIPS_NOISE_TAG_SIZE);
    fips_aead_key key;
    CHECK(fips_aead_key_init(&key, kb) == FIPS_AEAD_OK);
    CHECK(fips_aead_seal_nonce(&key, nonce, ad, sizeof(ad), plain, len, out) == 0);
    CHECK(memcmp(out, cipher, clen) == 0);
    CHECK(fips_aead_open_nonce(&key, nonce, ad, sizeof(ad), cipher, clen, out) == 0);
    CHECK(memcmp(out, plain, len) == 0);
    if (noise_nonce) {
        uint64_t counter = UINT64_C(0x0807060504030201);
        uint8_t n[12];
        fips_aead_nonce(counter, n);
        CHECK(memcmp(n, nonce, sizeof(n)) == 0);
        size_t out_len = SIZE_MAX;
        CHECK(fips_aead_seal(&key, counter, ad, sizeof(ad), plain, len,
                             out, sizeof(out), &out_len) == FIPS_AEAD_OK);
        CHECK(out_len == clen && memcmp(out, cipher, clen) == 0);
        CHECK(fips_aead_open(&key, counter, ad, sizeof(ad), cipher, clen,
                             out, sizeof(out), &out_len) == FIPS_AEAD_OK);
        CHECK(out_len == len && memcmp(out, plain, len) == 0);
    }
    fips_aead_key_destroy(&key);
    CHECK(filled((const uint8_t *)&key, sizeof(key), 0));
    return 0;
}

static int nonces(void)
{
    const uint64_t counters[] = {0, 1, UINT64_C(1) << 32, UINT64_MAX};
    const uint8_t expected[][12] = {
        {0}, {0,0,0,0,1,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,1,0,0,0}, {0,0,0,0,255,255,255,255,255,255,255,255}
    };
    for (size_t i = 0; i < 4; ++i) {
        uint8_t out[14];
        memset(out, 0xa5, sizeof(out));
        fips_aead_nonce(counters[i], out + 1);
        CHECK(memcmp(out + 1, expected[i], 12) == 0);
        CHECK(out[0] == 0xa5 && out[13] == 0xa5);
    }
    return 0;
}

static int roundtrips(void)
{
    uint8_t kb[32] = {9}, ad[16] = {0x5a};
    fips_aead_key key;
    CHECK(fips_aead_key_init(&key, kb) == FIPS_AEAD_OK);
    CHECK(fips_aead_key_init(&key, key.bytes) == FIPS_AEAD_OK);
    CHECK(memcmp(key.bytes, kb, sizeof(kb)) == 0);
    uint8_t plain[FIPS_NOISE_MAX_MESSAGE_SIZE], wire[FIPS_NOISE_MAX_MESSAGE_SIZE + 1];
    uint8_t out[FIPS_NOISE_MAX_MESSAGE_SIZE + 1];
    for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)i;
    const size_t sizes[] = {0, 1, FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE};
    for (size_t i = 0; i < 3; ++i) {
        size_t n = sizes[i], clen = n + FIPS_NOISE_TAG_SIZE, got = SIZE_MAX;
        memset(wire, 0xa5, sizeof(wire));
        CHECK(fips_aead_seal(&key, i, ad, sizeof(ad), n ? plain : NULL, n,
                             wire, clen, &got) == FIPS_AEAD_OK);
        CHECK(got == clen && wire[clen] == 0xa5);
        memset(out, 0xa5, sizeof(out));
        CHECK(fips_aead_open(&key, i, ad, sizeof(ad), wire, clen,
                             out, n, &got) == FIPS_AEAD_OK);
        CHECK(got == n && memcmp(out, plain, n) == 0 && out[n] == 0xa5);
        /* Exact in-place, including empty AD and empty plaintext. */
        memcpy(out, plain, n);
        CHECK(fips_aead_seal(&key, i + 3, NULL, 0, out, n, out, clen, &got) == FIPS_AEAD_OK);
        CHECK(fips_aead_open(&key, i + 3, NULL, 0, out, clen, out, n, &got) == FIPS_AEAD_OK);
        CHECK(got == n && memcmp(out, plain, n) == 0);
        memset(out, 0xa5, sizeof(out));
        got = SIZE_MAX;
        CHECK(fips_aead_seal(&key, i, ad, sizeof(ad), plain, n, out,
                             clen - 1, &got) == FIPS_AEAD_ERR_RANGE);
        CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5));
        if (n) {
            CHECK(fips_aead_open(&key, i, ad, sizeof(ad), wire, clen, out,
                                 n - 1, &got) == FIPS_AEAD_ERR_RANGE);
            CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5));
        }
    }
    size_t got = SIZE_MAX;
    memset(out, 0xa5, sizeof(out));
    const size_t invalid[] = {FIPS_NOISE_MAX_MESSAGE_SIZE, SIZE_MAX};
    for (size_t i = 0; i < 2; ++i) {
        CHECK(fips_aead_seal(&key, 0, NULL, 0, plain, invalid[i], out,
                             sizeof(out), &got) == FIPS_AEAD_ERR_RANGE);
        CHECK(fips_aead_open(&key, 0, NULL, 0, wire, invalid[i] + (i == 0), out,
                             sizeof(out), &got) == FIPS_AEAD_ERR_RANGE);
        CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5));
    }
    CHECK(fips_aead_seal(&key, 0, NULL, 0, plain,
                         FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE + 1,
                         out, sizeof(out), &got) == FIPS_AEAD_ERR_RANGE);
    CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5));
    for (size_t len = 0; len < FIPS_NOISE_TAG_SIZE; ++len) {
        CHECK(fips_aead_open(&key, 0, NULL, 0, len ? wire : NULL, len,
                             out, 0, &got) == FIPS_AEAD_ERR_AUTH);
        CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5));
    }
    fips_aead_key_destroy(&key);
    CHECK(filled(key.bytes, sizeof(key.bytes), 0));
    fips_aead_key_destroy(&key);
    return 0;
}

static int tamper(void)
{
    uint8_t kb[32] = {1}, ad[16] = {2}, plain[64] = {3}, wire[80], out[96];
    fips_aead_key key;
    CHECK(fips_aead_key_init(&key, kb) == FIPS_AEAD_OK);
    size_t got;
    CHECK(fips_aead_seal(&key, 123, ad, sizeof(ad), plain, sizeof(plain),
                         wire, sizeof(wire), &got) == FIPS_AEAD_OK);
    for (unsigned which = 0; which < 5; ++which) {
        uint8_t bad[80];
        memcpy(bad, wire, sizeof(bad));
        if (which == 0) bad[0] ^= 1;
        if (which == 1) bad[79] ^= 1;
        if (which == 2) ad[0] ^= 1;
        if (which == 4) key.bytes[0] ^= 1;
        for (int inplace = 0; inplace < 2; ++inplace) {
            memset(out, 0xa5, sizeof(out));
            if (inplace) memcpy(out, bad, sizeof(bad));
            got = SIZE_MAX;
            CHECK(fips_aead_open(&key, which == 3 ? 124 : 123, ad, sizeof(ad),
                                 inplace ? out : bad, sizeof(bad), out,
                                 sizeof(plain), &got) == FIPS_AEAD_ERR_AUTH);
            CHECK(got == SIZE_MAX && filled(out, sizeof(plain), 0));
            CHECK(filled(out + sizeof(bad), sizeof(out) - sizeof(bad), 0xa5));
            if (!inplace) CHECK(filled(out + sizeof(plain), sizeof(out) - sizeof(plain), 0xa5));
        }
        if (which == 2) ad[0] ^= 1;
        if (which == 4) key.bytes[0] ^= 1;
    }
    /* An invalid tag on an empty payload must not touch any output. */
    CHECK(fips_aead_seal(&key, 124, NULL, 0, NULL, 0, wire, sizeof(wire), &got) == FIPS_AEAD_OK);
    wire[0] ^= 1;
    memset(out, 0xa5, sizeof(out));
    got = SIZE_MAX;
    CHECK(fips_aead_open(&key, 124, NULL, 0, wire, 16, out, 0, &got) == FIPS_AEAD_ERR_AUTH);
    CHECK(got == SIZE_MAX && filled(out, sizeof(out), 0xa5));
    fips_aead_key_destroy(&key);
    return 0;
}

/* Backend vs libsodium's reference AEAD, byte for byte: every plaintext length
 * 0..300 plus block/MTU/maximum edges, AAD lengths around the Poly1305 block,
 * a fresh key and nonce per op (re-keying a cached context), exact in-place,
 * and forged tags that must fail as AUTH with zeroed plaintext. */
static int reference(void)
{
    static uint8_t plain[FIPS_NOISE_MAX_MESSAGE_SIZE], expect[FIPS_NOISE_MAX_MESSAGE_SIZE];
    static uint8_t out[FIPS_NOISE_MAX_MESSAGE_SIZE], back[FIPS_NOISE_MAX_MESSAGE_SIZE];
    uint8_t seed[randombytes_SEEDBYTES] = {0x19};
    randombytes_buf_deterministic(plain, sizeof(plain), seed);
    const size_t ad_lens[] = {0, 1, 12, 15, 16, 17, 33, 64, 300};
    const size_t edges[] = {511, 512, 513, 1023, 1024, 1400, 4096,
                            FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE};
    uint8_t ad[300];
    for (size_t i = 0; i < 301 + sizeof(edges) / sizeof(edges[0]); ++i) {
        size_t len = i <= 300 ? i : edges[i - 301];
        size_t ad_len = ad_lens[i % (sizeof(ad_lens) / sizeof(ad_lens[0]))];
        uint8_t kb[32], nonce[12];
        seed[1] = (uint8_t)i; seed[2] = (uint8_t)(i >> 8);
        randombytes_buf_deterministic(kb, sizeof(kb), seed);
        seed[3] = 1;
        randombytes_buf_deterministic(nonce, sizeof(nonce), seed);
        seed[3] = 2;
        randombytes_buf_deterministic(ad, sizeof(ad), seed);
        seed[3] = 0;
        fips_aead_key key;
        CHECK(fips_aead_key_init(&key, kb) == FIPS_AEAD_OK);
        CHECK(crypto_aead_chacha20poly1305_ietf_encrypt(expect, NULL, plain, len, ad,
              ad_len, NULL, nonce, kb) == 0);
        size_t clen = len + FIPS_NOISE_TAG_SIZE;
        CHECK(fips_aead_seal_nonce(&key, nonce, ad_len ? ad : NULL, ad_len,
                                   len ? plain : NULL, len, out) == 0);
        CHECK(memcmp(out, expect, clen) == 0);
        memset(back, 0xa5, len + 1);
        CHECK(fips_aead_open_nonce(&key, nonce, ad, ad_len, out, clen, back) == 0);
        CHECK(memcmp(back, plain, len) == 0 && back[len] == 0xa5);
        memcpy(back, plain, len);
        CHECK(fips_aead_seal_nonce(&key, nonce, ad, ad_len, back, len, back) == 0);
        CHECK(memcmp(back, expect, clen) == 0);
        CHECK(fips_aead_open_nonce(&key, nonce, ad, ad_len, back, clen, back) == 0);
        CHECK(memcmp(back, plain, len) == 0);
        memcpy(back, expect, clen);
        back[i % clen] ^= 0x80;
        CHECK(fips_aead_open_nonce(&key, nonce, ad, ad_len, back, clen, back) ==
              FIPS_AEAD_PRIMITIVE_ERR_AUTH);
        CHECK(filled(back, len, 0));
#if defined(FIPS_AEAD_BACKEND_OPENSSL)
        CHECK(ERR_peek_error() == 0); /* forgeries leave no OpenSSL error state */
#endif
        fips_aead_key_destroy(&key);
    }
#if defined(FIPS_AEAD_BACKEND_OPENSSL)
    /* A host's pending OpenSSL error survives a forged open untouched. */
    uint8_t kb[32] = {4}, nonce[12] = {5}, wire[32] = {6};
    fips_aead_key key;
    CHECK(fips_aead_key_init(&key, kb) == FIPS_AEAD_OK);
    ERR_raise(ERR_LIB_USER, 42);
    CHECK(fips_aead_open_nonce(&key, nonce, NULL, 0, wire, sizeof(wire), out) ==
          FIPS_AEAD_PRIMITIVE_ERR_AUTH);
    CHECK(ERR_GET_REASON(ERR_peek_last_error()) == 42);
    CHECK(ERR_GET_REASON(ERR_get_error()) == 42 && ERR_peek_error() == 0);
    fips_aead_key_destroy(&key);
#endif
    return 0;
}

/* Per-thread contexts: concurrent threads with distinct keys, each created
 * lazily and released by the thread-exit destructor (ASan/LSan see leaks). */
struct thread_job { uint8_t id; int failed; };

static void *thread_main(void *arg)
{
    struct thread_job *job = arg;
    uint8_t kb[32], ad[16], plain[1400], expect[1416], out[1416], back[1400];
    memset(kb, job->id + 1, sizeof(kb)); memset(ad, job->id, sizeof(ad));
    for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)(i * 7 + job->id);
    fips_aead_key key;
    if (fips_aead_key_init(&key, kb) != FIPS_AEAD_OK) { job->failed = 1; return NULL; }
    for (uint64_t counter = 0; counter < 500 && !job->failed; ++counter) {
        size_t len = (size_t)(counter * 37 % sizeof(plain)), got;
        uint8_t nonce[12];
        fips_aead_nonce(counter, nonce);
        if (crypto_aead_chacha20poly1305_ietf_encrypt(expect, NULL, plain, len, ad,
                sizeof(ad), NULL, nonce, kb) != 0 ||
            fips_aead_seal(&key, counter, ad, sizeof(ad), plain, len, out, sizeof(out),
                           &got) != FIPS_AEAD_OK ||
            got != len + 16 || memcmp(out, expect, got) != 0 ||
            fips_aead_open(&key, counter, ad, sizeof(ad), out, got, back, sizeof(back),
                           &got) != FIPS_AEAD_OK ||
            got != len || memcmp(back, plain, len) != 0)
            job->failed = 1;
    }
    fips_aead_key_destroy(&key);
    return NULL;
}

static int threads(void)
{
    enum { N = 8 };
    pthread_t tid[N];
    struct thread_job jobs[N];
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < N; ++i) {
            jobs[i] = (struct thread_job){(uint8_t)(round * N + i), 0};
            CHECK(pthread_create(&tid[i], NULL, thread_main, &jobs[i]) == 0);
        }
        for (int i = 0; i < N; ++i) {
            CHECK(pthread_join(tid[i], NULL) == 0);
            CHECK(!jobs[i].failed);
        }
    }
    return 0;
}

int main(void)
{
    CHECK(vector(rfc_2_8_2_key, rfc_2_8_2_nonce, rfc_2_8_2_ad,
                 rfc_2_8_2_plaintext, rfc_2_8_2_ciphertext, 0) == 0);
    CHECK(vector(rfc_a_5_key, rfc_a_5_nonce, rfc_a_5_ad,
                 rfc_a_5_plaintext, rfc_a_5_ciphertext, 1) == 0);
    CHECK(nonces() == 0);
    CHECK(roundtrips() == 0);
    CHECK(tamper() == 0);
    CHECK(reference() == 0);
    CHECK(threads() == 0);
    printf("AEAD (%s): RFC 8439 2.8.2/A.5, nonce, bounds, roundtrip, tamper, "
           "libsodium reference and thread tests passed\n", fips_aead_backend_name());
    return 0;
}
