/**
 * NIP-29: Relay-based Groups — Unit Tests
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nip29.h"
#include "nostr-kinds.h"
#include "nostr/nip19/nip19.h"

static void add_tag(NostrEvent *event, NostrTag *tag) {
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    assert(tags != NULL);
    nostr_tags_append(tags, tag);
}

static NostrEvent *snapshot_event(int kind, int64_t created_at, const char *group_id) {
    NostrEvent *event = nostr_event_new();
    assert(event != NULL);
    nostr_event_set_kind(event, kind);
    nostr_event_set_created_at(event, created_at);
    nostr_event_set_content(event, "");
    add_tag(event, nostr_tag_new("d", group_id, NULL));
    return event;
}

static bool event_has_tag_key(const NostrEvent *event, const char *key) {
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        const char *tag_key = nostr_tag_get_key(tag);
        if (tag_key && strcmp(tag_key, key) == 0) {
            return true;
        }
    }
    return false;
}

static NostrTag *first_tag(const NostrEvent *event, const char *key) {
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        NostrTag *tag = nostr_tags_get(tags, i);
        const char *tag_key = nostr_tag_get_key(tag);
        if (tag_key && strcmp(tag_key, key) == 0) {
            return tag;
        }
    }
    return NULL;
}

static NostrTag *role_tag_named(const NostrEvent *event, const char *name) {
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        NostrTag *tag = nostr_tags_get(tags, i);
        const char *tag_key = nostr_tag_get_key(tag);
        const char *tag_name = nostr_tag_size(tag) > 1 ? nostr_tag_get(tag, 1) : NULL;
        if (tag_key && tag_name && strcmp(tag_key, "role") == 0 && strcmp(tag_name, name) == 0) {
            return tag;
        }
    }
    return NULL;
}

static void test_new_group_valid(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'test-group");
    assert(group != NULL);
    assert(strcmp(group->address.relay, "wss://relay.example.com") == 0);
    assert(strcmp(group->address.id, "test-group") == 0);
    assert(strcmp(group->name, "test-group") == 0);
    assert(group->picture == NULL);
    assert(group->about == NULL);
    assert(group->is_private == false);
    assert(group->is_restricted == false);
    assert(group->is_hidden == false);
    assert(group->is_closed == false);
    assert(group->admins_loaded == false);
    assert(group->members_loaded == false);
    assert(group->roles_loaded == false);
    assert(group->members_may_be_partial == false);

    char *encoded = nostr_group_address_to_string(&group->address);
    assert(encoded != NULL);
    assert(strcmp(encoded, "wss://relay.example.com'test-group") == 0);
    free(encoded);

    nostr_free_group(group);
}

static void test_new_group_invalid_addresses(void) {
    assert(nostr_new_group(NULL) == NULL);
    assert(nostr_new_group("") == NULL);
    assert(nostr_new_group("wss://relay.example.com/test-group") == NULL);
    assert(nostr_new_group("wss://relay.example.com'") == NULL);
    assert(nostr_new_group("'test-group") == NULL);
    assert(nostr_new_group("wss://relay.example.com'BadGroup") == NULL);
    assert(nostr_new_group("wss://relay.example.com'test group") == NULL);
}

static void test_free_null(void) {
    nostr_free_group(NULL);
}

static void test_permission_conversions(void) {
    assert(nostr_permission_from_string("put-user") == NOSTR_PERMISSION_PUT_USER);
    assert(nostr_permission_from_string("add-user") == NOSTR_PERMISSION_PUT_USER);
    assert(nostr_permission_from_string("remove-user") == NOSTR_PERMISSION_REMOVE_USER);
    assert(nostr_permission_from_string("edit-metadata") == NOSTR_PERMISSION_EDIT_METADATA);
    assert(nostr_permission_from_string("delete-event") == NOSTR_PERMISSION_DELETE_EVENT);
    assert(nostr_permission_from_string("create-group") == NOSTR_PERMISSION_CREATE_GROUP);
    assert(nostr_permission_from_string("delete-group") == NOSTR_PERMISSION_DELETE_GROUP);
    assert(nostr_permission_from_string("create-invite") == NOSTR_PERMISSION_CREATE_INVITE);
    assert(nostr_permission_from_string("update-pin-list") == NOSTR_PERMISSION_UPDATE_PIN_LIST);
    assert(nostr_permission_from_string("add-permission") == NOSTR_PERMISSION_UNKNOWN);
    assert(nostr_permission_from_string("remove-permission") == NOSTR_PERMISSION_UNKNOWN);
    assert(nostr_permission_from_string("edit-group-status") == NOSTR_PERMISSION_UNKNOWN);
    assert(nostr_permission_from_string(NULL) == NOSTR_PERMISSION_UNKNOWN);

    assert(strcmp(nostr_permission_to_string(NOSTR_PERMISSION_PUT_USER), "put-user") == 0);
    assert(strcmp(nostr_permission_to_string(NOSTR_PERMISSION_CREATE_INVITE), "create-invite") == 0);
    assert(strcmp(nostr_permission_to_string(NOSTR_PERMISSION_UPDATE_PIN_LIST), "update-pin-list") == 0);
    assert(nostr_permission_to_string(NOSTR_PERMISSION_UNKNOWN) == NULL);
}

static void test_metadata_merge_flags_copy_and_replacement(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'test-group");
    assert(group != NULL);

    NostrEvent *metadata = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 10, "test-group");
    add_tag(metadata, nostr_tag_new("name", "Modern Group", NULL));
    add_tag(metadata, nostr_tag_new("about", "relay-owned snapshot", NULL));
    add_tag(metadata, nostr_tag_new("picture", "https://example.com/group.png", NULL));
    add_tag(metadata, nostr_tag_new("private", NULL));
    add_tag(metadata, nostr_tag_new("restricted", NULL));
    add_tag(metadata, nostr_tag_new("hidden", NULL));
    add_tag(metadata, nostr_tag_new("closed", NULL));

    assert(nostr_group_merge_in_metadata_event(group, metadata));
    nostr_event_free(metadata);

    assert(strcmp(group->name, "Modern Group") == 0);
    assert(strcmp(group->about, "relay-owned snapshot") == 0);
    assert(strcmp(group->picture, "https://example.com/group.png") == 0);
    assert(group->is_private);
    assert(group->is_restricted);
    assert(group->is_hidden);
    assert(group->is_closed);
    assert(group->last_metadata_update == 10);

    NostrEvent *older = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 9, "test-group");
    add_tag(older, nostr_tag_new("name", "Older", NULL));
    assert(!nostr_group_merge_in_metadata_event(group, older));
    nostr_event_free(older);
    assert(strcmp(group->name, "Modern Group") == 0);

    NostrEvent *wrong_group = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 11, "other-group");
    assert(!nostr_group_merge_in_metadata_event(group, wrong_group));
    nostr_event_free(wrong_group);

    NostrEvent *replacement = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 12, "test-group");
    add_tag(replacement, nostr_tag_new("name", "Renamed", NULL));
    assert(nostr_group_merge_in_metadata_event(group, replacement));
    nostr_event_free(replacement);

    assert(strcmp(group->name, "Renamed") == 0);
    assert(group->about == NULL);
    assert(group->picture == NULL);
    assert(!group->is_private);
    assert(!group->is_restricted);
    assert(!group->is_hidden);
    assert(!group->is_closed);
    assert(group->last_metadata_update == 12);

    nostr_free_group(group);
}

static void test_metadata_output_current_flags_only(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'flags");
    assert(group != NULL);
    free(group->name);
    group->name = strdup("Flags");
    group->about = strdup("current flags");
    group->picture = strdup("https://example.com/p.png");
    group->is_private = true;
    group->is_restricted = true;
    group->is_hidden = true;
    group->is_closed = true;
    group->last_metadata_update = 33;

    NostrEvent *event = nostr_group_to_metadata_event(group);
    assert(event != NULL);
    assert(nostr_event_get_kind(event) == NOSTR_KIND_SIMPLE_GROUP_METADATA);
    assert(nostr_event_get_created_at(event) == 33);
    assert(event_has_tag_key(event, "d"));
    assert(event_has_tag_key(event, "private"));
    assert(event_has_tag_key(event, "restricted"));
    assert(event_has_tag_key(event, "hidden"));
    assert(event_has_tag_key(event, "closed"));
    assert(!event_has_tag_key(event, "public"));
    assert(!event_has_tag_key(event, "open"));
    nostr_event_free(event);

    group->is_private = false;
    group->is_restricted = false;
    group->is_hidden = false;
    group->is_closed = false;
    event = nostr_group_to_metadata_event(group);
    assert(event != NULL);
    assert(!event_has_tag_key(event, "private"));
    assert(!event_has_tag_key(event, "restricted"));
    assert(!event_has_tag_key(event, "hidden"));
    assert(!event_has_tag_key(event, "closed"));
    assert(!event_has_tag_key(event, "public"));
    assert(!event_has_tag_key(event, "open"));
    nostr_event_free(event);

    nostr_free_group(group);
}

static void test_admins_merge_snapshot_and_copy(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'general");
    assert(group != NULL);

    NostrEvent *admins = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_ADMINS, 20, "general");
    add_tag(admins, nostr_tag_new("p", "admin1", "owner", "moderator", NULL));
    add_tag(admins, nostr_tag_new("p", "admin2", NULL));
    add_tag(admins, nostr_tag_new("p", "admin1", "moderator", NULL));
    assert(nostr_group_merge_in_admins_event(group, admins));
    nostr_event_free(admins);

    assert(group->admins_loaded);
    assert(group->admins_len == 2);
    nostr_group_admin_t *admin1 = nostr_group_get_admin(group, "admin1");
    assert(admin1 != NULL);
    assert(admin1->roles_len == 2);
    assert(strcmp(admin1->roles[0], "owner") == 0);
    assert(strcmp(admin1->roles[1], "moderator") == 0);
    assert(nostr_group_get_admin(group, "admin2") != NULL);

    NostrEvent *older = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_ADMINS, 19, "general");
    add_tag(older, nostr_tag_new("p", "late-admin", "owner", NULL));
    assert(!nostr_group_merge_in_admins_event(group, older));
    nostr_event_free(older);
    assert(nostr_group_get_admin(group, "late-admin") == NULL);

    NostrEvent *out = nostr_group_to_admins_event(group);
    assert(out != NULL);
    assert(nostr_event_get_kind(out) == NOSTR_KIND_SIMPLE_GROUP_ADMINS);
    NostrTag *p = first_tag(out, "p");
    assert(p != NULL);
    assert(nostr_tag_size(p) >= 2);
    assert(strcmp(nostr_tag_get(p, 0), "p") == 0);
    nostr_event_free(out);

    nostr_free_group(group);
}

static void test_members_optional_partial_and_replacement(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'general");
    assert(group != NULL);
    assert(!group->members_loaded);

    NostrEvent *empty_members = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_MEMBERS, 30, "general");
    assert(nostr_group_merge_in_members_event(group, empty_members));
    nostr_event_free(empty_members);
    assert(group->members_loaded);
    assert(group->members_may_be_partial);
    assert(group->members_len == 0);

    NostrEvent *members = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_MEMBERS, 31, "general");
    add_tag(members, nostr_tag_new("p", "member1", NULL));
    add_tag(members, nostr_tag_new("p", "member2", "display label", NULL));
    assert(nostr_group_merge_in_members_event(group, members));
    nostr_event_free(members);

    assert(group->members_len == 2);
    nostr_group_member_t *member2 = nostr_group_get_member(group, "member2");
    assert(member2 != NULL);
    assert(strcmp(member2->label, "display label") == 0);

    NostrEvent *out = nostr_group_to_members_event(group);
    assert(out != NULL);
    assert(nostr_event_get_kind(out) == NOSTR_KIND_SIMPLE_GROUP_MEMBERS);
    assert(event_has_tag_key(out, "p"));
    nostr_event_free(out);

    nostr_free_group(group);
}

static void test_roles_merge_snapshot_and_output(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'general");
    assert(group != NULL);

    NostrEvent *roles = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_ROLES, 40, "general");
    add_tag(roles, nostr_tag_new("role", "owner", "can manage group", NULL));
    add_tag(roles, nostr_tag_new("role", "moderator", "can delete messages", NULL));
    assert(nostr_group_merge_in_roles_event(group, roles));
    nostr_event_free(roles);

    assert(group->roles_loaded);
    assert(group->roles_len == 2);
    nostr_group_role_t *owner = nostr_group_get_role(group, "owner");
    assert(owner != NULL);
    assert(strcmp(owner->description, "can manage group") == 0);

    NostrEvent *older = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_ROLES, 39, "general");
    add_tag(older, nostr_tag_new("role", "ignored", "too old", NULL));
    assert(!nostr_group_merge_in_roles_event(group, older));
    nostr_event_free(older);
    assert(nostr_group_get_role(group, "ignored") == NULL);

    NostrEvent *out = nostr_group_to_roles_event(group);
    assert(out != NULL);
    assert(nostr_event_get_kind(out) == NOSTR_KIND_SIMPLE_GROUP_ROLES);
    assert(event_has_tag_key(out, "role"));
    nostr_event_free(out);

    nostr_free_group(group);
}

static void test_mutators_mark_snapshots_loaded_and_preserve_null_role_description(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'manual");
    assert(group != NULL);

    nostr_group_admin_t *admin = nostr_group_add_admin(group, "admin");
    assert(admin != NULL);
    assert(nostr_group_admin_add_role(admin, "owner"));
    assert(group->admins_loaded);

    assert(nostr_group_add_member(group, "member", NULL) != NULL);
    assert(group->members_loaded);
    assert(!group->members_may_be_partial);

    assert(nostr_group_add_role(group, "observer", NULL) != NULL);
    assert(group->roles_loaded);

    NostrEvent *roles = nostr_group_to_roles_event(group);
    assert(roles != NULL);
    NostrTag *observer = role_tag_named(roles, "observer");
    assert(observer != NULL);
    assert(nostr_tag_size(observer) == 2);
    nostr_event_free(roles);
    nostr_free_group(group);
}

static void test_wrong_kind_rejected(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'general");
    assert(group != NULL);
    NostrEvent *event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_MEMBERS, 50, "general");
    assert(!nostr_group_merge_in_metadata_event(group, event));
    nostr_event_free(event);
    nostr_free_group(group);
}

#define HEX_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HEX_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define HEX_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

static size_t count_tags(const NostrEvent *event, const char *key) {
    size_t n = 0;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); ++i) {
        const char *tag_key = nostr_tag_get_key(nostr_tags_get(tags, i));
        if (tag_key && strcmp(tag_key, key) == 0) n++;
    }
    return n;
}

/* da1629e: `banner` on kind:39000. */
static void test_metadata_banner_round_trip(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'pizza");
    NostrEvent *event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 10, "pizza");
    add_tag(event, nostr_tag_new("picture", "https://pizza.com/pizza.png", NULL));
    add_tag(event, nostr_tag_new("banner", "https://pizza.com/banner.png", NULL));
    assert(nostr_group_merge_in_metadata_event(group, event));
    nostr_event_free(event);
    assert(strcmp(group->banner, "https://pizza.com/banner.png") == 0);
    assert(strcmp(group->picture, "https://pizza.com/pizza.png") == 0);

    NostrEvent *out = nostr_group_to_metadata_event(group);
    NostrTag *banner = first_tag(out, "banner");
    assert(banner && strcmp(nostr_tag_get(banner, 1), "https://pizza.com/banner.png") == 0);
    nostr_event_free(out);

    /* A later snapshot without banner clears it (snapshot, not patch). */
    event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 20, "pizza");
    assert(nostr_group_merge_in_metadata_event(group, event));
    nostr_event_free(event);
    assert(group->banner == NULL);
    out = nostr_group_to_metadata_event(group);
    assert(!event_has_tag_key(out, "banner"));
    nostr_event_free(out);
    nostr_free_group(group);
}

/* 223ddb3: subgroups — `parent` / ordered `child` tags on kind:39000. */
static void test_metadata_subgroups(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'nostr");
    assert(nostr_group_is_root(group));

    NostrEvent *event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 10, "nostr");
    add_tag(event, nostr_tag_new("parent", "tech", NULL));
    add_tag(event, nostr_tag_new("parent", "social", NULL));   /* only one allowed: first wins */
    add_tag(event, nostr_tag_new("child", "nip29", NULL));
    add_tag(event, nostr_tag_new("child", "nostr", NULL));     /* self-reference dropped */
    add_tag(event, nostr_tag_new("child", "Bad Id", NULL));    /* invalid id dropped */
    add_tag(event, nostr_tag_new("child", "blossom", NULL));
    add_tag(event, nostr_tag_new("child", "nip29", NULL));     /* duplicate dropped */
    add_tag(event, nostr_tag_new("child", "", NULL));
    assert(nostr_group_merge_in_metadata_event(group, event));
    nostr_event_free(event);

    assert(!nostr_group_is_root(group));
    assert(strcmp(group->parent, "tech") == 0);
    assert(group->children_len == 2);
    assert(strcmp(group->children[0], "nip29") == 0);
    assert(strcmp(group->children[1], "blossom") == 0);

    NostrEvent *out = nostr_group_to_metadata_event(group);
    assert(count_tags(out, "parent") == 1);
    assert(count_tags(out, "child") == 2);
    /* Order is the display order and must survive serialization. */
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(out);
    const char *seen[2] = {NULL, NULL};
    size_t k = 0;
    for (size_t i = 0; i < nostr_tags_size(tags); ++i) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (strcmp(nostr_tag_get_key(tag), "child") == 0) seen[k++] = nostr_tag_get(tag, 1);
    }
    assert(strcmp(seen[0], "nip29") == 0 && strcmp(seen[1], "blossom") == 0);
    nostr_event_free(out);

    /* Self-parent is a cycle: treated as no parent (root). Promotion to root
     * is a snapshot without `parent`. */
    event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 20, "nostr");
    add_tag(event, nostr_tag_new("parent", "nostr", NULL));
    assert(nostr_group_merge_in_metadata_event(group, event));
    nostr_event_free(event);
    assert(nostr_group_is_root(group));
    assert(group->children_len == 0 && group->children == NULL);
    nostr_free_group(group);
}

/* f19d0e3 + bdfa7e6: kind:39005 pinned events with `e` and `a` tags. */
static void test_pins_merge_and_output(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'pizza");
    assert(!group->pins_loaded && group->pins_len == 0);

    NostrEvent *event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS, 10, "pizza");
    add_tag(event, nostr_tag_new("e", HEX_A, NULL));
    add_tag(event, nostr_tag_new("a", "30023:" HEX_B ":my-article", NULL));
    add_tag(event, nostr_tag_new("e", HEX_C, NULL));
    add_tag(event, nostr_tag_new("a", "31922:" HEX_B ":", NULL));          /* empty d is legal */
    add_tag(event, nostr_tag_new("a", "30023:" HEX_B ":a:b", NULL));      /* d may contain ':' */
    add_tag(event, nostr_tag_new("e", HEX_A, NULL));                      /* duplicate */
    add_tag(event, nostr_tag_new("e", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", NULL));
    add_tag(event, nostr_tag_new("e", "abc", NULL));
    add_tag(event, nostr_tag_new("a", "x:" HEX_B ":d", NULL));
    add_tag(event, nostr_tag_new("a", "30023:" HEX_B, NULL));             /* missing d separator */
    add_tag(event, nostr_tag_new("a", "999999:" HEX_B ":d", NULL));
    add_tag(event, nostr_tag_new("p", HEX_A, NULL));                      /* not a pin */
    assert(nostr_group_merge_in_pins_event(group, event));
    nostr_event_free(event);

    assert(group->pins_loaded);
    assert(group->pins_len == 5);
    assert(group->pins[0].type == NOSTR_GROUP_PIN_EVENT && strcmp(group->pins[0].value, HEX_A) == 0);
    assert(group->pins[1].type == NOSTR_GROUP_PIN_ADDRESS &&
           strcmp(group->pins[1].value, "30023:" HEX_B ":my-article") == 0);
    assert(group->pins[2].type == NOSTR_GROUP_PIN_EVENT && strcmp(group->pins[2].value, HEX_C) == 0);
    assert(group->pins[3].type == NOSTR_GROUP_PIN_ADDRESS);
    assert(group->pins[4].type == NOSTR_GROUP_PIN_ADDRESS);

    NostrEvent *out = nostr_group_to_pins_event(group);
    assert(nostr_event_get_kind(out) == NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS);
    assert(strcmp(nostr_tag_get(first_tag(out, "d"), 1), "pizza") == 0);
    assert(count_tags(out, "e") == 2 && count_tags(out, "a") == 3);
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(out);
    assert(strcmp(nostr_tag_get_key(nostr_tags_get(tags, 1)), "e") == 0);
    assert(strcmp(nostr_tag_get_key(nostr_tags_get(tags, 2)), "a") == 0);
    assert(strcmp(nostr_tag_get_key(nostr_tags_get(tags, 3)), "e") == 0);
    nostr_event_free(out);

    /* Stale and foreign-group snapshots are rejected; an empty newer list
     * clears the pins (unpinning everything is a new, empty list). */
    event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS, 5, "pizza");
    assert(!nostr_group_merge_in_pins_event(group, event));
    nostr_event_free(event);
    event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS, 50, "other");
    assert(!nostr_group_merge_in_pins_event(group, event));
    nostr_event_free(event);
    event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_METADATA, 50, "pizza");
    assert(!nostr_group_merge_in_pins_event(group, event));
    nostr_event_free(event);
    assert(group->pins_len == 5);
    event = snapshot_event(NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS, 60, "pizza");
    assert(nostr_group_merge_in_pins_event(group, event));
    nostr_event_free(event);
    assert(group->pins_loaded && group->pins_len == 0 && group->pins == NULL);

    assert(nostr_group_add_pin(group, NOSTR_GROUP_PIN_EVENT, HEX_B) != NULL);
    assert(nostr_group_add_pin(group, NOSTR_GROUP_PIN_EVENT, HEX_B) == &group->pins[0]);
    assert(nostr_group_add_pin(group, NOSTR_GROUP_PIN_ADDRESS, HEX_B) == NULL);
    assert(group->pins_len == 1);
    nostr_free_group(group);
}

/* 6834e8b: `naddr1...?invite=<code>` group references. */
static char *make_group_naddr(int kind, const char *d, const char *relay) {
    char *relays[1] = {(char *)relay};
    NostrEntityPointer ptr = {
        .public_key = HEX_A,
        .kind = kind,
        .identifier = (char *)d,
        .relays = relay ? relays : NULL,
        .relays_count = relay ? 1 : 0,
    };
    char *bech = NULL;
    assert(nostr_nip19_encode_naddr(&ptr, &bech) == 0 && bech);
    return bech;
}

static void test_reference_parse(void) {
    nostr_group_address_t addr;
    char *code = (char *)"sentinel";

    assert(nostr_group_reference_parse("wss://relay.example.com'pizza", &addr, &code));
    assert(strcmp(addr.relay, "wss://relay.example.com") == 0 && strcmp(addr.id, "pizza") == 0);
    assert(code == NULL);
    nostr_group_address_clear(&addr);

    /* The relay URL's own query string is not mistaken for the suffix. */
    assert(nostr_group_reference_parse("wss://relay.example.com/?x=1'pizza?invite=a%2Fb%20c&utm=x",
                                       &addr, &code));
    assert(strcmp(addr.relay, "wss://relay.example.com/?x=1") == 0);
    assert(strcmp(addr.id, "pizza") == 0);
    assert(code && strcmp(code, "a/b c") == 0);
    free(code);
    nostr_group_address_clear(&addr);

    char *naddr = make_group_naddr(NOSTR_KIND_SIMPLE_GROUP_METADATA, "pizza", "wss://groups.example.com");
    char buf[1024];
    snprintf(buf, sizeof buf, "nostr:%s?invite=Xy7-Q", naddr);
    assert(nostr_group_reference_parse(buf, &addr, &code));
    assert(strcmp(addr.relay, "wss://groups.example.com") == 0 && strcmp(addr.id, "pizza") == 0);
    assert(code && strcmp(code, "Xy7-Q") == 0);
    free(code);
    nostr_group_address_clear(&addr);

    /* Clients that ignore the suffix still see a valid identifier; a NULL
     * out-pointer is how such callers opt out. */
    assert(nostr_group_reference_parse(naddr, &addr, NULL));
    nostr_group_address_clear(&addr);
    snprintf(buf, sizeof buf, "%s?invite=", naddr);
    assert(nostr_group_reference_parse(buf, &addr, &code) && code == NULL);
    nostr_group_address_clear(&addr);
    snprintf(buf, sizeof buf, "%s?invite=%%zz", naddr);
    assert(!nostr_group_reference_parse(buf, &addr, &code));
    assert(addr.relay == NULL && addr.id == NULL && code == NULL);
    snprintf(buf, sizeof buf, "%s?invite=%%00x", naddr);
    assert(!nostr_group_reference_parse(buf, &addr, &code));
    free(naddr);

    naddr = make_group_naddr(30023, "pizza", "wss://groups.example.com");     /* not a group */
    assert(!nostr_group_reference_parse(naddr, &addr, &code));
    free(naddr);
    naddr = make_group_naddr(NOSTR_KIND_SIMPLE_GROUP_METADATA, "pizza", NULL); /* no relay hint */
    assert(!nostr_group_reference_parse(naddr, &addr, &code));
    free(naddr);

    assert(!nostr_group_reference_parse("pizza?invite=x", &addr, &code));
    assert(!nostr_group_reference_parse("naddr1invalid", &addr, &code));
    assert(!nostr_group_reference_parse(NULL, &addr, &code));
}

int main(void) {
    test_new_group_valid();
    test_new_group_invalid_addresses();
    test_free_null();
    test_permission_conversions();
    test_metadata_merge_flags_copy_and_replacement();
    test_metadata_output_current_flags_only();
    test_admins_merge_snapshot_and_copy();
    test_members_optional_partial_and_replacement();
    test_roles_merge_snapshot_and_output();
    test_mutators_mark_snapshots_loaded_and_preserve_null_role_description();
    test_wrong_kind_rejected();
    test_metadata_banner_round_trip();
    test_metadata_subgroups();
    test_pins_merge_and_output();
    test_reference_parse();

    printf("nip29 ok\n");
    return 0;
}
