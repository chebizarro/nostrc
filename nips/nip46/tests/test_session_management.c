/**
 * Session management tests for NIP-46.
 * Tests session creation, getters/setters, relay preservation, and lifecycle.
 */

#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test helper macros */
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQ_STR(a, b, msg) do { \
    const char *_a = (a); const char *_b = (b); \
    if (!(_a && _b && strcmp(_a, _b) == 0)) { \
        printf("FAIL: %s (line %d): %s - got '%s', expected '%s'\n", \
               __func__, __LINE__, msg, _a ? _a : "(null)", _b ? _b : "(null)"); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQ_INT(a, b, msg) do { \
    size_t _a = (size_t)(a); size_t _b = (size_t)(b); \
    if (_a != _b) { \
        printf("FAIL: %s (line %d): %s - got %zu, expected %zu\n", \
               __func__, __LINE__, msg, _a, _b); \
        return 1; \
    } \
} while(0)

/* --- Client Session Tests --- */

static int test_client_new_free(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "nostr_nip46_client_new should return non-NULL");

    nostr_nip46_session_free(s);
    return 0;
}

static int test_client_free_null(void) {
    /* Should not crash */
    nostr_nip46_session_free(NULL);
    return 0;
}

static int test_client_connect_bunker_preserves_relays(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay1.example.com"
                      "&relay=wss%3A%2F%2Frelay2.example.com"
                      "&relay=wss%3A%2F%2Frelay3.example.com";
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    TEST_ASSERT(nostr_nip46_client_connect(s, uri, NULL) == 0, "connect succeeds");

    /* Verify relays are preserved */
    char **relays = NULL;
    size_t n = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(s, &relays, &n) == 0, "get_relays succeeds");
    TEST_ASSERT_EQ_INT(n, 3, "three relays preserved");
    TEST_ASSERT_EQ_STR(relays[0], "wss://relay1.example.com", "relay 1");
    TEST_ASSERT_EQ_STR(relays[1], "wss://relay2.example.com", "relay 2");
    TEST_ASSERT_EQ_STR(relays[2], "wss://relay3.example.com", "relay 3");

    for (size_t i = 0; i < n; i++) free(relays[i]);
    free(relays);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_client_connect_nostrconnect_preserves_relays(void) {
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
                      "?relay=wss%3A%2F%2Frelay1.example.com"
                      "&relay=wss%3A%2F%2Frelay2.example.com";
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    TEST_ASSERT(nostr_nip46_client_connect(s, uri, NULL) == 0, "connect succeeds");

    char **relays = NULL;
    size_t n = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(s, &relays, &n) == 0, "get_relays succeeds");
    TEST_ASSERT_EQ_INT(n, 2, "two relays preserved");
    TEST_ASSERT_EQ_STR(relays[0], "wss://relay1.example.com", "relay 1");
    TEST_ASSERT_EQ_STR(relays[1], "wss://relay2.example.com", "relay 2");

    for (size_t i = 0; i < n; i++) free(relays[i]);
    free(relays);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_session_set_relays(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    const char *relays_in[] = {
        "wss://relay1.example.com",
        "wss://relay2.example.com",
        "wss://relay3.example.com"
    };

    TEST_ASSERT(nostr_nip46_session_set_relays(s, relays_in, 3) == 0, "set_relays succeeds");

    char **relays_out = NULL;
    size_t n = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(s, &relays_out, &n) == 0, "get_relays succeeds");
    TEST_ASSERT_EQ_INT(n, 3, "three relays set");
    TEST_ASSERT_EQ_STR(relays_out[0], "wss://relay1.example.com", "relay 1");
    TEST_ASSERT_EQ_STR(relays_out[1], "wss://relay2.example.com", "relay 2");
    TEST_ASSERT_EQ_STR(relays_out[2], "wss://relay3.example.com", "relay 3");

    for (size_t i = 0; i < n; i++) free(relays_out[i]);
    free(relays_out);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_session_set_relays_replaces_existing(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    const char *relays1[] = { "wss://old1.com", "wss://old2.com" };
    TEST_ASSERT(nostr_nip46_session_set_relays(s, relays1, 2) == 0, "set first relays");

    const char *relays2[] = { "wss://new.com" };
    TEST_ASSERT(nostr_nip46_session_set_relays(s, relays2, 1) == 0, "set new relays");

    char **relays_out = NULL;
    size_t n = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(s, &relays_out, &n) == 0, "get_relays succeeds");
    TEST_ASSERT_EQ_INT(n, 1, "only one relay now");
    TEST_ASSERT_EQ_STR(relays_out[0], "wss://new.com", "new relay");

    for (size_t i = 0; i < n; i++) free(relays_out[i]);
    free(relays_out);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_session_set_relays_empty(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    const char *relays1[] = { "wss://relay.com" };
    TEST_ASSERT(nostr_nip46_session_set_relays(s, relays1, 1) == 0, "set initial relays");

    /* Clear relays */
    TEST_ASSERT(nostr_nip46_session_set_relays(s, NULL, 0) == 0, "clear relays");

    char **relays_out = NULL;
    size_t n = 99; /* intentionally wrong to verify it gets set to 0 */
    TEST_ASSERT(nostr_nip46_session_get_relays(s, &relays_out, &n) == 0, "get_relays succeeds");
    TEST_ASSERT_EQ_INT(n, 0, "no relays");
    TEST_ASSERT(relays_out == NULL, "relays array is NULL");

    nostr_nip46_session_free(s);
    return 0;
}

static int test_session_get_remote_pubkey(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.com";
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    TEST_ASSERT(nostr_nip46_client_connect(s, uri, NULL) == 0, "connect succeeds");

    char *pubkey = NULL;
    TEST_ASSERT(nostr_nip46_session_get_remote_pubkey(s, &pubkey) == 0, "get_remote_pubkey succeeds");
    TEST_ASSERT_EQ_STR(pubkey, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "pubkey matches");

    free(pubkey);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_session_get_client_pubkey(void) {
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
                      "?relay=wss%3A%2F%2Frelay.com";
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    TEST_ASSERT(nostr_nip46_client_connect(s, uri, NULL) == 0, "connect succeeds");

    char *pubkey = NULL;
    TEST_ASSERT(nostr_nip46_session_get_client_pubkey(s, &pubkey) == 0, "get_client_pubkey succeeds");
    TEST_ASSERT_EQ_STR(pubkey, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "pubkey matches");

    free(pubkey);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_session_get_secret_bunker(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.com&secret=my-auth-token";
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");
    TEST_ASSERT(nostr_nip46_client_connect(s, uri, NULL) == 0, "connect succeeds");

    char *secret = NULL;
    TEST_ASSERT(nostr_nip46_session_get_secret(s, &secret) == 0, "get_secret succeeds");
    TEST_ASSERT_EQ_STR(secret, "my-auth-token", "secret matches");

    free(secret);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_client_set_secret_valid(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    const char *secret = "0000000000000000000000000000000000000000000000000000000000000001";
    TEST_ASSERT(nostr_nip46_client_set_secret(s, secret) == 0, "set_secret succeeds");

    char *out = NULL;
    TEST_ASSERT(nostr_nip46_session_get_secret(s, &out) == 0, "get_secret succeeds");
    TEST_ASSERT_EQ_STR(out, secret, "secret matches");

    free(out);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_client_set_secret_invalid_length(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    /* Too short */
    TEST_ASSERT(nostr_nip46_client_set_secret(s, "abcd") != 0, "rejects short secret");

    /* Too long */
    TEST_ASSERT(nostr_nip46_client_set_secret(s,
        "00000000000000000000000000000000000000000000000000000000000000000") != 0, "rejects long secret");

    nostr_nip46_session_free(s);
    return 0;
}

static int test_client_set_secret_invalid_hex(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    /* Contains non-hex characters */
    TEST_ASSERT(nostr_nip46_client_set_secret(s,
        "ghijklmnopqrstuvwxyz01234567890123456789abcdef0123456789abcdef01") != 0, "rejects non-hex secret");

    nostr_nip46_session_free(s);
    return 0;
}

static int test_client_set_signer_pubkey(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    const char *pubkey = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    TEST_ASSERT(nostr_nip46_client_set_signer_pubkey(s, pubkey) == 0, "set_signer_pubkey succeeds");

    char *out = NULL;
    TEST_ASSERT(nostr_nip46_session_get_remote_pubkey(s, &out) == 0, "get_remote_pubkey succeeds");
    TEST_ASSERT_EQ_STR(out, pubkey, "pubkey matches");

    free(out);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_client_set_signer_pubkey_invalid_length(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    TEST_ASSERT(nostr_nip46_client_set_signer_pubkey(s, "abcd") != 0, "rejects short pubkey");

    nostr_nip46_session_free(s);
    return 0;
}

/* --- Bunker Session Tests --- */

static int test_bunker_new_free(void) {
    NostrNip46Session *s = nostr_nip46_bunker_new(NULL);
    TEST_ASSERT(s != NULL, "nostr_nip46_bunker_new should return non-NULL");

    nostr_nip46_session_free(s);
    return 0;
}

static int test_bunker_new_with_callbacks(void) {
    NostrNip46BunkerCallbacks cbs = {0};
    cbs.authorize_cb = NULL;
    cbs.sign_cb = NULL;
    cbs.user_data = (void *)0xDEADBEEF;

    NostrNip46Session *s = nostr_nip46_bunker_new(&cbs);
    TEST_ASSERT(s != NULL, "nostr_nip46_bunker_new with callbacks should return non-NULL");

    nostr_nip46_session_free(s);
    return 0;
}

/* --- Reconnect behavior --- */

static int test_client_reconnect_clears_old_state(void) {
    NostrNip46Session *s = nostr_nip46_client_new();
    TEST_ASSERT(s != NULL, "session created");

    /* First connect */
    const char *uri1 = "bunker://1111111111111111111111111111111111111111111111111111111111111111"
                       "?relay=wss%3A%2F%2Frelay1.com&secret=secret1";
    TEST_ASSERT(nostr_nip46_client_connect(s, uri1, NULL) == 0, "first connect succeeds");

    char *pubkey1 = NULL;
    TEST_ASSERT(nostr_nip46_session_get_remote_pubkey(s, &pubkey1) == 0, "get pubkey1");
    TEST_ASSERT_EQ_STR(pubkey1, "1111111111111111111111111111111111111111111111111111111111111111", "pubkey1 correct");
    free(pubkey1);

    /* Second connect - should replace state */
    const char *uri2 = "bunker://2222222222222222222222222222222222222222222222222222222222222222"
                       "?relay=wss%3A%2F%2Frelay2.com&secret=secret2";
    TEST_ASSERT(nostr_nip46_client_connect(s, uri2, NULL) == 0, "second connect succeeds");

    char *pubkey2 = NULL;
    TEST_ASSERT(nostr_nip46_session_get_remote_pubkey(s, &pubkey2) == 0, "get pubkey2");
    TEST_ASSERT_EQ_STR(pubkey2, "2222222222222222222222222222222222222222222222222222222222222222", "pubkey2 correct");
    free(pubkey2);

    char **relays = NULL;
    size_t n = 0;
    TEST_ASSERT(nostr_nip46_session_get_relays(s, &relays, &n) == 0, "get relays");
    TEST_ASSERT_EQ_INT(n, 1, "one relay");
    TEST_ASSERT_EQ_STR(relays[0], "wss://relay2.com", "correct relay");
    for (size_t i = 0; i < n; i++) free(relays[i]);
    free(relays);

    char *secret = NULL;
    TEST_ASSERT(nostr_nip46_session_get_secret(s, &secret) == 0, "get secret");
    TEST_ASSERT_EQ_STR(secret, "secret2", "correct secret");
    free(secret);

    nostr_nip46_session_free(s);
    return 0;
}

/* --- Main --- */

int main(void) {
    int rc = 0;
    int total = 0;
    int passed = 0;

    #define RUN_TEST(fn) do { \
        total++; \
        int r = fn(); \
        if (r == 0) { passed++; } \
        else { rc = 1; } \
    } while(0)

    /* Client session tests */
    RUN_TEST(test_client_new_free);
    RUN_TEST(test_client_free_null);
    RUN_TEST(test_client_connect_bunker_preserves_relays);
    RUN_TEST(test_client_connect_nostrconnect_preserves_relays);
    RUN_TEST(test_session_set_relays);
    RUN_TEST(test_session_set_relays_replaces_existing);
    RUN_TEST(test_session_set_relays_empty);
    RUN_TEST(test_session_get_remote_pubkey);
    RUN_TEST(test_session_get_client_pubkey);
    RUN_TEST(test_session_get_secret_bunker);
    RUN_TEST(test_client_set_secret_valid);
    RUN_TEST(test_client_set_secret_invalid_length);
    RUN_TEST(test_client_set_secret_invalid_hex);
    RUN_TEST(test_client_set_signer_pubkey);
    RUN_TEST(test_client_set_signer_pubkey_invalid_length);

    /* Bunker session tests */
    RUN_TEST(test_bunker_new_free);
    RUN_TEST(test_bunker_new_with_callbacks);

    /* Reconnect tests */
    RUN_TEST(test_client_reconnect_clears_old_state);

    printf("test_session_management: %d/%d passed\n", passed, total);
    return rc;
}
