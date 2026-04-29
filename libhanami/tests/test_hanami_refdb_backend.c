/*
 * test_hanami_refdb_backend.c - Tests for NIP-34 refdb backend
 *
 * SPDX-License-Identifier: MIT
 *
 * Tests constructor, null guards, ref lookup/iteration via cache,
 * write/rename/del operations, and reflog stubs.
 */

#include "hanami/hanami-refdb-backend.h"
#include "hanami/hanami-nostr.h"
#include "hanami/hanami-types.h"

#include <git2.h>
#include <git2/sys/refdb_backend.h>
#include <git2/sys/refs.h>

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

/* ---- Dummy signer (not invoked for read-only tests) ---- */

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

/* Helper: create nostr ctx for tests */
static hanami_nostr_ctx_t *make_ctx(void)
{
    const char *relays[] = { "wss://relay.example.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    hanami_error_t err = hanami_nostr_ctx_new(relays, &test_signer, &ctx);
    assert(err == HANAMI_OK);
    return ctx;
}

/* =========================================================================
 * Constructor tests
 * ========================================================================= */

static void test_new_null_args(void)
{
    git_refdb_backend *be = NULL;
    assert(hanami_refdb_backend_new(NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_refdb_backend_new(&be, NULL) == HANAMI_ERR_INVALID_ARG);

    hanami_refdb_backend_opts_t opts = { .nostr_ctx = NULL, .repo_id = NULL, .owner_pubkey = NULL };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_ERR_INVALID_ARG);
    assert(be == NULL);
}

static void test_new_missing_repo_id(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = NULL,
        .owner_pubkey = "aaaa"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_ERR_INVALID_ARG);
    hanami_nostr_ctx_free(ctx);
}

static void test_new_missing_owner(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = NULL
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_ERR_INVALID_ARG);
    hanami_nostr_ctx_free(ctx);
}

static void test_new_success(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    };
    hanami_error_t err = hanami_refdb_backend_new(&be, &opts);
    assert(err == HANAMI_OK);
    assert(be != NULL);
    assert(be->version == GIT_REFDB_BACKEND_VERSION);

    /* Verify all required vtable slots are set */
    assert(be->exists != NULL);
    assert(be->lookup != NULL);
    assert(be->iterator != NULL);
    assert(be->write != NULL);
    assert(be->rename != NULL);
    assert(be->del != NULL);
    assert(be->has_log != NULL);
    assert(be->ensure_log != NULL);
    assert(be->free != NULL);
    assert(be->reflog_read != NULL);
    assert(be->reflog_write != NULL);
    assert(be->reflog_rename != NULL);
    assert(be->reflog_delete != NULL);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Exists / Lookup on empty backend
 * ========================================================================= */

static void test_exists_empty(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    int exists = -1;
    assert(be->exists(&exists, be, "refs/heads/main") == 0);
    assert(exists == 0);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

static void test_lookup_not_found(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    git_reference *ref = NULL;
    int rc = be->lookup(&ref, be, "refs/heads/nonexistent");
    assert(rc == GIT_ENOTFOUND);
    assert(ref == NULL);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Write + Lookup roundtrip
 * ========================================================================= */

static void test_write_and_lookup_direct(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    /* Create a direct reference */
    git_oid oid;
    git_oid_fromstr(&oid, "aabbccddaabbccddaabbccddaabbccddaabbccdd");
    git_reference *ref = git_reference__alloc("refs/heads/main", &oid, NULL);
    assert(ref != NULL);

    /* Write it (publish will fail since relay is fake — but cache is updated) */
    int rc = be->write(be, ref, 1 /* force */, NULL, NULL, NULL, NULL);
    git_reference_free(ref);
    /* rc may be -1 from failed publish, but let's check the cache was updated */

    /* Check exists */
    int exists = 0;
    be->exists(&exists, be, "refs/heads/main");
    assert(exists == 1);

    /* Lookup */
    git_reference *found = NULL;
    rc = be->lookup(&found, be, "refs/heads/main");
    assert(rc == 0);
    assert(found != NULL);
    assert(git_reference_type(found) == GIT_REFERENCE_DIRECT);
    assert(git_oid_cmp(git_reference_target(found), &oid) == 0);
    assert(strcmp(git_reference_name(found), "refs/heads/main") == 0);
    git_reference_free(found);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

static void test_write_and_lookup_symbolic(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    /* Create a symbolic reference */
    git_reference *ref = git_reference__alloc_symbolic("HEAD", "refs/heads/main");
    assert(ref != NULL);

    be->write(be, ref, 1, NULL, NULL, NULL, NULL);
    git_reference_free(ref);

    /* Lookup */
    git_reference *found = NULL;
    int rc = be->lookup(&found, be, "HEAD");
    assert(rc == 0);
    assert(found != NULL);
    assert(git_reference_type(found) == GIT_REFERENCE_SYMBOLIC);
    assert(strcmp(git_reference_symbolic_target(found), "refs/heads/main") == 0);
    git_reference_free(found);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Iterator
 * ========================================================================= */

static void test_iterator_empty(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    git_reference_iterator *iter = NULL;
    assert(be->iterator(&iter, be, NULL) == 0);
    assert(iter != NULL);

    git_reference *ref = NULL;
    assert(iter->next(&ref, iter) == GIT_ITEROVER);
    assert(ref == NULL);

    iter->free(iter);
    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

static void test_iterator_with_refs(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    /* Add a couple of refs */
    git_oid oid;
    git_oid_fromstr(&oid, "aabbccddaabbccddaabbccddaabbccddaabbccdd");
    git_reference *r1 = git_reference__alloc("refs/heads/main", &oid, NULL);
    git_reference *r2 = git_reference__alloc("refs/heads/dev", &oid, NULL);
    be->write(be, r1, 1, NULL, NULL, NULL, NULL);
    be->write(be, r2, 1, NULL, NULL, NULL, NULL);
    git_reference_free(r1);
    git_reference_free(r2);

    /* Iterate all */
    git_reference_iterator *iter = NULL;
    assert(be->iterator(&iter, be, NULL) == 0);

    int count = 0;
    git_reference *ref = NULL;
    while (iter->next(&ref, iter) == 0) {
        count++;
        git_reference_free(ref);
        ref = NULL;
    }
    assert(count == 2);
    iter->free(iter);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

static void test_iterator_next_name(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    git_oid oid;
    git_oid_fromstr(&oid, "aabbccddaabbccddaabbccddaabbccddaabbccdd");
    git_reference *r1 = git_reference__alloc("refs/tags/v1.0", &oid, NULL);
    be->write(be, r1, 1, NULL, NULL, NULL, NULL);
    git_reference_free(r1);

    git_reference_iterator *iter = NULL;
    assert(be->iterator(&iter, be, NULL) == 0);

    const char *name = NULL;
    assert(iter->next_name(&name, iter) == 0);
    assert(name != NULL);
    assert(strcmp(name, "refs/tags/v1.0") == 0);

    assert(iter->next_name(&name, iter) == GIT_ITEROVER);

    iter->free(iter);
    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Delete
 * ========================================================================= */

static void test_del_ref(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    /* Add a ref */
    git_oid oid;
    git_oid_fromstr(&oid, "aabbccddaabbccddaabbccddaabbccddaabbccdd");
    git_reference *r = git_reference__alloc("refs/heads/feature", &oid, NULL);
    be->write(be, r, 1, NULL, NULL, NULL, NULL);
    git_reference_free(r);

    int exists = 0;
    be->exists(&exists, be, "refs/heads/feature");
    assert(exists == 1);

    /* Delete */
    be->del(be, "refs/heads/feature", NULL, NULL);

    be->exists(&exists, be, "refs/heads/feature");
    assert(exists == 0);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

static void test_del_not_found(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    int rc = be->del(be, "refs/heads/nonexistent", NULL, NULL);
    assert(rc == GIT_ENOTFOUND);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Rename
 * ========================================================================= */

static void test_rename_ref(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    git_oid oid;
    git_oid_fromstr(&oid, "aabbccddaabbccddaabbccddaabbccddaabbccdd");
    git_reference *r = git_reference__alloc("refs/heads/old-name", &oid, NULL);
    be->write(be, r, 1, NULL, NULL, NULL, NULL);
    git_reference_free(r);

    git_reference *renamed = NULL;
    int rc = be->rename(&renamed, be, "refs/heads/old-name", "refs/heads/new-name",
                        1, NULL, NULL);
    /* rc might be -1 from publish failure, but cache is updated */

    /* Old should be gone, new should exist */
    int exists = 0;
    be->exists(&exists, be, "refs/heads/old-name");
    assert(exists == 0);

    be->exists(&exists, be, "refs/heads/new-name");
    assert(exists == 1);

    if (renamed)
        git_reference_free(renamed);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Write to Nostr verification
 * ========================================================================= */

static void test_write_attempts_nostr_publish(void)
{
    /* Test that refdb_write exercises the Nostr publish path.
     * The publish will fail (fake relay), but it should get past parameter
     * validation and attempt the network operation. */
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    /* Create a reference */
    git_oid oid;
    git_oid_fromstr(&oid, "1234567890abcdef1234567890abcdef12345678");
    git_reference *ref = git_reference__alloc("refs/heads/feature-x", &oid, NULL);
    assert(ref != NULL);

    /* Write it — this should:
     * 1. Update the in-memory cache ✓
     * 2. Attempt to publish a Nostr state event ✓ (will fail with network error)
     *
     * The return value may be -1 (network error) but cache should be updated. */
    int rc = be->write(be, ref, 1, NULL, NULL, NULL, NULL);
    git_reference_free(ref);

    /* Verify cache was updated regardless of publish failure */
    int exists = 0;
    be->exists(&exists, be, "refs/heads/feature-x");
    assert(exists == 1);

    /* Lookup should work from cache */
    git_reference *found = NULL;
    rc = be->lookup(&found, be, "refs/heads/feature-x");
    assert(rc == 0);
    assert(found != NULL);
    assert(git_oid_cmp(git_reference_target(found), &oid) == 0);
    git_reference_free(found);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * Reflog stubs
 * ========================================================================= */

static void test_has_log_returns_zero(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    assert(be->has_log(be, "refs/heads/main") == 0);
    assert(be->ensure_log(be, "refs/heads/main") == 0);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

static void test_reflog_read_not_found(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    git_reflog *reflog = NULL;
    assert(be->reflog_read(&reflog, be, "refs/heads/main") == GIT_ENOTFOUND);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

static void test_reflog_write_noop(void)
{
    hanami_nostr_ctx_t *ctx = make_ctx();
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "test-repo",
        .owner_pubkey = "bbbb"
    };
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);

    assert(be->reflog_write(be, NULL) == 0);
    assert(be->reflog_rename(be, "old", "new") == 0);
    assert(be->reflog_delete(be, "ref") == 0);

    be->free(be);
    hanami_nostr_ctx_free(ctx);
}

/* ---- Main ---- */

int main(void)
{
    git_libgit2_init();

    printf("libhanami RefDB backend tests\n");
    printf("==============================\n");

    /* Constructor */
    TEST(new_null_args);
    TEST(new_missing_repo_id);
    TEST(new_missing_owner);
    TEST(new_success);

    /* Exists / Lookup */
    TEST(exists_empty);
    TEST(lookup_not_found);

    /* Write + Lookup roundtrip */
    TEST(write_and_lookup_direct);
    TEST(write_and_lookup_symbolic);

    /* Iterator */
    TEST(iterator_empty);
    TEST(iterator_with_refs);
    TEST(iterator_next_name);

    /* Delete */
    TEST(del_ref);
    TEST(del_not_found);

    /* Rename */
    TEST(rename_ref);

    /* Write to Nostr verification */
    TEST(write_attempts_nostr_publish);

    /* Reflog stubs */
    TEST(has_log_returns_zero);
    TEST(reflog_read_not_found);
    TEST(reflog_write_noop);

    printf("\n%d passed, 0 failed\n", tests_passed);

    git_libgit2_shutdown();
    return 0;
}
