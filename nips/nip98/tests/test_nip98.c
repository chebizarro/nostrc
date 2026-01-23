/**
 * @file test_nip98.c
 * @brief Unit tests for NIP-98 HTTP Auth implementation
 */

#include "nostr/nip98/nip98.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test private key (for testing only - never use in production) */
static const char *TEST_PRIVATE_KEY = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Running %s... ", #name); \
    test_##name(); \
    printf("PASSED\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

/* Test: Create auth event without payload */
TEST(create_auth_event_basic) {
    const char *url = "https://example.com/api/upload";
    const char *method = "PUT";

    NostrEvent *event = nostr_nip98_create_auth_event(url, method, NULL);
    ASSERT_NOT_NULL(event);

    /* Check kind */
    ASSERT_EQ(nostr_event_get_kind(event), NOSTR_KIND_HTTP_AUTH);

    /* Check URL tag */
    const char *got_url = nostr_nip98_get_url(event);
    ASSERT_NOT_NULL(got_url);
    ASSERT_STR_EQ(got_url, url);

    /* Check method tag */
    const char *got_method = nostr_nip98_get_method(event);
    ASSERT_NOT_NULL(got_method);
    ASSERT_STR_EQ(got_method, method);

    /* Payload should be NULL */
    ASSERT_NULL(nostr_nip98_get_payload_hash(event));

    /* Content should be empty */
    const char *content = nostr_event_get_content(event);
    ASSERT_NOT_NULL(content);
    ASSERT_STR_EQ(content, "");

    nostr_event_free(event);
}

/* Test: Create auth event with payload hash */
TEST(create_auth_event_with_payload) {
    const char *url = "https://blossom.example/upload";
    const char *method = "PUT";
    const char *payload_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    NostrEvent *event = nostr_nip98_create_auth_event(url, method, payload_hash);
    ASSERT_NOT_NULL(event);

    /* Check payload tag */
    const char *got_payload = nostr_nip98_get_payload_hash(event);
    ASSERT_NOT_NULL(got_payload);
    ASSERT_STR_EQ(got_payload, payload_hash);

    nostr_event_free(event);
}

/* Test: Create auth event with NULL parameters fails */
TEST(create_auth_event_null_params) {
    ASSERT_NULL(nostr_nip98_create_auth_event(NULL, "GET", NULL));
    ASSERT_NULL(nostr_nip98_create_auth_event("https://example.com", NULL, NULL));
    ASSERT_NULL(nostr_nip98_create_auth_event(NULL, NULL, NULL));
}

/* Test: Create and parse auth header */
TEST(auth_header_roundtrip) {
    const char *url = "https://api.snort.social/api/v1/n5sp/list";
    const char *method = "GET";

    /* Create event */
    NostrEvent *event = nostr_nip98_create_auth_event(url, method, NULL);
    ASSERT_NOT_NULL(event);

    /* Sign it */
    int sign_result = nostr_event_sign(event, TEST_PRIVATE_KEY);
    ASSERT_EQ(sign_result, 0);

    /* Create header */
    char *header = nostr_nip98_create_auth_header(event);
    ASSERT_NOT_NULL(header);

    /* Check header starts with "Nostr " */
    ASSERT(strncmp(header, "Nostr ", 6) == 0);

    /* Parse header back */
    NostrEvent *parsed = NULL;
    NostrNip98Result result = nostr_nip98_parse_auth_header(header, &parsed);
    ASSERT_EQ(result, NOSTR_NIP98_OK);
    ASSERT_NOT_NULL(parsed);

    /* Verify parsed event matches original */
    ASSERT_EQ(nostr_event_get_kind(parsed), NOSTR_KIND_HTTP_AUTH);
    ASSERT_STR_EQ(nostr_nip98_get_url(parsed), url);
    ASSERT_STR_EQ(nostr_nip98_get_method(parsed), method);

    free(header);
    nostr_event_free(event);
    nostr_event_free(parsed);
}

/* Test: Parse invalid header */
TEST(parse_invalid_header) {
    NostrEvent *event = NULL;

    /* Not a Nostr header */
    ASSERT_EQ(nostr_nip98_parse_auth_header("Bearer xyz123", &event), NOSTR_NIP98_ERR_INVALID_HEADER);
    ASSERT_NULL(event);

    /* Empty after Nostr prefix */
    ASSERT_EQ(nostr_nip98_parse_auth_header("Nostr ", &event), NOSTR_NIP98_ERR_INVALID_HEADER);
    ASSERT_NULL(event);

    /* Invalid base64 */
    ASSERT_EQ(nostr_nip98_parse_auth_header("Nostr !!invalid!!", &event), NOSTR_NIP98_ERR_DECODE);
    ASSERT_NULL(event);

    /* NULL parameters */
    ASSERT_EQ(nostr_nip98_parse_auth_header(NULL, &event), NOSTR_NIP98_ERR_NULL_PARAM);
    ASSERT_EQ(nostr_nip98_parse_auth_header("Nostr abc", NULL), NOSTR_NIP98_ERR_NULL_PARAM);
}

/* Test: Validate auth event success */
TEST(validate_auth_event_success) {
    const char *url = "https://example.com/upload";
    const char *method = "PUT";
    const char *payload_hash = "abc123def456";

    NostrEvent *event = nostr_nip98_create_auth_event(url, method, payload_hash);
    ASSERT_NOT_NULL(event);

    /* Sign the event */
    int sign_result = nostr_event_sign(event, TEST_PRIVATE_KEY);
    ASSERT_EQ(sign_result, 0);

    /* Validate without payload check */
    NostrNip98Result result = nostr_nip98_validate_auth_event(event, url, method, NULL);
    ASSERT_EQ(result, NOSTR_NIP98_OK);

    /* Validate with payload check */
    NostrNip98ValidateOptions opts = {
        .time_window_seconds = 120,
        .expected_payload_hash = payload_hash
    };
    result = nostr_nip98_validate_auth_event(event, url, method, &opts);
    ASSERT_EQ(result, NOSTR_NIP98_OK);

    nostr_event_free(event);
}

/* Test: Validate auth event - wrong kind */
TEST(validate_auth_event_wrong_kind) {
    NostrEvent *event = nostr_event_new();
    ASSERT_NOT_NULL(event);

    nostr_event_set_kind(event, 1); /* Text note, not HTTP auth */

    NostrNip98Result result = nostr_nip98_validate_auth_event(event, "https://example.com", "GET", NULL);
    ASSERT_EQ(result, NOSTR_NIP98_ERR_INVALID_KIND);

    nostr_event_free(event);
}

/* Test: Validate auth event - URL mismatch */
TEST(validate_auth_event_url_mismatch) {
    const char *url = "https://example.com/upload";

    NostrEvent *event = nostr_nip98_create_auth_event(url, "GET", NULL);
    ASSERT_NOT_NULL(event);

    int sign_result = nostr_event_sign(event, TEST_PRIVATE_KEY);
    ASSERT_EQ(sign_result, 0);

    NostrNip98Result result = nostr_nip98_validate_auth_event(
        event, "https://different.com/upload", "GET", NULL);
    ASSERT_EQ(result, NOSTR_NIP98_ERR_URL_MISMATCH);

    nostr_event_free(event);
}

/* Test: Validate auth event - method mismatch */
TEST(validate_auth_event_method_mismatch) {
    const char *url = "https://example.com/upload";

    NostrEvent *event = nostr_nip98_create_auth_event(url, "GET", NULL);
    ASSERT_NOT_NULL(event);

    int sign_result = nostr_event_sign(event, TEST_PRIVATE_KEY);
    ASSERT_EQ(sign_result, 0);

    NostrNip98Result result = nostr_nip98_validate_auth_event(event, url, "POST", NULL);
    ASSERT_EQ(result, NOSTR_NIP98_ERR_METHOD_MISMATCH);

    nostr_event_free(event);
}

/* Test: Validate auth event - payload mismatch */
TEST(validate_auth_event_payload_mismatch) {
    const char *url = "https://example.com/upload";
    const char *payload_hash = "abc123";

    NostrEvent *event = nostr_nip98_create_auth_event(url, "PUT", payload_hash);
    ASSERT_NOT_NULL(event);

    int sign_result = nostr_event_sign(event, TEST_PRIVATE_KEY);
    ASSERT_EQ(sign_result, 0);

    NostrNip98ValidateOptions opts = {
        .time_window_seconds = 60,
        .expected_payload_hash = "different_hash"
    };

    NostrNip98Result result = nostr_nip98_validate_auth_event(event, url, "PUT", &opts);
    ASSERT_EQ(result, NOSTR_NIP98_ERR_PAYLOAD_MISMATCH);

    nostr_event_free(event);
}

/* Test: Error messages */
TEST(error_messages) {
    ASSERT_STR_EQ(nostr_nip98_strerror(NOSTR_NIP98_OK), "Success");
    ASSERT_STR_EQ(nostr_nip98_strerror(NOSTR_NIP98_ERR_NULL_PARAM), "Null parameter");
    ASSERT_STR_EQ(nostr_nip98_strerror(NOSTR_NIP98_ERR_INVALID_KIND), "Invalid event kind (expected 27235)");
    ASSERT_STR_EQ(nostr_nip98_strerror(NOSTR_NIP98_ERR_TIMESTAMP_EXPIRED), "Event timestamp outside valid window");
    ASSERT_STR_EQ(nostr_nip98_strerror(NOSTR_NIP98_ERR_URL_MISMATCH), "URL does not match expected value");
    ASSERT_STR_EQ(nostr_nip98_strerror(NOSTR_NIP98_ERR_METHOD_MISMATCH), "HTTP method does not match expected value");
    ASSERT_NOT_NULL(nostr_nip98_strerror((NostrNip98Result)-999)); /* Unknown error */
}

int main(void) {
    printf("NIP-98 HTTP Auth Tests\n");
    printf("======================\n\n");

    RUN_TEST(create_auth_event_basic);
    RUN_TEST(create_auth_event_with_payload);
    RUN_TEST(create_auth_event_null_params);
    RUN_TEST(auth_header_roundtrip);
    RUN_TEST(parse_invalid_header);
    RUN_TEST(validate_auth_event_success);
    RUN_TEST(validate_auth_event_wrong_kind);
    RUN_TEST(validate_auth_event_url_mismatch);
    RUN_TEST(validate_auth_event_method_mismatch);
    RUN_TEST(validate_auth_event_payload_mismatch);
    RUN_TEST(error_messages);

    printf("\n======================\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
