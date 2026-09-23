/*
 * test_porthome_crypto.c — unit tests for nh_porthome_crypto.
 *
 * Covers: convergence within a home_key, disjointness across home_keys,
 * wrong-key AEAD failure, tamper-detection, name-encryption determinism
 * and cross-home disjointness, path validation, and seed→home_key
 * determinism.
 */

#include "nh_porthome_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); return 1; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL("%s:%d assertion failed: %s", __FILE__, __LINE__, #cond); } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) FAIL("%s:%d %s (=%ld) != %s (=%ld)", __FILE__, __LINE__, #a, (long)(a), #b, (long)(b)); } while (0)

/* Tiny deterministic PRNG (splitmix64) — no libc rand dependency. */
static uint64_t sm_state;
static uint64_t sm_next(void) {
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static void fill_random(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(sm_next() & 0xFF);
}

static int test_key_derive_deterministic(void) {
    uint8_t seed[32]; for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    uint8_t k1[32], k2[32];
    ASSERT_EQ(nh_porthome_key_derive(seed, k1), 0);
    ASSERT_EQ(nh_porthome_key_derive(seed, k2), 0);
    ASSERT_EQ(memcmp(k1, k2, 32), 0);

    /* Different seed -> different key. */
    seed[0] ^= 1;
    uint8_t k3[32];
    ASSERT_EQ(nh_porthome_key_derive(seed, k3), 0);
    ASSERT(memcmp(k1, k3, 32) != 0);
    return 0;
}

static int test_convergence(void) {
    sm_state = 0xC0FFEEULL;
    uint8_t home_key_a[32]; fill_random(home_key_a, 32);
    uint8_t home_key_b[32]; fill_random(home_key_b, 32);

    /* Encrypt the same plaintext twice under the same key — expect
     * byte-identical ciphertext. */
    const size_t sizes[] = {0, 1, 15, 16, 17, 63, 64, 65, 4095, 4096, 4097, 65536};
    for (size_t si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
        size_t n = sizes[si];
        uint8_t *pt = (uint8_t *)malloc(n ? n : 1);
        fill_random(pt, n);

        uint8_t *ct1 = NULL, *ct2 = NULL, *ct_other = NULL;
        size_t ct1_len = 0, ct2_len = 0, ct_other_len = 0;
        uint8_t h1[32], h2[32], h_other[32];

        ASSERT_EQ(nh_porthome_encrypt_chunk(home_key_a, pt, n, &ct1, &ct1_len, h1), 0);
        ASSERT_EQ(nh_porthome_encrypt_chunk(home_key_a, pt, n, &ct2, &ct2_len, h2), 0);
        ASSERT_EQ(ct1_len, ct2_len);
        ASSERT_EQ(memcmp(ct1, ct2, ct1_len), 0);
        ASSERT_EQ(memcmp(h1, h2, 32), 0);

        /* Different home_key -> different ciphertext AND address. */
        ASSERT_EQ(nh_porthome_encrypt_chunk(home_key_b, pt, n, &ct_other, &ct_other_len, h_other), 0);
        ASSERT_EQ(ct_other_len, ct1_len);
        ASSERT(memcmp(ct1, ct_other, ct1_len) != 0);
        ASSERT(memcmp(h1, h_other, 32) != 0);

        /* Decrypt round-trip. */
        uint8_t *rt = NULL; size_t rt_len = 0;
        ASSERT_EQ(nh_porthome_decrypt_chunk(home_key_a, ct1, ct1_len, &rt, &rt_len), 0);
        ASSERT_EQ(rt_len, n);
        if (n) ASSERT_EQ(memcmp(rt, pt, n), 0);
        free(rt);

        /* Wrong key -> AEAD failure. */
        uint8_t *bogus = NULL; size_t bogus_len = 0;
        int rc = nh_porthome_decrypt_chunk(home_key_b, ct1, ct1_len, &bogus, &bogus_len);
        ASSERT(rc != 0);
        ASSERT(bogus == NULL);

        /* Tamper one byte of ciphertext -> AEAD failure. */
        if (n > 0) {
            uint8_t saved = ct1[13 + n/2];
            ct1[13 + n/2] ^= 0x55;
            uint8_t *bad = NULL; size_t bad_len = 0;
            rc = nh_porthome_decrypt_chunk(home_key_a, ct1, ct1_len, &bad, &bad_len);
            ASSERT(rc != 0);
            ASSERT(bad == NULL);
            ct1[13 + n/2] = saved;
        }
        /* Tamper tag -> AEAD failure. */
        ct1[ct1_len - 1] ^= 0x55;
        uint8_t *tbad = NULL; size_t tbad_len = 0;
        int trc = nh_porthome_decrypt_chunk(home_key_a, ct1, ct1_len, &tbad, &tbad_len);
        ASSERT(trc != 0);

        free(ct1); free(ct2); free(ct_other); free(pt);
    }
    return 0;
}

static int test_manifest_disjoint_from_chunk(void) {
    sm_state = 0xDECAFULL;
    uint8_t home_key[32]; fill_random(home_key, 32);
    uint8_t pt[128]; fill_random(pt, sizeof(pt));

    uint8_t *ct_chunk = NULL, *ct_manif = NULL;
    size_t l_chunk = 0, l_manif = 0;
    ASSERT_EQ(nh_porthome_encrypt_chunk(home_key, pt, sizeof(pt),
                                        &ct_chunk, &l_chunk, NULL), 0);
    ASSERT_EQ(nh_porthome_encrypt_manifest(home_key, pt, sizeof(pt),
                                           &ct_manif, &l_manif), 0);
    ASSERT_EQ(l_chunk, l_manif);
    /* Different domain-separation salt -> different bytes. */
    ASSERT(memcmp(ct_chunk, ct_manif, l_chunk) != 0);

    /* Wrong purpose -> AEAD failure. */
    uint8_t *pt_back = NULL; size_t pt_back_len = 0;
    int rc = nh_porthome_decrypt_manifest(home_key, ct_chunk, l_chunk, &pt_back, &pt_back_len);
    ASSERT(rc != 0);
    rc = nh_porthome_decrypt_chunk(home_key, ct_manif, l_manif, &pt_back, &pt_back_len);
    ASSERT(rc != 0);

    free(ct_chunk); free(ct_manif);
    return 0;
}

static int test_name_enc(void) {
    sm_state = 0xBEEFULL;
    uint8_t home_key_a[32]; fill_random(home_key_a, 32);
    uint8_t home_key_b[32]; fill_random(home_key_b, 32);

    char e1a[NH_PORTHOME_NAME_HEX_LEN], e2a[NH_PORTHOME_NAME_HEX_LEN], eb[NH_PORTHOME_NAME_HEX_LEN];

    ASSERT_EQ(nh_porthome_encrypt_name(home_key_a, "Documents", e1a), 0);
    ASSERT_EQ(nh_porthome_encrypt_name(home_key_a, "Documents", e2a), 0);
    ASSERT_EQ(memcmp(e1a, e2a, NH_PORTHOME_NAME_HEX_LEN), 0);
    ASSERT_EQ(nh_porthome_encrypt_name(home_key_b, "Documents", eb), 0);
    ASSERT(memcmp(e1a, eb, NH_PORTHOME_NAME_HEX_LEN) != 0);

    /* Rejections. */
    ASSERT(nh_porthome_encrypt_name(home_key_a, "", e1a) != 0);
    ASSERT(nh_porthome_encrypt_name(home_key_a, ".", e1a) != 0);
    ASSERT(nh_porthome_encrypt_name(home_key_a, "..", e1a) != 0);
    ASSERT(nh_porthome_encrypt_name(home_key_a, "with/slash", e1a) != 0);
    ASSERT(nh_porthome_encrypt_name(home_key_a, "back\\slash", e1a) != 0);

    /* Path encryption: valid and invalid cases. */
    char *p = NULL;
    ASSERT_EQ(nh_porthome_encrypt_path(home_key_a, "a/b/c", &p), 0);
    /* 3 components * 48 hex + 2 slashes = 146 chars + NUL. */
    ASSERT_EQ((int)strlen(p), 3 * 48 + 2);
    free(p);

    ASSERT_EQ(nh_porthome_encrypt_path(home_key_a, "/leading/slash", &p), 0);
    free(p);

    ASSERT(nh_porthome_encrypt_path(home_key_a, "trailing/", &p) != 0);
    ASSERT(nh_porthome_encrypt_path(home_key_a, "a//b", &p) != 0);
    ASSERT(nh_porthome_encrypt_path(home_key_a, "a/../b", &p) != 0);
    ASSERT(nh_porthome_encrypt_path(home_key_a, "a/./b", &p) != 0);
    ASSERT(nh_porthome_encrypt_path(home_key_a, "", &p) != 0);
    return 0;
}

static int test_hex_helpers(void) {
    uint8_t bytes[32];
    for (int i = 0; i < 32; i++) bytes[i] = (uint8_t)(0xA0 + i);
    char hex[65];
    nh_porthome_hex64(bytes, hex);
    ASSERT(hex[64] == '\0');
    uint8_t back[32];
    ASSERT_EQ(nh_porthome_from_hex64(hex, back), 0);
    ASSERT_EQ(memcmp(bytes, back, 32), 0);
    /* Too short. */
    ASSERT(nh_porthome_from_hex64("deadbeef", back) != 0);
    /* Non-hex. */
    char bad[65]; memcpy(bad, hex, 65); bad[10] = 'z';
    ASSERT(nh_porthome_from_hex64(bad, back) != 0);
    /* Too long. */
    char longer[66]; memcpy(longer, hex, 64); longer[64] = 'a'; longer[65] = '\0';
    ASSERT(nh_porthome_from_hex64(longer, back) != 0);
    return 0;
}

int main(void) {
    if (test_key_derive_deterministic()) return 1;
    if (test_convergence()) return 1;
    if (test_manifest_disjoint_from_chunk()) return 1;
    if (test_name_enc()) return 1;
    if (test_hex_helpers()) return 1;
    printf("OK porthome_crypto\n");
    return 0;
}
