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
    assert(nostr_permission_from_string("add-permission") == NOSTR_PERMISSION_UNKNOWN);
    assert(nostr_permission_from_string("remove-permission") == NOSTR_PERMISSION_UNKNOWN);
    assert(nostr_permission_from_string("edit-group-status") == NOSTR_PERMISSION_UNKNOWN);
    assert(nostr_permission_from_string(NULL) == NOSTR_PERMISSION_UNKNOWN);

    assert(strcmp(nostr_permission_to_string(NOSTR_PERMISSION_PUT_USER), "put-user") == 0);
    assert(strcmp(nostr_permission_to_string(NOSTR_PERMISSION_CREATE_INVITE), "create-invite") == 0);
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

    printf("nip29 ok\n");
    return 0;
}
