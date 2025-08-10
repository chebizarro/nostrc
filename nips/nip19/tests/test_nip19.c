#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nip19.h"

static int hex2bin(const char *hex, uint8_t *out, size_t out_len) {
    size_t n = strlen(hex);
    if (n % 2 || out_len < n/2) return -1;
    for (size_t i = 0; i < n/2; ++i) {
        unsigned int v; if (sscanf(hex + 2*i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v;
    }
    return 0;
}

static int test_vector_npub(void) {
    const char *bech = "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg"; // docs/nips/19.md line 59
    const char *hex =  "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e";
    uint8_t pk[32]; if (hex2bin(hex, pk, sizeof pk) != 0) return -1;
    char *enc = NULL; if (nostr_nip19_encode_npub(pk, &enc) != 0) return -1;
    int ok = strcmp(enc, bech) == 0; free(enc);
    uint8_t dec[32]; if (nostr_nip19_decode_npub(bech, dec) != 0) return -1;
    ok &= (memcmp(dec, pk, 32) == 0);
    return ok ? 0 : -1;
}

static int test_vector_nsec(void) {
    const char *bech = "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5"; // docs/nips/19.md line 60
    const char *hex =  "67dea2ed018072d675f5415ecfaed7d2597555e202d85b3d65ea4e58d2d92ffa";
    uint8_t sk[32]; if (hex2bin(hex, sk, sizeof sk) != 0) return -1;
    char *enc = NULL; if (nostr_nip19_encode_nsec(sk, &enc) != 0) return -1;
    int ok = strcmp(enc, bech) == 0; free(enc);
    uint8_t dec[32]; if (nostr_nip19_decode_nsec(bech, dec) != 0) return -1;
    ok &= (memcmp(dec, sk, 32) == 0);
    memset(dec, 0, sizeof dec); memset(sk, 0, sizeof sk);
    return ok ? 0 : -1;
}

static int test_roundtrip_note(void) {
    uint8_t id[32]; for (size_t i=0;i<32;++i) id[i] = (uint8_t)i;
    char *enc = NULL; if (nostr_nip19_encode_note(id, &enc) != 0) return -1;
    uint8_t dec[32]; if (nostr_nip19_decode_note(enc, dec) != 0) { free(enc); return -1; }
    int ok = memcmp(dec, id, 32) == 0; free(enc); return ok ? 0 : -1;
}

static int test_inspect(void) {
    NostrBech32Type t;
    if (nostr_nip19_inspect("npub1xyz1qqqqqq", &t) != 0) return -1; if (t != NOSTR_B32_NPUB) return -1;
    if (nostr_nip19_inspect("nevent1xyz1qqq", &t) != 0) return -1; if (t != NOSTR_B32_NEVENT) return -1;
    return 0;
}

int main(void) {
    int rc = 0;
    if ((rc = test_vector_npub()) != 0) { fprintf(stderr, "npub vector failed\n"); return 1; }
    if ((rc = test_vector_nsec()) != 0) { fprintf(stderr, "nsec vector failed\n"); return 1; }
    if ((rc = test_roundtrip_note()) != 0) { fprintf(stderr, "note roundtrip failed\n"); return 1; }
    if ((rc = test_inspect()) != 0) { fprintf(stderr, "inspect failed\n"); return 1; }
    printf("test_nip19: OK\n");
    return 0;
}
