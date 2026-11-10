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

#define ISSUER "1111111111111111111111111111111111111111111111111111111111111111"
#define RECIPIENT "2222222222222222222222222222222222222222222222222222222222222222"
#define AWARD "3333333333333333333333333333333333333333333333333333333333333333"
static void test_canonical_and_sets(void) {
    assert(NOSTR_NIP58_KIND_PROFILE_BADGES == 30008);
    assert(NOSTR_NIP58_KIND_PROFILE_BADGE_LIST == 10008);
    assert(NOSTR_NIP58_KIND_BADGE_SETS == 30008);
    NostrNip58ProfileBadge badge = {"30009:" ISSUER ":badge:with:colons", AWARD, ""};
    const char *sets[] = {"30008:" RECIPIENT ":collection"};
    NostrEvent *ev = nostr_nip58_build_profile_badge_list(RECIPIENT, 5, &badge, 1, sets, 1);
    assert(ev && nostr_event_get_kind(ev) == 10008);
    NostrNip58ProfileBadge out[2]; size_t n;
    assert(!nostr_nip58_parse_profile_badge_list(ev, out, 2, &n) && n == 1);
    assert(!strcmp(out[0].badge_ref, badge.badge_ref) && !strcmp(out[0].award_relay, ""));
    const char *refs[1];
    assert(!nostr_nip58_profile_badge_sets(ev, refs, 1, &n) && n == 1 && !strcmp(refs[0], sets[0]));
    assert(nostr_nip58_parse_profile_badge_list(ev, NULL, 0, &n) == -ENOSPC && n == 1);
    /* An intervening extension breaks a pair, as does a set reference. */
    NostrTags *tags = nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("a", badge.badge_ref, NULL));
    nostr_tags_append(tags, nostr_tag_new("future", "metadata", NULL));
    nostr_tags_append(tags, nostr_tag_new("e", AWARD, NULL));
    assert(!nostr_nip58_parse_profile_badge_list(ev, out, 2, &n) && n == 1);
    NostrEvent *legacy = nostr_event_new();
    assert(!nostr_nip58_create_profile_badges(legacy, &badge, 1));
    NostrEvent *empty = nostr_nip58_build_profile_badge_list(RECIPIENT, 6, NULL, 0, NULL, 0);
    assert(empty && nostr_nip58_select_profile_badges(empty, legacy) == empty);
    assert(!nostr_nip58_parse_profile_badge_list(empty, NULL, 0, &n) && n == 0);
    assert(nostr_nip58_select_profile_badges(NULL, legacy) == legacy);
    NostrEvent *set = nostr_nip58_build_badge_set(RECIPIENT, 7, "collection", "Title", &badge, 1);
    assert(set && nostr_event_get_kind(set) == 30008);
    assert(!nostr_nip58_parse_badge_set(set, out, 2, &n) && n == 1);
    assert(nostr_nip58_parse_profile_badge_list(set, out, 2, &n) < 0);
    nostr_event_free(set); nostr_event_free(empty); nostr_event_free(legacy); nostr_event_free(ev);
}
static void test_relationship(void) {
    NostrNip58ProfileBadge pair = {"30009:" ISSUER ":badge:with:colons", AWARD, NULL};
    NostrEvent *award = nostr_event_new(), *def = nostr_event_new(); assert(award && def);
    const char *recipients[] = {RECIPIENT};
    assert(!nostr_nip58_create_award(award, pair.badge_ref, recipients, 1));
    nostr_event_set_pubkey(award, ISSUER);
    char *award_id = nostr_event_get_id(award); assert(award_id); pair.award_id = award_id;
    NostrNip58BadgeDef metadata = {.identifier = "badge:with:colons"};
    assert(!nostr_nip58_create_definition(def, &metadata)); nostr_event_set_pubkey(def, ISSUER);
    assert(nostr_nip58_validate_relationship(&pair, award, def, RECIPIENT));
    assert(!nostr_nip58_validate_relationship(&pair, award, def, ISSUER));
    nostr_event_set_pubkey(award, RECIPIENT);
    assert(!nostr_nip58_validate_relationship(&pair, award, def, RECIPIENT));
    nostr_event_set_pubkey(award, ISSUER); nostr_event_set_content(award, "altered");
    assert(!nostr_nip58_validate_relationship(&pair, award, def, RECIPIENT));
    nostr_event_set_content(award, "");
    nostr_tag_set(nostr_tags_get(nostr_event_get_tags(def), 0), 1, "different");
    assert(!nostr_nip58_validate_relationship(&pair, award, def, RECIPIENT));
    free(award_id); nostr_event_free(award); nostr_event_free(def);
}

int main(void) {
    test_canonical_and_sets();
    test_relationship();
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
