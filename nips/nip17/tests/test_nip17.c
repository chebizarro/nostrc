/**
 * NIP-17 Test Suite
 *
 * Tests for gift-wrapped direct messages
 */

#include "nostr/nip17/nip17.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-kinds.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test keys - generated for testing only */
static const char *ALICE_SK = NULL;
static const char *ALICE_PK = NULL;
static const char *BOB_SK = NULL;
static const char *BOB_PK = NULL;

static void setup_keys(void) {
    /* Generate fresh keypairs for each test run */
    ALICE_SK = nostr_key_generate_private();
    assert(ALICE_SK != NULL);
    ALICE_PK = nostr_key_get_public(ALICE_SK);
    assert(ALICE_PK != NULL);

    BOB_SK = nostr_key_generate_private();
    assert(BOB_SK != NULL);
    BOB_PK = nostr_key_get_public(BOB_SK);
    assert(BOB_PK != NULL);

    printf("Test keys generated\n");
}

static void cleanup_keys(void) {
    free((void *)ALICE_SK);
    free((void *)ALICE_PK);
    free((void *)BOB_SK);
    free((void *)BOB_PK);
}

static void test_create_rumor(void) {
    printf("Testing rumor creation...\n");

    NostrEvent *rumor = nostr_nip17_create_rumor(ALICE_PK, BOB_PK,
                                                  "Hello Bob!", 0);
    assert(rumor != NULL);

    /* Check kind */
    assert(nostr_event_get_kind(rumor) == NOSTR_KIND_DIRECT_MESSAGE);

    /* Check pubkey */
    assert(strcmp(nostr_event_get_pubkey(rumor), ALICE_PK) == 0);

    /* Check content */
    assert(strcmp(nostr_event_get_content(rumor), "Hello Bob!") == 0);

    /* Rumor should NOT be signed */
    assert(nostr_event_get_sig(rumor) == NULL);

    nostr_event_free(rumor);
    printf("  OK: rumor created correctly\n");
}

static void test_create_seal(void) {
    printf("Testing seal creation...\n");

    NostrEvent *rumor = nostr_nip17_create_rumor(ALICE_PK, BOB_PK,
                                                  "Secret message", 0);
    assert(rumor != NULL);

    NostrEvent *seal = nostr_nip17_create_seal(rumor, ALICE_SK, BOB_PK);
    assert(seal != NULL);

    /* Check kind */
    assert(nostr_event_get_kind(seal) == NOSTR_KIND_SEAL);

    /* Check pubkey is Alice's */
    assert(strcmp(nostr_event_get_pubkey(seal), ALICE_PK) == 0);

    /* Seal should be signed */
    assert(nostr_event_get_sig(seal) != NULL);

    /* Signature should verify */
    assert(nostr_event_check_signature(seal));

    /* Content should be encrypted (base64) */
    const char *content = nostr_event_get_content(seal);
    assert(content != NULL);
    assert(strlen(content) > 0);

    nostr_event_free(rumor);
    nostr_event_free(seal);
    printf("  OK: seal created and signed correctly\n");
}

static void test_create_gift_wrap(void) {
    printf("Testing gift wrap creation...\n");

    NostrEvent *rumor = nostr_nip17_create_rumor(ALICE_PK, BOB_PK,
                                                  "Wrapped message", 0);
    assert(rumor != NULL);

    NostrEvent *seal = nostr_nip17_create_seal(rumor, ALICE_SK, BOB_PK);
    assert(seal != NULL);

    NostrEvent *gift_wrap = nostr_nip17_create_gift_wrap(seal, BOB_PK);
    assert(gift_wrap != NULL);

    /* Check kind */
    assert(nostr_event_get_kind(gift_wrap) == NOSTR_KIND_GIFT_WRAP);

    /* Pubkey should be ephemeral (not Alice's) */
    assert(strcmp(nostr_event_get_pubkey(gift_wrap), ALICE_PK) != 0);

    /* Gift wrap should be signed */
    assert(nostr_event_get_sig(gift_wrap) != NULL);
    assert(nostr_event_check_signature(gift_wrap));

    /* Should validate */
    assert(nostr_nip17_validate_gift_wrap(gift_wrap));

    nostr_event_free(rumor);
    nostr_event_free(seal);
    nostr_event_free(gift_wrap);
    printf("  OK: gift wrap created with ephemeral key\n");
}

static void test_wrap_dm_convenience(void) {
    printf("Testing wrap_dm convenience function...\n");

    NostrEvent *gift_wrap = nostr_nip17_wrap_dm(ALICE_SK, BOB_PK,
                                                 "Convenient message");
    assert(gift_wrap != NULL);

    assert(nostr_event_get_kind(gift_wrap) == NOSTR_KIND_GIFT_WRAP);
    assert(nostr_event_check_signature(gift_wrap));
    assert(nostr_nip17_validate_gift_wrap(gift_wrap));

    nostr_event_free(gift_wrap);
    printf("  OK: wrap_dm creates valid gift wrap\n");
}

static void test_unwrap_gift_wrap(void) {
    printf("Testing gift wrap unwrapping...\n");

    NostrEvent *gift_wrap = nostr_nip17_wrap_dm(ALICE_SK, BOB_PK,
                                                 "Message to unwrap");
    assert(gift_wrap != NULL);

    /* Bob unwraps the gift wrap */
    NostrEvent *seal = nostr_nip17_unwrap_gift_wrap(gift_wrap, BOB_SK);
    assert(seal != NULL);

    /* Should be a seal */
    assert(nostr_event_get_kind(seal) == NOSTR_KIND_SEAL);

    /* Seal should be from Alice */
    assert(strcmp(nostr_event_get_pubkey(seal), ALICE_PK) == 0);

    /* Seal signature should verify */
    assert(nostr_event_check_signature(seal));

    nostr_event_free(gift_wrap);
    nostr_event_free(seal);
    printf("  OK: gift wrap unwrapped to seal\n");
}

static void test_unwrap_seal(void) {
    printf("Testing seal unwrapping...\n");

    NostrEvent *gift_wrap = nostr_nip17_wrap_dm(ALICE_SK, BOB_PK,
                                                 "Deep message");
    assert(gift_wrap != NULL);

    NostrEvent *seal = nostr_nip17_unwrap_gift_wrap(gift_wrap, BOB_SK);
    assert(seal != NULL);

    /* Bob unwraps the seal */
    NostrEvent *rumor = nostr_nip17_unwrap_seal(seal, BOB_SK);
    assert(rumor != NULL);

    /* Should be a DM rumor */
    assert(nostr_event_get_kind(rumor) == NOSTR_KIND_DIRECT_MESSAGE);

    /* Content should match */
    assert(strcmp(nostr_event_get_content(rumor), "Deep message") == 0);

    /* Pubkey should be Alice's */
    assert(strcmp(nostr_event_get_pubkey(rumor), ALICE_PK) == 0);

    nostr_event_free(gift_wrap);
    nostr_event_free(seal);
    nostr_event_free(rumor);
    printf("  OK: seal unwrapped to rumor\n");
}

static void test_decrypt_dm_roundtrip(void) {
    printf("Testing full decrypt_dm roundtrip...\n");

    const char *original_msg = "Hello Bob, this is a secret message from Alice!";

    /* Alice creates a DM for Bob */
    NostrEvent *gift_wrap = nostr_nip17_wrap_dm(ALICE_SK, BOB_PK, original_msg);
    assert(gift_wrap != NULL);

    /* Bob decrypts the DM */
    char *content = NULL;
    char *sender_pk = NULL;

    int rc = nostr_nip17_decrypt_dm(gift_wrap, BOB_SK, &content, &sender_pk);
    assert(rc == 0);
    assert(content != NULL);
    assert(sender_pk != NULL);

    /* Verify content matches */
    assert(strcmp(content, original_msg) == 0);

    /* Verify sender is Alice */
    assert(strcmp(sender_pk, ALICE_PK) == 0);

    free(content);
    free(sender_pk);
    nostr_event_free(gift_wrap);
    printf("  OK: full roundtrip successful\n");
}

static void test_wrong_recipient_fails(void) {
    printf("Testing wrong recipient cannot decrypt...\n");

    /* Alice sends to Bob */
    NostrEvent *gift_wrap = nostr_nip17_wrap_dm(ALICE_SK, BOB_PK,
                                                 "Only for Bob");
    assert(gift_wrap != NULL);

    /* Alice (wrong recipient) tries to decrypt */
    char *content = NULL;
    int rc = nostr_nip17_decrypt_dm(gift_wrap, ALICE_SK, &content, NULL);

    /* Should fail */
    assert(rc != 0);
    assert(content == NULL);

    nostr_event_free(gift_wrap);
    printf("  OK: wrong recipient cannot decrypt\n");
}

static void test_validate_gift_wrap(void) {
    printf("Testing gift wrap validation...\n");

    NostrEvent *gift_wrap = nostr_nip17_wrap_dm(ALICE_SK, BOB_PK, "Test");
    assert(gift_wrap != NULL);

    assert(nostr_nip17_validate_gift_wrap(gift_wrap));

    /* Invalid: wrong kind */
    nostr_event_set_kind(gift_wrap, NOSTR_KIND_TEXT_NOTE);
    assert(!nostr_nip17_validate_gift_wrap(gift_wrap));

    nostr_event_free(gift_wrap);
    printf("  OK: validation works correctly\n");
}

static void test_validate_seal(void) {
    printf("Testing seal validation...\n");

    NostrEvent *rumor = nostr_nip17_create_rumor(ALICE_PK, BOB_PK, "Test", 0);
    assert(rumor != NULL);

    NostrEvent *seal = nostr_nip17_create_seal(rumor, ALICE_SK, BOB_PK);
    assert(seal != NULL);

    /* Valid seal */
    assert(nostr_nip17_validate_seal(seal, NULL));

    /* Valid seal with matching rumor */
    assert(nostr_nip17_validate_seal(seal, rumor));

    /* Invalid: mismatched pubkey */
    NostrEvent *bad_rumor = nostr_nip17_create_rumor(BOB_PK, ALICE_PK, "Test", 0);
    assert(!nostr_nip17_validate_seal(seal, bad_rumor));

    nostr_event_free(rumor);
    nostr_event_free(seal);
    nostr_event_free(bad_rumor);
    printf("  OK: seal validation works correctly\n");
}

/* ---- DM Relay Preferences Tests ---- */

static void test_create_dm_relay_list(void) {
    printf("Testing DM relay list creation...\n");

    const char *relays[] = {
        "wss://relay1.example.com",
        "wss://relay2.example.com",
        "wss://relay3.example.com",
        NULL
    };

    NostrEvent *event = nostr_nip17_create_dm_relay_list(relays, ALICE_SK);
    assert(event != NULL);

    /* Check kind is 10050 */
    assert(nostr_event_get_kind(event) == 10050);

    /* Check pubkey is Alice's */
    assert(strcmp(nostr_event_get_pubkey(event), ALICE_PK) == 0);

    /* Check signature */
    assert(nostr_event_check_signature(event));

    /* Content should be empty */
    const char *content = nostr_event_get_content(event);
    assert(content != NULL && strlen(content) == 0);

    nostr_event_free(event);
    printf("  OK: DM relay list event created correctly\n");
}

static void test_parse_dm_relay_list(void) {
    printf("Testing DM relay list parsing...\n");

    const char *relays[] = {
        "wss://dm.relay1.com",
        "wss://dm.relay2.com",
        NULL
    };

    NostrEvent *event = nostr_nip17_create_dm_relay_list(relays, ALICE_SK);
    assert(event != NULL);

    NostrDmRelayList *list = nostr_nip17_parse_dm_relay_list(event);
    assert(list != NULL);

    /* Check count */
    assert(list->count == 2);

    /* Check relay URLs */
    assert(strcmp(list->relays[0], "wss://dm.relay1.com") == 0);
    assert(strcmp(list->relays[1], "wss://dm.relay2.com") == 0);
    assert(list->relays[2] == NULL);  /* NULL terminated */

    nostr_nip17_free_dm_relay_list(list);
    nostr_event_free(event);
    printf("  OK: DM relay list parsed correctly\n");
}

static void test_dm_relay_list_roundtrip(void) {
    printf("Testing DM relay list roundtrip...\n");

    const char *original_relays[] = {
        "wss://inbox.nostr.example",
        "wss://dm-only.relay.io",
        "wss://private.messages.net",
        NULL
    };

    /* Create and parse */
    NostrEvent *event = nostr_nip17_create_dm_relay_list(original_relays, BOB_SK);
    assert(event != NULL);

    NostrDmRelayList *list = nostr_nip17_parse_dm_relay_list(event);
    assert(list != NULL);
    assert(list->count == 3);

    /* Verify all relays match */
    for (size_t i = 0; i < 3; i++) {
        assert(strcmp(list->relays[i], original_relays[i]) == 0);
    }

    nostr_nip17_free_dm_relay_list(list);
    nostr_event_free(event);
    printf("  OK: DM relay list roundtrip successful\n");
}

static void test_get_dm_relays_defaults(void) {
    printf("Testing DM relay fallback to defaults...\n");

    const char *defaults[] = {
        "wss://default1.relay.com",
        "wss://default2.relay.com",
        NULL
    };

    /* With NULL event, should return defaults */
    NostrDmRelayList *list = nostr_nip17_get_dm_relays_from_event(NULL, defaults);
    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->relays[0], "wss://default1.relay.com") == 0);
    assert(strcmp(list->relays[1], "wss://default2.relay.com") == 0);

    nostr_nip17_free_dm_relay_list(list);

    /* With NULL event and NULL defaults, should return NULL */
    list = nostr_nip17_get_dm_relays_from_event(NULL, NULL);
    assert(list == NULL);

    printf("  OK: DM relay defaults work correctly\n");
}

int main(void) {
    printf("NIP-17 Test Suite\n");
    printf("=================\n\n");

    setup_keys();

    /* Gift wrap tests */
    test_create_rumor();
    test_create_seal();
    test_create_gift_wrap();
    test_wrap_dm_convenience();
    test_unwrap_gift_wrap();
    test_unwrap_seal();
    test_decrypt_dm_roundtrip();
    test_wrong_recipient_fails();
    test_validate_gift_wrap();
    test_validate_seal();

    /* DM relay preferences tests */
    test_create_dm_relay_list();
    test_parse_dm_relay_list();
    test_dm_relay_list_roundtrip();
    test_get_dm_relays_defaults();

    cleanup_keys();

    printf("\n=================\n");
    printf("All tests passed!\n");
    return 0;
}
