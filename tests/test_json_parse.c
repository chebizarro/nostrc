/**
 * @file test_json_parse.c
 * @brief Tests for shared JSON parsing primitives (nostr-json-parse.h).
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "nostr-json-parse.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  %-40s ", #fn); \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

/* --- hexval --- */

static void test_hexval_digits(void) {
    for (char c = '0'; c <= '9'; c++)
        assert(nostr_json_hexval(c) == c - '0');
}

static void test_hexval_lower(void) {
    assert(nostr_json_hexval('a') == 10);
    assert(nostr_json_hexval('f') == 15);
}

static void test_hexval_upper(void) {
    assert(nostr_json_hexval('A') == 10);
    assert(nostr_json_hexval('F') == 15);
}

static void test_hexval_invalid(void) {
    assert(nostr_json_hexval('g') == -1);
    assert(nostr_json_hexval('G') == -1);
    assert(nostr_json_hexval(' ') == -1);
    assert(nostr_json_hexval('\0') == -1);
}

/* --- skip_ws --- */

static void test_skip_ws_spaces(void) {
    const char *s = "   hello";
    assert(nostr_json_skip_ws(s) == s + 3);
}

static void test_skip_ws_mixed(void) {
    const char *s = " \t\n\r{";
    assert(*nostr_json_skip_ws(s) == '{');
}

static void test_skip_ws_none(void) {
    const char *s = "hello";
    assert(nostr_json_skip_ws(s) == s);
}

static void test_skip_ws_empty(void) {
    const char *s = "";
    assert(nostr_json_skip_ws(s) == s);
}

/* --- utf8_encode --- */

static void test_utf8_ascii(void) {
    char buf[4];
    int n = nostr_json_utf8_encode('A', buf);
    assert(n == 1 && buf[0] == 'A');
}

static void test_utf8_2byte(void) {
    char buf[4];
    /* U+00E9 = e with acute = 0xC3 0xA9 */
    int n = nostr_json_utf8_encode(0x00E9, buf);
    assert(n == 2);
    assert((unsigned char)buf[0] == 0xC3);
    assert((unsigned char)buf[1] == 0xA9);
}

static void test_utf8_3byte(void) {
    char buf[4];
    /* U+4E16 = CJK character = 0xE4 0xB8 0x96 */
    int n = nostr_json_utf8_encode(0x4E16, buf);
    assert(n == 3);
    assert((unsigned char)buf[0] == 0xE4);
    assert((unsigned char)buf[1] == 0xB8);
    assert((unsigned char)buf[2] == 0x96);
}

static void test_utf8_4byte(void) {
    char buf[4];
    /* U+1F600 = grinning face = 0xF0 0x9F 0x98 0x80 */
    int n = nostr_json_utf8_encode(0x1F600, buf);
    assert(n == 4);
    assert((unsigned char)buf[0] == 0xF0);
    assert((unsigned char)buf[1] == 0x9F);
    assert((unsigned char)buf[2] == 0x98);
    assert((unsigned char)buf[3] == 0x80);
}

/* --- parse_string --- */

static void test_parse_string_simple(void) {
    const char *s = "\"hello\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "hello") == 0);
    assert(p == s + 7);
    free(out);
}

static void test_parse_string_empty(void) {
    const char *s = "\"\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "") == 0);
    free(out);
}

static void test_parse_string_escapes(void) {
    const char *s = "\"a\\nb\\tc\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "a\nb\tc") == 0);
    free(out);
}

static void test_parse_string_unicode(void) {
    /* \u00e9 = e with acute */
    const char *s = "\"caf\\u00e9\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "caf\xc3\xa9") == 0);
    free(out);
}

static void test_parse_string_surrogate_pair(void) {
    /* \uD83D\uDE00 = U+1F600 grinning face */
    const char *s = "\"\\uD83D\\uDE00\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "\xF0\x9F\x98\x80") == 0);
    free(out);
}

static void test_parse_string_fast_path(void) {
    /* No escapes -> fast path (direct memcpy) */
    const char *s = "\"no escapes here\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "no escapes here") == 0);
    free(out);
}

static void test_parse_string_unterminated(void) {
    const char *s = "\"missing end";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out == NULL);
}

static void test_parse_string_invalid_escape(void) {
    const char *s = "\"bad\\x\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out == NULL);
}

static void test_parse_string_lone_surrogate(void) {
    /* Lone high surrogate without low pair */
    const char *s = "\"\\uD83D\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out == NULL);
}

static void test_parse_string_lone_low_surrogate(void) {
    /* Lone low surrogate */
    const char *s = "\"\\uDE00\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out == NULL);
}

static void test_parse_string_with_leading_ws(void) {
    const char *s = "  \"hello\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "hello") == 0);
    free(out);
}

static void test_parse_string_all_escapes(void) {
    const char *s = "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"";
    const char *p = s;
    char *out = nostr_json_parse_string(&p);
    assert(out != NULL);
    assert(strcmp(out, "\"\\/\b\f\n\r\t") == 0);
    free(out);
}

/* --- parse_int64 --- */

static void test_parse_int64_positive(void) {
    const char *s = "42";
    const char *p = s;
    long long v = 0;
    assert(nostr_json_parse_int64(&p, &v) == 1);
    assert(v == 42);
}

static void test_parse_int64_negative(void) {
    const char *s = "-100";
    const char *p = s;
    long long v = 0;
    assert(nostr_json_parse_int64(&p, &v) == 1);
    assert(v == -100);
}

static void test_parse_int64_zero(void) {
    const char *s = "0";
    const char *p = s;
    long long v = 99;
    assert(nostr_json_parse_int64(&p, &v) == 1);
    assert(v == 0);
}

static void test_parse_int64_no_digits(void) {
    const char *s = "abc";
    const char *p = s;
    long long v = 0;
    assert(nostr_json_parse_int64(&p, &v) == 0);
}

static void test_parse_int64_leading_ws(void) {
    const char *s = "  1234";
    const char *p = s;
    long long v = 0;
    assert(nostr_json_parse_int64(&p, &v) == 1);
    assert(v == 1234);
}

static void test_parse_int64_large(void) {
    const char *s = "1700000000";
    const char *p = s;
    long long v = 0;
    assert(nostr_json_parse_int64(&p, &v) == 1);
    assert(v == 1700000000LL);
}

int main(void) {
    printf("test_json_parse:\n");

    RUN_TEST(test_hexval_digits);
    RUN_TEST(test_hexval_lower);
    RUN_TEST(test_hexval_upper);
    RUN_TEST(test_hexval_invalid);

    RUN_TEST(test_skip_ws_spaces);
    RUN_TEST(test_skip_ws_mixed);
    RUN_TEST(test_skip_ws_none);
    RUN_TEST(test_skip_ws_empty);

    RUN_TEST(test_utf8_ascii);
    RUN_TEST(test_utf8_2byte);
    RUN_TEST(test_utf8_3byte);
    RUN_TEST(test_utf8_4byte);

    RUN_TEST(test_parse_string_simple);
    RUN_TEST(test_parse_string_empty);
    RUN_TEST(test_parse_string_escapes);
    RUN_TEST(test_parse_string_unicode);
    RUN_TEST(test_parse_string_surrogate_pair);
    RUN_TEST(test_parse_string_fast_path);
    RUN_TEST(test_parse_string_unterminated);
    RUN_TEST(test_parse_string_invalid_escape);
    RUN_TEST(test_parse_string_lone_surrogate);
    RUN_TEST(test_parse_string_lone_low_surrogate);
    RUN_TEST(test_parse_string_with_leading_ws);
    RUN_TEST(test_parse_string_all_escapes);

    RUN_TEST(test_parse_int64_positive);
    RUN_TEST(test_parse_int64_negative);
    RUN_TEST(test_parse_int64_zero);
    RUN_TEST(test_parse_int64_no_digits);
    RUN_TEST(test_parse_int64_leading_ws);
    RUN_TEST(test_parse_int64_large);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
