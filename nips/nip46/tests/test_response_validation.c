/**
 * Response validation tests for NIP-46.
 * Tests request ID matching, error handling, result parsing,
 * and various response edge cases that have caused bugs.
 */

#include "nostr/nip46/nip46_msg.h"
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

/* --- Request ID Validation Tests --- */

/* Helper: Check if response matches expected request ID */
static int response_matches_request(const char *response_json, const char *expected_id) {
    NostrNip46Response resp = {0};
    if (nostr_nip46_response_parse(response_json, &resp) != 0) {
        return 0;
    }
    int match = resp.id && expected_id && strcmp(resp.id, expected_id) == 0;
    nostr_nip46_response_free(&resp);
    return match;
}

static int test_response_id_exact_match(void) {
    const char *req_id = "abc123";
    char *resp_json = nostr_nip46_response_build_ok(req_id, "\"result\"");
    TEST_ASSERT(resp_json != NULL, "build response");

    TEST_ASSERT(response_matches_request(resp_json, req_id), "ID matches exactly");
    TEST_ASSERT(!response_matches_request(resp_json, "abc124"), "different ID doesn't match");
    TEST_ASSERT(!response_matches_request(resp_json, "ABC123"), "case-sensitive mismatch");
    TEST_ASSERT(!response_matches_request(resp_json, "abc12"), "prefix doesn't match");
    TEST_ASSERT(!response_matches_request(resp_json, "abc1234"), "longer ID doesn't match");

    free(resp_json);
    return 0;
}

static int test_response_id_timestamp_format(void) {
    /* Real-world format: timestamp_counter */
    const char *req_id = "1704067200_42";
    char *resp_json = nostr_nip46_response_build_ok(req_id, "\"result\"");
    TEST_ASSERT(resp_json != NULL, "build response");

    TEST_ASSERT(response_matches_request(resp_json, req_id), "timestamp ID matches");
    TEST_ASSERT(!response_matches_request(resp_json, "1704067200_43"), "different counter");
    TEST_ASSERT(!response_matches_request(resp_json, "1704067201_42"), "different timestamp");

    free(resp_json);
    return 0;
}

static int test_response_id_uuid_format(void) {
    const char *req_id = "550e8400-e29b-41d4-a716-446655440000";
    char *resp_json = nostr_nip46_response_build_ok(req_id, "\"result\"");
    TEST_ASSERT(resp_json != NULL, "build response");

    TEST_ASSERT(response_matches_request(resp_json, req_id), "UUID ID matches");

    free(resp_json);
    return 0;
}

/* --- Error Response Tests --- */

static int test_response_error_string(void) {
    const char *json = "{\"id\":\"1\",\"error\":\"permission denied\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    TEST_ASSERT_EQ_STR(resp.id, "1", "id");
    TEST_ASSERT_EQ_STR(resp.error, "permission denied", "error message");
    TEST_ASSERT(resp.result == NULL, "no result");

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_both_result_and_error(void) {
    /* Per spec, should not happen, but test parser behavior */
    const char *json = "{\"id\":\"1\",\"result\":\"ok\",\"error\":\"also error\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse succeeds");

    /* Both should be parsed */
    TEST_ASSERT(resp.result != NULL, "has result");
    TEST_ASSERT(resp.error != NULL, "has error");

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_empty_error(void) {
    const char *json = "{\"id\":\"1\",\"error\":\"\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    TEST_ASSERT(resp.error != NULL, "error field exists");
    TEST_ASSERT(strlen(resp.error) == 0, "error is empty string");

    nostr_nip46_response_free(&resp);
    return 0;
}

/* --- Result Format Tests --- */

static int test_response_result_string(void) {
    /* Simple string result (pubkey) */
    const char *json = "{\"id\":\"1\",\"result\":\"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    TEST_ASSERT_EQ_STR(resp.result, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", "pubkey result");

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_result_json_object(void) {
    /* JSON object result (signed event) */
    const char *json = "{\"id\":\"1\",\"result\":{\"kind\":1,\"content\":\"hi\",\"sig\":\"abc\"}}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    TEST_ASSERT(resp.result != NULL, "has result");
    /* Result should be stored as raw JSON string */
    TEST_ASSERT(strstr(resp.result, "kind") != NULL, "contains kind");
    TEST_ASSERT(strstr(resp.result, "sig") != NULL, "contains sig");

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_result_ack(void) {
    /* "ack" response from connect */
    const char *json = "{\"id\":\"c1\",\"result\":\"ack\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    TEST_ASSERT_EQ_STR(resp.result, "ack", "ack result");

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_result_null(void) {
    /* null result */
    const char *json = "{\"id\":\"1\",\"result\":null}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    /* null should result in NULL or empty string depending on implementation */
    /* Just verify parsing doesn't crash */

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_result_escaped_json(void) {
    /* Result is a JSON string containing escaped JSON (some signers do this) */
    const char *json = "{\"id\":\"1\",\"result\":\"{\\\"kind\\\":1,\\\"content\\\":\\\"test\\\"}\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    TEST_ASSERT(resp.result != NULL, "has result");
    /* The parser should unescape the string */
    TEST_ASSERT(strstr(resp.result, "kind") != NULL, "contains kind after unescape");

    nostr_nip46_response_free(&resp);
    return 0;
}

/* --- Edge Cases --- */

static int test_response_whitespace(void) {
    /* Extra whitespace */
    const char *json = "  {  \"id\" : \"1\"  ,  \"result\" : \"ok\"  }  ";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse with whitespace");

    TEST_ASSERT_EQ_STR(resp.id, "1", "id");
    TEST_ASSERT_EQ_STR(resp.result, "ok", "result");

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_unicode(void) {
    /* Unicode in error message */
    const char *json = "{\"id\":\"1\",\"error\":\"\\u4e2d\\u6587\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) == 0, "parse");

    TEST_ASSERT(resp.error != NULL, "has error");
    /* Exact unicode handling depends on JSON parser */

    nostr_nip46_response_free(&resp);
    return 0;
}

static int test_response_missing_id(void) {
    /* Missing id field should fail */
    const char *json = "{\"result\":\"ok\"}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) != 0, "should fail without id");

    return 0;
}

static int test_response_empty_object(void) {
    const char *json = "{}";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) != 0, "should fail with empty object");

    return 0;
}

static int test_response_invalid_json(void) {
    const char *json = "not json at all";
    NostrNip46Response resp = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &resp) != 0, "should fail with invalid JSON");

    return 0;
}

static int test_response_truncated_json(void) {
    const char *json = "{\"id\":\"1\",\"result\":";
    NostrNip46Response resp = {0};
    /* Behavior depends on JSON parser - may fail or return partial */
    int rc = nostr_nip46_response_parse(json, &resp);
    if (rc == 0) {
        nostr_nip46_response_free(&resp);
    }
    /* Just verify it doesn't crash */
    return 0;
}

/* --- Response Ordering Tests (for clock skew scenarios) --- */

/*
 * These tests verify the response structure for scenarios where
 * responses might arrive out of order (though actual filtering
 * happens at the relay/event level, not in msg parsing).
 */

static int test_response_sequence_simulation(void) {
    /* Simulate parsing multiple responses and matching by ID */
    const char *expected_id = "req-42";

    const char *responses[] = {
        "{\"id\":\"req-40\",\"result\":\"old1\"}",
        "{\"id\":\"req-41\",\"result\":\"old2\"}",
        "{\"id\":\"req-42\",\"result\":\"target\"}",  /* This is the one we want */
        "{\"id\":\"req-43\",\"result\":\"future\"}"
    };

    char *found_result = NULL;

    for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); i++) {
        NostrNip46Response resp = {0};
        if (nostr_nip46_response_parse(responses[i], &resp) == 0) {
            if (resp.id && strcmp(resp.id, expected_id) == 0) {
                found_result = strdup(resp.result);
            }
            nostr_nip46_response_free(&resp);
        }
    }

    TEST_ASSERT(found_result != NULL, "found matching response");
    TEST_ASSERT_EQ_STR(found_result, "target", "correct result");

    free(found_result);
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

    /* ID matching tests */
    RUN_TEST(test_response_id_exact_match);
    RUN_TEST(test_response_id_timestamp_format);
    RUN_TEST(test_response_id_uuid_format);

    /* Error response tests */
    RUN_TEST(test_response_error_string);
    RUN_TEST(test_response_both_result_and_error);
    RUN_TEST(test_response_empty_error);

    /* Result format tests */
    RUN_TEST(test_response_result_string);
    RUN_TEST(test_response_result_json_object);
    RUN_TEST(test_response_result_ack);
    RUN_TEST(test_response_result_null);
    RUN_TEST(test_response_result_escaped_json);

    /* Edge cases */
    RUN_TEST(test_response_whitespace);
    RUN_TEST(test_response_unicode);
    RUN_TEST(test_response_missing_id);
    RUN_TEST(test_response_empty_object);
    RUN_TEST(test_response_invalid_json);
    RUN_TEST(test_response_truncated_json);

    /* Sequence simulation */
    RUN_TEST(test_response_sequence_simulation);

    printf("test_response_validation: %d/%d passed\n", passed, total);
    return rc;
}
