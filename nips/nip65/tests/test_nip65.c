/**
 * NIP-65 Relay List Metadata - Test Suite
 */
#include "nostr/nip65/nip65.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void fill32(unsigned char b[32], unsigned char v) {
    for (int i = 0; i < 32; ++i) b[i] = v;
}

static void test_url_validation(void) {
    printf("test_url_validation... ");

    /* Valid URLs */
    assert(nostr_nip65_is_valid_relay_url("wss://relay.example.com"));
    assert(nostr_nip65_is_valid_relay_url("ws://localhost"));
    assert(nostr_nip65_is_valid_relay_url("wss://relay.example.com:443"));
    assert(nostr_nip65_is_valid_relay_url("wss://relay.example.com/path"));
    assert(nostr_nip65_is_valid_relay_url("  wss://relay.example.com  "));

    /* Invalid URLs */
    assert(!nostr_nip65_is_valid_relay_url(NULL));
    assert(!nostr_nip65_is_valid_relay_url(""));
    assert(!nostr_nip65_is_valid_relay_url("https://relay.example.com"));
    assert(!nostr_nip65_is_valid_relay_url("http://relay.example.com"));
    assert(!nostr_nip65_is_valid_relay_url("wss://"));
    assert(!nostr_nip65_is_valid_relay_url("ftp://relay.example.com"));

    printf("OK\n");
}

static void test_url_normalization(void) {
    printf("test_url_normalization... ");

    char *normalized;

    /* Basic normalization */
    normalized = nostr_nip65_normalize_url("wss://RELAY.EXAMPLE.COM");
    assert(normalized != NULL);
    assert(strcmp(normalized, "wss://relay.example.com") == 0);
    free(normalized);

    /* Trailing slash removal */
    normalized = nostr_nip65_normalize_url("wss://relay.example.com/");
    assert(normalized != NULL);
    assert(strcmp(normalized, "wss://relay.example.com") == 0);
    free(normalized);

    /* Path preservation (non-root) */
    normalized = nostr_nip65_normalize_url("wss://relay.example.com/custom");
    assert(normalized != NULL);
    assert(strcmp(normalized, "wss://relay.example.com/custom") == 0);
    free(normalized);

    /* Port preservation */
    normalized = nostr_nip65_normalize_url("wss://relay.example.com:8080");
    assert(normalized != NULL);
    assert(strcmp(normalized, "wss://relay.example.com:8080") == 0);
    free(normalized);

    /* Whitespace trimming */
    normalized = nostr_nip65_normalize_url("  wss://relay.example.com  ");
    assert(normalized != NULL);
    assert(strcmp(normalized, "wss://relay.example.com") == 0);
    free(normalized);

    /* Invalid returns NULL */
    assert(nostr_nip65_normalize_url(NULL) == NULL);
    assert(nostr_nip65_normalize_url("") == NULL);
    assert(nostr_nip65_normalize_url("https://invalid.com") == NULL);

    printf("OK\n");
}

static void test_permission_conversion(void) {
    printf("test_permission_conversion... ");

    /* To string */
    assert(nostr_nip65_permission_to_string(NOSTR_RELAY_PERM_READ) != NULL);
    assert(strcmp(nostr_nip65_permission_to_string(NOSTR_RELAY_PERM_READ), "read") == 0);
    assert(strcmp(nostr_nip65_permission_to_string(NOSTR_RELAY_PERM_WRITE), "write") == 0);
    assert(nostr_nip65_permission_to_string(NOSTR_RELAY_PERM_READWRITE) == NULL);

    /* From string */
    assert(nostr_nip65_permission_from_string("read") == NOSTR_RELAY_PERM_READ);
    assert(nostr_nip65_permission_from_string("write") == NOSTR_RELAY_PERM_WRITE);
    assert(nostr_nip65_permission_from_string(NULL) == NOSTR_RELAY_PERM_READWRITE);
    assert(nostr_nip65_permission_from_string("") == NOSTR_RELAY_PERM_READWRITE);
    assert(nostr_nip65_permission_from_string("invalid") == NOSTR_RELAY_PERM_READWRITE);

    printf("OK\n");
}

static void test_entry_operations(void) {
    printf("test_entry_operations... ");

    /* Create entry */
    NostrRelayEntry *entry = nostr_nip65_entry_new("wss://relay.example.com", NOSTR_RELAY_PERM_READ);
    assert(entry != NULL);
    assert(entry->url != NULL);
    assert(strcmp(entry->url, "wss://relay.example.com") == 0);
    assert(entry->permission == NOSTR_RELAY_PERM_READ);

    /* Check permissions */
    assert(nostr_nip65_entry_is_readable(entry));
    assert(!nostr_nip65_entry_is_writable(entry));

    /* Copy entry */
    NostrRelayEntry *copy = nostr_nip65_entry_copy(entry);
    assert(copy != NULL);
    assert(copy != entry);
    assert(strcmp(copy->url, entry->url) == 0);
    assert(copy->permission == entry->permission);

    nostr_nip65_entry_free(copy);
    nostr_nip65_entry_free(entry);

    /* Create write-only entry */
    entry = nostr_nip65_entry_new("wss://write.relay.com", NOSTR_RELAY_PERM_WRITE);
    assert(!nostr_nip65_entry_is_readable(entry));
    assert(nostr_nip65_entry_is_writable(entry));
    nostr_nip65_entry_free(entry);

    /* Create readwrite entry */
    entry = nostr_nip65_entry_new("wss://rw.relay.com", NOSTR_RELAY_PERM_READWRITE);
    assert(nostr_nip65_entry_is_readable(entry));
    assert(nostr_nip65_entry_is_writable(entry));
    nostr_nip65_entry_free(entry);

    /* Invalid URL returns NULL */
    assert(nostr_nip65_entry_new(NULL, NOSTR_RELAY_PERM_READ) == NULL);
    assert(nostr_nip65_entry_new("", NOSTR_RELAY_PERM_READ) == NULL);
    assert(nostr_nip65_entry_new("https://invalid.com", NOSTR_RELAY_PERM_READ) == NULL);

    printf("OK\n");
}

static void test_list_operations(void) {
    printf("test_list_operations... ");

    NostrRelayList *list = nostr_nip65_list_new();
    assert(list != NULL);
    assert(list->count == 0);

    /* Add relays */
    int rc = nostr_nip65_add_relay(list, "wss://relay1.example.com", NOSTR_RELAY_PERM_READWRITE);
    assert(rc == 0);
    assert(list->count == 1);

    rc = nostr_nip65_add_relay(list, "wss://relay2.example.com", NOSTR_RELAY_PERM_READ);
    assert(rc == 0);
    assert(list->count == 2);

    rc = nostr_nip65_add_relay(list, "wss://relay3.example.com", NOSTR_RELAY_PERM_WRITE);
    assert(rc == 0);
    assert(list->count == 3);

    /* Update existing relay (dedup by URL) */
    rc = nostr_nip65_add_relay(list, "wss://relay1.example.com", NOSTR_RELAY_PERM_READ);
    assert(rc == 0);
    assert(list->count == 3);  /* Still 3, not 4 */
    NostrRelayEntry *found = nostr_nip65_find_relay(list, "wss://relay1.example.com");
    assert(found != NULL);
    assert(found->permission == NOSTR_RELAY_PERM_READ);  /* Updated */

    /* Find relay */
    found = nostr_nip65_find_relay(list, "wss://relay2.example.com");
    assert(found != NULL);
    assert(found->permission == NOSTR_RELAY_PERM_READ);

    found = nostr_nip65_find_relay(list, "wss://nonexistent.com");
    assert(found == NULL);

    /* Get read relays */
    size_t count;
    char **read_relays = nostr_nip65_get_read_relays(list, &count);
    assert(read_relays != NULL);
    assert(count == 2);  /* relay1 (now read) and relay2 */
    nostr_nip65_free_string_array(read_relays);

    /* Get write relays */
    char **write_relays = nostr_nip65_get_write_relays(list, &count);
    assert(write_relays != NULL);
    assert(count == 1);  /* relay3 only */
    nostr_nip65_free_string_array(write_relays);

    /* Remove relay */
    rc = nostr_nip65_remove_relay(list, "wss://relay2.example.com");
    assert(rc == 0);
    assert(list->count == 2);

    rc = nostr_nip65_remove_relay(list, "wss://nonexistent.com");
    assert(rc == -ENOENT);

    /* Copy list */
    NostrRelayList *copy = nostr_nip65_list_copy(list);
    assert(copy != NULL);
    assert(copy != list);
    assert(copy->count == list->count);

    nostr_nip65_list_free(copy);
    nostr_nip65_list_free(list);

    printf("OK\n");
}

static void test_event_creation_and_parsing(void) {
    printf("test_event_creation_and_parsing... ");

    /* Create relay list */
    NostrRelayList *list = nostr_nip65_list_new();
    nostr_nip65_add_relay(list, "wss://relay1.example.com", NOSTR_RELAY_PERM_READWRITE);
    nostr_nip65_add_relay(list, "wss://relay2.example.com", NOSTR_RELAY_PERM_READ);
    nostr_nip65_add_relay(list, "wss://relay3.example.com", NOSTR_RELAY_PERM_WRITE);

    /* Create event */
    NostrEvent *ev = nostr_event_new();
    unsigned char author[32];
    fill32(author, 0xAB);

    int rc = nostr_nip65_create_relay_list(ev, author, list, 1700000000);
    assert(rc == 0);
    assert(nostr_event_get_kind(ev) == 10002);

    /* Verify content is empty */
    const char *content = nostr_event_get_content(ev);
    assert(content != NULL);
    assert(strcmp(content, "") == 0);

    /* Parse back */
    NostrRelayList *parsed = NULL;
    rc = nostr_nip65_parse_relay_list(ev, &parsed);
    assert(rc == 0);
    assert(parsed != NULL);
    assert(parsed->count == 3);

    /* Verify entries */
    NostrRelayEntry *e1 = nostr_nip65_find_relay(parsed, "wss://relay1.example.com");
    assert(e1 != NULL);
    assert(e1->permission == NOSTR_RELAY_PERM_READWRITE);

    NostrRelayEntry *e2 = nostr_nip65_find_relay(parsed, "wss://relay2.example.com");
    assert(e2 != NULL);
    assert(e2->permission == NOSTR_RELAY_PERM_READ);

    NostrRelayEntry *e3 = nostr_nip65_find_relay(parsed, "wss://relay3.example.com");
    assert(e3 != NULL);
    assert(e3->permission == NOSTR_RELAY_PERM_WRITE);

    nostr_nip65_list_free(parsed);
    nostr_nip65_list_free(list);
    nostr_event_free(ev);

    printf("OK\n");
}

static void test_event_update(void) {
    printf("test_event_update... ");

    /* Create initial event */
    NostrRelayList *list1 = nostr_nip65_list_new();
    nostr_nip65_add_relay(list1, "wss://old.relay.com", NOSTR_RELAY_PERM_READWRITE);

    NostrEvent *ev = nostr_event_new();
    unsigned char author[32];
    fill32(author, 0xCD);

    nostr_nip65_create_relay_list(ev, author, list1, 1700000000);

    /* Update with new list */
    NostrRelayList *list2 = nostr_nip65_list_new();
    nostr_nip65_add_relay(list2, "wss://new.relay.com", NOSTR_RELAY_PERM_READ);
    nostr_nip65_add_relay(list2, "wss://another.relay.com", NOSTR_RELAY_PERM_WRITE);

    int rc = nostr_nip65_update_relay_list(ev, list2);
    assert(rc == 0);

    /* Parse and verify */
    NostrRelayList *parsed = NULL;
    rc = nostr_nip65_parse_relay_list(ev, &parsed);
    assert(rc == 0);
    assert(parsed->count == 2);
    assert(nostr_nip65_find_relay(parsed, "wss://old.relay.com") == NULL);
    assert(nostr_nip65_find_relay(parsed, "wss://new.relay.com") != NULL);
    assert(nostr_nip65_find_relay(parsed, "wss://another.relay.com") != NULL);

    nostr_nip65_list_free(parsed);
    nostr_nip65_list_free(list1);
    nostr_nip65_list_free(list2);
    nostr_event_free(ev);

    printf("OK\n");
}

static void test_parse_wrong_kind(void) {
    printf("test_parse_wrong_kind... ");

    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);  /* Note, not relay list */

    NostrRelayList *list = NULL;
    int rc = nostr_nip65_parse_relay_list(ev, &list);
    assert(rc == -ENOENT);
    assert(list == NULL);

    nostr_event_free(ev);

    printf("OK\n");
}

static void test_empty_list(void) {
    printf("test_empty_list... ");

    NostrRelayList *list = nostr_nip65_list_new();
    NostrEvent *ev = nostr_event_new();
    unsigned char author[32];
    fill32(author, 0xEF);

    /* Create event with empty list */
    int rc = nostr_nip65_create_relay_list(ev, author, list, 1700000000);
    assert(rc == 0);

    /* Parse back */
    NostrRelayList *parsed = NULL;
    rc = nostr_nip65_parse_relay_list(ev, &parsed);
    assert(rc == 0);
    assert(parsed != NULL);
    assert(parsed->count == 0);

    /* Get relays from empty list */
    size_t count;
    char **relays = nostr_nip65_get_read_relays(parsed, &count);
    assert(relays != NULL);
    assert(count == 0);
    nostr_nip65_free_string_array(relays);

    nostr_nip65_list_free(parsed);
    nostr_nip65_list_free(list);
    nostr_event_free(ev);

    printf("OK\n");
}

int main(void) {
    printf("Running NIP-65 tests...\n");

    test_url_validation();
    test_url_normalization();
    test_permission_conversion();
    test_entry_operations();
    test_list_operations();
    test_event_creation_and_parsing();
    test_event_update();
    test_parse_wrong_kind();
    test_empty_list();

    printf("\nAll NIP-65 tests passed!\n");
    return 0;
}
