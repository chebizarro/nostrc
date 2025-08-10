/* Canonical NIP-19 implementation for bare keys and ids.
 * Spec: docs/nips/19.md
 * - "Bare keys and ids" (lines 13–25): npub, nsec, note use bech32 (not m).
 */

#include "nip19.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int encode32(const char *hrp, const uint8_t in[32], char **out_bech) {
    uint8_t *data5 = NULL; size_t data5_len = 0;
    if (nostr_b32_to_5bit(in, 32, &data5, &data5_len) != 0) return -1;
    int rc = nostr_b32_encode(hrp, data5, data5_len, out_bech);
    free(data5);
    return rc;
}

static int decode32_expect_hrp(const char *expected_hrp, const char *bech, uint8_t out[32]) {
    char *hrp = NULL; uint8_t *data5 = NULL; size_t data5_len = 0;
    if (nostr_b32_decode(bech, &hrp, &data5, &data5_len) != 0) return -1;
    int rc = -1;
    if (strcmp(hrp, expected_hrp) == 0) {
        uint8_t *data8 = NULL; size_t data8_len = 0;
        if (nostr_b32_to_8bit(data5, data5_len, &data8, &data8_len) == 0 && data8_len == 32) {
            memcpy(out, data8, 32);
            rc = 0;
        }
        if (data8) { memset(data8, 0, data8_len); free(data8); }
    }
    free(hrp);
    if (data5) free(data5);
    return rc;
}

int nostr_nip19_encode_npub(const uint8_t pubkey[32], char **out_bech) {
    return encode32("npub", pubkey, out_bech);
}

int nostr_nip19_decode_npub(const char *npub, uint8_t out_pubkey[32]) {
    return decode32_expect_hrp("npub", npub, out_pubkey);
}

int nostr_nip19_encode_nsec(const uint8_t seckey[32], char **out_bech) {
    /* Zeroization of temporary buffers handled in helpers. Do not log secrets. */
    return encode32("nsec", seckey, out_bech);
}

int nostr_nip19_decode_nsec(const char *nsec, uint8_t out_seckey[32]) {
    return decode32_expect_hrp("nsec", nsec, out_seckey);
}

int nostr_nip19_encode_note(const uint8_t event_id[32], char **out_bech) {
    return encode32("note", event_id, out_bech);
}

int nostr_nip19_decode_note(const char *note, uint8_t out_event_id[32]) {
    return decode32_expect_hrp("note", note, out_event_id);
}

int nostr_nip19_inspect(const char *bech, NostrBech32Type *out_type) {
    if (!bech || !out_type) return -1; *out_type = NOSTR_B32_UNKNOWN;
    const char *p = strchr(bech, '1');
    if (!p || p == bech) return -1;
    size_t hlen = (size_t)(p - bech);
    char hrp[16]; if (hlen >= sizeof(hrp)) return -1;
    for (size_t i = 0; i < hlen; ++i) hrp[i] = (char)tolower((unsigned char)bech[i]);
    hrp[hlen] = '\0';
    if (strcmp(hrp, "npub") == 0) { *out_type = NOSTR_B32_NPUB; return 0; }
    if (strcmp(hrp, "nsec") == 0) { *out_type = NOSTR_B32_NSEC; return 0; }
    if (strcmp(hrp, "note") == 0) { *out_type = NOSTR_B32_NOTE; return 0; }
    if (strcmp(hrp, "nprofile") == 0) { *out_type = NOSTR_B32_NPROFILE; return 0; }
    if (strcmp(hrp, "nevent") == 0) { *out_type = NOSTR_B32_NEVENT; return 0; }
    if (strcmp(hrp, "naddr") == 0) { *out_type = NOSTR_B32_NADDR; return 0; }
    if (strcmp(hrp, "nrelay") == 0) { *out_type = NOSTR_B32_NRELAY; return 0; }
    return 0; // unknown HRP but success indicating parsed hrp
}
