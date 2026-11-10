/**
 * NIP-51 Test Suite
 *
 * Tests for user lists (mutes, bookmarks, etc.)
 */

#include "nostr/nip51/nip51.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-kinds.h"
#include "json.h"
#include "codec-internal.h"
#define PK1 "1111111111111111111111111111111111111111111111111111111111111111"
#define PK2 "2222222222222222222222222222222222222222222222222222222222222222"

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

    nostr_nip51_mute_user(list, PK1, false);
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
    nostr_nip51_mute_user(list, PK1, false);
    nostr_nip51_mute_user(list, PK2, true);
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
        if (strcmp(e->tag_name, "p") == 0 && strcmp(e->value, PK1) == 0)
            found_public_user = 1;
        if (strcmp(e->tag_name, "p") == 0 && strcmp(e->value, PK2) == 0)
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

    nostr_nip51_bookmark_event(list, PK1, "wss://relay1.example", false);
    nostr_nip51_bookmark_event(list, PK2, NULL, false);
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
    nostr_nip51_bookmark_event(list, PK1, "wss://relay.test", false);
    nostr_nip51_bookmark_event(list, PK2, NULL, true);
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
        if (strcmp(e->tag_name, "e") == 0 && strcmp(e->value, PK1) == 0)
            found_public_note = 1;
        if (strcmp(e->tag_name, "e") == 0 && strcmp(e->value, PK2) == 0)
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
    nostr_nip51_mute_user(list, PK1, false);
    nostr_nip51_mute_user(list, PK2, false);

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
    nostr_nip51_mute_user(list, PK1, false);
    nostr_nip51_mute_user(list, PK2, true);

    NostrEvent *event = nostr_nip51_create_mute_list(list, TEST_SK);
    assert(event != NULL);

    /* Parse without key - should only get public entries */
    NostrList *parsed = nostr_nip51_parse_list(event, NULL);
    assert(parsed != NULL);
    assert(parsed->count == 1);  /* Only public entry */
    assert(strcmp(parsed->entries[0]->value, PK1) == 0);

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

static void test_lossless(void) {
    NostrListDocument *d = nostr_nip51_document_new(NOSTR_KIND_SIMPLE_GROUP_LIST, NULL);
    assert(d);
    const char *fields[] = {"group", "id:\"\\\n雪", "wss://nos.lol", "", "tail", ""};
    NostrListTag *t = nostr_nip51_tag_new(fields, 6);
    assert(t && !nostr_nip51_document_append(d, false, t)); nostr_nip51_tag_free(t);
    const char *extension[] = {"future", "", "arbitrary", "trailing"};
    t = nostr_nip51_tag_new(extension, 4);
    assert(t && !nostr_nip51_document_append(d, false, t)); nostr_nip51_tag_free(t);
    const char *private_json = "[[\"group\",\"quoted \\\" \\\\ \\n雪\",\"wss://nos.lol\",\"\",\"extra\"],[\"future\",\"\",\"\"],[]]";
    assert(!nostr_nip51_document_parse_private_tags(d, private_json, strlen(private_json)));
    char *serialized = nostr_nip51_document_serialize_private_tags(d);
    assert(serialized && !strcmp(private_json, serialized));
    free(serialized);
    assert(nostr_nip51_document_tag_count(d, true) == 3);
    t = (NostrListTag *)nostr_nip51_document_tag(d, true, 0);
    assert(nostr_nip51_tag_size(t) == 5 && !strcmp(nostr_nip51_tag_field(t, 3), ""));
    assert(!nostr_nip51_tag_field(t, 5));
    const char *bad[] = {"", "{}", "[", "[[\"p\",1]]", "[null]", "[[\"word\",\"a\\u0000b\"]]", "[] garbage", "[[\"x\"],]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(nostr_nip51_document_parse_private_tags(d, bad[i], strlen(bad[i])) < 0);
        assert(nostr_nip51_document_tag_count(d, true) == 3); /* atomic on failure */
    }
    assert(!nostr_nip51_document_build_unsigned(d, TEST_PK, 1, NULL));
    NostrEvent *ev = nostr_nip51_document_build_unsigned(d, TEST_PK, 1, "external-ciphertext");
    assert(ev && !nostr_event_get_sig(ev));
    NostrListDocument *parsed = nostr_nip51_document_parse_event(ev);
    assert(parsed && nostr_nip51_document_tag_count(parsed, false) == 2);
    assert(!nostr_nip51_document_build_unsigned(parsed, TEST_PK, 1, ""));
    assert(nostr_nip51_document_private_state(parsed) == NOSTR_LIST_PRIVATE_ENCRYPTED_NIP44);
    assert(nostr_nip51_document_tag_count(parsed, true) == 0);
    NostrEvent *roundtrip = nostr_nip51_document_build_unsigned(parsed, TEST_PK, 1, NULL);
    assert(roundtrip && !strcmp(nostr_event_get_content(roundtrip), "external-ciphertext"));
    const NostrListTag *group = nostr_nip51_document_tag(parsed, false, 0);
    assert(nostr_nip51_tag_size(group) == 6 && !strcmp(nostr_nip51_tag_field(group, 5), ""));
    assert(!nostr_nip51_document_remove(parsed, false, 0));
    assert(!strcmp(nostr_nip51_tag_field(nostr_nip51_document_tag(parsed, false, 0), 0), "future"));
    nostr_nip51_document_mark_unreadable(parsed);
    assert(!nostr_nip51_document_build_unsigned(parsed, TEST_PK, 1, ""));
    nostr_nip51_document_free(parsed); nostr_event_free(roundtrip);
    /* Base64 NIP-44 payloads can contain the letters iv by chance. Only
     * the NIP-04 query marker selects the legacy decryptor. */
    nostr_event_set_content(ev, "Anip44ivpayload");
    parsed = nostr_nip51_document_parse_event(ev);
    assert(parsed && nostr_nip51_document_private_state(parsed) == NOSTR_LIST_PRIVATE_ENCRYPTED_NIP44);
    nostr_nip51_document_free(parsed);
    nostr_event_set_content(ev, "legacy?iv=123");
    parsed = nostr_nip51_document_parse_event(ev);
    assert(parsed && nostr_nip51_document_private_state(parsed) == NOSTR_LIST_PRIVATE_LEGACY_NIP04);
    assert(!nostr_nip51_parse_list(ev, TEST_SK)); /* never return a partial public list */
    nostr_nip51_document_free(parsed); nostr_event_free(ev); nostr_nip51_document_free(d);
}
static void test_registry_and_bounds(void) {
    FILE *fixture = fopen(NIP51_SCHEMA_FIXTURE, "r"); assert(fixture);
    char line[512]; size_t rows = 0;
    while (fgets(line, sizeof(line), fixture)) {
        if (line[0] == '#') continue;
        char *sep = strchr(line, '|'); assert(sep); *sep++ = 0;
        char *end = strchr(sep, '|'); assert(end); *end = 0;
        const NostrListSchema *s = nostr_nip51_schema_lookup(atoi(line));
        assert(s && !strcmp(nostr_nip51_schema_items(s), sep)); rows++;
    }
    fclose(fixture); assert(rows == nostr_nip51_schema_count());
    assert(nostr_nip51_schema_read_only(nostr_nip51_schema_lookup(3)));
    assert(nostr_nip51_schema_read_only(nostr_nip51_schema_lookup(10002)));
    assert(!nostr_nip51_schema_lookup(42));
    NostrListDocument *d = nostr_nip51_document_new(NOSTR_KIND_PRIVATE_RELAY_LIST, NULL);
    const char *relay[] = {"relay", "wss://nos.lol"};
    NostrListTag *t = nostr_nip51_tag_new(relay, 2); assert(t);
    assert(!nostr_nip51_document_append(d, false, t));
    assert(nostr_nip51_document_validate(d) < 0);
    assert(!nostr_nip51_document_remove(d, false, 0));
    assert(!nostr_nip51_document_append(d, true, t));
    assert(!nostr_nip51_document_validate(d)); nostr_nip51_document_free(d);
    d = nostr_nip51_document_new(NOSTR_KIND_MUTE_LIST, NULL); assert(d);
    for (size_t i = 0; i < NOSTR_LIST_MAX_TAGS; i++) assert(!nostr_nip51_document_append(d, false, t));
    assert(nostr_nip51_document_append(d, false, t) == -E2BIG);
    nostr_nip51_document_free(d);
    d = nostr_nip51_document_new(NOSTR_KIND_FOLLOW_LIST, NULL); assert(d);
    for (size_t i = 0; i < NOSTR_LIST_MAX_FOLLOW_TAGS; i++) assert(!nostr_nip51_document_append(d, false, t));
    assert(nostr_nip51_document_append(d, false, t) == -E2BIG);
    assert(!nostr_nip51_document_build_unsigned(d, TEST_PK, 1, "")); /* read only */
    nostr_nip51_document_free(d); nostr_nip51_tag_free(t);
    const char *many[33]; for (size_t i = 0; i < 33; i++) many[i] = "x";
    assert(!nostr_nip51_tag_new(many, 33));
    t = nostr_nip51_tag_new(many, 32); assert(t); nostr_nip51_tag_free(t);
    char field[16386]; memset(field, 'x', sizeof(field)); field[16384] = 0;
    const char *f[] = {"unknown", field};
    t = nostr_nip51_tag_new(f, 2); assert(t); nostr_nip51_tag_free(t);
    field[16384] = 'x'; field[16385] = 0; assert(!nostr_nip51_tag_new(f, 2));
    char id[258]; memset(id, 'x', 257); id[256] = 0;
    d = nostr_nip51_document_new(NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, id); assert(d); nostr_nip51_document_free(d);
    id[256] = 'x'; id[257] = 0; assert(!nostr_nip51_document_new(NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, id));
    d = nostr_nip51_document_new(NOSTR_KIND_MUTE_SETS, "01"); assert(d);
    assert(nostr_nip51_document_validate(d) < 0); nostr_nip51_document_free(d);
    d = nostr_nip51_document_new(NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, "mute");
    assert(d && nostr_nip51_document_deprecated(d)); nostr_nip51_document_free(d);
    d = nostr_nip51_document_new(NOSTR_KIND_SLASH_COMMAND_DEFINITION, "command"); assert(d);
    const char *arg[] = {"arg", "mode", "string", "optional", "Mode", "one", "two", "three"};
    t = nostr_nip51_tag_new(arg, 8); assert(t && !nostr_nip51_document_append(d, true, t));
    assert(nostr_nip51_validate_item(nostr_nip51_schema_lookup(NOSTR_KIND_SLASH_COMMAND_DEFINITION), t) == NOSTR_LIST_ITEM_VALID);
    nostr_nip51_tag_free(t); nostr_nip51_document_free(d);
}
static void test_remote_bounds(void) {
    NostrEvent *source = codec_event(NOSTR_KIND_FOLLOW_LIST, TEST_PK, 1, nostr_tags_new(0), ""); assert(source);
    const char *p[] = {"p", PK1, "", "petname"};
    for (size_t i = 0; i < 5000; i++) assert(!codec_tag_append(nostr_event_get_tags(source), p, 4));
    NostrListDocument *d = nostr_nip51_document_parse_event(source);
    assert(d && nostr_nip51_document_tag_count(d, false) == 5000);
    assert(!strcmp(nostr_nip51_tag_field(nostr_nip51_document_tag(d, false, 0), 2), ""));
    nostr_nip51_document_free(d);
    nostr_event_set_kind(source, NOSTR_KIND_MUTE_LIST);
    assert(!nostr_nip51_document_parse_event(source) && errno == E2BIG);
    nostr_event_set_kind(source, NOSTR_KIND_FOLLOW_LIST);
    for (size_t i = 5000; i <= NOSTR_LIST_MAX_FOLLOW_TAGS; i++) assert(!codec_tag_append(nostr_event_get_tags(source), NULL, 0));
    assert(!nostr_nip51_document_parse_event(source) && errno == E2BIG);
    nostr_event_free(source);
    char *huge = malloc(NOSTR_LIST_MAX_FOLLOW_BYTES + 2); assert(huge);
    memset(huge, 'x', NOSTR_LIST_MAX_FOLLOW_BYTES + 1); huge[NOSTR_LIST_MAX_FOLLOW_BYTES + 1] = 0;
    source = codec_event(NOSTR_KIND_FOLLOW_LIST, TEST_PK, 1, nostr_tags_new(0), huge);
    assert(source && !nostr_nip51_document_parse_event(source) && errno == E2BIG);
    free(huge); nostr_event_free(source);
    d = nostr_nip51_document_new(NOSTR_KIND_MUTE_LIST, NULL); assert(d);
    char *plain = malloc(NOSTR_LIST_MAX_PRIVATE_BYTES + 1); assert(plain);
    memset(plain, ' ', NOSTR_LIST_MAX_PRIVATE_BYTES + 1); plain[0] = '['; plain[1] = ']';
    assert(!nostr_nip51_document_parse_private_tags(d, plain, NOSTR_LIST_MAX_PRIVATE_BYTES));
    assert(nostr_nip51_document_parse_private_tags(d, plain, NOSTR_LIST_MAX_PRIVATE_BYTES + 1) < 0);
    free(plain); nostr_nip51_document_free(d);
}
static void test_fail_closed(void) {
    NostrList *list = nostr_nip51_list_new(); assert(list);
    const char *text = "quotes \" backslash \\ newline\n雪";
    nostr_nip51_mute_word(list, text, true);
    NostrEvent *ev = nostr_nip51_create_mute_list(list, TEST_SK); assert(ev);
    NostrList *parsed = nostr_nip51_parse_list(ev, TEST_SK);
    assert(parsed && parsed->count == 1 && !strcmp(parsed->entries[0]->value, text));
    nostr_nip51_list_free(parsed); nostr_event_free(ev); nostr_nip51_list_free(list);
    list = nostr_nip51_list_new(); assert(list);
    char *oversize = malloc(16385); assert(oversize); memset(oversize, 'x', 16384); oversize[16384] = 0;
    for (int i = 0; i < 4; i++) nostr_nip51_mute_word(list, oversize, true);
    assert(!nostr_nip51_create_mute_list(list, TEST_SK));
    free(oversize); nostr_nip51_list_free(list);
}

int main(void) {
    printf("NIP-51 Test Suite\n");
    printf("=================\n\n");

    nostr_json_init();
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

    test_lossless();
    test_registry_and_bounds();
    test_remote_bounds();
    test_fail_closed();
    cleanup_keys();
    nostr_json_cleanup();

    printf("\n=================\n");
    printf("All tests passed!\n");
    return 0;
}
