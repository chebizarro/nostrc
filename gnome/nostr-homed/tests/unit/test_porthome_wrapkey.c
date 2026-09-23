/*
 * test_porthome_wrapkey.c — vectors for nh_porthome_wrap_seed_from_ikm
 * and the nip44_plaintext parser. Bead nostrc-ck6i.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nh_porthome_wrapkey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ck(int cond, const char *tag) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", tag); exit(1); }
    printf("PASS: %s\n", tag);
    return 0;
}

/* Fixed 32-byte private key. */
static const uint8_t FIXED_PRIV[32] = {
    0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00,0x11,
    0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
};

int main(void) {
    /* 1. Determinism: same IKM -> same seed. */
    uint8_t s1[32], s2[32];
    ck(nh_porthome_wrap_seed_from_ikm(FIXED_PRIV, 32, s1) == 0, "derive #1");
    ck(nh_porthome_wrap_seed_from_ikm(FIXED_PRIV, 32, s2) == 0, "derive #2");
    ck(memcmp(s1, s2, 32) == 0, "determinism");

    /* 2. Non-zero: HKDF output is never all-zero for a non-zero ikm. */
    int all_zero = 1; for (int i = 0; i < 32; i++) if (s1[i]) { all_zero = 0; break; }
    ck(!all_zero, "non-zero seed");

    /* 3. Different IKM -> different seed. */
    uint8_t alt[32];
    memcpy(alt, FIXED_PRIV, 32); alt[0] ^= 1;
    uint8_t s3[32];
    ck(nh_porthome_wrap_seed_from_ikm(alt, 32, s3) == 0, "derive alt");
    ck(memcmp(s1, s3, 32) != 0, "alt seed differs");

    /* 4. NIP-46 raw 32-byte plaintext accepted. */
    uint8_t nseed[32];
    ck(nh_porthome_wrap_seed_from_nip44_plaintext(FIXED_PRIV, 32, nseed) == 0,
       "nip44 raw 32");
    ck(memcmp(nseed, FIXED_PRIV, 32) == 0, "nip44 raw copy");

    /* 5. NIP-46 64-hex plaintext accepted. */
    const char *hex64 = "aabbccddeeff00112233445566778899"
                        "0102030405060708090a0b0c0d0e0f10";
    uint8_t hseed[32];
    ck(nh_porthome_wrap_seed_from_nip44_plaintext((const uint8_t *)hex64, 64,
                                                  hseed) == 0,
       "nip44 hex64");
    ck(memcmp(hseed, FIXED_PRIV, 32) == 0, "nip44 hex64 decodes");

    /* 6. NIP-46 65-byte NUL-terminated hex plaintext accepted. */
    char hex65[65];
    memcpy(hex65, hex64, 64); hex65[64] = '\0';
    ck(nh_porthome_wrap_seed_from_nip44_plaintext((const uint8_t *)hex65, 65,
                                                  hseed) == 0,
       "nip44 hex65 NUL");
    ck(memcmp(hseed, FIXED_PRIV, 32) == 0, "nip44 hex65 NUL decodes");

    /* 7. Reject wrong lengths / non-hex. */
    uint8_t junk;
    ck(nh_porthome_wrap_seed_from_nip44_plaintext(&junk, 1, hseed) != 0,
       "reject 1-byte");
    ck(nh_porthome_wrap_seed_from_nip44_plaintext((const uint8_t *)"NNNN", 4,
                                                  hseed) != 0,
       "reject 4-byte");
    const char *bad_hex = "xxxxccddeeff00112233445566778899"
                          "0102030405060708090a0b0c0d0e0f10";
    ck(nh_porthome_wrap_seed_from_nip44_plaintext((const uint8_t *)bad_hex, 64,
                                                  hseed) != 0,
       "reject non-hex 64");

    /* 8. Argument-check paths. */
    ck(nh_porthome_wrap_seed_from_ikm(NULL, 32, s1) != 0, "reject null ikm");
    ck(nh_porthome_wrap_seed_from_ikm(FIXED_PRIV, 0, s1) != 0, "reject zero ikm_len");

    /* 9. Random seed is generated and non-zero. */
    uint8_t r[32];
    ck(nh_porthome_wrap_seed_random(r) == 0, "random seed");
    all_zero = 1; for (int i = 0; i < 32; i++) if (r[i]) { all_zero = 0; break; }
    ck(!all_zero, "random seed non-zero");

    printf("OK test_porthome_wrapkey\n");
    return 0;
}
