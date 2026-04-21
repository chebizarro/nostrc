/**
 * NIP-13: Proof of Work — Unit Tests
 *
 * Tests nip13_difficulty(), nip13_check(), and nip13_generate().
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nip13.h"
#include "nostr-event.h"
#include "nostr-tag.h"

/* ── nip13_difficulty ────────────────────────────────────────────────── */

static void test_difficulty_all_zeros(void) {
    /* 256 leading zero bits (all-zero hash) */
    const char *id = "0000000000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id) == 256);
}

static void test_difficulty_no_zeros(void) {
    /* First nibble is 'f' (1111...) → 0 leading zeros */
    const char *id = "ff00000000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id) == 0);
}

static void test_difficulty_known_values(void) {
    /* "00" = 0x00 → 8 leading zero bits */
    /* "01" = 0x01 → 7 leading zero bits → total 15 */
    const char *id = "000100000000000000000000000000000000000000000000000000000000abcd";
    assert(nip13_difficulty(id) == 15);

    /* "0000" = two zero bytes → 16 leading zero bits */
    const char *id2 = "0000ff0000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id2) == 16);

    /* 0x80 = 1000_0000 → 0 leading zeros → total 16 */
    const char *id3 = "0000800000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id3) == 16);

    /* 0x40 = 0100_0000 → 1 leading zero → total 17 */
    const char *id4 = "0000400000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id4) == 17);

    /* 0x20 = 0010_0000 → 2 leading zeros → total 18 */
    const char *id5 = "0000200000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id5) == 18);
}

static void test_difficulty_single_leading_zero_nibble(void) {
    /* "0f" = 0x0f = 0000_1111 → 4 leading zeros */
    const char *id = "0f00000000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id) == 4);
}

static void test_difficulty_null_input(void) {
    assert(nip13_difficulty(NULL) == -1);
}

static void test_difficulty_short_input(void) {
    assert(nip13_difficulty("00ff") == -1);
    assert(nip13_difficulty("") == -1);
}

static void test_difficulty_invalid_hex(void) {
    /* 'g' is not valid hex */
    const char *id = "gg00000000000000000000000000000000000000000000000000000000000000";
    assert(nip13_difficulty(id) == -1);
}

/* ── nip13_check ─────────────────────────────────────────────────────── */

static void test_check_sufficient(void) {
    /* 16 leading zero bits, require 16 → pass */
    const char *id = "0000ff0000000000000000000000000000000000000000000000000000000000";
    assert(nip13_check(id, 16) == 0);

    /* 16 leading zero bits, require 8 → pass */
    assert(nip13_check(id, 8) == 0);
}

static void test_check_insufficient(void) {
    /* 16 leading zero bits, require 17 → fail */
    const char *id = "0000ff0000000000000000000000000000000000000000000000000000000000";
    assert(nip13_check(id, 17) == NIP13_ERR_DIFFICULTY_TOO_LOW);
}

static void test_check_zero_difficulty(void) {
    /* Any ID satisfies difficulty 0 */
    const char *id = "ff00000000000000000000000000000000000000000000000000000000000000";
    assert(nip13_check(id, 0) == 0);
}

static void test_check_null_input(void) {
    assert(nip13_check(NULL, 8) == -1);
}

/* ── nip13_generate ──────────────────────────────────────────────────── */

static void test_generate_null_event(void) {
    assert(nip13_generate(NULL, 8, 5) == -1);
}

static void test_generate_adds_nonce_tag(void) {
    /*
     * Verify that nip13_generate appends a ["nonce", "<value>", "<target>"]
     * tag to the event. We can't reliably mine without a signing key, so
     * we just check that the tag was added and the function doesn't crash
     * with a short timeout.
     */
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test nip13 generate");

    /* Use an impossibly high difficulty with a 1-second timeout to test
     * the timeout path without waiting long. */
    int rc = nip13_generate(ev, 128, 1);
    /* Either it succeeded (unlikely) or timed out */
    assert(rc == 0 || rc == NIP13_ERR_GENERATE_TIMEOUT);

    /* Regardless, a nonce tag should have been appended */
    NostrTags *tags = nostr_event_get_tags(ev);
    assert(tags != NULL);

    int found_nonce = 0;
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (tag && nostr_tag_size(tag) >= 3) {
            const char *key = nostr_tag_get(tag, 0);
            if (key && strcmp(key, "nonce") == 0) {
                found_nonce = 1;
                /* Third element should be the target difficulty */
                const char *target = nostr_tag_get(tag, 2);
                assert(target != NULL);
                assert(strcmp(target, "128") == 0);
            }
        }
    }
    assert(found_nonce == 1);

    nostr_event_free(ev);
}

int main(void) {
    /* nip13_difficulty */
    test_difficulty_all_zeros();
    test_difficulty_no_zeros();
    test_difficulty_known_values();
    test_difficulty_single_leading_zero_nibble();
    test_difficulty_null_input();
    test_difficulty_short_input();
    test_difficulty_invalid_hex();

    /* nip13_check */
    test_check_sufficient();
    test_check_insufficient();
    test_check_zero_difficulty();
    test_check_null_input();

    /* nip13_generate */
    test_generate_null_event();
    test_generate_adds_nonce_tag();

    printf("nip13 ok\n");
    return 0;
}
