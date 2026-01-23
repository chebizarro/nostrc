/**
 * NIP-59 Test Suite
 *
 * Tests for gift wrap functionality
 */

#include "nostr/nip59/nip59.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-kinds.h"
#include "nostr-tag.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Test keys - generated for testing only */
static char *ALICE_SK = NULL;
static char *ALICE_PK = NULL;
static char *BOB_SK = NULL;
static char *BOB_PK = NULL;

static void setup_keys(void) {
    /* Generate fresh keypairs for each test run */
    ALICE_SK = nostr_key_generate_private();
    assert(ALICE_SK != NULL);
    ALICE_PK = nostr_key_get_public(ALICE_SK);
    assert(ALICE_PK != NULL);

    BOB_SK = nostr_key_generate_private();
    assert(BOB_SK != NULL);
    BOB_PK = nostr_key_get_public(BOB_SK);
    assert(BOB_PK != NULL);

    printf("Test keys generated\n");
}

static void cleanup_keys(void) {
    free(ALICE_SK);
    free(ALICE_PK);
    free(BOB_SK);
    free(BOB_PK);
}

/* Helper: create a simple text note event */
static NostrEvent *create_test_event(const char *pubkey, const char *content) {
    NostrEvent *event = nostr_event_new();
    if (!event) return NULL;

    nostr_event_set_kind(event, NOSTR_KIND_TEXT_NOTE);
    nostr_event_set_pubkey(event, pubkey);
    nostr_event_set_content(event, content);
    nostr_event_set_created_at(event, (int64_t)time(NULL));

    return event;
}

static void test_create_ephemeral_key(void) {
    printf("Testing ephemeral key creation...\n");

    char *sk = NULL;
    char *pk = NULL;

    int rc = nostr_nip59_create_ephemeral_key(&sk, &pk);
    assert(rc == NIP59_OK);
    assert(sk != NULL);
    assert(pk != NULL);

    /* Keys should be 64 hex chars */
    assert(strlen(sk) == 64);
    assert(strlen(pk) == 64);

    /* Keys should be valid */
    assert(nostr_key_is_valid_public_hex(pk));

    /* Derive pubkey from sk and verify it matches */
    char *derived_pk = nostr_key_get_public(sk);
    assert(derived_pk != NULL);
    assert(strcmp(pk, derived_pk) == 0);

    free(sk);
    free(pk);
    free(derived_pk);
    printf("  OK: ephemeral key created and valid\n");
}

static void test_randomize_timestamp(void) {
    printf("Testing timestamp randomization...\n");

    int64_t now = (int64_t)time(NULL);
    uint32_t window = 2 * 24 * 60 * 60;  /* 2 days */

    /* Generate multiple timestamps and verify they're in range */
    for (int i = 0; i < 10; i++) {
        int64_t ts = nostr_nip59_randomize_timestamp(now, window);
        assert(ts <= now);
        assert(ts >= now - (int64_t)window);
    }

    /* Test with zero base time (should use current time) */
    int64_t ts = nostr_nip59_randomize_timestamp(0, window);
    assert(ts <= time(NULL) + 1);  /* Allow 1 second tolerance */
    assert(ts >= time(NULL) - (int64_t)window - 1);

    /* Test with zero window (should use default) */
    ts = nostr_nip59_randomize_timestamp(now, 0);
    assert(ts <= now);

    printf("  OK: timestamps randomized within valid range\n");
}

static void test_wrap_unsigned_event(void) {
    printf("Testing wrapping unsigned event...\n");

    /* Create an unsigned event (like a rumor) */
    NostrEvent *inner = create_test_event(ALICE_PK, "This is an unsigned message");
    assert(inner != NULL);

    /* Wrap it for Bob */
    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    /* Verify gift wrap properties */
    assert(nostr_event_get_kind(gift_wrap) == NOSTR_KIND_GIFT_WRAP);

    /* Gift wrap should be signed */
    assert(nostr_event_get_sig(gift_wrap) != NULL);
    assert(nostr_event_check_signature(gift_wrap));

    /* Pubkey should be ephemeral (not Alice's) */
    assert(strcmp(nostr_event_get_pubkey(gift_wrap), ALICE_PK) != 0);

    /* Should validate */
    assert(nostr_nip59_validate_gift_wrap(gift_wrap));
    assert(nostr_nip59_is_gift_wrap(gift_wrap));

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: unsigned event wrapped correctly\n");
}

static void test_wrap_signed_event(void) {
    printf("Testing wrapping signed event...\n");

    /* Create and sign an event */
    NostrEvent *inner = create_test_event(ALICE_PK, "This is a signed message");
    assert(inner != NULL);
    assert(nostr_event_sign(inner, ALICE_SK) == 0);
    assert(nostr_event_check_signature(inner));

    /* Wrap it for Bob */
    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    assert(nostr_nip59_validate_gift_wrap(gift_wrap));

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: signed event wrapped correctly\n");
}

static void test_wrap_with_provided_key(void) {
    printf("Testing wrapping with provided ephemeral key...\n");

    NostrEvent *inner = create_test_event(ALICE_PK, "Test message");
    assert(inner != NULL);

    /* Generate our own ephemeral key */
    char *eph_sk = NULL;
    char *eph_pk = NULL;
    int rc = nostr_nip59_create_ephemeral_key(&eph_sk, &eph_pk);
    assert(rc == NIP59_OK);

    /* Wrap with provided key */
    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, eph_sk);
    assert(gift_wrap != NULL);

    /* Gift wrap pubkey should match our ephemeral key */
    assert(strcmp(nostr_event_get_pubkey(gift_wrap), eph_pk) == 0);

    free(eph_sk);
    free(eph_pk);
    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: custom ephemeral key used correctly\n");
}

static void test_unwrap_gift_wrap(void) {
    printf("Testing gift wrap unwrapping...\n");

    const char *original_content = "Secret message for unwrapping";

    NostrEvent *inner = create_test_event(ALICE_PK, original_content);
    assert(inner != NULL);

    /* Wrap for Bob */
    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    /* Bob unwraps */
    NostrEvent *unwrapped = nostr_nip59_unwrap(gift_wrap, BOB_SK);
    assert(unwrapped != NULL);

    /* Verify content matches */
    assert(strcmp(nostr_event_get_content(unwrapped), original_content) == 0);

    /* Verify pubkey matches */
    assert(strcmp(nostr_event_get_pubkey(unwrapped), ALICE_PK) == 0);

    /* Verify kind matches */
    assert(nostr_event_get_kind(unwrapped) == NOSTR_KIND_TEXT_NOTE);

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    nostr_event_free(unwrapped);
    printf("  OK: gift wrap unwrapped correctly\n");
}

static void test_unwrap_signed_event(void) {
    printf("Testing unwrap preserves signature...\n");

    NostrEvent *inner = create_test_event(ALICE_PK, "Signed content");
    assert(inner != NULL);
    assert(nostr_event_sign(inner, ALICE_SK) == 0);

    const char *original_sig = nostr_event_get_sig(inner);
    assert(original_sig != NULL);
    char *sig_copy = strdup(original_sig);

    /* Wrap and unwrap */
    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    NostrEvent *unwrapped = nostr_nip59_unwrap(gift_wrap, BOB_SK);
    assert(unwrapped != NULL);

    /* Signature should be preserved */
    const char *unwrapped_sig = nostr_event_get_sig(unwrapped);
    assert(unwrapped_sig != NULL);
    assert(strcmp(unwrapped_sig, sig_copy) == 0);

    /* Signature should still verify */
    assert(nostr_event_check_signature(unwrapped));

    free(sig_copy);
    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    nostr_event_free(unwrapped);
    printf("  OK: signature preserved through wrap/unwrap\n");
}

static void test_wrong_recipient_fails(void) {
    printf("Testing wrong recipient cannot unwrap...\n");

    NostrEvent *inner = create_test_event(ALICE_PK, "Only for Bob");
    assert(inner != NULL);

    /* Wrap for Bob */
    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    /* Alice (wrong recipient) tries to unwrap */
    NostrEvent *unwrapped = nostr_nip59_unwrap(gift_wrap, ALICE_SK);

    /* Should fail - decryption should not work */
    assert(unwrapped == NULL);

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: wrong recipient cannot unwrap\n");
}

static void test_wrap_different_kinds(void) {
    printf("Testing wrapping different event kinds...\n");

    /* Test wrapping various event kinds */
    int test_kinds[] = {
        NOSTR_KIND_TEXT_NOTE,      /* 1 */
        NOSTR_KIND_SEAL,           /* 13 */
        NOSTR_KIND_DIRECT_MESSAGE, /* 14 */
        NOSTR_KIND_REACTION,       /* 7 */
        NOSTR_KIND_DELETION,       /* 5 */
    };
    int num_kinds = sizeof(test_kinds) / sizeof(test_kinds[0]);

    for (int i = 0; i < num_kinds; i++) {
        NostrEvent *inner = nostr_event_new();
        assert(inner != NULL);
        nostr_event_set_kind(inner, test_kinds[i]);
        nostr_event_set_pubkey(inner, ALICE_PK);
        nostr_event_set_content(inner, "Test content");
        nostr_event_set_created_at(inner, (int64_t)time(NULL));

        NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
        assert(gift_wrap != NULL);

        NostrEvent *unwrapped = nostr_nip59_unwrap(gift_wrap, BOB_SK);
        assert(unwrapped != NULL);

        /* Kind should be preserved */
        assert(nostr_event_get_kind(unwrapped) == test_kinds[i]);

        nostr_event_free(inner);
        nostr_event_free(gift_wrap);
        nostr_event_free(unwrapped);
    }

    printf("  OK: all event kinds wrapped/unwrapped correctly\n");
}

static void test_validate_gift_wrap(void) {
    printf("Testing gift wrap validation...\n");

    NostrEvent *inner = create_test_event(ALICE_PK, "Test");
    assert(inner != NULL);

    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    /* Valid gift wrap */
    assert(nostr_nip59_validate_gift_wrap(gift_wrap));

    /* NULL should be invalid */
    assert(!nostr_nip59_validate_gift_wrap(NULL));

    /* Wrong kind should be invalid - modify in place like NIP-17 tests */
    nostr_event_set_kind(gift_wrap, NOSTR_KIND_TEXT_NOTE);
    assert(!nostr_nip59_validate_gift_wrap(gift_wrap));

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: validation works correctly\n");
}

static void test_get_recipient(void) {
    printf("Testing get recipient from gift wrap...\n");

    NostrEvent *inner = create_test_event(ALICE_PK, "Test");
    assert(inner != NULL);

    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    char *recipient = nostr_nip59_get_recipient(gift_wrap);
    assert(recipient != NULL);
    assert(strcmp(recipient, BOB_PK) == 0);

    free(recipient);
    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: recipient extracted correctly\n");
}

static void test_is_gift_wrap(void) {
    printf("Testing is_gift_wrap check...\n");

    NostrEvent *inner = create_test_event(ALICE_PK, "Test");
    assert(inner != NULL);

    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    /* Gift wrap should be detected */
    assert(nostr_nip59_is_gift_wrap(gift_wrap));

    /* Regular event should not be */
    assert(!nostr_nip59_is_gift_wrap(inner));

    /* NULL should not be */
    assert(!nostr_nip59_is_gift_wrap(NULL));

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: is_gift_wrap detection works\n");
}

static void test_roundtrip_with_tags(void) {
    printf("Testing roundtrip preserves tags...\n");

    NostrEvent *inner = nostr_event_new();
    assert(inner != NULL);
    nostr_event_set_kind(inner, NOSTR_KIND_TEXT_NOTE);
    nostr_event_set_pubkey(inner, ALICE_PK);
    nostr_event_set_content(inner, "Message with tags");
    nostr_event_set_created_at(inner, (int64_t)time(NULL));

    /* Add some tags */
    NostrTag *ptag = nostr_tag_new("p", BOB_PK, NULL);
    NostrTag *etag = nostr_tag_new("e", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", NULL);
    NostrTags *tags = nostr_tags_new(2, ptag, etag);
    nostr_event_set_tags(inner, tags);

    /* Wrap and unwrap */
    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    NostrEvent *unwrapped = nostr_nip59_unwrap(gift_wrap, BOB_SK);
    assert(unwrapped != NULL);

    /* Tags should be preserved */
    NostrTags *unwrapped_tags = nostr_event_get_tags(unwrapped);
    assert(unwrapped_tags != NULL);
    assert(nostr_tags_size(unwrapped_tags) == 2);

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    nostr_event_free(unwrapped);
    printf("  OK: tags preserved through roundtrip\n");
}

static void test_timestamp_is_randomized(void) {
    printf("Testing gift wrap timestamp is randomized...\n");

    NostrEvent *inner = create_test_event(ALICE_PK, "Test");
    assert(inner != NULL);

    int64_t before = (int64_t)time(NULL);

    NostrEvent *gift_wrap = nostr_nip59_wrap(inner, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    int64_t gift_wrap_time = nostr_event_get_created_at(gift_wrap);

    /* Timestamp should be in the past (randomized) */
    assert(gift_wrap_time <= before + 1);  /* Allow 1 sec tolerance */

    /* Should be within 2 days window */
    int64_t two_days = 2 * 24 * 60 * 60;
    assert(gift_wrap_time >= before - two_days - 1);

    nostr_event_free(inner);
    nostr_event_free(gift_wrap);
    printf("  OK: timestamp is randomized\n");
}

static void test_nip17_compatibility(void) {
    printf("Testing NIP-17 compatibility...\n");

    /* Create a seal-like event (kind 13) */
    NostrEvent *seal = nostr_event_new();
    assert(seal != NULL);
    nostr_event_set_kind(seal, NOSTR_KIND_SEAL);
    nostr_event_set_pubkey(seal, ALICE_PK);
    nostr_event_set_content(seal, "Encrypted rumor content would go here");
    nostr_event_set_created_at(seal, (int64_t)time(NULL));
    assert(nostr_event_sign(seal, ALICE_SK) == 0);

    /* Gift wrap the seal (as NIP-17 does) */
    NostrEvent *gift_wrap = nostr_nip59_wrap(seal, BOB_PK, NULL);
    assert(gift_wrap != NULL);

    /* Verify it's a valid gift wrap */
    assert(nostr_nip59_validate_gift_wrap(gift_wrap));
    assert(nostr_event_get_kind(gift_wrap) == NOSTR_KIND_GIFT_WRAP);

    /* Bob unwraps to get the seal */
    NostrEvent *unwrapped_seal = nostr_nip59_unwrap(gift_wrap, BOB_SK);
    assert(unwrapped_seal != NULL);

    /* Verify it's the seal */
    assert(nostr_event_get_kind(unwrapped_seal) == NOSTR_KIND_SEAL);
    assert(strcmp(nostr_event_get_pubkey(unwrapped_seal), ALICE_PK) == 0);

    /* Seal signature should verify */
    assert(nostr_event_check_signature(unwrapped_seal));

    nostr_event_free(seal);
    nostr_event_free(gift_wrap);
    nostr_event_free(unwrapped_seal);
    printf("  OK: compatible with NIP-17 seal wrapping\n");
}

int main(void) {
    printf("NIP-59 Test Suite\n");
    printf("=================\n\n");

    setup_keys();

    /* Ephemeral key tests */
    test_create_ephemeral_key();

    /* Timestamp tests */
    test_randomize_timestamp();

    /* Wrap tests */
    test_wrap_unsigned_event();
    test_wrap_signed_event();
    test_wrap_with_provided_key();
    test_wrap_different_kinds();

    /* Unwrap tests */
    test_unwrap_gift_wrap();
    test_unwrap_signed_event();
    test_wrong_recipient_fails();

    /* Validation tests */
    test_validate_gift_wrap();
    test_get_recipient();
    test_is_gift_wrap();

    /* Roundtrip tests */
    test_roundtrip_with_tags();
    test_timestamp_is_randomized();

    /* Compatibility tests */
    test_nip17_compatibility();

    cleanup_keys();

    printf("\n=================\n");
    printf("All tests passed!\n");
    return 0;
}
