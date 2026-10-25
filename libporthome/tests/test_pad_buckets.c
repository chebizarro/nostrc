/* SPDX-License-Identifier: MIT */
#include "test_helpers.h"
#include <limits.h>

int main(void) {
    /* Published NIP-44 bucket schedule, extended beyond its text-message cap.
     * PCS chunks have no u16 length prefix and support the full 1 MiB bound. */
    const struct { size_t plain, padded; } cases[] = {
        {0,32}, {1,32}, {31,32}, {32,32}, {33,64}, {63,64}, {64,64},
        {65,96}, {95,96}, {96,96}, {97,128}, {255,256}, {256,256},
        {257,320}, {319,320}, {320,320}, {321,384}, {511,512},
        {512,512}, {513,640}, {65535,65536}, {65536,65536},
        {65537,81920}, {262144,262144}, {262145,327680}, {1048576,1048576}
    };
    uint8_t root[32], key[32];
    fixture(root, key);
    porthome_ring_entry_t entry = {.epoch = 1};
    memcpy(entry.node_key, key, 32);
    porthome_ring_t ring = {&entry, 1};
    uint8_t *pt = malloc(PORTHOME_CHUNK_MAX);
    assert(pt);
    for (size_t i = 0; i < PORTHOME_CHUNK_MAX; ++i) pt[i] = (uint8_t)(i % 255 + 1);
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        uint8_t *blob = NULL, *out = NULL;
        size_t len = 0, got = 0;
        size_t n = cases[i].plain;
        assert(porthome_encrypt_chunk_v2(key, pt, n, &blob, &len, NULL) == 0);
        assert(len == cases[i].padded + PORTHOME_SEAL_OVERHEAD);
        assert(porthome_decrypt_chunk_v2(&ring, 1, blob, len, n, &out, &got) == 0);
        assert(got == n && !memcmp(out, pt, n));
        free(out);
        if (n) {
            /* A shorter true length either has a different bucket or exposes
             * authenticated nonzero content as purported padding. */
            assert(porthome_decrypt_chunk_v2(&ring, 1, blob, len, n-1, &out, &got) < 0);
            assert(!out && !got);
        }
        assert(porthome_decrypt_chunk_v2(&ring, 1, blob, len, cases[i].padded+1, &out, &got) < 0);
        assert(!out && !got);
        free(blob);
    }
    /* Appended zeros are real plaintext, not padding: nonce binds the
     * ORIGINAL length/content, even when padded payloads would be identical. */
    uint8_t a[] = {'x'}, b[] = {'x',0}, *one = NULL, *two = NULL;
    size_t one_len = 0, two_len = 0;
    assert(porthome_encrypt_chunk_v2(key, a, sizeof(a), &one, &one_len, NULL) == 0);
    assert(porthome_encrypt_chunk_v2(key, b, sizeof(b), &two, &two_len, NULL) == 0);
    assert(one_len == two_len && memcmp(one, two, one_len));
    free(one); free(two);

    uint8_t dummy = 0, *out = &dummy;
    size_t got = 99;
    assert(porthome_encrypt_chunk_v2(NULL, pt, 1, &out, &got, NULL) == PORTHOME_ERR_ARG);
    assert(!out && !got);
    assert(porthome_encrypt_chunk_v2(key, NULL, 1, &out, &got, NULL) == PORTHOME_ERR_ARG);
    assert(porthome_encrypt_chunk_v2(key, pt, (size_t)INT_MAX+1, &out, &got, NULL) == PORTHOME_ERR_TOO_LARGE);
    assert(!out && !got);
    assert(porthome_encrypt_manifest_v2(key, pt, (size_t)INT_MAX+1, &out, &got, NULL) == PORTHOME_ERR_TOO_LARGE);
    assert(!out && !got);
    assert(porthome_encrypt_manifest_v2(NULL, pt, 1, &out, &got, NULL) == PORTHOME_ERR_ARG);
    assert(!out && !got);
    free(pt);
    return 0;
}
