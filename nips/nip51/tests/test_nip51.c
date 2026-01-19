/**
 * NIP-51 Test Suite
 *
 * Tests for user lists (mutes, bookmarks, etc.)
 */

#include "nostr/nip51/nip51.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-kinds.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test keys - generated for testing only */
static const char *TEST_SK = NULL;
static const char *TEST_PK = NULL;

static void setup_keys(void) {
    TEST_SK = nostr_key_generate_private();
    assert(TEST_SK != NULL);
    TEST_PK = nostr_key_get_public(TEST_SK);
    assert(TEST_PK != NULL);
    printf("Test keys generated\n");
}

static void cleanup_keys(void) {
    free((void *)TEST_SK);
    free((void *)TEST_PK);
}

/* ---- Memory Management Tests ---- */

static void test_list_new_free(void) {
    printf("Testing list creation and free...\n");

    NostrList *list = nostr_nip51_list_new();
    assert(list != NULL);
    assert(list->count == 0);
    assert(list->entries != NULL);

    nostr_nip51_list_free(list);
    printf("  OK: list created and freed\n");
}

static void test_entry_new_free(void) {
    printf("Testing entry creation and free...\n");

    NostrListEntry *entry = nostr_nip51_entry_new("p", "abc123", "wss://relay.example", true);
    assert(entry != NULL);
    assert(strcmp(entry->tag_name, "p") == 0);
    assert(strcmp(entry->value, "abc123") == 0);
    assert(strcmp(entry->extra, "wss://relay.example") == 0);
    assert(entry->is_private == true);

    nostr_nip51_entry_free(entry);

    /* Test without extra */
    entry = nostr_nip51_entry_new("word", "spam", NULL, false);
    assert(entry != NULL);
    assert(entry->extra == NULL);
    assert(entry->is_private == false);

    nostr_nip51_entry_free(entry);
    printf("  OK: entries created and freed\n");
}

static void test_list_add_entry(void) {
    printf("Testing list entry addition...\n");

    NostrList *list = nostr_nip51_list_new();
    assert(list != NULL);

    for (int i = 0; i < 20; i++) {
        char val[32];
        snprintf(val, sizeof(val), "value%d", i);
        NostrListEntry *entry = nostr_nip51_entry_new("p", val, NULL, false);
        assert(entry != NULL);
        nostr_nip51_list_add_entry(list, entry);
    }

    assert(list->count == 20);

    nostr_nip51_list_free(list);
    printf("  OK: added 20 entries with dynamic growth\n");
}

/* ---- Convenience Builder Tests ---- */

static void test_mute_builders(void) {
    printf("Testing mute convenience builders...\n");

    NostrList *list = nostr_nip51_list_new();
    assert(list != NULL);

    nostr_nip51_mute_user(list, "pubkey123", false);
    nostr_nip51_mute_word(list, "spam", true);
    nostr_nip51_mute_hashtag(list, "scam", false);
    nostr_nip51_mute_event(list, "eventid456", true);

    assert(list->count == 4);
    assert(strcmp(list->entries[0]->tag_name, "p") == 0);
    assert(strcmp(list->entries[1]->tag_name, "word") == 0);
    assert(strcmp(list->entries[2]->tag_name, "t") == 0);
    assert(strcmp(list->entries[3]->tag_name, "e") == 0);

    assert(list->entries[0]->is_private == false);
    assert(list->entries[1]->is_private == true);
    assert(list->entries[2]->is_private == false);
    assert(list->entries[3]->is_private == true);

    nostr_nip51_list_free(list);
    printf("  OK: mute builders work correctly\n");
}

static void test_bookmark_builders(void) {
    printf("Testing bookmark convenience builders...\n");

    NostrList *list = nostr_nip51_list_new();
    assert(list != NULL);

    nostr_nip51_bookmark_event(list, "event123", "wss://relay.example", false);
    nostr_nip51_bookmark_url(list, "https://example.com", true);

    assert(list->count == 2);
    assert(strcmp(list->entries[0]->tag_name, "e") == 0);
    assert(strcmp(list->entries[0]->extra, "wss://relay.example") == 0);
    assert(strcmp(list->entries[1]->tag_name, "r") == 0);
    assert(list->entries[1]->is_private == true);

    nostr_nip51_list_free(list);
    printf("  OK: bookmark builders work correctly\n");
}

/* ---- Private Entry Encryption Tests ---- */

static void test_private_entry_encryption(void) {
    printf("Testing private entry encryption/decryption...\n");

    /* Create some entries */
    NostrListEntry *entries[3];
    entries[0] = nostr_nip51_entry_new("p", "pubkey123456789", NULL, true);
    entries[1] = nostr_nip51_entry_new("word", "secretword", NULL, true);
    entries[2] = nostr_nip51_entry_new("e", "eventid987654321", "wss://relay.test", true);

    assert(entries[0] && entries[1] && entries[2]);

    /* Encrypt */
    char *encrypted = nostr_nip51_encrypt_private_entries(entries, 3, TEST_SK);
    assert(encrypted != NULL);
    assert(strlen(encrypted) > 0);

    /* Decrypt */
    size_t count = 0;
    NostrListEntry **decrypted = nostr_nip51_decrypt_private_entries(encrypted, TEST_SK, &count);
    assert(decrypted != NULL);
    assert(count == 3);

    /* Verify entries match */
    assert(strcmp(decrypted[0]->tag_name, "p") == 0);
    assert(strcmp(decrypted[0]->value, "pubkey123456789") == 0);

    assert(strcmp(decrypted[1]->tag_name, "word") == 0);
    assert(strcmp(decrypted[1]->value, "secretword") == 0);

    assert(strcmp(decrypted[2]->tag_name, "e") == 0);
    assert(strcmp(decrypted[2]->value, "eventid987654321") == 0);
    assert(strcmp(decrypted[2]->extra, "wss://relay.test") == 0);

    /* Cleanup */
    nostr_nip51_entry_free(entries[0]);
    nostr_nip51_entry_free(entries[1]);
    nostr_nip51_entry_free(entries[2]);
    nostr_nip51_free_entries(decrypted, count);
    free(encrypted);

    printf("  OK: private entry encryption roundtrip successful\n");
}

/* ---- Mute List Tests ---- */

static void test_create_mute_list(void) {
    printf("Testing mute list creation...\n");

    NostrList *list = nostr_nip51_list_new();
    assert(list != NULL);

    nostr_nip51_mute_user(list, "pubkey111", false);
    nostr_nip51_mute_word(list, "badword", false);
    nostr_nip51_mute_hashtag(list, "spam", false);

    NostrEvent *event = nostr_nip51_create_mute_list(list, TEST_SK);
    assert(event != NULL);

    /* Verify kind */
    assert(nostr_event_get_kind(event) == NOSTR_KIND_MUTE_LIST);

    /* Verify pubkey */
    assert(strcmp(nostr_event_get_pubkey(event), TEST_PK) == 0);

    /* Verify signature */
    assert(nostr_event_check_signature(event));

    nostr_nip51_list_free(list);
    nostr_event_free(event);
    printf("  OK: mute list event created and signed\n");
}

static void test_mute_list_roundtrip(void) {
    printf("Testing mute list roundtrip...\n");

    /* Create list with public and private entries */
    NostrList *list = nostr_nip51_list_new();
    nostr_nip51_mute_user(list, "public_user", false);
    nostr_nip51_mute_user(list, "private_user", true);
    nostr_nip51_mute_word(list, "public_word", false);
    nostr_nip51_mute_word(list, "secret_word", true);

    /* Create event */
    NostrEvent *event = nostr_nip51_create_mute_list(list, TEST_SK);
    assert(event != NULL);

    /* Parse event */
    NostrList *parsed = nostr_nip51_parse_list(event, TEST_SK);
    assert(parsed != NULL);
    assert(parsed->count == 4);

    /* Verify we got all entries */
    int found_public_user = 0, found_private_user = 0;
    int found_public_word = 0, found_secret_word = 0;

    for (size_t i = 0; i < parsed->count; i++) {
        NostrListEntry *e = parsed->entries[i];
        if (strcmp(e->tag_name, "p") == 0 && strcmp(e->value, "public_user") == 0)
            found_public_user = 1;
        if (strcmp(e->tag_name, "p") == 0 && strcmp(e->value, "private_user") == 0)
            found_private_user = 1;
        if (strcmp(e->tag_name, "word") == 0 && strcmp(e->value, "public_word") == 0)
            found_public_word = 1;
        if (strcmp(e->tag_name, "word") == 0 && strcmp(e->value, "secret_word") == 0)
            found_secret_word = 1;
    }

    assert(found_public_user);
    assert(found_private_user);
    assert(found_public_word);
    assert(found_secret_word);

    nostr_nip51_list_free(list);
    nostr_nip51_list_free(parsed);
    nostr_event_free(event);
    printf("  OK: mute list roundtrip with public and private entries\n");
}

/* ---- Bookmark List Tests ---- */

static void test_create_bookmark_list(void) {
    printf("Testing bookmark list creation...\n");

    NostrList *list = nostr_nip51_list_new();
    assert(list != NULL);

    nostr_nip51_bookmark_event(list, "note123", "wss://relay1.example", false);
    nostr_nip51_bookmark_event(list, "note456", NULL, false);
    nostr_nip51_bookmark_url(list, "https://example.com/article", false);

    NostrEvent *event = nostr_nip51_create_bookmark_list(list, TEST_SK);
    assert(event != NULL);

    /* Verify kind */
    assert(nostr_event_get_kind(event) == NOSTR_KIND_BOOKMARK_LIST);

    /* Verify signature */
    assert(nostr_event_check_signature(event));

    nostr_nip51_list_free(list);
    nostr_event_free(event);
    printf("  OK: bookmark list event created\n");
}

static void test_bookmark_roundtrip(void) {
    printf("Testing bookmark list roundtrip...\n");

    NostrList *list = nostr_nip51_list_new();
    nostr_nip51_bookmark_event(list, "public_note", "wss://relay.test", false);
    nostr_nip51_bookmark_event(list, "private_note", NULL, true);
    nostr_nip51_bookmark_url(list, "https://public.example", false);
    nostr_nip51_bookmark_url(list, "https://private.example", true);

    NostrEvent *event = nostr_nip51_create_bookmark_list(list, TEST_SK);
    assert(event != NULL);

    NostrList *parsed = nostr_nip51_parse_list(event, TEST_SK);
    assert(parsed != NULL);
    assert(parsed->count == 4);

    /* Verify entries */
    int found_public_note = 0, found_private_note = 0;
    int found_public_url = 0, found_private_url = 0;

    for (size_t i = 0; i < parsed->count; i++) {
        NostrListEntry *e = parsed->entries[i];
        if (strcmp(e->tag_name, "e") == 0 && strcmp(e->value, "public_note") == 0)
            found_public_note = 1;
        if (strcmp(e->tag_name, "e") == 0 && strcmp(e->value, "private_note") == 0)
            found_private_note = 1;
        if (strcmp(e->tag_name, "r") == 0 && strcmp(e->value, "https://public.example") == 0)
            found_public_url = 1;
        if (strcmp(e->tag_name, "r") == 0 && strcmp(e->value, "https://private.example") == 0)
            found_private_url = 1;
    }

    assert(found_public_note);
    assert(found_private_note);
    assert(found_public_url);
    assert(found_private_url);

    nostr_nip51_list_free(list);
    nostr_nip51_list_free(parsed);
    nostr_event_free(event);
    printf("  OK: bookmark list roundtrip with mixed entries\n");
}

/* ---- Addressable List Tests ---- */

static void test_addressable_list(void) {
    printf("Testing addressable list (kind 30000)...\n");

    NostrList *list = nostr_nip51_list_new();
    nostr_nip51_list_set_identifier(list, "my-people-list");
    nostr_nip51_list_set_title(list, "My Friends");
    nostr_nip51_mute_user(list, "friend1", false);
    nostr_nip51_mute_user(list, "friend2", false);

    /* Create addressable list */
    NostrEvent *event = nostr_nip51_create_list(NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, list, TEST_SK);
    assert(event != NULL);
    assert(nostr_event_get_kind(event) == 30000);
    assert(nostr_event_check_signature(event));

    /* Parse and verify */
    NostrList *parsed = nostr_nip51_parse_list(event, NULL);
    assert(parsed != NULL);
    assert(parsed->identifier != NULL);
    assert(strcmp(parsed->identifier, "my-people-list") == 0);
    assert(parsed->title != NULL);
    assert(strcmp(parsed->title, "My Friends") == 0);
    assert(parsed->count == 2);

    nostr_nip51_list_free(list);
    nostr_nip51_list_free(parsed);
    nostr_event_free(event);
    printf("  OK: addressable list with d-tag and title\n");
}

/* ---- Edge Cases ---- */

static void test_parse_without_key(void) {
    printf("Testing parse without private key...\n");

    NostrList *list = nostr_nip51_list_new();
    nostr_nip51_mute_user(list, "public_user", false);
    nostr_nip51_mute_user(list, "private_user", true);

    NostrEvent *event = nostr_nip51_create_mute_list(list, TEST_SK);
    assert(event != NULL);

    /* Parse without key - should only get public entries */
    NostrList *parsed = nostr_nip51_parse_list(event, NULL);
    assert(parsed != NULL);
    assert(parsed->count == 1);  /* Only public entry */
    assert(strcmp(parsed->entries[0]->value, "public_user") == 0);

    nostr_nip51_list_free(list);
    nostr_nip51_list_free(parsed);
    nostr_event_free(event);
    printf("  OK: parse without key returns only public entries\n");
}

static void test_empty_list(void) {
    printf("Testing empty list...\n");

    NostrList *list = nostr_nip51_list_new();

    NostrEvent *event = nostr_nip51_create_mute_list(list, TEST_SK);
    assert(event != NULL);
    assert(nostr_event_check_signature(event));

    NostrList *parsed = nostr_nip51_parse_list(event, TEST_SK);
    assert(parsed != NULL);
    assert(parsed->count == 0);

    nostr_nip51_list_free(list);
    nostr_nip51_list_free(parsed);
    nostr_event_free(event);
    printf("  OK: empty list creates valid event\n");
}

int main(void) {
    printf("NIP-51 Test Suite\n");
    printf("=================\n\n");

    setup_keys();

    /* Memory management */
    test_list_new_free();
    test_entry_new_free();
    test_list_add_entry();

    /* Convenience builders */
    test_mute_builders();
    test_bookmark_builders();

    /* Private entry encryption */
    test_private_entry_encryption();

    /* Mute lists */
    test_create_mute_list();
    test_mute_list_roundtrip();

    /* Bookmark lists */
    test_create_bookmark_list();
    test_bookmark_roundtrip();

    /* Addressable lists */
    test_addressable_list();

    /* Edge cases */
    test_parse_without_key();
    test_empty_list();

    cleanup_keys();

    printf("\n=================\n");
    printf("All tests passed!\n");
    return 0;
}
