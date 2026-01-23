#include "nostr/nip18/nip18.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test data */
static const unsigned char TEST_EVENT_ID[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

static const unsigned char TEST_PUBKEY[32] = {
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99
};

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) do { \
    test_count++; \
    printf("Running: %s... ", #name); \
} while(0)

#define PASS() do { \
    pass_count++; \
    printf("PASSED\n"); \
} while(0)

#define FAIL(msg) do { \
    printf("FAILED: %s\n", msg); \
} while(0)

static void test_create_repost_from_id(void) {
    TEST(test_create_repost_from_id);

    NostrEvent *repost = nostr_nip18_create_repost_from_id(
        TEST_EVENT_ID,
        TEST_PUBKEY,
        "wss://relay.example.com",
        NULL
    );

    if (!repost) {
        FAIL("Failed to create repost event");
        return;
    }

    /* Check kind is 6 */
    int kind = nostr_event_get_kind(repost);
    if (kind != 6) {
        FAIL("Expected kind 6");
        nostr_event_free(repost);
        return;
    }

    /* Check tags contain e and p */
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(repost);
    if (!tags || nostr_tags_size(tags) < 2) {
        FAIL("Expected at least 2 tags");
        nostr_event_free(repost);
        return;
    }

    /* Check e-tag */
    NostrTag *e_tag = nostr_tags_get(tags, 0);
    if (!e_tag || strcmp(nostr_tag_get(e_tag, 0), "e") != 0) {
        FAIL("First tag should be 'e'");
        nostr_event_free(repost);
        return;
    }

    /* Check relay hint in e-tag */
    if (nostr_tag_size(e_tag) < 3 || strcmp(nostr_tag_get(e_tag, 2), "wss://relay.example.com") != 0) {
        FAIL("e-tag should have relay hint");
        nostr_event_free(repost);
        return;
    }

    /* Check p-tag */
    NostrTag *p_tag = nostr_tags_get(tags, 1);
    if (!p_tag || strcmp(nostr_tag_get(p_tag, 0), "p") != 0) {
        FAIL("Second tag should be 'p'");
        nostr_event_free(repost);
        return;
    }

    nostr_event_free(repost);
    PASS();
}

static void test_create_generic_repost_from_id(void) {
    TEST(test_create_generic_repost_from_id);

    NostrEvent *repost = nostr_nip18_create_generic_repost_from_id(
        TEST_EVENT_ID,
        TEST_PUBKEY,
        30023,  /* Article kind */
        "wss://relay.example.com",
        NULL
    );

    if (!repost) {
        FAIL("Failed to create generic repost event");
        return;
    }

    /* Check kind is 16 */
    int kind = nostr_event_get_kind(repost);
    if (kind != 16) {
        FAIL("Expected kind 16");
        nostr_event_free(repost);
        return;
    }

    /* Check tags contain e, p, and k */
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(repost);
    if (!tags || nostr_tags_size(tags) < 3) {
        FAIL("Expected at least 3 tags");
        nostr_event_free(repost);
        return;
    }

    /* Check k-tag exists and has correct value */
    bool found_k_tag = false;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (t && nostr_tag_size(t) >= 2 && strcmp(nostr_tag_get(t, 0), "k") == 0) {
            if (strcmp(nostr_tag_get(t, 1), "30023") == 0) {
                found_k_tag = true;
            }
            break;
        }
    }

    if (!found_k_tag) {
        FAIL("k-tag with value 30023 not found");
        nostr_event_free(repost);
        return;
    }

    nostr_event_free(repost);
    PASS();
}

static void test_parse_repost(void) {
    TEST(test_parse_repost);

    /* Create a repost to parse */
    NostrEvent *repost = nostr_nip18_create_repost_from_id(
        TEST_EVENT_ID,
        TEST_PUBKEY,
        "wss://relay.example.com",
        "{\"kind\":1,\"content\":\"test\"}"
    );

    if (!repost) {
        FAIL("Failed to create repost for parsing");
        return;
    }

    NostrRepostInfo info;
    int rc = nostr_nip18_parse_repost(repost, &info);

    if (rc != 0) {
        FAIL("Failed to parse repost");
        nostr_event_free(repost);
        return;
    }

    if (!info.has_repost_event) {
        FAIL("Should have repost event id");
        nostr_nip18_repost_info_clear(&info);
        nostr_event_free(repost);
        return;
    }

    if (!info.has_repost_pubkey) {
        FAIL("Should have repost pubkey");
        nostr_nip18_repost_info_clear(&info);
        nostr_event_free(repost);
        return;
    }

    if (memcmp(info.repost_event_id, TEST_EVENT_ID, 32) != 0) {
        FAIL("Repost event id mismatch");
        nostr_nip18_repost_info_clear(&info);
        nostr_event_free(repost);
        return;
    }

    if (memcmp(info.repost_pubkey, TEST_PUBKEY, 32) != 0) {
        FAIL("Repost pubkey mismatch");
        nostr_nip18_repost_info_clear(&info);
        nostr_event_free(repost);
        return;
    }

    if (!info.relay_hint || strcmp(info.relay_hint, "wss://relay.example.com") != 0) {
        FAIL("Relay hint mismatch");
        nostr_nip18_repost_info_clear(&info);
        nostr_event_free(repost);
        return;
    }

    if (!info.embedded_json) {
        FAIL("Should have embedded JSON");
        nostr_nip18_repost_info_clear(&info);
        nostr_event_free(repost);
        return;
    }

    nostr_nip18_repost_info_clear(&info);
    nostr_event_free(repost);
    PASS();
}

static void test_is_repost(void) {
    TEST(test_is_repost);

    NostrEvent *repost6 = nostr_nip18_create_repost_from_id(
        TEST_EVENT_ID, TEST_PUBKEY, NULL, NULL);
    NostrEvent *repost16 = nostr_nip18_create_generic_repost_from_id(
        TEST_EVENT_ID, TEST_PUBKEY, 7, NULL, NULL);
    NostrEvent *regular = nostr_event_new();
    nostr_event_set_kind(regular, 1);

    if (!nostr_nip18_is_repost(repost6)) {
        FAIL("Kind 6 should be a repost");
        goto cleanup;
    }

    if (!nostr_nip18_is_repost(repost16)) {
        FAIL("Kind 16 should be a repost");
        goto cleanup;
    }

    if (nostr_nip18_is_repost(regular)) {
        FAIL("Kind 1 should not be a repost");
        goto cleanup;
    }

    if (!nostr_nip18_is_note_repost(repost6)) {
        FAIL("Kind 6 should be a note repost");
        goto cleanup;
    }

    if (nostr_nip18_is_note_repost(repost16)) {
        FAIL("Kind 16 should not be a note repost");
        goto cleanup;
    }

    if (!nostr_nip18_is_generic_repost(repost16)) {
        FAIL("Kind 16 should be a generic repost");
        goto cleanup;
    }

    PASS();

cleanup:
    if (repost6) nostr_event_free(repost6);
    if (repost16) nostr_event_free(repost16);
    if (regular) nostr_event_free(regular);
}

static void test_add_q_tag(void) {
    TEST(test_add_q_tag);

    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "Check out this post!");

    int rc = nostr_nip18_add_q_tag(ev, TEST_EVENT_ID, "wss://relay.example.com", TEST_PUBKEY);

    if (rc != 0) {
        FAIL("Failed to add q-tag");
        nostr_event_free(ev);
        return;
    }

    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(ev);
    if (!tags || nostr_tags_size(tags) < 1) {
        FAIL("Expected at least 1 tag");
        nostr_event_free(ev);
        return;
    }

    NostrTag *q_tag = nostr_tags_get(tags, 0);
    if (!q_tag || strcmp(nostr_tag_get(q_tag, 0), "q") != 0) {
        FAIL("First tag should be 'q'");
        nostr_event_free(ev);
        return;
    }

    if (nostr_tag_size(q_tag) < 4) {
        FAIL("q-tag should have 4 elements");
        nostr_event_free(ev);
        return;
    }

    nostr_event_free(ev);
    PASS();
}

static void test_get_quote(void) {
    TEST(test_get_quote);

    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "Quote post!");

    nostr_nip18_add_q_tag(ev, TEST_EVENT_ID, "wss://relay.example.com", TEST_PUBKEY);

    NostrQuoteInfo info;
    int rc = nostr_nip18_get_quote(ev, &info);

    if (rc != 0) {
        FAIL("Failed to get quote");
        nostr_event_free(ev);
        return;
    }

    if (!info.has_quoted_event) {
        FAIL("Should have quoted event");
        nostr_nip18_quote_info_clear(&info);
        nostr_event_free(ev);
        return;
    }

    if (memcmp(info.quoted_event_id, TEST_EVENT_ID, 32) != 0) {
        FAIL("Quoted event id mismatch");
        nostr_nip18_quote_info_clear(&info);
        nostr_event_free(ev);
        return;
    }

    if (!info.has_quoted_pubkey) {
        FAIL("Should have quoted pubkey");
        nostr_nip18_quote_info_clear(&info);
        nostr_event_free(ev);
        return;
    }

    if (memcmp(info.quoted_pubkey, TEST_PUBKEY, 32) != 0) {
        FAIL("Quoted pubkey mismatch");
        nostr_nip18_quote_info_clear(&info);
        nostr_event_free(ev);
        return;
    }

    if (!info.relay_hint || strcmp(info.relay_hint, "wss://relay.example.com") != 0) {
        FAIL("Relay hint mismatch");
        nostr_nip18_quote_info_clear(&info);
        nostr_event_free(ev);
        return;
    }

    nostr_nip18_quote_info_clear(&info);
    nostr_event_free(ev);
    PASS();
}

static void test_has_quote(void) {
    TEST(test_has_quote);

    NostrEvent *ev_with_quote = nostr_event_new();
    nostr_event_set_kind(ev_with_quote, 1);
    nostr_nip18_add_q_tag(ev_with_quote, TEST_EVENT_ID, NULL, NULL);

    NostrEvent *ev_without_quote = nostr_event_new();
    nostr_event_set_kind(ev_without_quote, 1);

    if (!nostr_nip18_has_quote(ev_with_quote)) {
        FAIL("Should have quote");
        nostr_event_free(ev_with_quote);
        nostr_event_free(ev_without_quote);
        return;
    }

    if (nostr_nip18_has_quote(ev_without_quote)) {
        FAIL("Should not have quote");
        nostr_event_free(ev_with_quote);
        nostr_event_free(ev_without_quote);
        return;
    }

    nostr_event_free(ev_with_quote);
    nostr_event_free(ev_without_quote);
    PASS();
}

static void test_repost_info_clear(void) {
    TEST(test_repost_info_clear);

    NostrRepostInfo info = {0};
    info.relay_hint = strdup("wss://test.relay");
    info.embedded_json = strdup("{\"test\":true}");
    info.has_repost_event = true;
    info.has_repost_pubkey = true;

    nostr_nip18_repost_info_clear(&info);

    if (info.relay_hint != NULL) {
        FAIL("relay_hint should be NULL after clear");
        return;
    }

    if (info.embedded_json != NULL) {
        FAIL("embedded_json should be NULL after clear");
        return;
    }

    if (info.has_repost_event || info.has_repost_pubkey) {
        FAIL("Flags should be false after clear");
        return;
    }

    PASS();
}

int main(void) {
    printf("NIP-18 Tests\n");
    printf("============\n\n");

    test_create_repost_from_id();
    test_create_generic_repost_from_id();
    test_parse_repost();
    test_is_repost();
    test_add_q_tag();
    test_get_quote();
    test_has_quote();
    test_repost_info_clear();

    printf("\n============\n");
    printf("Results: %d/%d passed\n", pass_count, test_count);

    return (pass_count == test_count) ? 0 : 1;
}
