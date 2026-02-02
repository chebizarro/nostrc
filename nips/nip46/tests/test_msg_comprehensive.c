/**
 * Comprehensive message building/parsing tests for NIP-46.
 * Tests request building, response building, JSON format validation,
 * edge cases with special characters, and error handling.
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

#define TEST_ASSERT_EQ_INT(a, b, msg) do { \
    size_t _a = (size_t)(a); size_t _b = (size_t)(b); \
    if (_a != _b) { \
        printf("FAIL: %s (line %d): %s - got %zu, expected %zu\n", \
               __func__, __LINE__, msg, _a, _b); \
        return 1; \
    } \
} while(0)

/* --- Request Building Tests --- */

static int test_request_build_no_params(void) {
    char *json = nostr_nip46_request_build("req-1", "get_public_key", NULL, 0);
    TEST_ASSERT(json != NULL, "build should succeed");
    TEST_ASSERT(strstr(json, "\"id\":\"req-1\"") != NULL, "has id");
    TEST_ASSERT(strstr(json, "\"method\":\"get_public_key\"") != NULL, "has method");
    TEST_ASSERT(strstr(json, "\"params\":[]") != NULL, "has empty params");

    free(json);
    return 0;
}

static int test_request_build_string_params(void) {
    const char *params[] = {"param1", "param2"};
    char *json = nostr_nip46_request_build("req-2", "connect", params, 2);
    TEST_ASSERT(json != NULL, "build should succeed");
    TEST_ASSERT(strstr(json, "\"id\":\"req-2\"") != NULL, "has id");
    TEST_ASSERT(strstr(json, "\"method\":\"connect\"") != NULL, "has method");
    TEST_ASSERT(strstr(json, "\"param1\"") != NULL, "has param1");
    TEST_ASSERT(strstr(json, "\"param2\"") != NULL, "has param2");

    free(json);
    return 0;
}

static int test_request_build_json_object_param(void) {
    const char *event_json = "{\"kind\":1,\"content\":\"hello\",\"tags\":[]}";
    const char *params[] = {event_json};
    char *json = nostr_nip46_request_build("req-3", "sign_event", params, 1);
    TEST_ASSERT(json != NULL, "build should succeed");
    /* JSON objects should be embedded raw, not quoted */
    TEST_ASSERT(strstr(json, "{\"kind\":1") != NULL, "has raw JSON object");
    /* Should NOT be double-quoted like "\"{\\"kind\\":1" */
    TEST_ASSERT(strstr(json, "\"{\\\"kind\\\":1") == NULL, "JSON not double-quoted");

    free(json);
    return 0;
}

static int test_request_build_json_array_param(void) {
    const char *array_json = "[1,2,3]";
    const char *params[] = {array_json};
    char *json = nostr_nip46_request_build("req-4", "test", params, 1);
    TEST_ASSERT(json != NULL, "build should succeed");
    TEST_ASSERT(strstr(json, "[1,2,3]") != NULL, "has raw JSON array");

    free(json);
    return 0;
}

static int test_request_build_special_chars(void) {
    const char *params[] = {"hello \"world\"", "line1\nline2", "tab\there"};
    char *json = nostr_nip46_request_build("req-5", "test", params, 3);
    TEST_ASSERT(json != NULL, "build should succeed");
    /* Special chars should be escaped */
    TEST_ASSERT(strstr(json, "\\\"world\\\"") != NULL, "quotes escaped");
    TEST_ASSERT(strstr(json, "\\n") != NULL, "newline escaped");
    TEST_ASSERT(strstr(json, "\\t") != NULL, "tab escaped");

    free(json);
    return 0;
}

static int test_request_build_null_id(void) {
    char *json = nostr_nip46_request_build(NULL, "method", NULL, 0);
    TEST_ASSERT(json == NULL, "should fail with NULL id");
    return 0;
}

static int test_request_build_null_method(void) {
    char *json = nostr_nip46_request_build("id", NULL, NULL, 0);
    TEST_ASSERT(json == NULL, "should fail with NULL method");
    return 0;
}

static int test_request_build_empty_param(void) {
    const char *params[] = {""};
    char *json = nostr_nip46_request_build("req-6", "test", params, 1);
    TEST_ASSERT(json != NULL, "build should succeed");
    TEST_ASSERT(strstr(json, "\"\"") != NULL, "has empty string param");

    free(json);
    return 0;
}

/* --- Request Parsing Tests --- */

static int test_request_parse_simple(void) {
    const char *json = "{\"id\":\"42\",\"method\":\"ping\",\"params\":[]}";
    NostrNip46Request req = {0};
    TEST_ASSERT(nostr_nip46_request_parse(json, &req) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(req.id, "42", "id matches");
    TEST_ASSERT_EQ_STR(req.method, "ping", "method matches");
    TEST_ASSERT_EQ_INT(req.n_params, 0, "no params");

    nostr_nip46_request_free(&req);
    return 0;
}

static int test_request_parse_with_string_params(void) {
    const char *json = "{\"id\":\"1\",\"method\":\"connect\",\"params\":[\"pk\",\"secret\",\"perms\"]}";
    NostrNip46Request req = {0};
    TEST_ASSERT(nostr_nip46_request_parse(json, &req) == 0, "parse should succeed");
    TEST_ASSERT_EQ_INT(req.n_params, 3, "three params");
    TEST_ASSERT_EQ_STR(req.params[0], "pk", "param 0");
    TEST_ASSERT_EQ_STR(req.params[1], "secret", "param 1");
    TEST_ASSERT_EQ_STR(req.params[2], "perms", "param 2");

    nostr_nip46_request_free(&req);
    return 0;
}

static int test_request_parse_with_object_param(void) {
    const char *json = "{\"id\":\"2\",\"method\":\"sign_event\",\"params\":[{\"kind\":1,\"content\":\"hi\"}]}";
    NostrNip46Request req = {0};
    TEST_ASSERT(nostr_nip46_request_parse(json, &req) == 0, "parse should succeed");
    TEST_ASSERT_EQ_INT(req.n_params, 1, "one param");
    /* The object param should be stored as raw JSON string */
    TEST_ASSERT(req.params[0] != NULL, "param not null");
    TEST_ASSERT(strstr(req.params[0], "kind") != NULL, "contains kind");

    nostr_nip46_request_free(&req);
    return 0;
}

static int test_request_roundtrip(void) {
    const char *id = "roundtrip-1";
    const char *method = "sign_event";
    const char *params[] = {"{\"kind\":1,\"content\":\"test\"}"};

    char *json = nostr_nip46_request_build(id, method, params, 1);
    TEST_ASSERT(json != NULL, "build should succeed");

    NostrNip46Request req = {0};
    TEST_ASSERT(nostr_nip46_request_parse(json, &req) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(req.id, id, "id matches");
    TEST_ASSERT_EQ_STR(req.method, method, "method matches");
    TEST_ASSERT_EQ_INT(req.n_params, 1, "one param");

    free(json);
    nostr_nip46_request_free(&req);
    return 0;
}

static int test_request_parse_null_json(void) {
    NostrNip46Request req = {0};
    TEST_ASSERT(nostr_nip46_request_parse(NULL, &req) != 0, "should fail with NULL json");
    return 0;
}

static int test_request_parse_null_output(void) {
    TEST_ASSERT(nostr_nip46_request_parse("{\"id\":\"1\"}", NULL) != 0, "should fail with NULL output");
    return 0;
}

static int test_request_free_null(void) {
    /* Should not crash */
    nostr_nip46_request_free(NULL);

    NostrNip46Request req = {0};
    nostr_nip46_request_free(&req);  /* Empty struct */

    return 0;
}

/* --- Response Building Tests --- */

static int test_response_build_ok_string(void) {
    char *json = nostr_nip46_response_build_ok("resp-1", "\"pubkey123\"");
    TEST_ASSERT(json != NULL, "build should succeed");
    TEST_ASSERT(strstr(json, "\"id\":\"resp-1\"") != NULL, "has id");
    TEST_ASSERT(strstr(json, "\"result\":\"pubkey123\"") != NULL, "has result");
    TEST_ASSERT(strstr(json, "error") == NULL, "no error field");

    free(json);
    return 0;
}

static int test_response_build_ok_object(void) {
    char *json = nostr_nip46_response_build_ok("resp-2", "{\"signed\":true}");
    TEST_ASSERT(json != NULL, "build should succeed");
    TEST_ASSERT(strstr(json, "\"result\":{\"signed\":true}") != NULL, "has object result");

    free(json);
    return 0;
}

static int test_response_build_error(void) {
    char *json = nostr_nip46_response_build_err("resp-3", "permission denied");
    TEST_ASSERT(json != NULL, "build should succeed");
    TEST_ASSERT(strstr(json, "\"id\":\"resp-3\"") != NULL, "has id");
    TEST_ASSERT(strstr(json, "\"error\":\"permission denied\"") != NULL, "has error");

    free(json);
    return 0;
}

static int test_response_build_null_id(void) {
    char *json = nostr_nip46_response_build_ok(NULL, "\"result\"");
    TEST_ASSERT(json == NULL, "should fail with NULL id");
    return 0;
}

static int test_response_build_null_result(void) {
    char *json = nostr_nip46_response_build_ok("id", NULL);
    TEST_ASSERT(json == NULL, "should fail with NULL result");
    return 0;
}

/* --- Response Parsing Tests --- */

static int test_response_parse_ok_string(void) {
    const char *json = "{\"id\":\"1\",\"result\":\"pubkey\"}";
    NostrNip46Response res = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &res) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(res.id, "1", "id matches");
    TEST_ASSERT_EQ_STR(res.result, "pubkey", "result matches");
    TEST_ASSERT(res.error == NULL, "no error");

    nostr_nip46_response_free(&res);
    return 0;
}

static int test_response_parse_ok_object(void) {
    const char *json = "{\"id\":\"2\",\"result\":{\"kind\":1,\"sig\":\"abc\"}}";
    NostrNip46Response res = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &res) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(res.id, "2", "id matches");
    /* Object results stored as raw JSON */
    TEST_ASSERT(res.result != NULL, "result not null");
    TEST_ASSERT(strstr(res.result, "kind") != NULL, "contains kind");

    nostr_nip46_response_free(&res);
    return 0;
}

static int test_response_parse_error(void) {
    const char *json = "{\"id\":\"3\",\"error\":\"denied\"}";
    NostrNip46Response res = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &res) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(res.id, "3", "id matches");
    TEST_ASSERT_EQ_STR(res.error, "denied", "error matches");

    nostr_nip46_response_free(&res);
    return 0;
}

static int test_response_roundtrip_ok(void) {
    const char *id = "rt-1";
    const char *result = "\"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\"";

    char *json = nostr_nip46_response_build_ok(id, result);
    TEST_ASSERT(json != NULL, "build should succeed");

    NostrNip46Response res = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &res) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(res.id, id, "id matches");
    /* Note: result has outer quotes stripped by parser */
    TEST_ASSERT(res.result != NULL, "has result");

    free(json);
    nostr_nip46_response_free(&res);
    return 0;
}

static int test_response_roundtrip_error(void) {
    const char *id = "rt-2";
    const char *error = "forbidden";

    char *json = nostr_nip46_response_build_err(id, error);
    TEST_ASSERT(json != NULL, "build should succeed");

    NostrNip46Response res = {0};
    TEST_ASSERT(nostr_nip46_response_parse(json, &res) == 0, "parse should succeed");
    TEST_ASSERT_EQ_STR(res.id, id, "id matches");
    TEST_ASSERT_EQ_STR(res.error, error, "error matches");

    free(json);
    nostr_nip46_response_free(&res);
    return 0;
}

static int test_response_parse_null_json(void) {
    NostrNip46Response res = {0};
    TEST_ASSERT(nostr_nip46_response_parse(NULL, &res) != 0, "should fail with NULL json");
    return 0;
}

static int test_response_parse_null_output(void) {
    TEST_ASSERT(nostr_nip46_response_parse("{\"id\":\"1\"}", NULL) != 0, "should fail with NULL output");
    return 0;
}

static int test_response_free_null(void) {
    /* Should not crash */
    nostr_nip46_response_free(NULL);

    NostrNip46Response res = {0};
    nostr_nip46_response_free(&res);  /* Empty struct */

    return 0;
}

/* --- Request ID Validation Tests (for response matching) --- */

static int test_request_id_preserved(void) {
    /* Test that request IDs with various formats are preserved */
    const char *ids[] = {
        "simple",
        "with-dash",
        "with_underscore",
        "12345",
        "uuid-4cf2a1b3-7d89-4e12-b345-67890abcdef0",
        "timestamp_1234567890_1"
    };

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char *json = nostr_nip46_request_build(ids[i], "ping", NULL, 0);
        TEST_ASSERT(json != NULL, "build should succeed");

        NostrNip46Request req = {0};
        TEST_ASSERT(nostr_nip46_request_parse(json, &req) == 0, "parse should succeed");
        TEST_ASSERT_EQ_STR(req.id, ids[i], "id preserved");

        free(json);
        nostr_nip46_request_free(&req);
    }
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

    /* Request building tests */
    RUN_TEST(test_request_build_no_params);
    RUN_TEST(test_request_build_string_params);
    RUN_TEST(test_request_build_json_object_param);
    RUN_TEST(test_request_build_json_array_param);
    RUN_TEST(test_request_build_special_chars);
    RUN_TEST(test_request_build_null_id);
    RUN_TEST(test_request_build_null_method);
    RUN_TEST(test_request_build_empty_param);

    /* Request parsing tests */
    RUN_TEST(test_request_parse_simple);
    RUN_TEST(test_request_parse_with_string_params);
    RUN_TEST(test_request_parse_with_object_param);
    RUN_TEST(test_request_roundtrip);
    RUN_TEST(test_request_parse_null_json);
    RUN_TEST(test_request_parse_null_output);
    RUN_TEST(test_request_free_null);

    /* Response building tests */
    RUN_TEST(test_response_build_ok_string);
    RUN_TEST(test_response_build_ok_object);
    RUN_TEST(test_response_build_error);
    RUN_TEST(test_response_build_null_id);
    RUN_TEST(test_response_build_null_result);

    /* Response parsing tests */
    RUN_TEST(test_response_parse_ok_string);
    RUN_TEST(test_response_parse_ok_object);
    RUN_TEST(test_response_parse_error);
    RUN_TEST(test_response_roundtrip_ok);
    RUN_TEST(test_response_roundtrip_error);
    RUN_TEST(test_response_parse_null_json);
    RUN_TEST(test_response_parse_null_output);
    RUN_TEST(test_response_free_null);

    /* ID preservation tests */
    RUN_TEST(test_request_id_preserved);

    printf("test_msg_comprehensive: %d/%d passed\n", passed, total);
    return rc;
}
