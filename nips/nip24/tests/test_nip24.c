#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip24/nip24.h"

static void test_parse_full_profile(void) {
    const char *json =
        "{"
        "\"name\":\"satoshi\","
        "\"display_name\":\"Satoshi Nakamoto\","
        "\"about\":\"Creator of Bitcoin\","
        "\"picture\":\"https://example.com/avatar.png\","
        "\"banner\":\"https://example.com/banner.png\","
        "\"website\":\"https://bitcoin.org\","
        "\"nip05\":\"satoshi@bitcoin.org\","
        "\"lud06\":\"lnurl1abc\","
        "\"lud16\":\"satoshi@getalby.com\","
        "\"bot\":false"
        "}";

    NostrNip24Profile prof;
    int rc = nostr_nip24_parse_profile(json, &prof);
    assert(rc == 0);

    assert(strcmp(prof.name, "satoshi") == 0);
    assert(strcmp(prof.display_name, "Satoshi Nakamoto") == 0);
    assert(strcmp(prof.about, "Creator of Bitcoin") == 0);
    assert(strcmp(prof.picture, "https://example.com/avatar.png") == 0);
    assert(strcmp(prof.banner, "https://example.com/banner.png") == 0);
    assert(strcmp(prof.website, "https://bitcoin.org") == 0);
    assert(strcmp(prof.nip05, "satoshi@bitcoin.org") == 0);
    assert(strcmp(prof.lud06, "lnurl1abc") == 0);
    assert(strcmp(prof.lud16, "satoshi@getalby.com") == 0);
    assert(prof.bot == false);

    nostr_nip24_profile_free(&prof);
}

static void test_parse_minimal_profile(void) {
    const char *json = "{\"name\":\"alice\"}";

    NostrNip24Profile prof;
    int rc = nostr_nip24_parse_profile(json, &prof);
    assert(rc == 0);

    assert(strcmp(prof.name, "alice") == 0);
    assert(prof.display_name == NULL);
    assert(prof.about == NULL);
    assert(prof.picture == NULL);
    assert(prof.banner == NULL);
    assert(prof.website == NULL);
    assert(prof.nip05 == NULL);
    assert(prof.lud06 == NULL);
    assert(prof.lud16 == NULL);
    assert(prof.bot == false);

    nostr_nip24_profile_free(&prof);
}

static void test_parse_bot_true(void) {
    const char *json = "{\"name\":\"botaccount\",\"bot\":true}";

    NostrNip24Profile prof;
    int rc = nostr_nip24_parse_profile(json, &prof);
    assert(rc == 0);

    assert(strcmp(prof.name, "botaccount") == 0);
    assert(prof.bot == true);

    nostr_nip24_profile_free(&prof);
}

static void test_parse_escaped_strings(void) {
    const char *json = "{\"about\":\"line1\\nline2\\ttab\",\"name\":\"test\\\"user\"}";

    NostrNip24Profile prof;
    int rc = nostr_nip24_parse_profile(json, &prof);
    assert(rc == 0);

    assert(strcmp(prof.about, "line1\nline2\ttab") == 0);
    assert(strcmp(prof.name, "test\"user") == 0);

    nostr_nip24_profile_free(&prof);
}

static void test_parse_empty_json(void) {
    const char *json = "{}";

    NostrNip24Profile prof;
    int rc = nostr_nip24_parse_profile(json, &prof);
    assert(rc == 0);

    assert(prof.name == NULL);
    assert(prof.display_name == NULL);
    assert(prof.bot == false);

    nostr_nip24_profile_free(&prof);
}

static void test_parse_null_inputs(void) {
    NostrNip24Profile prof;
    assert(nostr_nip24_parse_profile(NULL, &prof) == -EINVAL);
    assert(nostr_nip24_parse_profile("{}", NULL) == -EINVAL);
}

static void test_get_display_name_prefers_display_name(void) {
    const char *json = "{\"name\":\"alice\",\"display_name\":\"Alice Wonderland\"}";

    char *dn = nostr_nip24_get_display_name(json);
    assert(dn != NULL);
    assert(strcmp(dn, "Alice Wonderland") == 0);
    free(dn);
}

static void test_get_display_name_falls_back_to_name(void) {
    const char *json = "{\"name\":\"bob\"}";

    char *dn = nostr_nip24_get_display_name(json);
    assert(dn != NULL);
    assert(strcmp(dn, "bob") == 0);
    free(dn);
}

static void test_get_display_name_empty_display_name(void) {
    const char *json = "{\"name\":\"charlie\",\"display_name\":\"\"}";

    char *dn = nostr_nip24_get_display_name(json);
    assert(dn != NULL);
    assert(strcmp(dn, "charlie") == 0);
    free(dn);
}

static void test_get_display_name_null(void) {
    assert(nostr_nip24_get_display_name(NULL) == NULL);
}

static void test_get_display_name_no_fields(void) {
    const char *json = "{\"about\":\"some bio\"}";

    char *dn = nostr_nip24_get_display_name(json);
    assert(dn == NULL);
}

static void test_is_bot_true(void) {
    const char *json = "{\"name\":\"mybot\",\"bot\":true}";
    assert(nostr_nip24_is_bot(json) == true);
}

static void test_is_bot_false(void) {
    const char *json = "{\"name\":\"human\",\"bot\":false}";
    assert(nostr_nip24_is_bot(json) == false);
}

static void test_is_bot_missing(void) {
    const char *json = "{\"name\":\"human\"}";
    assert(nostr_nip24_is_bot(json) == false);
}

static void test_is_bot_null(void) {
    assert(nostr_nip24_is_bot(NULL) == false);
}

static void test_profile_free_null(void) {
    /* Should not crash */
    nostr_nip24_profile_free(NULL);
}

static void test_parse_with_whitespace(void) {
    const char *json =
        "{\n"
        "  \"name\" : \"spaced\" ,\n"
        "  \"display_name\" : \"Spaced Out\"\n"
        "}";

    NostrNip24Profile prof;
    int rc = nostr_nip24_parse_profile(json, &prof);
    assert(rc == 0);

    assert(strcmp(prof.name, "spaced") == 0);
    assert(strcmp(prof.display_name, "Spaced Out") == 0);

    nostr_nip24_profile_free(&prof);
}

int main(void) {
    test_parse_full_profile();
    test_parse_minimal_profile();
    test_parse_bot_true();
    test_parse_escaped_strings();
    test_parse_empty_json();
    test_parse_null_inputs();
    test_get_display_name_prefers_display_name();
    test_get_display_name_falls_back_to_name();
    test_get_display_name_empty_display_name();
    test_get_display_name_null();
    test_get_display_name_no_fields();
    test_is_bot_true();
    test_is_bot_false();
    test_is_bot_missing();
    test_is_bot_null();
    test_profile_free_null();
    test_parse_with_whitespace();
    printf("nip24 ok\n");
    return 0;
}
