/*
 * test_hanami_bud02_auth.c - Tests for BUD-02 Blossom authorization
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-bud02-auth.h"

/* libnostr headers — accessed via hanami's transitive include paths */
#include <nostr-event.h>
#include <nostr-tag.h>
#include <nostr-kinds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <inttypes.h>

/* Test private key (for testing only) */
static const char *TEST_PRIVATE_KEY =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-50s ", #name); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* ---- Action string helpers ---- */

static void test_action_str_roundtrip(void)
{
    assert(strcmp(hanami_bud02_action_str(HANAMI_BUD02_ACTION_UPLOAD), "upload") == 0);
    assert(strcmp(hanami_bud02_action_str(HANAMI_BUD02_ACTION_DELETE), "delete") == 0);
    assert(strcmp(hanami_bud02_action_str(HANAMI_BUD02_ACTION_GET),    "get")    == 0);
    assert(strcmp(hanami_bud02_action_str(HANAMI_BUD02_ACTION_LIST),   "list")   == 0);
    assert(strcmp(hanami_bud02_action_str(HANAMI_BUD02_ACTION_MIRROR), "mirror") == 0);
    assert(hanami_bud02_action_str((hanami_bud02_action_t)99) == NULL);

    hanami_bud02_action_t out;
    assert(hanami_bud02_action_from_str("upload", &out) == HANAMI_BUD02_OK);
    assert(out == HANAMI_BUD02_ACTION_UPLOAD);
    assert(hanami_bud02_action_from_str("delete", &out) == HANAMI_BUD02_OK);
    assert(out == HANAMI_BUD02_ACTION_DELETE);
    assert(hanami_bud02_action_from_str("bogus", &out) == HANAMI_BUD02_ERR_INVALID_ACTION);
    assert(hanami_bud02_action_from_str(NULL, &out) == HANAMI_BUD02_ERR_NULL_PARAM);
}

/* ---- Event creation ---- */

static void test_create_auth_event_upload(void)
{
    const char *hash = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";

    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hash, 0, NULL);
    assert(ev != NULL);

    /* Kind must be 24242 */
    assert(nostr_event_get_kind(ev) == HANAMI_BUD02_KIND);

    /* Action tag */
    const char *t = hanami_bud02_get_action(ev);
    assert(t != NULL);
    assert(strcmp(t, "upload") == 0);

    /* Hash tag */
    const char *x = hanami_bud02_get_hash(ev);
    assert(x != NULL);
    assert(strcmp(x, hash) == 0);

    /* Expiration tag — should be set (default: now + 300s) */
    int64_t exp = hanami_bud02_get_expiration(ev);
    assert(exp > (int64_t)time(NULL));

    nostr_event_free(ev);
}

static void test_create_auth_event_get_no_hash(void)
{
    /* GET action without a hash is valid */
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_GET, NULL, 0, NULL);
    assert(ev != NULL);

    assert(strcmp(hanami_bud02_get_action(ev), "get") == 0);
    assert(hanami_bud02_get_hash(ev) == NULL); /* no x tag */

    nostr_event_free(ev);
}

static void test_create_auth_event_with_server(void)
{
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD,
        "abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234",
        0,
        "https://blossom.example.com");
    assert(ev != NULL);

    const char *srv = hanami_bud02_get_server(ev);
    assert(srv != NULL);
    assert(strcmp(srv, "https://blossom.example.com") == 0);

    nostr_event_free(ev);
}

static void test_create_auth_event_custom_expiration(void)
{
    int64_t custom_exp = (int64_t)time(NULL) + 3600; /* 1 hour */
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_DELETE,
        "aaaa",
        custom_exp,
        NULL);
    assert(ev != NULL);

    int64_t exp = hanami_bud02_get_expiration(ev);
    assert(exp == custom_exp);

    nostr_event_free(ev);
}

static void test_create_auth_event_null_params(void)
{
    /* Invalid action */
    assert(hanami_bud02_create_auth_event(
        (hanami_bud02_action_t)99, NULL, 0, NULL) == NULL);
}

/* ---- Header roundtrip ---- */

static void test_auth_header_roundtrip(void)
{
    const char *hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hash, 0, NULL);
    assert(ev != NULL);

    /* Sign the event */
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);

    /* Create header */
    char *header = hanami_bud02_create_auth_header(ev);
    assert(header != NULL);
    assert(strncmp(header, "Nostr ", 6) == 0);

    /* Parse header back */
    NostrEvent *parsed = NULL;
    hanami_bud02_result_t res = hanami_bud02_parse_auth_header(header, &parsed);
    assert(res == HANAMI_BUD02_OK);
    assert(parsed != NULL);

    /* Verify parsed event */
    assert(nostr_event_get_kind(parsed) == HANAMI_BUD02_KIND);
    assert(strcmp(hanami_bud02_get_action(parsed), "upload") == 0);
    assert(strcmp(hanami_bud02_get_hash(parsed), hash) == 0);

    free(header);
    nostr_event_free(ev);
    nostr_event_free(parsed);
}

static void test_parse_invalid_header(void)
{
    NostrEvent *ev = NULL;

    assert(hanami_bud02_parse_auth_header("Bearer xyz", &ev) ==
           HANAMI_BUD02_ERR_INVALID_HEADER);
    assert(ev == NULL);

    assert(hanami_bud02_parse_auth_header("Nostr ", &ev) ==
           HANAMI_BUD02_ERR_INVALID_HEADER);

    assert(hanami_bud02_parse_auth_header(NULL, &ev) ==
           HANAMI_BUD02_ERR_NULL_PARAM);
    assert(hanami_bud02_parse_auth_header("Nostr abc", NULL) ==
           HANAMI_BUD02_ERR_NULL_PARAM);
}

/* ---- Validation ---- */

static void test_validate_success(void)
{
    const char *hash = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hash, 0, NULL);
    assert(ev != NULL);
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);

    /* Validate without hash check */
    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, NULL);
    assert(res == HANAMI_BUD02_OK);

    /* Validate with hash check */
    hanami_bud02_validate_options_t opts = {
        .expected_sha256 = hash,
        .now_override = 0
    };
    res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, &opts);
    assert(res == HANAMI_BUD02_OK);

    nostr_event_free(ev);
}

static void test_validate_wrong_kind(void)
{
    /* Manually create an event with wrong kind */
    NostrEvent *ev = nostr_event_new();
    assert(ev != NULL);
    nostr_event_set_kind(ev, 1); /* text note, not BUD-02 */

    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, NULL);
    assert(res == HANAMI_BUD02_ERR_INVALID_KIND);

    nostr_event_free(ev);
}

static void test_validate_action_mismatch(void)
{
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, "aaaa", 0, NULL);
    assert(ev != NULL);
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);

    /* Expect DELETE but event says UPLOAD */
    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_DELETE, NULL);
    assert(res == HANAMI_BUD02_ERR_ACTION_MISMATCH);

    nostr_event_free(ev);
}

static void test_validate_expired(void)
{
    /* Create event with expiration in the past */
    int64_t past_exp = (int64_t)time(NULL) - 60;
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, "aaaa", past_exp, NULL);
    assert(ev != NULL);
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);

    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, NULL);
    assert(res == HANAMI_BUD02_ERR_EXPIRED);

    nostr_event_free(ev);
}

static void test_validate_hash_mismatch(void)
{
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, "aaaa", 0, NULL);
    assert(ev != NULL);
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);

    hanami_bud02_validate_options_t opts = {
        .expected_sha256 = "bbbb", /* doesn't match "aaaa" */
        .now_override = 0
    };
    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, &opts);
    assert(res == HANAMI_BUD02_ERR_HASH_MISMATCH);

    nostr_event_free(ev);
}

static void test_validate_null_event(void)
{
    assert(hanami_bud02_validate_auth_event(NULL, HANAMI_BUD02_ACTION_UPLOAD, NULL)
           == HANAMI_BUD02_ERR_NULL_PARAM);
}

static void test_validate_pubkey_match(void)
{
    const char *hash = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hash, 0, NULL);
    assert(ev != NULL);
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);

    /* Get the pubkey from the signed event */
    const char *pubkey = nostr_event_get_pubkey(ev);
    assert(pubkey != NULL);

    /* Validate with matching pubkey */
    hanami_bud02_validate_options_t opts = {
        .expected_sha256 = NULL,
        .expected_pubkey = pubkey,
        .now_override = 0
    };
    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, &opts);
    assert(res == HANAMI_BUD02_OK);

    nostr_event_free(ev);
}

static void test_validate_pubkey_mismatch(void)
{
    const char *hash = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hash, 0, NULL);
    assert(ev != NULL);
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);

    /* Use a different pubkey */
    const char *wrong_pubkey = "0000000000000000000000000000000000000000000000000000000000000000";

    hanami_bud02_validate_options_t opts = {
        .expected_sha256 = NULL,
        .expected_pubkey = wrong_pubkey,
        .now_override = 0
    };
    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, &opts);
    assert(res == HANAMI_BUD02_ERR_PUBKEY_MISMATCH);

    nostr_event_free(ev);
}

/* ---- Batch event creation (nostrc-xeby) ---- */

static void test_create_batch_auth_event_shape(void)
{
    /* Three deterministic 64-hex hashes. */
    const char *h1 = "11111111111111111111111111111111"
                     "11111111111111111111111111111111";
    const char *h2 = "22222222222222222222222222222222"
                     "22222222222222222222222222222222";
    const char *h3 = "33333333333333333333333333333333"
                     "33333333333333333333333333333333";
    const char *const hashes[3] = { h1, h2, h3 };

    NostrEvent *ev = hanami_bud02_create_batch_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hashes, 3, 0, NULL);
    assert(ev != NULL);

    /* Kind must be 24242. */
    assert(nostr_event_get_kind(ev) == HANAMI_BUD02_KIND);

    /* Enumerate tags: expect one "t", one "expiration", three "x", NO "server". */
    NostrTags *tags = ev->tags;
    assert(tags != NULL);
    int n_t = 0, n_exp = 0, n_x = 0, n_srv = 0;
    int seen_h1 = 0, seen_h2 = 0, seen_h3 = 0;
    for (size_t i = 0; i < tags->count; i++) {
        NostrTag *tag = tags->data[i];
        assert(tag != NULL && tag->size >= 2);
        const char *k = string_array_get(tag, 0);
        const char *v = string_array_get(tag, 1);
        if (strcmp(k, "t") == 0) { n_t++; assert(strcmp(v, "upload") == 0); }
        else if (strcmp(k, "expiration") == 0) { n_exp++; }
        else if (strcmp(k, "x") == 0) {
            n_x++;
            if (strcmp(v, h1) == 0) seen_h1++;
            else if (strcmp(v, h2) == 0) seen_h2++;
            else if (strcmp(v, h3) == 0) seen_h3++;
        }
        else if (strcmp(k, "server") == 0) { n_srv++; }
    }
    assert(n_t == 1);
    assert(n_exp == 1);
    assert(n_x == 3);
    assert(n_srv == 0); /* Default: NO server tag (blossom.band-safe) */
    assert(seen_h1 == 1 && seen_h2 == 1 && seen_h3 == 1);

    /* Expiration default: now + 120s window; upper bound now + 900s cap. */
    int64_t now = (int64_t)time(NULL);
    int64_t exp = hanami_bud02_get_expiration(ev);
    assert(exp > now);
    assert(exp - now <= HANAMI_BUD02_BATCH_MAX_EXPIRATION);
    assert(exp - now >= HANAMI_BUD02_BATCH_DEFAULT_EXPIRATION - 5); /* small clock slack */

    /* Signature verifies over the full N-x-tag event. */
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);
    hanami_bud02_result_t vres = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, NULL);
    assert(vres == HANAMI_BUD02_OK);

    nostr_event_free(ev);
}

static void test_create_batch_auth_event_with_server_tag(void)
{
    /* Callers that KNOW the server tolerates a server tag (via probe)
     * may pass it. Must round-trip through the "server" accessor. */
    const char *h[1] = { "aaaa11112222333344445555666677778888"
                         "9999aaaabbbbccccddddeeeeffff0000" };
    NostrEvent *ev = hanami_bud02_create_batch_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, h, 1, 0,
        "https://blossom.sharegap.net");
    assert(ev != NULL);
    const char *srv = hanami_bud02_get_server(ev);
    assert(srv != NULL);
    assert(strcmp(srv, "https://blossom.sharegap.net") == 0);
    nostr_event_free(ev);
}

static void test_create_batch_auth_event_expiration_cap(void)
{
    /* Caller-provided expiration that exceeds the 900s cap MUST be
     * clamped down. */
    const char *h[1] = { "0000000000000000000000000000000000000000000000000000000000000000" };
    int64_t now = (int64_t)time(NULL);
    int64_t huge = now + 4 * 3600; /* 4 hours — well past cap */
    NostrEvent *ev = hanami_bud02_create_batch_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, h, 1, huge, NULL);
    assert(ev != NULL);
    int64_t exp = hanami_bud02_get_expiration(ev);
    assert(exp - now <= HANAMI_BUD02_BATCH_MAX_EXPIRATION + 1);
    nostr_event_free(ev);
}

static void test_create_batch_auth_event_null_params(void)
{
    /* count=0 -> NULL */
    const char *h[1] = { "aaaa" };
    assert(hanami_bud02_create_batch_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, h, 0, 0, NULL) == NULL);
    /* NULL array */
    assert(hanami_bud02_create_batch_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, NULL, 1, 0, NULL) == NULL);
    /* NULL element */
    const char *hbad[2] = { "aaaa", NULL };
    assert(hanami_bud02_create_batch_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hbad, 2, 0, NULL) == NULL);
    /* Bad action */
    assert(hanami_bud02_create_batch_auth_event(
        (hanami_bud02_action_t)99, h, 1, 0, NULL) == NULL);
}

static void test_validate_missing_expiration(void)
{
    /* Manually create an event without expiration tag */
    NostrEvent *ev = nostr_event_new();
    assert(ev != NULL);
    
    int64_t now = (int64_t)time(NULL);
    nostr_event_set_kind(ev, HANAMI_BUD02_KIND);
    nostr_event_set_created_at(ev, now);
    nostr_event_set_content(ev, "Authorize");
    
    /* Create tags with only "t" tag, no expiration */
    NostrTags *tags = malloc(sizeof(NostrTags));
    assert(tags != NULL);
    tags->data = malloc(sizeof(StringArray *));
    assert(tags->data != NULL);
    tags->count = 0;
    tags->capacity = 1;
    
    /* Add only [t, upload] tag */
    StringArray *t_tag = new_string_array(2);
    assert(t_tag != NULL);
    string_array_add(t_tag, "t");
    string_array_add(t_tag, "upload");
    tags->data[tags->count++] = t_tag;
    
    nostr_event_set_tags(ev, tags);
    assert(nostr_event_sign(ev, TEST_PRIVATE_KEY) == 0);
    
    /* Validation should fail with missing expiration */
    hanami_bud02_result_t res = hanami_bud02_validate_auth_event(
        ev, HANAMI_BUD02_ACTION_UPLOAD, NULL);
    assert(res == HANAMI_BUD02_ERR_MISSING_EXPIRATION);
    
    nostr_event_free(ev);
}

/* ---- Error strings ---- */

static void test_strerror(void)
{
    assert(strcmp(hanami_bud02_strerror(HANAMI_BUD02_OK), "Success") == 0);
    assert(strcmp(hanami_bud02_strerror(HANAMI_BUD02_ERR_NULL_PARAM),
                 "Null parameter") == 0);
    assert(strcmp(hanami_bud02_strerror(HANAMI_BUD02_ERR_INVALID_KIND),
                 "Invalid event kind (expected 24242)") == 0);
    assert(strcmp(hanami_bud02_strerror(HANAMI_BUD02_ERR_EXPIRED),
                 "Authorization event has expired") == 0);
    assert(strcmp(hanami_bud02_strerror(HANAMI_BUD02_ERR_PUBKEY_MISMATCH),
                 "Event pubkey does not match expected pubkey") == 0);
    assert(strcmp(hanami_bud02_strerror(HANAMI_BUD02_ERR_MISSING_EXPIRATION),
                 "Expiration tag is missing (required by BUD-02)") == 0);
    /* Unknown code */
    const char *unk = hanami_bud02_strerror((hanami_bud02_result_t)-999);
    assert(unk != NULL);
    assert(strcmp(unk, "Unknown BUD-02 error") == 0);
}

/* ---- Main ---- */

int main(void)
{
    printf("libhanami BUD-02 auth tests\n");
    printf("===========================\n");

    /* Action helpers */
    TEST(action_str_roundtrip);

    /* Event creation */
    TEST(create_auth_event_upload);
    TEST(create_auth_event_get_no_hash);
    TEST(create_auth_event_with_server);
    TEST(create_auth_event_custom_expiration);
    TEST(create_auth_event_null_params);

    /* Batch event creation (nostrc-xeby) */
    TEST(create_batch_auth_event_shape);
    TEST(create_batch_auth_event_with_server_tag);
    TEST(create_batch_auth_event_expiration_cap);
    TEST(create_batch_auth_event_null_params);

    /* Header roundtrip */
    TEST(auth_header_roundtrip);
    TEST(parse_invalid_header);

    /* Validation */
    TEST(validate_success);
    TEST(validate_wrong_kind);
    TEST(validate_action_mismatch);
    TEST(validate_expired);
    TEST(validate_hash_mismatch);
    TEST(validate_null_event);
    TEST(validate_pubkey_match);
    TEST(validate_pubkey_mismatch);
    TEST(validate_missing_expiration);

    /* Error strings */
    TEST(strerror);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
