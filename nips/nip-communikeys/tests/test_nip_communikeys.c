#include "nip_communikeys.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PK_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PK_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PK_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define ID_D "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

static void add_tag(NostrEvent *event, NostrTag *tag) {
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    assert(tags);
    nostr_tags_append(tags, tag);
}

static NostrEvent *event_new(int kind, const char *pubkey) {
    NostrEvent *event = nostr_event_new();
    assert(event);
    nostr_event_set_kind(event, kind);
    nostr_event_set_pubkey(event, pubkey);
    nostr_event_set_content(event, "");
    return event;
}

static NostrEvent *valid_definition_event(void) {
    NostrEvent *event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("blossom", "https://media.example", NULL));
    add_tag(event, nostr_tag_new("content", "General", NULL));
    add_tag(event, nostr_tag_new("k", "1111", NULL));
    add_tag(event, nostr_tag_new("k", "11", "threads", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_A ":General",
                                 "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("badge", "30009:" PK_B ":contributor", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_A ":Chat", NULL));
    add_tag(event, nostr_tag_new("description", "A community", NULL));
    return event;
}

static void test_definition_parse_build_and_resolve(void) {
    NostrEvent *event = valid_definition_event();
    assert(nostr_communikeys_definition_validate_event(event) ==
           NOSTR_COMMUNIKEYS_OK);

    nostr_communikeys_definition_t definition;
    assert(nostr_communikeys_definition_parse(event, &definition));
    assert(definition.valid);
    assert(definition.sections_len == 2);
    assert(definition.relays_len == 1);
    assert(strcmp(definition.description, "A community") == 0);

    const nostr_communikeys_section_t *section =
        nostr_communikeys_definition_find_section(&definition, 11, "threads");
    assert(section && strcmp(section->name, "General") == 0);
    assert(!nostr_communikeys_definition_find_section(&definition, 11, NULL));
    section = nostr_communikeys_definition_find_section(&definition, 9, NULL);
    assert(section && strcmp(section->name, "Chat") == 0);

    NostrEvent *rebuilt =
        nostr_communikeys_definition_to_event(&definition, 1234);
    assert(rebuilt);
    assert(nostr_event_get_kind(rebuilt) == CAS_COMMUNITY_DEFINITION);
    assert(nostr_event_get_created_at(rebuilt) == 1234);
    assert(nostr_communikeys_definition_validate_event(rebuilt) ==
           NOSTR_COMMUNIKEYS_OK);

    nostr_event_free(rebuilt);
    nostr_communikeys_definition_clear(&definition);
    nostr_event_free(event);
}

static void test_definition_malformed_fails_closed(void) {
    NostrEvent *event = valid_definition_event();
    add_tag(event, nostr_tag_new("content", "Duplicate", NULL));
    add_tag(event, nostr_tag_new("k", "1111", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_A ":Duplicate", NULL));

    nostr_communikeys_definition_t definition;
    assert(nostr_communikeys_definition_parse(event, &definition));
    assert(!definition.valid);
    assert(definition.validation_status ==
           NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER ||
           definition.validation_status ==
           NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT);
    assert(!nostr_communikeys_definition_find_section(&definition, 1111, NULL));
    nostr_communikeys_definition_clear(&definition);
    nostr_event_free(event);

    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("description", "interrupts", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_A ":Chat", NULL));
    assert(nostr_communikeys_definition_validate_event(event) ==
           NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER);
    nostr_event_free(event);
}

static void test_profile_list_access(void) {
    NostrEvent *definition_event = valid_definition_event();
    nostr_communikeys_definition_t definition;
    assert(nostr_communikeys_definition_parse(definition_event, &definition));

    NostrEvent *list = event_new(NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST, PK_A);
    add_tag(list, nostr_tag_new("d", "Chat", NULL));
    add_tag(list, nostr_tag_new("p", PK_B, NULL));
    add_tag(list, nostr_tag_new("p", PK_B, "duplicate hint ignored", NULL));

    nostr_communikeys_profile_list_t parsed;
    assert(nostr_communikeys_profile_list_parse(list, PK_A, "Chat", &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(parsed.members_len == 1);
    assert(nostr_communikeys_profile_list_contains(&parsed, PK_B));
    assert(!nostr_communikeys_profile_list_contains(&parsed, PK_C));
    nostr_communikeys_profile_list_clear(&parsed);

    assert(nostr_communikeys_author_can_publish(
        &definition, 9, NULL, list, PK_B));
    assert(!nostr_communikeys_author_can_publish(
        &definition, 9, NULL, list, PK_C));
    assert(!nostr_communikeys_author_can_publish(
        &definition, 11, "threads", list, PK_B));

    nostr_event_free(list);
    nostr_communikeys_definition_clear(&definition);
    nostr_event_free(definition_event);
}

static void test_targeted_publication(void) {
    NostrEvent *original = event_new(30023, PK_C);
    char *original_id = nostr_event_get_id(original);
    assert(original_id != NULL);

    nostr_communikeys_target_t targets[] = {
        {.pubkey = PK_A, .relay = "wss://one.example"},
        {.pubkey = PK_B, .relay = NULL}
    };
    nostr_communikeys_targeted_publication_t publication = {
        .identifier = "route-1",
        .reference_type = NOSTR_COMMUNIKEYS_REFERENCE_EVENT,
        .reference = original_id,
        .reference_relay = "wss://source.example",
        .reference_author = PK_C,
        .original_kind = 30023,
        .original_author = PK_C,
        .targets = targets,
        .targets_len = 2
    };
    NostrEvent *event =
        nostr_communikeys_targeted_publication_to_event(&publication, 99);
    assert(event);
    assert(nostr_communikeys_targeted_publication_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_OK);

    nostr_communikeys_targeted_publication_t parsed;
    assert(nostr_communikeys_targeted_publication_parse(event, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(parsed.targets_len == 2);
    assert(strcmp(parsed.targets[0].relay, "wss://one.example") == 0);
    assert(parsed.targets[1].relay == NULL);
    nostr_communikeys_targeted_publication_clear(&parsed);

    assert(nostr_communikeys_targeted_publication_validate(event, original) ==
           NOSTR_COMMUNIKEYS_OK);
    nostr_event_set_kind(original, 1);
    assert(nostr_communikeys_targeted_publication_validate(event, original) ==
           NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH);
    nostr_event_free(original);
    free(original_id);
    nostr_event_free(event);

    event = event_new(CAS_TARGETED_PUBLICATION, PK_C);
    add_tag(event, nostr_tag_new("d", "bad", NULL));
    add_tag(event, nostr_tag_new("e", ID_D, NULL));
    add_tag(event, nostr_tag_new("k", "1", NULL));
    add_tag(event, nostr_tag_new("p", PK_A, NULL));
    add_tag(event, nostr_tag_new("p", PK_A, NULL));
    assert(nostr_communikeys_targeted_publication_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS);
    nostr_event_free(event);
}

static void test_exclusive_h(void) {
    NostrEvent *event = event_new(9, PK_B);
    assert(nostr_communikeys_exclusive_add_h(event, PK_A));
    char community[65];
    assert(nostr_communikeys_exclusive_validate(event, community) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(strcmp(community, PK_A) == 0);
    assert(!nostr_communikeys_exclusive_add_h(event, PK_B));
    add_tag(event, nostr_tag_new("h", PK_B, NULL));
    assert(nostr_communikeys_exclusive_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
    nostr_event_free(event);

    event = event_new(1, PK_B);
    assert(!nostr_communikeys_exclusive_add_h(event, PK_A));
    assert(nostr_communikeys_exclusive_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_ERR_WRONG_KIND);
    nostr_event_free(event);
}

static void test_ncommunity_identifier(void) {
    nostr_communikeys_identifier_t identifier;
    assert(nostr_communikeys_identifier_parse(
        "ncommunity://" PK_A
        "?relay=wss%3A%2F%2Fone.example&ignored=x&relay=ws%3A%2F%2Flocal%3A8080",
        &identifier) == NOSTR_COMMUNIKEYS_OK);
    assert(strcmp(identifier.pubkey, PK_A) == 0);
    assert(identifier.relays_len == 2);
    assert(strcmp(identifier.relays[0], "wss://one.example") == 0);
    assert(strcmp(identifier.relays[1], "ws://local:8080") == 0);

    char *formatted = nostr_communikeys_identifier_format(&identifier);
    assert(formatted);
    assert(strcmp(formatted,
        "ncommunity://" PK_A
        "?relay=wss%3A%2F%2Fone.example&relay=ws%3A%2F%2Flocal%3A8080") == 0);
    free(formatted);
    nostr_communikeys_identifier_clear(&identifier);

    assert(nostr_communikeys_identifier_parse(PK_B, &identifier) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(identifier.relays_len == 0);
    nostr_communikeys_identifier_clear(&identifier);
    assert(nostr_communikeys_identifier_parse(
        "ncommunity://" PK_A "?relay=https%3A%2F%2Fbad.example",
        &identifier) == NOSTR_COMMUNIKEYS_ERR_BAD_URI);
    assert(nostr_communikeys_identifier_parse(
        "ncommunity://" PK_A "?relay=wss%ZZbad",
        &identifier) == NOSTR_COMMUNIKEYS_ERR_BAD_URI);
}

static void test_coordinates(void) {
    nostr_communikeys_coordinate_t coordinate;
    assert(nostr_communikeys_coordinate_parse(
        "30000:" PK_A ":General:sub", "wss://relay.example", &coordinate));
    assert(coordinate.kind == 30000);
    assert(strcmp(coordinate.identifier, "General:sub") == 0);
    char *value = nostr_communikeys_coordinate_format(&coordinate);
    assert(value && strcmp(value, "30000:" PK_A ":General:sub") == 0);
    free(value);
    nostr_communikeys_coordinate_clear(&coordinate);
    assert(!nostr_communikeys_coordinate_parse(
        "30000:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA:x",
        NULL, &coordinate));
}

int main(void) {
    test_definition_parse_build_and_resolve();
    test_definition_malformed_fails_closed();
    test_profile_list_access();
    test_targeted_publication();
    test_exclusive_h();
    test_ncommunity_identifier();
    test_coordinates();
    puts("nip-communikeys ok");
    return 0;
}
