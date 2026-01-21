/**
 * @file test_harness_example.c
 * @brief Example test demonstrating test harness utilities
 *
 * This file shows how to use the nostr_testing library for writing
 * reproducible Nostr protocol tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/testing/test_harness.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"

/* ============================================================================
 * Test: Deterministic Keypairs
 * ============================================================================
 */
static void test_deterministic_keypairs(void) {
    printf("Testing deterministic keypairs...\n");

    /* Get well-known keypairs */
    const NostrTestKeypair *alice = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_ALICE);
    const NostrTestKeypair *bob = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_BOB);
    const NostrTestKeypair *carol = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_CAROL);

    NOSTR_ASSERT_NOT_NULL(alice, "Alice keypair should not be NULL");
    NOSTR_ASSERT_NOT_NULL(bob, "Bob keypair should not be NULL");
    NOSTR_ASSERT_NOT_NULL(carol, "Carol keypair should not be NULL");

    /* Verify keypairs are different */
    NOSTR_ASSERT(strcmp(alice->pubkey_hex, bob->pubkey_hex) != 0,
                 "Alice and Bob should have different pubkeys");
    NOSTR_ASSERT(strcmp(alice->pubkey_hex, carol->pubkey_hex) != 0,
                 "Alice and Carol should have different pubkeys");
    NOSTR_ASSERT(strcmp(bob->pubkey_hex, carol->pubkey_hex) != 0,
                 "Bob and Carol should have different pubkeys");

    /* Verify hex lengths */
    NOSTR_ASSERT_EQ(strlen(alice->privkey_hex), 64, "Private key hex length should be 64");
    NOSTR_ASSERT_EQ(strlen(alice->pubkey_hex), 64, "Public key hex length should be 64");

    /* Verify determinism - getting the same keypair twice should return identical values */
    const NostrTestKeypair *alice2 = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_ALICE);
    NOSTR_ASSERT_STR_EQ(alice->privkey_hex, alice2->privkey_hex,
                        "Same keypair should return same privkey");
    NOSTR_ASSERT_STR_EQ(alice->pubkey_hex, alice2->pubkey_hex,
                        "Same keypair should return same pubkey");

    printf("  Keypairs test passed!\n");
}

/* ============================================================================
 * Test: Event Factories
 * ============================================================================
 */
static void test_event_factories(void) {
    printf("Testing event factories...\n");

    /* Create a text note */
    NostrEvent *note = nostr_test_make_text_note("Hello, Nostr!", 1700000000);
    NOSTR_ASSERT_NOT_NULL(note, "Text note should be created");
    NOSTR_ASSERT_EQ(note->kind, 1, "Text note should be kind 1");
    NOSTR_ASSERT_STR_EQ(note->content, "Hello, Nostr!", "Content should match");
    NOSTR_ASSERT_EQ(note->created_at, 1700000000, "Timestamp should match");
    nostr_event_free(note);

    /* Create metadata */
    NostrEvent *metadata = nostr_test_make_metadata("Alice", "A test user", NULL, 1700000001);
    NOSTR_ASSERT_NOT_NULL(metadata, "Metadata should be created");
    NOSTR_ASSERT_EQ(metadata->kind, 0, "Metadata should be kind 0");
    NOSTR_ASSERT(strstr(metadata->content, "\"name\":\"Alice\"") != NULL,
                 "Metadata should contain name");
    nostr_event_free(metadata);

    /* Create DM (kind 4) */
    const NostrTestKeypair *bob = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_BOB);
    NostrEvent *dm = nostr_test_make_dm("Secret message", bob->pubkey_hex, 4, 1700000002);
    NOSTR_ASSERT_NOT_NULL(dm, "DM should be created");
    NOSTR_ASSERT_EQ(dm->kind, 4, "DM should be kind 4");
    NOSTR_ASSERT_TAG_EXISTS(dm, "p", bob->pubkey_hex);
    nostr_event_free(dm);

    /* Create DM (kind 14 - NIP-17) */
    NostrEvent *sealed_dm = nostr_test_make_dm("Sealed message", bob->pubkey_hex, 14, 1700000003);
    NOSTR_ASSERT_NOT_NULL(sealed_dm, "Sealed DM should be created");
    NOSTR_ASSERT_EQ(sealed_dm->kind, 14, "Sealed DM should be kind 14");
    nostr_event_free(sealed_dm);

    printf("  Event factories test passed!\n");
}

/* ============================================================================
 * Test: Signed Events
 * ============================================================================
 */
static void test_signed_events(void) {
    printf("Testing signed events...\n");

    const NostrTestKeypair *alice = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_ALICE);
    NOSTR_ASSERT_NOT_NULL(alice, "Alice keypair should exist");

    /* Create and sign an event */
    NostrEvent *ev = nostr_test_make_signed_event(1, "Signed note",
                                                   alice->privkey_hex, NULL);
    NOSTR_ASSERT_NOT_NULL(ev, "Signed event should be created");
    NOSTR_ASSERT_EQ(ev->kind, 1, "Kind should be 1");
    NOSTR_ASSERT_STR_EQ(ev->pubkey, alice->pubkey_hex, "Pubkey should match Alice");
    NOSTR_ASSERT_NOT_NULL(ev->sig, "Event should have signature");
    NOSTR_ASSERT_NOT_NULL(ev->id, "Event should have ID");

    /* Verify signature */
    NOSTR_ASSERT_SIG_VALID(ev);

    nostr_event_free(ev);

    /* Create event with tags */
    NostrTags *tags = nostr_tags_new(0);
    NostrTag *t_tag = nostr_tag_new("t", "test", NULL);
    nostr_tags_append(tags, t_tag);

    NostrEvent *ev_with_tags = nostr_test_make_signed_event(1, "Tagged note",
                                                             alice->privkey_hex, tags);
    NOSTR_ASSERT_NOT_NULL(ev_with_tags, "Tagged event should be created");
    NOSTR_ASSERT_TAG_EXISTS(ev_with_tags, "t", "test");
    NOSTR_ASSERT_SIG_VALID(ev_with_tags);

    nostr_event_free(ev_with_tags);

    printf("  Signed events test passed!\n");
}

/* ============================================================================
 * Test: Batch Event Generation
 * ============================================================================
 */
static void test_batch_generation(void) {
    printf("Testing batch event generation...\n");

    /* Generate unsigned events */
    NostrEvent **events = nostr_test_generate_events(5, 1, NULL, 1700000000, 60);
    NOSTR_ASSERT_NOT_NULL(events, "Events array should be created");

    for (size_t i = 0; i < 5; i++) {
        NOSTR_ASSERT_NOT_NULL(events[i], "Each event should exist");
        NOSTR_ASSERT_EQ(events[i]->kind, 1, "Events should be kind 1");
        NOSTR_ASSERT_EQ(events[i]->created_at, 1700000000 + (int64_t)(i * 60),
                        "Timestamps should increment by step");
    }

    nostr_test_free_events(events, 5);

    /* Generate signed events */
    const NostrTestKeypair *alice = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_ALICE);
    NostrEvent **signed_events = nostr_test_generate_signed_events(3, -1, alice,
                                                                    1700000000, 100);
    NOSTR_ASSERT_NOT_NULL(signed_events, "Signed events array should be created");

    for (size_t i = 0; i < 3; i++) {
        NOSTR_ASSERT_NOT_NULL(signed_events[i], "Each signed event should exist");
        NOSTR_ASSERT_STR_EQ(signed_events[i]->pubkey, alice->pubkey_hex,
                            "All events should be from Alice");
        NOSTR_ASSERT_SIG_VALID(signed_events[i]);
    }

    nostr_test_free_events(signed_events, 3);

    printf("  Batch generation test passed!\n");
}

/* ============================================================================
 * Test: Filter Matching Assertions
 * ============================================================================
 */
static void test_filter_assertions(void) {
    printf("Testing filter assertions...\n");

    const NostrTestKeypair *alice = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_ALICE);

    /* Create a signed event */
    NostrEvent *ev = nostr_test_make_signed_event(1, "Test content",
                                                   alice->privkey_hex, NULL);
    NOSTR_ASSERT_NOT_NULL(ev, "Event should be created");

    /* Create filter that should match */
    NostrFilter *filter_match = nostr_filter_new();
    nostr_filter_add_kind(filter_match, 1);
    NOSTR_ASSERT_EVENT_MATCHES(ev, filter_match);

    /* Create filter that should NOT match */
    NostrFilter *filter_no_match = nostr_filter_new();
    nostr_filter_add_kind(filter_no_match, 0);  /* Kind 0, but event is kind 1 */
    NOSTR_ASSERT_EVENT_NOT_MATCHES(ev, filter_no_match);

    /* Filter by author */
    NostrFilter *filter_author = nostr_filter_new();
    nostr_filter_add_author(filter_author, alice->pubkey_hex);
    NOSTR_ASSERT_EVENT_MATCHES(ev, filter_author);

    nostr_filter_free(filter_match);
    nostr_filter_free(filter_no_match);
    nostr_filter_free(filter_author);
    nostr_event_free(ev);

    printf("  Filter assertions test passed!\n");
}

/* ============================================================================
 * Test: Event Equality Assertions
 * ============================================================================
 */
static void test_event_equality(void) {
    printf("Testing event equality assertions...\n");

    const NostrTestKeypair *alice = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_ALICE);

    /* Create two identical events */
    NostrEvent *ev1 = nostr_test_make_signed_event_with_pubkey(
        1, "Same content", alice->privkey_hex, alice->pubkey_hex, NULL, 1700000000);
    NostrEvent *ev2 = nostr_test_make_signed_event_with_pubkey(
        1, "Same content", alice->privkey_hex, alice->pubkey_hex, NULL, 1700000000);

    NOSTR_ASSERT_NOT_NULL(ev1, "Event 1 should be created");
    NOSTR_ASSERT_NOT_NULL(ev2, "Event 2 should be created");

    /* Events should be equal (same kind, content, pubkey, timestamp) */
    NOSTR_ASSERT_EVENT_EQUALS(ev1, ev2);

    nostr_event_free(ev1);
    nostr_event_free(ev2);

    printf("  Event equality test passed!\n");
}

/* ============================================================================
 * Test: Tag Assertions
 * ============================================================================
 */
static void test_tag_assertions(void) {
    printf("Testing tag assertions...\n");

    const NostrTestKeypair *alice = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_ALICE);
    const NostrTestKeypair *bob = nostr_test_keypair_get(NOSTR_TEST_KEYPAIR_BOB);

    /* Create event with multiple tags */
    NostrTags *tags = nostr_tags_new(0);
    NostrTag *p_tag = nostr_tag_new("p", bob->pubkey_hex, NULL);
    NostrTag *t_tag1 = nostr_tag_new("t", "nostr", NULL);
    NostrTag *t_tag2 = nostr_tag_new("t", "test", NULL);
    NostrTag *e_tag = nostr_tag_new("e", "abc123", "wss://relay.example.com", NULL);

    nostr_tags_append(tags, p_tag);
    nostr_tags_append(tags, t_tag1);
    nostr_tags_append(tags, t_tag2);
    nostr_tags_append(tags, e_tag);

    NostrEvent *ev = nostr_test_make_signed_event(1, "Tagged content",
                                                   alice->privkey_hex, tags);
    NOSTR_ASSERT_NOT_NULL(ev, "Tagged event should be created");

    /* Test tag exists assertions */
    NOSTR_ASSERT_TAG_EXISTS(ev, "p", bob->pubkey_hex);
    NOSTR_ASSERT_TAG_EXISTS(ev, "t", "nostr");
    NOSTR_ASSERT_TAG_EXISTS(ev, "t", "test");
    NOSTR_ASSERT_TAG_EXISTS(ev, "e", "abc123");
    NOSTR_ASSERT_TAG_EXISTS(ev, "t", NULL);  /* Any t tag */

    /* Test tag not exists assertions */
    NOSTR_ASSERT_TAG_NOT_EXISTS(ev, "p", alice->pubkey_hex);  /* Wrong pubkey */
    NOSTR_ASSERT_TAG_NOT_EXISTS(ev, "t", "bitcoin");  /* Tag doesn't exist */
    NOSTR_ASSERT_TAG_NOT_EXISTS(ev, "d", NULL);  /* No d tags at all */

    nostr_event_free(ev);

    printf("  Tag assertions test passed!\n");
}

/* ============================================================================
 * Test: Custom Keypair Generation
 * ============================================================================
 */
static void test_custom_keypair(void) {
    printf("Testing custom keypair generation...\n");

    /* Generate deterministic keypair from seed */
    NostrTestKeypair kp1, kp2;
    nostr_test_keypair_from_seed(&kp1, 12345);
    nostr_test_keypair_from_seed(&kp2, 12345);

    /* Same seed should produce same keypair */
    NOSTR_ASSERT_STR_EQ(kp1.privkey_hex, kp2.privkey_hex, "Same seed should give same privkey");
    NOSTR_ASSERT_STR_EQ(kp1.pubkey_hex, kp2.pubkey_hex, "Same seed should give same pubkey");

    /* Different seed should produce different keypair */
    NostrTestKeypair kp3;
    nostr_test_keypair_from_seed(&kp3, 54321);
    NOSTR_ASSERT(strcmp(kp1.privkey_hex, kp3.privkey_hex) != 0,
                 "Different seeds should give different privkeys");

    /* Generate random keypair */
    NostrTestKeypair random_kp;
    nostr_test_generate_keypair(&random_kp);
    NOSTR_ASSERT_EQ(strlen(random_kp.privkey_hex), 64, "Random privkey should be 64 hex chars");
    NOSTR_ASSERT_EQ(strlen(random_kp.pubkey_hex), 64, "Random pubkey should be 64 hex chars");

    printf("  Custom keypair test passed!\n");
}

/* ============================================================================
 * Main
 * ============================================================================
 */
int main(void) {
    printf("=== Test Harness Example ===\n\n");

    test_deterministic_keypairs();
    test_event_factories();
    test_signed_events();
    test_batch_generation();
    test_filter_assertions();
    test_event_equality();
    test_tag_assertions();
    test_custom_keypair();

    printf("\n=== All tests passed! ===\n");
    return 0;
}
