/**
 * Comprehensive URI parsing tests for NIP-46.
 * Tests bunker:// and nostrconnect:// URIs with multiple relays, edge cases,
 * special characters, and error handling.
 */

#include "nostr/nip46/nip46_uri.h"
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
    int _a = (a); int _b = (b); \
    if (_a != _b) { \
        printf("FAIL: %s (line %d): %s - got %d, expected %d\n", \
               __func__, __LINE__, msg, _a, _b); \
        return 1; \
    } \
} while(0)

/* --- bunker:// URI Tests --- */

static int test_bunker_uri_basic(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT(u.remote_signer_pubkey_hex != NULL, "pubkey should not be NULL");
    TEST_ASSERT_EQ_INT((int)strlen(u.remote_signer_pubkey_hex), 64, "pubkey length");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 0, "no relays");
    TEST_ASSERT(u.secret == NULL, "no secret");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

static int test_bunker_uri_single_relay(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?relay=wss%3A%2F%2Frelay.example.com";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 1, "one relay");
    TEST_ASSERT_EQ_STR(u.relays[0], "wss://relay.example.com", "relay URL decoded");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

static int test_bunker_uri_multiple_relays(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay1.example.com"
                      "&relay=wss%3A%2F%2Frelay2.example.com"
                      "&relay=wss%3A%2F%2Frelay3.example.com%2Fpath";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 3, "three relays");
    TEST_ASSERT_EQ_STR(u.relays[0], "wss://relay1.example.com", "relay 1");
    TEST_ASSERT_EQ_STR(u.relays[1], "wss://relay2.example.com", "relay 2");
    TEST_ASSERT_EQ_STR(u.relays[2], "wss://relay3.example.com/path", "relay 3 with path");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

static int test_bunker_uri_with_secret(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?relay=wss%3A%2F%2Frelay.example.com&secret=my-secret-token";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(u.secret, "my-secret-token", "secret parsed");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 1, "one relay");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

static int test_bunker_uri_secret_with_special_chars(void) {
    /* Secret contains URL-encoded special characters */
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?secret=hello%20world%21%26%3D";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(u.secret, "hello world!&=", "secret with special chars decoded");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

static int test_bunker_uri_secret_before_relay(void) {
    /* Order shouldn't matter */
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?secret=sec&relay=wss%3A%2F%2Frelay.com";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(u.secret, "sec", "secret parsed");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 1, "one relay");
    TEST_ASSERT_EQ_STR(u.relays[0], "wss://relay.com", "relay parsed");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

static int test_bunker_uri_unknown_params_ignored(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                      "?unknown=value&relay=wss%3A%2F%2Frelay.com&other=test";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 1, "one relay parsed");
    TEST_ASSERT_EQ_STR(u.relays[0], "wss://relay.com", "relay correct");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

/* --- bunker:// Error Cases --- */

static int test_bunker_uri_invalid_scheme(void) {
    const char *uri = "invalid://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) != 0, "should reject invalid scheme");

    return 0;
}

static int test_bunker_uri_pubkey_too_short(void) {
    const char *uri = "bunker://abcdef";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) != 0, "should reject short pubkey");

    return 0;
}

static int test_bunker_uri_pubkey_non_hex(void) {
    const char *uri = "bunker://ghijklmnopqrstuvwxyz0123456789abcdef0123456789abcdef0123456789ab";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) != 0, "should reject non-hex pubkey");

    return 0;
}

static int test_bunker_uri_null_input(void) {
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(NULL, &u) != 0, "should reject NULL uri");
    TEST_ASSERT(nostr_nip46_uri_parse_bunker("bunker://abc", NULL) != 0, "should reject NULL output");

    return 0;
}

static int test_bunker_uri_empty_string(void) {
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker("", &u) != 0, "should reject empty string");

    return 0;
}

/* --- nostrconnect:// URI Tests --- */

static int test_connect_uri_basic(void) {
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    NostrNip46ConnectURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_connect(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT(u.client_pubkey_hex != NULL, "pubkey should not be NULL");
    TEST_ASSERT_EQ_INT((int)strlen(u.client_pubkey_hex), 64, "pubkey length");

    nostr_nip46_uri_connect_free(&u);
    return 0;
}

static int test_connect_uri_full_params(void) {
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
                      "?relay=wss%3A%2F%2Fr1.example.com"
                      "&relay=wss%3A%2F%2Fr2.example.com"
                      "&secret=connect-secret"
                      "&perms=sign_event%3A1%2Cnip04_encrypt%2Cnip44_decrypt"
                      "&name=MyApp"
                      "&url=https%3A%2F%2Fmyapp.example.com"
                      "&image=https%3A%2F%2Fmyapp.example.com%2Flogo.png";
    NostrNip46ConnectURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_connect(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 2, "two relays");
    TEST_ASSERT_EQ_STR(u.relays[0], "wss://r1.example.com", "relay 1");
    TEST_ASSERT_EQ_STR(u.relays[1], "wss://r2.example.com", "relay 2");
    TEST_ASSERT_EQ_STR(u.secret, "connect-secret", "secret");
    TEST_ASSERT_EQ_STR(u.perms_csv, "sign_event:1,nip04_encrypt,nip44_decrypt", "perms");
    TEST_ASSERT_EQ_STR(u.name, "MyApp", "name");
    TEST_ASSERT_EQ_STR(u.url, "https://myapp.example.com", "url");
    TEST_ASSERT_EQ_STR(u.image, "https://myapp.example.com/logo.png", "image");

    nostr_nip46_uri_connect_free(&u);
    return 0;
}

static int test_connect_uri_minimal(void) {
    /* Just pubkey, no query params */
    const char *uri = "nostrconnect://1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
    NostrNip46ConnectURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_connect(uri, &u) == 0, "parse should succeed");
    TEST_ASSERT(u.client_pubkey_hex != NULL, "pubkey present");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 0, "no relays");
    TEST_ASSERT(u.secret == NULL, "no secret");
    TEST_ASSERT(u.perms_csv == NULL, "no perms");
    TEST_ASSERT(u.name == NULL, "no name");
    TEST_ASSERT(u.url == NULL, "no url");
    TEST_ASSERT(u.image == NULL, "no image");

    nostr_nip46_uri_connect_free(&u);
    return 0;
}

/* --- nostrconnect:// Error Cases --- */

static int test_connect_uri_invalid_scheme(void) {
    const char *uri = "nostr://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    NostrNip46ConnectURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_connect(uri, &u) != 0, "should reject wrong scheme");

    return 0;
}

static int test_connect_uri_pubkey_too_long(void) {
    /* 66 chars is valid (compressed SEC1), test truly wrong length */
    const char *uri = "nostrconnect://abc";  /* way too short */
    NostrNip46ConnectURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_connect(uri, &u) != 0, "should reject short pubkey");

    return 0;
}

/* --- Test cases for SEC1 compressed pubkey format (66 hex chars) --- */

static int test_bunker_uri_sec1_compressed(void) {
    /* 66 hex chars = SEC1 compressed format (0x02 or 0x03 prefix) */
    const char *uri = "bunker://0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798"
                      "?relay=wss%3A%2F%2Frelay.example.com";
    NostrNip46BunkerURI u = {0};

    TEST_ASSERT(nostr_nip46_uri_parse_bunker(uri, &u) == 0, "parse SEC1 compressed should succeed");
    TEST_ASSERT_EQ_INT((int)strlen(u.remote_signer_pubkey_hex), 66, "pubkey length is 66");
    TEST_ASSERT_EQ_INT((int)u.n_relays, 1, "one relay");

    nostr_nip46_uri_bunker_free(&u);
    return 0;
}

/* --- Free function safety tests --- */

static int test_bunker_free_null(void) {
    /* Should not crash */
    nostr_nip46_uri_bunker_free(NULL);

    NostrNip46BunkerURI u = {0};
    nostr_nip46_uri_bunker_free(&u);  /* Empty struct */

    return 0;
}

static int test_connect_free_null(void) {
    /* Should not crash */
    nostr_nip46_uri_connect_free(NULL);

    NostrNip46ConnectURI u = {0};
    nostr_nip46_uri_connect_free(&u);  /* Empty struct */

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

    /* bunker:// tests */
    RUN_TEST(test_bunker_uri_basic);
    RUN_TEST(test_bunker_uri_single_relay);
    RUN_TEST(test_bunker_uri_multiple_relays);
    RUN_TEST(test_bunker_uri_with_secret);
    RUN_TEST(test_bunker_uri_secret_with_special_chars);
    RUN_TEST(test_bunker_uri_secret_before_relay);
    RUN_TEST(test_bunker_uri_unknown_params_ignored);
    RUN_TEST(test_bunker_uri_invalid_scheme);
    RUN_TEST(test_bunker_uri_pubkey_too_short);
    RUN_TEST(test_bunker_uri_pubkey_non_hex);
    RUN_TEST(test_bunker_uri_null_input);
    RUN_TEST(test_bunker_uri_empty_string);
    RUN_TEST(test_bunker_uri_sec1_compressed);
    RUN_TEST(test_bunker_free_null);

    /* nostrconnect:// tests */
    RUN_TEST(test_connect_uri_basic);
    RUN_TEST(test_connect_uri_full_params);
    RUN_TEST(test_connect_uri_minimal);
    RUN_TEST(test_connect_uri_invalid_scheme);
    RUN_TEST(test_connect_uri_pubkey_too_long);
    RUN_TEST(test_connect_free_null);

    printf("test_uri_comprehensive: %d/%d passed\n", passed, total);
    return rc;
}
