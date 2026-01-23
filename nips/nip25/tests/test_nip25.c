#include "nostr/nip25/nip25.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test helper: create a hex string from bytes */
static void bytes_to_hex(const unsigned char *bytes, size_t len, char *out) {
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2*i]   = hex[(bytes[i] >> 4) & 0xF];
        out[2*i+1] = hex[bytes[i] & 0xF];
    }
    out[2*len] = '\0';
}

/* Test: reaction type detection */
static void test_reaction_type(void) {
    printf("Testing reaction type detection...\n");

    assert(nostr_nip25_get_reaction_type("+") == NOSTR_REACTION_LIKE);
    assert(nostr_nip25_get_reaction_type("-") == NOSTR_REACTION_DISLIKE);
    assert(nostr_nip25_get_reaction_type("") == NOSTR_REACTION_LIKE);
    assert(nostr_nip25_get_reaction_type(NULL) == NOSTR_REACTION_LIKE);
    assert(nostr_nip25_get_reaction_type(":fire:") == NOSTR_REACTION_EMOJI);
    assert(nostr_nip25_get_reaction_type("custom") == NOSTR_REACTION_EMOJI);

    printf("  PASS: reaction type detection\n");
}

/* Test: create reaction with hex strings */
static void test_create_reaction_hex(void) {
    printf("Testing create_reaction_hex...\n");

    const char *event_id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *author_pk = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    /* Test basic like reaction */
    NostrEvent *ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", NULL);
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == NOSTR_KIND_REACTION);
    assert(strcmp(nostr_event_get_content(ev), "+") == 0);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    assert(tags != NULL);
    assert(nostr_tags_size(tags) >= 2); /* e-tag and p-tag */

    nostr_event_free(ev);

    /* Test dislike reaction */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "-", NULL);
    assert(ev != NULL);
    assert(strcmp(nostr_event_get_content(ev), "-") == 0);
    nostr_event_free(ev);

    /* Test emoji reaction */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, ":fire:", NULL);
    assert(ev != NULL);
    assert(strcmp(nostr_event_get_content(ev), ":fire:") == 0);
    nostr_event_free(ev);

    /* Test default content (NULL -> "+") */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, NULL, NULL);
    assert(ev != NULL);
    assert(strcmp(nostr_event_get_content(ev), "+") == 0);
    nostr_event_free(ev);

    /* Test with relay URL */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", "wss://relay.example.com");
    assert(ev != NULL);
    tags = (NostrTags *)nostr_event_get_tags(ev);
    NostrTag *e_tag = nostr_tags_get(tags, 0);
    assert(nostr_tag_size(e_tag) >= 3);
    assert(strcmp(nostr_tag_get(e_tag, 2), "wss://relay.example.com") == 0);
    nostr_event_free(ev);

    /* Test without author pubkey */
    ev = nostr_nip25_create_reaction_hex(event_id, NULL, 1, "+", NULL);
    assert(ev != NULL);
    tags = (NostrTags *)nostr_event_get_tags(ev);
    /* Should only have e-tag and k-tag, no p-tag */
    assert(nostr_tags_size(tags) == 2);
    nostr_event_free(ev);

    /* Test with unknown kind (-1 should omit k-tag) */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, -1, "+", NULL);
    assert(ev != NULL);
    tags = (NostrTags *)nostr_event_get_tags(ev);
    /* Should have e-tag and p-tag, no k-tag */
    assert(nostr_tags_size(tags) == 2);
    nostr_event_free(ev);

    /* Test invalid event ID */
    ev = nostr_nip25_create_reaction_hex("invalid", author_pk, 1, "+", NULL);
    assert(ev == NULL);

    ev = nostr_nip25_create_reaction_hex(NULL, author_pk, 1, "+", NULL);
    assert(ev == NULL);

    printf("  PASS: create_reaction_hex\n");
}

/* Test: create reaction with binary inputs */
static void test_create_reaction_binary(void) {
    printf("Testing create_reaction (binary)...\n");

    unsigned char event_id[32];
    unsigned char author_pk[32];
    for (int i = 0; i < 32; ++i) {
        event_id[i] = (unsigned char)i;
        author_pk[i] = (unsigned char)(31 - i);
    }

    NostrEvent *ev = nostr_nip25_create_reaction(event_id, author_pk, 1, "+", NULL);
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == NOSTR_KIND_REACTION);

    /* Verify the event ID in e-tag matches */
    char *reacted_id = nostr_nip25_get_reacted_event_id_hex(ev);
    assert(reacted_id != NULL);

    char expected_id[65];
    bytes_to_hex(event_id, 32, expected_id);
    assert(strcmp(reacted_id, expected_id) == 0);
    free(reacted_id);

    nostr_event_free(ev);

    printf("  PASS: create_reaction (binary)\n");
}

/* Test: parse reaction */
static void test_parse_reaction(void) {
    printf("Testing parse_reaction...\n");

    const char *event_id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *author_pk = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    /* Create and parse a like reaction */
    NostrEvent *ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", NULL);
    assert(ev != NULL);

    NostrReaction reaction;
    int ret = nostr_nip25_parse_reaction(ev, &reaction);
    assert(ret == 0);
    assert(reaction.type == NOSTR_REACTION_LIKE);
    assert(strcmp(reaction.content, "+") == 0);
    assert(reaction.has_event_id == true);
    assert(reaction.has_author_pubkey == true);
    assert(reaction.reacted_kind == 1);

    nostr_event_free(ev);

    /* Create and parse a dislike reaction */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "-", NULL);
    ret = nostr_nip25_parse_reaction(ev, &reaction);
    assert(ret == 0);
    assert(reaction.type == NOSTR_REACTION_DISLIKE);
    assert(strcmp(reaction.content, "-") == 0);

    nostr_event_free(ev);

    /* Create and parse an emoji reaction */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, ":fire:", NULL);
    ret = nostr_nip25_parse_reaction(ev, &reaction);
    assert(ret == 0);
    assert(reaction.type == NOSTR_REACTION_EMOJI);
    assert(strcmp(reaction.content, ":fire:") == 0);

    nostr_event_free(ev);

    /* Test parsing a non-reaction event */
    NostrEvent *text_note = nostr_event_new();
    nostr_event_set_kind(text_note, 1);
    nostr_event_set_content(text_note, "Hello world");
    ret = nostr_nip25_parse_reaction(text_note, &reaction);
    assert(ret == -1); /* Should fail for non-reaction */
    nostr_event_free(text_note);

    printf("  PASS: parse_reaction\n");
}

/* Test: is_like / is_dislike / is_reaction */
static void test_reaction_checks(void) {
    printf("Testing is_like/is_dislike/is_reaction...\n");

    const char *event_id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *author_pk = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    /* Like reaction */
    NostrEvent *ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", NULL);
    assert(nostr_nip25_is_reaction(ev) == true);
    assert(nostr_nip25_is_like(ev) == true);
    assert(nostr_nip25_is_dislike(ev) == false);
    nostr_event_free(ev);

    /* Dislike reaction */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "-", NULL);
    assert(nostr_nip25_is_reaction(ev) == true);
    assert(nostr_nip25_is_like(ev) == false);
    assert(nostr_nip25_is_dislike(ev) == true);
    nostr_event_free(ev);

    /* Emoji reaction */
    ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, ":fire:", NULL);
    assert(nostr_nip25_is_reaction(ev) == true);
    assert(nostr_nip25_is_like(ev) == false);
    assert(nostr_nip25_is_dislike(ev) == false);
    nostr_event_free(ev);

    /* Non-reaction event */
    NostrEvent *text_note = nostr_event_new();
    nostr_event_set_kind(text_note, 1);
    assert(nostr_nip25_is_reaction(text_note) == false);
    assert(nostr_nip25_is_like(text_note) == false);
    assert(nostr_nip25_is_dislike(text_note) == false);
    nostr_event_free(text_note);

    /* NULL event */
    assert(nostr_nip25_is_reaction(NULL) == false);
    assert(nostr_nip25_is_like(NULL) == false);
    assert(nostr_nip25_is_dislike(NULL) == false);

    printf("  PASS: is_like/is_dislike/is_reaction\n");
}

/* Test: get reacted event ID */
static void test_get_reacted_event_id(void) {
    printf("Testing get_reacted_event_id...\n");

    const char *event_id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *author_pk = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    NostrEvent *ev = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", NULL);
    assert(ev != NULL);

    /* Test binary extraction */
    unsigned char out_id[32];
    bool found = nostr_nip25_get_reacted_event_id(ev, out_id);
    assert(found == true);

    /* Verify by converting back to hex */
    char out_hex[65];
    bytes_to_hex(out_id, 32, out_hex);
    assert(strcmp(out_hex, event_id) == 0);

    /* Test hex extraction */
    char *hex_id = nostr_nip25_get_reacted_event_id_hex(ev);
    assert(hex_id != NULL);
    assert(strcmp(hex_id, event_id) == 0);
    free(hex_id);

    nostr_event_free(ev);

    /* Test with event without e-tag */
    NostrEvent *no_tags = nostr_event_new();
    nostr_event_set_kind(no_tags, NOSTR_KIND_REACTION);
    nostr_event_set_content(no_tags, "+");
    found = nostr_nip25_get_reacted_event_id(no_tags, out_id);
    assert(found == false);
    hex_id = nostr_nip25_get_reacted_event_id_hex(no_tags);
    assert(hex_id == NULL);
    nostr_event_free(no_tags);

    printf("  PASS: get_reacted_event_id\n");
}

/* Test: aggregate reactions */
static void test_aggregate_reactions(void) {
    printf("Testing aggregate_reactions...\n");

    const char *event_id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *author_pk = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

    /* Create a mix of reactions */
    NostrEvent *reactions[6];
    reactions[0] = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", NULL);
    reactions[1] = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", NULL);
    reactions[2] = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "+", NULL);
    reactions[3] = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, "-", NULL);
    reactions[4] = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, ":fire:", NULL);
    reactions[5] = nostr_nip25_create_reaction_hex(event_id, author_pk, 1, ":heart:", NULL);

    NostrReactionStats stats;
    int ret = nostr_nip25_aggregate_reactions((const NostrEvent **)reactions, 6, &stats);
    assert(ret == 0);
    assert(stats.like_count == 3);
    assert(stats.dislike_count == 1);
    assert(stats.emoji_count == 2);
    assert(stats.total_count == 6);

    for (int i = 0; i < 6; ++i) {
        nostr_event_free(reactions[i]);
    }

    /* Test empty array */
    ret = nostr_nip25_aggregate_reactions(NULL, 0, &stats);
    assert(ret == 0);
    assert(stats.total_count == 0);

    printf("  PASS: aggregate_reactions\n");
}

int main(void) {
    printf("=== NIP-25 Reactions Tests ===\n\n");

    test_reaction_type();
    test_create_reaction_hex();
    test_create_reaction_binary();
    test_parse_reaction();
    test_reaction_checks();
    test_get_reacted_event_id();
    test_aggregate_reactions();

    printf("\n=== All NIP-25 tests passed! ===\n");
    return 0;
}
