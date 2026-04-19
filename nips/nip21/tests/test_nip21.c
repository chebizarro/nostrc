#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nostr/nip21/nip21.h"

static int hex2bin(const char *hex, uint8_t *out, size_t out_len) {
    size_t n = strlen(hex);
    if (n % 2 || out_len < n / 2) return -1;
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned int v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* Known test vector from NIP-19 spec */
static const char *TEST_NPUB = "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg";
static const char *TEST_PUBKEY_HEX = "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e";

static void test_is_uri_valid(void) {
    /* Valid nostr: URIs */
    char uri[256];
    snprintf(uri, sizeof(uri), "nostr:%s", TEST_NPUB);
    assert(nostr_nip21_is_uri(uri));

    /* Case-insensitive prefix */
    snprintf(uri, sizeof(uri), "NOSTR:%s", TEST_NPUB);
    assert(nostr_nip21_is_uri(uri));

    snprintf(uri, sizeof(uri), "Nostr:%s", TEST_NPUB);
    assert(nostr_nip21_is_uri(uri));
}

static void test_is_uri_invalid(void) {
    assert(!nostr_nip21_is_uri(NULL));
    assert(!nostr_nip21_is_uri(""));
    assert(!nostr_nip21_is_uri("nostr:"));
    assert(!nostr_nip21_is_uri("nostr:garbage"));
    assert(!nostr_nip21_is_uri("http://example.com"));
    assert(!nostr_nip21_is_uri(TEST_NPUB));  /* Missing prefix */
    assert(!nostr_nip21_is_uri("nostr: "));
}

static void test_parse_npub(void) {
    char uri[256];
    snprintf(uri, sizeof(uri), "nostr:%s", TEST_NPUB);

    NostrBech32Type type = NOSTR_B32_UNKNOWN;
    const char *bech32 = NULL;
    assert(nostr_nip21_parse(uri, &type, &bech32) == 0);
    assert(type == NOSTR_B32_NPUB);
    assert(bech32 != NULL);
    assert(strcmp(bech32, TEST_NPUB) == 0);
    /* bech32 should point into uri (borrowed) */
    assert(bech32 == uri + NOSTR_NIP21_PREFIX_LEN);
}

static void test_parse_invalid(void) {
    NostrBech32Type type;
    const char *bech32;

    assert(nostr_nip21_parse(NULL, &type, &bech32) == -EINVAL);
    assert(nostr_nip21_parse("garbage", &type, &bech32) == -EINVAL);
    assert(nostr_nip21_parse("nostr:", &type, &bech32) == -EINVAL);
    assert(nostr_nip21_parse("nostr:invalid", &type, &bech32) == -EINVAL);
}

static void test_build_from_bech32(void) {
    char *uri = nostr_nip21_build(TEST_NPUB);
    assert(uri != NULL);

    char expected[256];
    snprintf(expected, sizeof(expected), "nostr:%s", TEST_NPUB);
    assert(strcmp(uri, expected) == 0);

    free(uri);

    /* NULL / empty */
    assert(nostr_nip21_build(NULL) == NULL);
    assert(nostr_nip21_build("") == NULL);
}

static void test_build_npub(void) {
    uint8_t pubkey[32];
    assert(hex2bin(TEST_PUBKEY_HEX, pubkey, 32) == 0);

    char *uri = nostr_nip21_build_npub(pubkey);
    assert(uri != NULL);

    /* Should start with nostr:npub */
    assert(strncmp(uri, "nostr:npub", 10) == 0);

    /* Should parse back correctly */
    NostrBech32Type type;
    const char *bech32;
    assert(nostr_nip21_parse(uri, &type, &bech32) == 0);
    assert(type == NOSTR_B32_NPUB);

    /* Decode the bech32 and verify it matches the original pubkey */
    uint8_t decoded[32];
    assert(nostr_nip19_decode_npub(bech32, decoded) == 0);
    assert(memcmp(decoded, pubkey, 32) == 0);

    free(uri);
}

static void test_build_note(void) {
    uint8_t event_id[32];
    for (size_t i = 0; i < 32; ++i) event_id[i] = (uint8_t)i;

    char *uri = nostr_nip21_build_note(event_id);
    assert(uri != NULL);
    assert(strncmp(uri, "nostr:note", 10) == 0);

    /* Roundtrip */
    NostrBech32Type type;
    const char *bech32;
    assert(nostr_nip21_parse(uri, &type, &bech32) == 0);
    assert(type == NOSTR_B32_NOTE);

    uint8_t decoded[32];
    assert(nostr_nip19_decode_note(bech32, decoded) == 0);
    assert(memcmp(decoded, event_id, 32) == 0);

    free(uri);
}

static void test_build_nprofile(void) {
    uint8_t pubkey[32];
    assert(hex2bin(TEST_PUBKEY_HEX, pubkey, 32) == 0);

    NostrProfilePointer *p = nostr_profile_pointer_new();
    assert(p);
    p->public_key = strdup(TEST_PUBKEY_HEX);

    char *uri = nostr_nip21_build_nprofile(p);
    assert(uri != NULL);
    assert(strncmp(uri, "nostr:nprofile", 14) == 0);

    /* Parse back */
    NostrBech32Type type;
    const char *bech32;
    assert(nostr_nip21_parse(uri, &type, &bech32) == 0);
    assert(type == NOSTR_B32_NPROFILE);

    free(uri);
    nostr_profile_pointer_free(p);
}

static void test_build_nevent(void) {
    NostrEventPointer *e = nostr_event_pointer_new();
    assert(e);
    /* 64-char hex event ID */
    e->id = strdup("46d731680add2990efe1cc619dc9b8014feeb23261ab9dee50e9d11814de5a2b");

    char *uri = nostr_nip21_build_nevent(e);
    assert(uri != NULL);
    assert(strncmp(uri, "nostr:nevent", 12) == 0);

    NostrBech32Type type;
    const char *bech32;
    assert(nostr_nip21_parse(uri, &type, &bech32) == 0);
    assert(type == NOSTR_B32_NEVENT);

    free(uri);
    nostr_event_pointer_free(e);
}

static void test_build_null_inputs(void) {
    assert(nostr_nip21_build_npub(NULL) == NULL);
    assert(nostr_nip21_build_note(NULL) == NULL);
    assert(nostr_nip21_build_nprofile(NULL) == NULL);
    assert(nostr_nip21_build_nevent(NULL) == NULL);
    assert(nostr_nip21_build_naddr(NULL) == NULL);
}

static void test_roundtrip_npub(void) {
    /* Full roundtrip: hex → npub URI → parse → decode → hex */
    uint8_t pubkey[32];
    assert(hex2bin(TEST_PUBKEY_HEX, pubkey, 32) == 0);

    char *uri = nostr_nip21_build_npub(pubkey);
    assert(uri != NULL);

    assert(nostr_nip21_is_uri(uri));

    NostrBech32Type type;
    const char *bech32;
    assert(nostr_nip21_parse(uri, &type, &bech32) == 0);
    assert(type == NOSTR_B32_NPUB);

    uint8_t decoded[32];
    assert(nostr_nip19_decode_npub(bech32, decoded) == 0);
    assert(memcmp(decoded, pubkey, 32) == 0);

    free(uri);
}

int main(void) {
    test_is_uri_valid();
    test_is_uri_invalid();
    test_parse_npub();
    test_parse_invalid();
    test_build_from_bech32();
    test_build_npub();
    test_build_note();
    test_build_nprofile();
    test_build_nevent();
    test_build_null_inputs();
    test_roundtrip_npub();
    printf("nip21 ok\n");
    return 0;
}
