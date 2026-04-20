/*
 * test_hanami_nostr.c - Tests for Nostr integration layer
 *
 * SPDX-License-Identifier: MIT
 *
 * Tests context lifecycle, filter builders, null guards, and
 * publishing error paths without requiring live relays.
 */

#include "hanami/hanami-nostr.h"
#include "hanami/hanami-types.h"
#include <nostr-filter.h>
#include <nostr-event.h>
#include <nip34.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-55s ", #name); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* ---- Dummy signer ---- */

static hanami_error_t dummy_sign(const char *event_json,
                                  char **out_signed_json,
                                  void *user_data)
{
    (void)user_data;
    *out_signed_json = strdup(event_json);
    return *out_signed_json ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

static const hanami_signer_t test_signer = {
    .pubkey = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    .sign = dummy_sign,
    .user_data = NULL
};

/* =========================================================================
 * Context lifecycle
 * ========================================================================= */

static void test_ctx_new_basic(void)
{
    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    hanami_error_t err = hanami_nostr_ctx_new(relays, NULL, &ctx);
    assert(err == HANAMI_OK);
    assert(ctx != NULL);
    hanami_nostr_ctx_free(ctx);
}

static void test_ctx_new_with_signer(void)
{
    const char *relays[] = { "wss://relay1.example.com", "wss://relay2.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    hanami_error_t err = hanami_nostr_ctx_new(relays, &test_signer, &ctx);
    assert(err == HANAMI_OK);
    assert(ctx != NULL);
    hanami_nostr_ctx_free(ctx);
}

static void test_ctx_new_null_relays(void)
{
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(NULL, NULL, &ctx) == HANAMI_ERR_INVALID_ARG);
    assert(ctx == NULL);
}

static void test_ctx_new_null_out(void)
{
    const char *relays[] = { "wss://relay.example.com", NULL };
    assert(hanami_nostr_ctx_new(relays, NULL, NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_ctx_new_empty_relays(void)
{
    const char *relays[] = { NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, NULL, &ctx) == HANAMI_ERR_INVALID_ARG);
    assert(ctx == NULL);
}

static void test_ctx_free_null(void)
{
    hanami_nostr_ctx_free(NULL); /* must not crash */
}

/* =========================================================================
 * Filter builders
 * ========================================================================= */

static void test_build_repo_filter_basic(void)
{
    NostrFilter *f = hanami_nostr_build_repo_filter("my-repo", NULL);
    assert(f != NULL);

    /* Should have kind 30617 */
    assert(nostr_filter_kinds_len(f) == 1);
    assert(nostr_filter_kinds_get(f, 0) == NIP34_KIND_REPOSITORY);

    /* Should have limit = 1 */
    assert(nostr_filter_get_limit(f) == 1);

    /* No author when NULL */
    assert(nostr_filter_authors_len(f) == 0);

    /* Should have a "d" tag */
    assert(nostr_filter_tags_len(f) >= 1);

    nostr_filter_free(f);
}

static void test_build_repo_filter_with_author(void)
{
    const char *pk = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    NostrFilter *f = hanami_nostr_build_repo_filter("my-repo", pk);
    assert(f != NULL);

    /* Should have 1 author */
    assert(nostr_filter_authors_len(f) == 1);
    assert(strcmp(nostr_filter_authors_get(f, 0), pk) == 0);

    nostr_filter_free(f);
}

static void test_build_repo_filter_null_id(void)
{
    NostrFilter *f = hanami_nostr_build_repo_filter(NULL, NULL);
    assert(f == NULL);
}

static void test_build_state_filter_basic(void)
{
    NostrFilter *f = hanami_nostr_build_state_filter("my-repo", NULL);
    assert(f != NULL);

    assert(nostr_filter_kinds_len(f) == 1);
    assert(nostr_filter_kinds_get(f, 0) == NIP34_KIND_REPOSITORY_STATE);
    assert(nostr_filter_get_limit(f) == 1);
    assert(nostr_filter_authors_len(f) == 0);

    nostr_filter_free(f);
}

static void test_build_state_filter_with_author(void)
{
    const char *pk = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    NostrFilter *f = hanami_nostr_build_state_filter("my-repo", pk);
    assert(f != NULL);

    assert(nostr_filter_authors_len(f) == 1);
    assert(strcmp(nostr_filter_authors_get(f, 0), pk) == 0);

    nostr_filter_free(f);
}

static void test_build_state_filter_null_id(void)
{
    NostrFilter *f = hanami_nostr_build_state_filter(NULL, NULL);
    assert(f == NULL);
}

static void test_build_patches_filter_basic(void)
{
    const char *addr = "30617:aaaa:my-repo";
    NostrFilter *f = hanami_nostr_build_patches_filter(addr);
    assert(f != NULL);

    assert(nostr_filter_kinds_len(f) == 1);
    assert(nostr_filter_kinds_get(f, 0) == NIP34_KIND_PATCH);

    /* Should have an "a" tag */
    assert(nostr_filter_tags_len(f) >= 1);

    nostr_filter_free(f);
}

static void test_build_patches_filter_null_addr(void)
{
    NostrFilter *f = hanami_nostr_build_patches_filter(NULL);
    assert(f == NULL);
}

/* =========================================================================
 * Publishing error paths (no live relays)
 * ========================================================================= */

static void test_publish_event_null_ctx(void)
{
    NostrEvent *ev = nostr_event_new();
    assert(ev != NULL);
    assert(hanami_nostr_publish_event(NULL, ev) == HANAMI_ERR_INVALID_ARG);
    nostr_event_free(ev);
}

static void test_publish_event_null_event(void)
{
    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, &test_signer, &ctx) == HANAMI_OK);
    assert(hanami_nostr_publish_event(ctx, NULL) == HANAMI_ERR_INVALID_ARG);
    hanami_nostr_ctx_free(ctx);
}

static void test_publish_repo_no_signer(void)
{
    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, NULL, &ctx) == HANAMI_OK);

    /* Publishing without a signer should fail with AUTH error */
    hanami_error_t err = hanami_nostr_publish_repo(ctx, "test-repo", "Test", NULL, NULL, NULL);
    assert(err == HANAMI_ERR_AUTH);

    hanami_nostr_ctx_free(ctx);
}

static void test_publish_repo_null_args(void)
{
    assert(hanami_nostr_publish_repo(NULL, "repo", "name", NULL, NULL, NULL) == HANAMI_ERR_INVALID_ARG);

    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, &test_signer, &ctx) == HANAMI_OK);

    assert(hanami_nostr_publish_repo(ctx, NULL, "name", NULL, NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_nostr_publish_repo(ctx, "repo", NULL, NULL, NULL, NULL) == HANAMI_ERR_INVALID_ARG);

    hanami_nostr_ctx_free(ctx);
}

static void test_publish_state_no_signer(void)
{
    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, NULL, &ctx) == HANAMI_OK);

    hanami_error_t err = hanami_nostr_publish_state(ctx, "test-repo", NULL, 0, NULL);
    assert(err == HANAMI_ERR_AUTH);

    hanami_nostr_ctx_free(ctx);
}

static void test_publish_state_null_args(void)
{
    assert(hanami_nostr_publish_state(NULL, "repo", NULL, 0, NULL) == HANAMI_ERR_INVALID_ARG);

    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, &test_signer, &ctx) == HANAMI_OK);

    assert(hanami_nostr_publish_state(ctx, NULL, NULL, 0, NULL) == HANAMI_ERR_INVALID_ARG);

    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Querying error paths
 * ========================================================================= */

static void test_fetch_repo_null_args(void)
{
    nip34_repository_t *repo = NULL;
    assert(hanami_nostr_fetch_repo(NULL, "id", "pk", &repo) == HANAMI_ERR_INVALID_ARG);

    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, NULL, &ctx) == HANAMI_OK);

    assert(hanami_nostr_fetch_repo(ctx, NULL, "pk", &repo) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_nostr_fetch_repo(ctx, "id", NULL, &repo) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_nostr_fetch_repo(ctx, "id", "pk", NULL) == HANAMI_ERR_INVALID_ARG);

    hanami_nostr_ctx_free(ctx);
}

static void test_fetch_state_null_args(void)
{
    nip34_repo_state_t *state = NULL;
    assert(hanami_nostr_fetch_state(NULL, "id", "pk", &state) == HANAMI_ERR_INVALID_ARG);

    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, NULL, &ctx) == HANAMI_OK);

    assert(hanami_nostr_fetch_state(ctx, NULL, "pk", &state) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_nostr_fetch_state(ctx, "id", NULL, &state) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_nostr_fetch_state(ctx, "id", "pk", NULL) == HANAMI_ERR_INVALID_ARG);

    hanami_nostr_ctx_free(ctx);
}

/* ---- Main ---- */

int main(void)
{
    printf("libhanami Nostr integration tests\n");
    printf("==================================\n");

    /* Context lifecycle */
    TEST(ctx_new_basic);
    TEST(ctx_new_with_signer);
    TEST(ctx_new_null_relays);
    TEST(ctx_new_null_out);
    TEST(ctx_new_empty_relays);
    TEST(ctx_free_null);

    /* Filter builders */
    TEST(build_repo_filter_basic);
    TEST(build_repo_filter_with_author);
    TEST(build_repo_filter_null_id);
    TEST(build_state_filter_basic);
    TEST(build_state_filter_with_author);
    TEST(build_state_filter_null_id);
    TEST(build_patches_filter_basic);
    TEST(build_patches_filter_null_addr);

    /* Publishing error paths */
    TEST(publish_event_null_ctx);
    TEST(publish_event_null_event);
    TEST(publish_repo_no_signer);
    TEST(publish_repo_null_args);
    TEST(publish_state_no_signer);
    TEST(publish_state_null_args);

    /* Querying error paths */
    TEST(fetch_repo_null_args);
    TEST(fetch_state_null_args);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
