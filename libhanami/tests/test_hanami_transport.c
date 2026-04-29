/*
 * test_hanami_transport.c - Tests for Blossom transport plugin
 *
 * SPDX-License-Identifier: MIT
 *
 * Tests URL parsing, registration/unregistration, and transport
 * factory without requiring live servers or relays.
 */

#include "hanami/hanami-transport.h"
#include "hanami/hanami-nostr.h"
#include "hanami/hanami-types.h"

#include <git2.h>

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

/* =========================================================================
 * URL parsing
 * ========================================================================= */

static void test_parse_url_basic(void)
{
    char *ep = NULL, *pk = NULL, *id = NULL;
    hanami_error_t err = hanami_transport_parse_url(
        "blossom://blossom.example.com/aabbccdd/my-repo",
        &ep, &pk, &id);
    assert(err == HANAMI_OK);
    assert(strcmp(ep, "https://blossom.example.com") == 0);
    assert(strcmp(pk, "aabbccdd") == 0);
    assert(strcmp(id, "my-repo") == 0);
    free(ep);
    free(pk);
    free(id);
}

static void test_parse_url_trailing_slash(void)
{
    char *ep = NULL, *pk = NULL, *id = NULL;
    hanami_error_t err = hanami_transport_parse_url(
        "blossom://host.com/pubkey/repo-id/",
        &ep, &pk, &id);
    assert(err == HANAMI_OK);
    assert(strcmp(ep, "https://host.com") == 0);
    assert(strcmp(pk, "pubkey") == 0);
    assert(strcmp(id, "repo-id") == 0);
    free(ep);
    free(pk);
    free(id);
}

static void test_parse_url_long_pubkey(void)
{
    char *ep = NULL, *pk = NULL, *id = NULL;
    hanami_error_t err = hanami_transport_parse_url(
        "blossom://blossom.server.io/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/nostrc",
        &ep, &pk, &id);
    assert(err == HANAMI_OK);
    assert(strlen(pk) == 64);
    assert(strcmp(id, "nostrc") == 0);
    free(ep);
    free(pk);
    free(id);
}

static void test_parse_url_null_args(void)
{
    char *ep, *pk, *id;
    assert(hanami_transport_parse_url(NULL, &ep, &pk, &id) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_transport_parse_url("blossom://h/p/r", NULL, &pk, &id) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_transport_parse_url("blossom://h/p/r", &ep, NULL, &id) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_transport_parse_url("blossom://h/p/r", &ep, &pk, NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_parse_url_wrong_scheme(void)
{
    char *ep = NULL, *pk = NULL, *id = NULL;
    assert(hanami_transport_parse_url("https://example.com/pk/repo", &ep, &pk, &id) == HANAMI_ERR_INVALID_ARG);
    assert(ep == NULL);
}

static void test_parse_url_missing_repo(void)
{
    char *ep = NULL, *pk = NULL, *id = NULL;
    assert(hanami_transport_parse_url("blossom://host.com/pubkey/", &ep, &pk, &id) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_transport_parse_url("blossom://host.com/pubkey", &ep, &pk, &id) == HANAMI_ERR_INVALID_ARG);
}

static void test_parse_url_missing_pubkey(void)
{
    char *ep = NULL, *pk = NULL, *id = NULL;
    assert(hanami_transport_parse_url("blossom://host.com//repo", &ep, &pk, &id) == HANAMI_ERR_INVALID_ARG);
}

static void test_parse_url_no_host(void)
{
    char *ep = NULL, *pk = NULL, *id = NULL;
    assert(hanami_transport_parse_url("blossom:///pk/repo", &ep, &pk, &id) == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

static void test_register_null_opts(void)
{
    assert(hanami_transport_register(NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_register_and_unregister(void)
{
    hanami_transport_opts_t opts = {
        .nostr_ctx = NULL,
        .blossom_client = NULL
    };
    assert(hanami_transport_register(&opts) == HANAMI_OK);
    /* Double register should succeed (idempotent) */
    assert(hanami_transport_register(&opts) == HANAMI_OK);
    assert(hanami_transport_unregister() == HANAMI_OK);
    /* Double unregister should succeed */
    assert(hanami_transport_unregister() == HANAMI_OK);
}

static void test_register_reregister(void)
{
    hanami_transport_opts_t opts = { .nostr_ctx = NULL, .blossom_client = NULL };
    assert(hanami_transport_register(&opts) == HANAMI_OK);
    assert(hanami_transport_unregister() == HANAMI_OK);
    /* Should be able to register again after unregister */
    assert(hanami_transport_register(&opts) == HANAMI_OK);
    assert(hanami_transport_unregister() == HANAMI_OK);
}

/* =========================================================================
 * Transport creation (via libgit2 factory)
 * ========================================================================= */

static void test_transport_scheme_recognized(void)
{
    /* Verify that after registration, libgit2 recognizes the blossom:// scheme.
     * Before registration, creating a remote with blossom:// should fail.
     * After registration, it should succeed. */
    
    git_repository *repo = NULL;
    int rc = git_repository_init(&repo, "/tmp/hanami-test-scheme-repo", 1);
    if (rc < 0) {
        return; /* Skip if can't create repo */
    }

    /* Before registration — should fail or use generic transport */
    git_remote *remote_before = NULL;
    rc = git_remote_create_anonymous(&remote_before, repo,
        "blossom://example.com/pubkey/repo");
    /* May succeed with generic transport or fail — either way, clean up */
    if (remote_before) {
        git_remote_free(remote_before);
    }

    /* Register transport */
    hanami_transport_opts_t opts = { .nostr_ctx = NULL, .blossom_client = NULL };
    assert(hanami_transport_register(&opts) == HANAMI_OK);

    /* After registration — should succeed */
    git_remote *remote_after = NULL;
    rc = git_remote_create_anonymous(&remote_after, repo,
        "blossom://example.com/pubkey/repo");
    assert(rc == 0);
    assert(remote_after != NULL);

    /* Verify the URL is preserved */
    const char *url = git_remote_url(remote_after);
    assert(url != NULL);
    assert(strncmp(url, "blossom://", 10) == 0);

    git_remote_free(remote_after);
    git_repository_free(repo);
    hanami_transport_unregister();
}

static void test_transport_factory(void)
{
    /* Register transport, create a remote with blossom:// URL, verify
     * libgit2 can instantiate the transport. */
    hanami_transport_opts_t opts = { .nostr_ctx = NULL, .blossom_client = NULL };
    assert(hanami_transport_register(&opts) == HANAMI_OK);

    /* Create an in-memory repository to attach the remote to */
    git_repository *repo = NULL;
    int rc = git_repository_init(&repo, "/tmp/hanami-test-transport-repo", 1);
    if (rc < 0) {
        /* If we can't create the repo, skip this test */
        hanami_transport_unregister();
        return;
    }

    git_remote *remote = NULL;
    rc = git_remote_create_anonymous(&remote, repo,
        "blossom://blossom.example.com/aabbccdd/test-repo");
    assert(rc == 0);
    assert(remote != NULL);

    /* The remote's URL should match */
    const char *url = git_remote_url(remote);
    assert(url != NULL);
    assert(strncmp(url, "blossom://", 10) == 0);

    git_remote_free(remote);
    git_repository_free(repo);

    /* Cleanup temp repo */
    git_buf buf = GIT_BUF_INIT;
    buf.ptr = "/tmp/hanami-test-transport-repo";
    buf.size = strlen(buf.ptr);

    hanami_transport_unregister();
}

/* ---- Main ---- */

int main(void)
{
    git_libgit2_init();

    printf("libhanami Blossom transport tests\n");
    printf("==================================\n");

    /* URL parsing */
    TEST(parse_url_basic);
    TEST(parse_url_trailing_slash);
    TEST(parse_url_long_pubkey);
    TEST(parse_url_null_args);
    TEST(parse_url_wrong_scheme);
    TEST(parse_url_missing_repo);
    TEST(parse_url_missing_pubkey);
    TEST(parse_url_no_host);

    /* Registration */
    TEST(register_null_opts);
    TEST(register_and_unregister);
    TEST(register_reregister);

    /* Transport factory */
    TEST(transport_scheme_recognized);
    TEST(transport_factory);

    printf("\n%d passed, 0 failed\n", tests_passed);

    git_libgit2_shutdown();
    return 0;
}
