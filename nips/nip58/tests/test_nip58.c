#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip58/nip58.h"

/* ============== Badge Definition Tests ============== */

static void test_parse_definition(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_DEFINITION);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "early-adopter", NULL));
    nostr_tags_append(tags, nostr_tag_new("name", "Early Adopter", NULL));
    nostr_tags_append(tags, nostr_tag_new("description", "Joined during beta", NULL));
    nostr_tags_append(tags, nostr_tag_new("image", "https://example.com/badge.png",
                                           "1024x1024", NULL));
    nostr_tags_append(tags, nostr_tag_new("thumb", "https://example.com/thumb.png",
                                           "256x256", NULL));

    NostrNip58BadgeDef def;
    assert(nostr_nip58_parse_definition(ev, &def) == 0);
    assert(strcmp(def.identifier, "early-adopter") == 0);
    assert(strcmp(def.name, "Early Adopter") == 0);
    assert(strcmp(def.description, "Joined during beta") == 0);
    assert(strcmp(def.image_url, "https://example.com/badge.png") == 0);
    assert(strcmp(def.image_dims, "1024x1024") == 0);
    assert(strcmp(def.thumb_url, "https://example.com/thumb.png") == 0);
    assert(strcmp(def.thumb_dims, "256x256") == 0);

    nostr_event_free(ev);
}

static void test_parse_definition_minimal(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "my-badge", NULL));

    NostrNip58BadgeDef def;
    assert(nostr_nip58_parse_definition(ev, &def) == 0);
    assert(strcmp(def.identifier, "my-badge") == 0);
    assert(def.name == NULL);
    assert(def.description == NULL);
    assert(def.image_url == NULL);
    assert(def.image_dims == NULL);
    assert(def.thumb_url == NULL);
    assert(def.thumb_dims == NULL);

    nostr_event_free(ev);
}

static void test_parse_definition_no_d_tag(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("name", "Badge Name", NULL));

    NostrNip58BadgeDef def;
    assert(nostr_nip58_parse_definition(ev, &def) == -ENOENT);

    nostr_event_free(ev);
}

static void test_validate_definition(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_DEFINITION);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "test-badge", NULL));

    assert(nostr_nip58_validate_definition(ev));

    /* Wrong kind */
    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip58_validate_definition(ev));

    nostr_event_free(ev);

    /* Missing d tag */
    ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_DEFINITION);
    assert(!nostr_nip58_validate_definition(ev));
    nostr_event_free(ev);
}

static void test_create_definition(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip58BadgeDef def = {
        .identifier = "contributor",
        .name = "Contributor",
        .description = "Made contributions",
        .image_url = "https://example.com/img.png",
        .image_dims = "512x512",
        .thumb_url = NULL,
        .thumb_dims = NULL,
    };

    assert(nostr_nip58_create_definition(ev, &def) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP58_KIND_BADGE_DEFINITION);
    assert(nostr_nip58_validate_definition(ev));

    /* Round-trip parse */
    NostrNip58BadgeDef parsed;
    assert(nostr_nip58_parse_definition(ev, &parsed) == 0);
    assert(strcmp(parsed.identifier, "contributor") == 0);
    assert(strcmp(parsed.name, "Contributor") == 0);
    assert(strcmp(parsed.image_url, "https://example.com/img.png") == 0);
    assert(strcmp(parsed.image_dims, "512x512") == 0);
    assert(parsed.thumb_url == NULL);

    nostr_event_free(ev);
}

/* ============== Badge Award Tests ============== */

static void test_parse_award(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_AWARD);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("a", "30009:abc123:early-adopter", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "pubkey1", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "pubkey2", NULL));

    NostrNip58BadgeAward award;
    assert(nostr_nip58_parse_award(ev, &award) == 0);
    assert(strcmp(award.badge_ref, "30009:abc123:early-adopter") == 0);

    nostr_event_free(ev);
}

static void test_award_awardees(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_AWARD);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("a", "30009:abc:badge", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "alice", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "bob", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "charlie", NULL));

    assert(nostr_nip58_award_count_awardees(ev) == 3);

    const char *pubkeys[4];
    size_t count = 0;
    assert(nostr_nip58_award_get_awardees(ev, pubkeys, 4, &count) == 0);
    assert(count == 3);
    assert(strcmp(pubkeys[0], "alice") == 0);
    assert(strcmp(pubkeys[1], "bob") == 0);
    assert(strcmp(pubkeys[2], "charlie") == 0);

    /* Test max limit */
    count = 0;
    assert(nostr_nip58_award_get_awardees(ev, pubkeys, 2, &count) == 0);
    assert(count == 2);

    nostr_event_free(ev);
}

static void test_validate_award(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_AWARD);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("a", "30009:abc:badge", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "pubkey", NULL));

    assert(nostr_nip58_validate_award(ev));

    /* Missing p tag */
    NostrEvent *ev2 = nostr_event_new();
    nostr_event_set_kind(ev2, NOSTR_NIP58_KIND_BADGE_AWARD);
    NostrTags *tags2 = (NostrTags *)nostr_event_get_tags(ev2);
    nostr_tags_append(tags2, nostr_tag_new("a", "30009:abc:badge", NULL));
    assert(!nostr_nip58_validate_award(ev2));

    nostr_event_free(ev);
    nostr_event_free(ev2);
}

static void test_create_award(void) {
    NostrEvent *ev = nostr_event_new();

    const char *pubkeys[] = { "alice_pk", "bob_pk" };
    assert(nostr_nip58_create_award(ev, "30009:issuer:badge-id", pubkeys, 2) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP58_KIND_BADGE_AWARD);
    assert(nostr_nip58_validate_award(ev));

    NostrNip58BadgeAward award;
    assert(nostr_nip58_parse_award(ev, &award) == 0);
    assert(strcmp(award.badge_ref, "30009:issuer:badge-id") == 0);

    assert(nostr_nip58_award_count_awardees(ev) == 2);

    nostr_event_free(ev);
}

/* ============== Profile Badges Tests ============== */

static void test_parse_profile_badges(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_PROFILE_BADGES);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "profile_badges", NULL));
    nostr_tags_append(tags, nostr_tag_new("a", "30009:issuer1:badge1", NULL));
    nostr_tags_append(tags, nostr_tag_new("e", "award_event_1", "wss://relay.example.com", NULL));
    nostr_tags_append(tags, nostr_tag_new("a", "30009:issuer2:badge2", NULL));
    nostr_tags_append(tags, nostr_tag_new("e", "award_event_2", NULL));

    NostrNip58ProfileBadge entries[4];
    size_t count = 0;
    assert(nostr_nip58_parse_profile_badges(ev, entries, 4, &count) == 0);
    assert(count == 2);

    assert(strcmp(entries[0].badge_ref, "30009:issuer1:badge1") == 0);
    assert(strcmp(entries[0].award_id, "award_event_1") == 0);
    assert(entries[0].award_relay != NULL);
    assert(strcmp(entries[0].award_relay, "wss://relay.example.com") == 0);

    assert(strcmp(entries[1].badge_ref, "30009:issuer2:badge2") == 0);
    assert(strcmp(entries[1].award_id, "award_event_2") == 0);
    assert(entries[1].award_relay == NULL);

    nostr_event_free(ev);
}

static void test_parse_profile_badges_wrong_d(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "something_else", NULL));

    NostrNip58ProfileBadge entries[4];
    size_t count = 0;
    assert(nostr_nip58_parse_profile_badges(ev, entries, 4, &count) == -ENOENT);

    nostr_event_free(ev);
}

static void test_validate_profile_badges(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_PROFILE_BADGES);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "profile_badges", NULL));

    assert(nostr_nip58_validate_profile_badges(ev));

    /* Wrong kind */
    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip58_validate_profile_badges(ev));

    nostr_event_free(ev);
}

static void test_create_profile_badges(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip58ProfileBadge badges[] = {
        { .badge_ref = "30009:pk1:badge1", .award_id = "award1",
          .award_relay = "wss://relay.example.com" },
        { .badge_ref = "30009:pk2:badge2", .award_id = "award2",
          .award_relay = NULL },
    };

    assert(nostr_nip58_create_profile_badges(ev, badges, 2) == 0);
    assert(nostr_nip58_validate_profile_badges(ev));

    /* Round-trip parse */
    NostrNip58ProfileBadge parsed[4];
    size_t count = 0;
    assert(nostr_nip58_parse_profile_badges(ev, parsed, 4, &count) == 0);
    assert(count == 2);
    assert(strcmp(parsed[0].badge_ref, "30009:pk1:badge1") == 0);
    assert(strcmp(parsed[0].award_id, "award1") == 0);
    assert(parsed[0].award_relay != NULL);
    assert(strcmp(parsed[1].badge_ref, "30009:pk2:badge2") == 0);
    assert(parsed[1].award_relay == NULL);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(!nostr_nip58_validate_definition(NULL));
    assert(!nostr_nip58_validate_award(NULL));
    assert(!nostr_nip58_validate_profile_badges(NULL));

    assert(nostr_nip58_award_count_awardees(NULL) == 0);

    NostrNip58BadgeDef def;
    assert(nostr_nip58_parse_definition(NULL, &def) == -EINVAL);

    NostrNip58BadgeAward award;
    assert(nostr_nip58_parse_award(NULL, &award) == -EINVAL);

    NostrNip58ProfileBadge entries[2];
    size_t count;
    assert(nostr_nip58_parse_profile_badges(NULL, entries, 2, &count) == -EINVAL);

    const char *pks[1];
    assert(nostr_nip58_award_get_awardees(NULL, pks, 1, &count) == -EINVAL);
    assert(nostr_nip58_create_award(NULL, "a", pks, 1) == -EINVAL);
}

int main(void) {
    test_parse_definition();
    test_parse_definition_minimal();
    test_parse_definition_no_d_tag();
    test_validate_definition();
    test_create_definition();
    test_parse_award();
    test_award_awardees();
    test_validate_award();
    test_create_award();
    test_parse_profile_badges();
    test_parse_profile_badges_wrong_d();
    test_validate_profile_badges();
    test_create_profile_badges();
    test_invalid_inputs();
    printf("nip58 ok\n");
    return 0;
}
