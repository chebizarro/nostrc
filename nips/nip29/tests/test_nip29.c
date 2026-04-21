/**
 * NIP-29: Relay-based Groups — Unit Tests
 *
 * Tests nostr_new_group(), nostr_free_group(), and nostr_permission_from_string().
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nip29.h"

/* ── nostr_new_group ─────────────────────────────────────────────────── */

static void test_new_group_valid(void) {
    /* Format: relay'id */
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'test-group");
    assert(group != NULL);
    assert(group->address.relay != NULL);
    assert(strcmp(group->address.relay, "wss://relay.example.com") == 0);
    assert(group->address.id != NULL);
    assert(strcmp(group->address.id, "test-group") == 0);
    assert(group->name != NULL);
    assert(strcmp(group->name, "test-group") == 0);
    assert(group->picture == NULL);
    assert(group->about == NULL);
    assert(group->members == NULL);
    assert(group->members_len == 0);
    assert(group->is_private == false);
    assert(group->is_closed == false);
    assert(group->last_metadata_update == 0);
    assert(group->last_admins_update == 0);
    assert(group->last_members_update == 0);

    nostr_free_group(group);
}

static void test_new_group_missing_separator(void) {
    /* No apostrophe separator → should fail */
    nostr_group_t *group = nostr_new_group("wss://relay.example.com/test-group");
    assert(group == NULL);
}

static void test_new_group_null(void) {
    nostr_group_t *group = nostr_new_group(NULL);
    assert(group == NULL);
}

static void test_new_group_empty_string(void) {
    nostr_group_t *group = nostr_new_group("");
    assert(group == NULL);
}

/* ── nostr_free_group ────────────────────────────────────────────────── */

static void test_free_null(void) {
    /* Should not crash */
    nostr_free_group(NULL);
}

static void test_free_valid_group(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.example.com'my-group");
    assert(group != NULL);
    /* Should not crash or leak */
    nostr_free_group(group);
}

/* ── nostr_permission_from_string ────────────────────────────────────── */

static void test_permission_roundtrip(void) {
    /* Test all known permission strings */
    assert(nostr_permission_from_string("add-user") == NOSTR_PERMISSION_ADD_USER);
    assert(nostr_permission_from_string("edit-metadata") == NOSTR_PERMISSION_EDIT_METADATA);
    assert(nostr_permission_from_string("delete-event") == NOSTR_PERMISSION_DELETE_EVENT);
    assert(nostr_permission_from_string("remove-user") == NOSTR_PERMISSION_REMOVE_USER);
    assert(nostr_permission_from_string("add-permission") == NOSTR_PERMISSION_ADD_PERMISSION);
    assert(nostr_permission_from_string("remove-permission") == NOSTR_PERMISSION_REMOVE_PERMISSION);
    assert(nostr_permission_from_string("edit-group-status") == NOSTR_PERMISSION_EDIT_GROUP_STATUS);
}

static void test_permission_unknown(void) {
    /* Unknown permission string returns -1 */
    nostr_permission_t perm = nostr_permission_from_string("unknown-perm");
    assert((int)perm == -1);
}

/* ── group defaults ──────────────────────────────────────────────────── */

static void test_group_defaults(void) {
    nostr_group_t *group = nostr_new_group("wss://relay.test'defaults");
    assert(group != NULL);
    assert(group->is_private == false);
    assert(group->is_closed == false);
    assert(group->members_len == 0);
    nostr_free_group(group);
}

int main(void) {
    /* nostr_new_group */
    test_new_group_valid();
    test_new_group_missing_separator();
    test_new_group_null();
    test_new_group_empty_string();

    /* nostr_free_group */
    test_free_null();
    test_free_valid_group();

    /* nostr_permission_from_string */
    test_permission_roundtrip();
    test_permission_unknown();

    /* Defaults */
    test_group_defaults();

    printf("nip29 ok\n");
    return 0;
}
