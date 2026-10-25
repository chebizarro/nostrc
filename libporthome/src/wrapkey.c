/*
 * porthome_wrapkey.c — Phase 2 wrap-seed derivation.
 * SPDX-License-Identifier: MIT
 *
 * See porthome_wrapkey.h. EXPERIMENTAL.
 */

#include <porthome/wrapkey.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <nostr/nip44/kdf.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;  /* uppercase and everything else refused */
}

static int hex64_to_bytes(const char *s, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(s[2*i]);
        int lo = hex_nibble(s[2*i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int porthome_wrap_seed_from_ikm(const uint8_t *ikm, size_t ikm_len,
                                   uint8_t out_seed[PORTHOME_KEY_LEN]) {
    if (!out_seed) return PORTHOME_ERR_ARG;
    if (ikm_len == 0 || (!ikm && ikm_len)) return PORTHOME_ERR_ARG;
    /* Cap IKM length defensively — the local vault case is 32 bytes; a
     * pathological caller passing gigabytes is a bug. */
    if (ikm_len > 4096) return PORTHOME_ERR_ARG;

    uint8_t prk[32];
    if (nostr_nip44_hkdf_extract((const uint8_t *)PORTHOME_WRAPKEY_SALT_LOCAL,
                           sizeof(PORTHOME_WRAPKEY_SALT_LOCAL) - 1,
                           ikm, ikm_len, prk) != 0) {
        OPENSSL_cleanse(out_seed, PORTHOME_KEY_LEN);
        return PORTHOME_ERR_CRYPTO;
    }
    /* info="" — the salt supplies the domain separation. */
    int rc = nostr_nip44_hkdf_expand(prk, NULL, 0, out_seed, PORTHOME_KEY_LEN);
    OPENSSL_cleanse(prk, sizeof(prk));
    if (rc != 0) {
        OPENSSL_cleanse(out_seed, PORTHOME_KEY_LEN);
        return PORTHOME_ERR_CRYPTO;
    }
    return PORTHOME_OK;
}

int porthome_wrap_seed_from_nip44_plaintext(const uint8_t *pt, size_t pt_len,
                                               uint8_t out_seed[PORTHOME_KEY_LEN]) {
    if (!pt || !out_seed) return PORTHOME_ERR_ARG;
    if (pt_len == PORTHOME_KEY_LEN) {
        memcpy(out_seed, pt, PORTHOME_KEY_LEN);
        return PORTHOME_OK;
    }
    if (pt_len == 64) {
        return hex64_to_bytes((const char *)pt, out_seed) == 0
                   ? PORTHOME_OK
                   : PORTHOME_ERR_ARG;
    }
    if (pt_len == 65 && pt[64] == '\0') {
        return hex64_to_bytes((const char *)pt, out_seed) == 0
                   ? PORTHOME_OK
                   : PORTHOME_ERR_ARG;
    }
    return PORTHOME_ERR_ARG;
}

int porthome_wrap_seed_random(uint8_t out_seed[PORTHOME_KEY_LEN]) {
    if (!out_seed) return PORTHOME_ERR_ARG;
    if (RAND_bytes(out_seed, PORTHOME_KEY_LEN) != 1) {
        OPENSSL_cleanse(out_seed, PORTHOME_KEY_LEN);
        return PORTHOME_ERR_CRYPTO;
    }
    return PORTHOME_OK;
}
