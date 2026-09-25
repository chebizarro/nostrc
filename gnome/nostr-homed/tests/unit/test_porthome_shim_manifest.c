/*
 * test_porthome_shim_manifest.c — regression test for nostrc-wmb5.
 *
 * The bug: with NOSTR_HOMED_BLOSSOM_PNG_SHIM=1, the pusher's manifest
 * chunk_hash recorded sha256(ct) (the pre-shim ciphertext) while the
 * upload path PUTs sha256(shim||ct) to Blossom. The manifest and the
 * Blossom URL disagreed, and every pull 404'd. See
 * docs/reviews/porthome-jmx0-two-machine-2026-09-25.md Part 2 for the
 * empirical trace.
 *
 * The fix wires cmd_push and the syncd pusher to compute
 * `hanami_blossom_shim_sha256(ct, ctl, addr)` when
 * `hanami_blossom_shim_active()` returns true, so the manifest and the
 * uploader use the same 32-byte address. This test locks that
 * invariant in place at the primitive layer — anything that regresses
 * the shim/no-shim decision or the shim-hash computation trips a
 * check here before it can bite live infrastructure again.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nh_porthome_crypto.h"

#include <hanami/hanami-blossom-shim.h>
#include <hanami/hanami-types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FAIL(...)  do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); return 1; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL("%s:%d assertion failed: %s", __FILE__, __LINE__, #cond); } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) FAIL("%s:%d %s (=%ld) != %s (=%ld)", __FILE__, __LINE__, #a, (long)(a), #b, (long)(b)); } while (0)

/* Deterministic PRNG (splitmix64). */
static uint64_t sm_state;
static uint64_t sm_next(void) {
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static void fill_pseudorandom(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(sm_next() & 0xFF);
}

/* Encrypt one chunk with a known home_key and pseudo-random plaintext.
 * Returns 0 on success; caller frees *out_ct. */
static int encrypt_one_chunk(uint8_t **out_ct, size_t *out_ct_len,
                             uint8_t out_addr[32],
                             const uint8_t home_key[32],
                             uint64_t seed, size_t pt_len)
{
    sm_state = seed;
    uint8_t *pt = (uint8_t *)malloc(pt_len ? pt_len : 1);
    if (!pt) return -1;
    fill_pseudorandom(pt, pt_len);
    int rc = nh_porthome_encrypt_chunk(home_key, pt, pt_len,
                                       out_ct, out_ct_len, out_addr);
    free(pt);
    return rc;
}

/* Case 1: with shim on, the "authoritative manifest hash" (what
 * cmd_push now writes into chs[ci].sha256) MUST equal both
 *   (a) hanami_blossom_shim_sha256(ct, ctl, ...) — the helper the
 *       provisioner + syncd pusher call, AND
 *   (b) sha256 of the encoded shim||ct buffer built independently via
 *       hanami_blossom_shim_encode() + nh_porthome_sha256() — i.e. what
 *       the Blossom upload path actually PUTs and hashes.
 *
 * If either equality breaks, the pull returns 404 for every chunk. */
static int test_shim_on_manifest_matches_upload(void)
{
    if (setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "1", 1) != 0) FAIL("setenv");
    ASSERT(hanami_blossom_shim_active());

    uint8_t home_key[32];
    for (int i = 0; i < 32; i++) home_key[i] = (uint8_t)(0x11 * (i + 1));

    /* One 4 KiB chunk mirrors the design's default file boundary
     * behaviour (the 4-KiB smoke path in porthome-crypto-spec.md D4). */
    uint8_t *ct = NULL; size_t ctl = 0; uint8_t pre_shim_addr[32];
    ASSERT_EQ(encrypt_one_chunk(&ct, &ctl, pre_shim_addr,
                                home_key, 0xC0FFEEULL, 4096), 0);
    ASSERT(ct && ctl > 0);

    /* (a) The helper — what cmd_push + syncd now call to derive the
     * manifest chunk_hash under the shim. */
    uint8_t helper_hash[32];
    ASSERT_EQ(hanami_blossom_shim_sha256(ct, ctl, helper_hash), HANAMI_OK);

    /* (b) The independent reference — build shim||ct as a physical
     * buffer and hash it end-to-end. This mirrors exactly what the
     * Blossom upload path does before it PUTs. */
    size_t enclen = hanami_blossom_shim_encoded_len(ctl);
    uint8_t *shimmed = (uint8_t *)malloc(enclen);
    ASSERT(shimmed);
    ASSERT_EQ(hanami_blossom_shim_encode(ct, ctl, shimmed, enclen), HANAMI_OK);
    uint8_t reference_hash[32];
    ASSERT_EQ(nh_porthome_sha256(shimmed, enclen, reference_hash), 0);
    ASSERT_EQ(memcmp(helper_hash, reference_hash, 32), 0);

    /* And that hash must be different from the pre-shim ciphertext
     * hash — otherwise a caller that "correctly" swapped in the shim
     * hash would still hit the old bug on paper. */
    ASSERT(memcmp(helper_hash, pre_shim_addr, 32) != 0);

    /* Cross-check: nh_porthome_encrypt_chunk's own returned address
     * is still sha256(ct) — the helper does NOT retroactively change
     * what encrypt_chunk returns; the caller (cmd_push / syncd) is
     * responsible for the swap. Guard against a well-meaning
     * refactor that folds the shim into encrypt_chunk itself, which
     * would silently break the "shim off" path. */
    uint8_t ct_hash[32];
    ASSERT_EQ(nh_porthome_sha256(ct, ctl, ct_hash), 0);
    ASSERT_EQ(memcmp(ct_hash, pre_shim_addr, 32), 0);

    free(ct); free(shimmed);
    return 0;
}

/* Case 2: with the shim OFF, the manifest hash MUST equal sha256(ct)
 * bit-for-bit — the pre-wmb5 behaviour. Anything else is a
 * regression of the non-shim path. */
static int test_shim_off_manifest_is_plain_sha(void)
{
    if (unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM") != 0) FAIL("unsetenv");
    ASSERT(!hanami_blossom_shim_active());

    uint8_t home_key[32];
    for (int i = 0; i < 32; i++) home_key[i] = (uint8_t)(0x22 * (i + 3));

    uint8_t *ct = NULL; size_t ctl = 0; uint8_t addr[32];
    ASSERT_EQ(encrypt_one_chunk(&ct, &ctl, addr,
                                home_key, 0xDEADBEEFULL, 4096), 0);
    ASSERT(ct && ctl > 0);

    /* Manifest hash under the "shim off" branch is exactly the
     * address encrypt_chunk returned — sha256(ct). */
    uint8_t plain_hash[32];
    ASSERT_EQ(nh_porthome_sha256(ct, ctl, plain_hash), 0);
    ASSERT_EQ(memcmp(plain_hash, addr, 32), 0);

    /* And critically it must differ from what the shim path would
     * produce — proving the two branches are genuinely independent. */
    uint8_t shim_hash[32];
    ASSERT_EQ(hanami_blossom_shim_sha256(ct, ctl, shim_hash), HANAMI_OK);
    ASSERT(memcmp(plain_hash, shim_hash, 32) != 0);

    free(ct);
    return 0;
}

/* Case 3: guard the value-of "1" contract on the flag. A stray
 * "true" or "0" or empty must NOT enable the shim — otherwise a
 * misconfigured deployment silently switches the hash policy. */
static int test_shim_active_flag_is_strict_one(void)
{
    ASSERT_EQ(setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "true", 1), 0);
    ASSERT(!hanami_blossom_shim_active());

    ASSERT_EQ(setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "0", 1), 0);
    ASSERT(!hanami_blossom_shim_active());

    ASSERT_EQ(setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "", 1), 0);
    ASSERT(!hanami_blossom_shim_active());

    ASSERT_EQ(setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "1", 1), 0);
    ASSERT(hanami_blossom_shim_active());

    ASSERT_EQ(setenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM", "10", 1), 0);
    ASSERT(!hanami_blossom_shim_active());

    ASSERT_EQ(unsetenv("NOSTR_HOMED_BLOSSOM_PNG_SHIM"), 0);
    ASSERT(!hanami_blossom_shim_active());
    return 0;
}

/* Case 4: determinism — two independent shim-hash computations over
 * the same ciphertext MUST produce the same 32 bytes. This preserves
 * D4 convergence through the shim (same file bytes on two machines
 * ⇒ same address on Blossom). */
static int test_shim_hash_is_deterministic(void)
{
    uint8_t home_key[32];
    for (int i = 0; i < 32; i++) home_key[i] = (uint8_t)(i ^ 0x5A);

    uint8_t *ct = NULL; size_t ctl = 0; uint8_t addr[32];
    ASSERT_EQ(encrypt_one_chunk(&ct, &ctl, addr,
                                home_key, 0x1234ULL, 1024), 0);

    uint8_t h1[32], h2[32];
    ASSERT_EQ(hanami_blossom_shim_sha256(ct, ctl, h1), HANAMI_OK);
    ASSERT_EQ(hanami_blossom_shim_sha256(ct, ctl, h2), HANAMI_OK);
    ASSERT_EQ(memcmp(h1, h2, 32), 0);

    /* And a mutation in the ciphertext must move the hash — the
     * helper isn't fabricating a constant. */
    ct[ctl / 2] ^= 0x01;
    uint8_t h3[32];
    ASSERT_EQ(hanami_blossom_shim_sha256(ct, ctl, h3), HANAMI_OK);
    ASSERT(memcmp(h1, h3, 32) != 0);

    free(ct);
    return 0;
}

int main(void)
{
    int failed = 0;
    #define RUN(fn) do { \
        printf("  %-56s", #fn); fflush(stdout); \
        int r = fn(); \
        printf("%s\n", r == 0 ? "OK" : "FAIL"); \
        if (r != 0) failed++; \
    } while (0)

    printf("porthome shim manifest hash tests (nostrc-wmb5)\n");
    printf("================================================\n");
    RUN(test_shim_on_manifest_matches_upload);
    RUN(test_shim_off_manifest_is_plain_sha);
    RUN(test_shim_active_flag_is_strict_one);
    RUN(test_shim_hash_is_deterministic);

    printf("\n%s\n", failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}
