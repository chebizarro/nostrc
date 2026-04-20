/*
 * test_hanami_grasp.c - Tests for GRASP server compatibility
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-grasp.h"
#include "hanami/hanami-types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int tests_run = 0;
static int tests_failed = 0;

#define RUN_TEST(fn) do { \
    printf("  %-55s", #fn); \
    fflush(stdout); \
    fn(); \
    tests_run++; \
    printf("OK\n"); \
} while(0)

/* A valid npub for testing (63 chars after npub1) */
#define TEST_NPUB "npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqspc55m"
#define TEST_HOST "relay.example.com"
#define TEST_REPO "my-project"

/* =========================================================================
 * 1. GRASP detection tests
 * ========================================================================= */

static void test_is_grasp_basic(void)
{
    const char *clone = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git";
    const char *relays[] = { "wss://" TEST_HOST, NULL };
    assert(hanami_is_grasp_server(clone, relays, 1) == true);
}

static void test_is_grasp_http(void)
{
    const char *clone = "http://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git";
    const char *relays[] = { "ws://" TEST_HOST, NULL };
    assert(hanami_is_grasp_server(clone, relays, 1) == true);
}

static void test_is_grasp_relay_mismatch(void)
{
    const char *clone = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git";
    const char *relays[] = { "wss://other.relay.com", NULL };
    assert(hanami_is_grasp_server(clone, relays, 1) == false);
}

static void test_is_grasp_not_npub(void)
{
    /* Path has a hex pubkey instead of npub */
    const char *clone = "https://" TEST_HOST "/aabbccdd/" TEST_REPO ".git";
    const char *relays[] = { "wss://" TEST_HOST, NULL };
    assert(hanami_is_grasp_server(clone, relays, 1) == false);
}

static void test_is_grasp_no_git_suffix(void)
{
    const char *clone = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO;
    const char *relays[] = { "wss://" TEST_HOST, NULL };
    assert(hanami_is_grasp_server(clone, relays, 1) == false);
}

static void test_is_grasp_null_args(void)
{
    const char *relays[] = { "wss://" TEST_HOST, NULL };
    assert(hanami_is_grasp_server(NULL, relays, 1) == false);
    assert(hanami_is_grasp_server("https://x/" TEST_NPUB "/r.git", NULL, 0) == false);
}

static void test_is_grasp_multiple_relays(void)
{
    const char *clone = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git";
    const char *relays[] = {
        "wss://other1.com",
        "wss://other2.com",
        "wss://" TEST_HOST,
        NULL
    };
    assert(hanami_is_grasp_server(clone, relays, 3) == true);
}

static void test_is_grasp_case_insensitive_host(void)
{
    const char *clone = "https://Relay.Example.COM/" TEST_NPUB "/" TEST_REPO ".git";
    const char *relays[] = { "wss://relay.example.com", NULL };
    assert(hanami_is_grasp_server(clone, relays, 1) == true);
}

/* =========================================================================
 * 2. Clone URL parsing tests
 * ========================================================================= */

static void test_parse_valid_url(void)
{
    const char *url = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git";
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url(url, &info) == HANAMI_OK);
    assert(info != NULL);
    assert(strcmp(info->host, TEST_HOST) == 0);
    assert(strcmp(info->npub, TEST_NPUB) == 0);
    assert(strcmp(info->repo_name, TEST_REPO) == 0);
    assert(info->uses_tls == true);
    assert(info->clone_url != NULL);
    assert(strcmp(info->clone_url, url) == 0);
    assert(info->relay_url != NULL);
    /* relay URL should be wss://<host>/ */
    assert(strncmp(info->relay_url, "wss://", 6) == 0);
    /* pubkey should be decoded from npub */
    assert(info->pubkey != NULL);
    assert(strlen(info->pubkey) == 64);
    hanami_grasp_info_free(info);
}

static void test_parse_http_url(void)
{
    const char *url = "http://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git";
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url(url, &info) == HANAMI_OK);
    assert(info->uses_tls == false);
    assert(strncmp(info->relay_url, "ws://", 5) == 0);
    hanami_grasp_info_free(info);
}

static void test_parse_invalid_scheme(void)
{
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url("ftp://host/npub1.../r.git", &info)
           == HANAMI_ERR_INVALID_ARG);
    assert(info == NULL);
}

static void test_parse_no_npub(void)
{
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url("https://host/hexkey/r.git", &info)
           == HANAMI_ERR_INVALID_ARG);
}

static void test_parse_no_git_suffix(void)
{
    const char *url = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO;
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url(url, &info) == HANAMI_ERR_INVALID_ARG);
}

static void test_parse_null_args(void)
{
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url(NULL, &info) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_grasp_parse_clone_url("https://h/" TEST_NPUB "/r.git", NULL)
           == HANAMI_ERR_INVALID_ARG);
}

static void test_parse_empty_repo(void)
{
    /* Just ".git" with no name */
    const char *url = "https://" TEST_HOST "/" TEST_NPUB "/.git";
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url(url, &info) == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * 3. URL builders
 * ========================================================================= */

static void test_build_clone_url_https(void)
{
    char *url = NULL;
    assert(hanami_grasp_build_clone_url(TEST_HOST, TEST_NPUB, TEST_REPO,
                                         true, &url) == HANAMI_OK);
    assert(url != NULL);

    char expected[256];
    snprintf(expected, sizeof(expected), "https://%s/%s/%s.git",
             TEST_HOST, TEST_NPUB, TEST_REPO);
    assert(strcmp(url, expected) == 0);
    free(url);
}

static void test_build_clone_url_http(void)
{
    char *url = NULL;
    assert(hanami_grasp_build_clone_url(TEST_HOST, TEST_NPUB, TEST_REPO,
                                         false, &url) == HANAMI_OK);
    assert(strncmp(url, "http://", 7) == 0);
    free(url);
}

static void test_build_clone_url_null_args(void)
{
    char *url = NULL;
    assert(hanami_grasp_build_clone_url(NULL, TEST_NPUB, TEST_REPO, true, &url)
           == HANAMI_ERR_INVALID_ARG);
    assert(hanami_grasp_build_clone_url(TEST_HOST, NULL, TEST_REPO, true, &url)
           == HANAMI_ERR_INVALID_ARG);
    assert(hanami_grasp_build_clone_url(TEST_HOST, TEST_NPUB, NULL, true, &url)
           == HANAMI_ERR_INVALID_ARG);
}

static void test_build_relay_url_wss(void)
{
    char *url = NULL;
    assert(hanami_grasp_build_relay_url(TEST_HOST, true, &url) == HANAMI_OK);
    assert(url != NULL);

    char expected[128];
    snprintf(expected, sizeof(expected), "wss://%s", TEST_HOST);
    assert(strcmp(url, expected) == 0);
    free(url);
}

static void test_build_relay_url_ws(void)
{
    char *url = NULL;
    assert(hanami_grasp_build_relay_url(TEST_HOST, false, &url) == HANAMI_OK);
    assert(strncmp(url, "ws://", 5) == 0);
    free(url);
}

static void test_build_relay_url_null(void)
{
    char *url = NULL;
    assert(hanami_grasp_build_relay_url(NULL, true, &url) == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * 4. PR refname helper
 * ========================================================================= */

static void test_pr_refname(void)
{
    char *ref = NULL;
    assert(hanami_grasp_pr_refname("abc123def456", &ref) == HANAMI_OK);
    assert(ref != NULL);
    assert(strcmp(ref, "refs/nostr/abc123def456") == 0);
    free(ref);
}

static void test_pr_refname_null_args(void)
{
    char *ref = NULL;
    assert(hanami_grasp_pr_refname(NULL, &ref) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_grasp_pr_refname("abc", NULL) == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * 5. Push workflow validation
 * ========================================================================= */

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

static void test_push_null_repo(void)
{
    const char *relays[] = { "wss://relay.test.com", NULL };
    hanami_grasp_push_opts_t opts = {
        .clone_url = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git",
        .relay_urls = relays,
        .relay_count = 1,
        .signer = &test_signer,
        .repo_id = "test-repo",
    };
    assert(hanami_push_to_grasp(NULL, &opts) == HANAMI_ERR_INVALID_ARG);
}

static void test_push_null_opts(void)
{
    /* We can't easily create a real repo, just test null validation */
    assert(hanami_push_to_grasp((git_repository *)1, NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_push_missing_signer(void)
{
    const char *relays[] = { "wss://relay.test.com", NULL };
    hanami_grasp_push_opts_t opts = {
        .clone_url = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git",
        .relay_urls = relays,
        .relay_count = 1,
        .signer = NULL,
        .repo_id = "test-repo",
    };
    assert(hanami_push_to_grasp((git_repository *)1, &opts) == HANAMI_ERR_INVALID_ARG);
}

static void test_push_invalid_url(void)
{
    const char *relays[] = { "wss://relay.test.com", NULL };
    hanami_grasp_push_opts_t opts = {
        .clone_url = "not-a-valid-url",
        .relay_urls = relays,
        .relay_count = 1,
        .signer = &test_signer,
        .repo_id = "test-repo",
    };
    /* Will fail at URL validation — the (git_repository*)1 won't be dereferenced */
    assert(hanami_push_to_grasp((git_repository *)1, &opts) == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * 6. Fetch validation
 * ========================================================================= */

static void test_fetch_null_url(void)
{
    const char *relays[] = { "wss://relay.test.com", NULL };
    assert(hanami_grasp_fetch(NULL, NULL, relays, 1) == HANAMI_ERR_INVALID_ARG);
}

static void test_fetch_invalid_url(void)
{
    const char *relays[] = { "wss://relay.test.com", NULL };
    assert(hanami_grasp_fetch(NULL, "not-grasp-url", relays, 1) == HANAMI_ERR_INVALID_ARG);
}

static void test_fetch_valid_url_no_repo(void)
{
    /* With NULL repo, should just validate URL and return OK */
    const char *url = "https://" TEST_HOST "/" TEST_NPUB "/" TEST_REPO ".git";
    const char *relays[] = { "wss://" TEST_HOST, NULL };
    assert(hanami_grasp_fetch(NULL, url, relays, 1) == HANAMI_OK);
}

/* =========================================================================
 * 7. Npub bech32 decode verification
 * ========================================================================= */

static void test_npub_decode_known_vector(void)
{
    /* Use a well-known npub → verify the decoded pubkey is 64 hex chars.
     * We can verify the npub encodes correctly by checking parse + pubkey field. */
    /* npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqspc55m
     * encodes 32 zero bytes (all-zero pubkey) */
    const char *url = "https://test.com/" TEST_NPUB "/repo.git";
    hanami_grasp_info_t *info = NULL;
    assert(hanami_grasp_parse_clone_url(url, &info) == HANAMI_OK);
    assert(info->pubkey != NULL);
    assert(strlen(info->pubkey) == 64);
    /* All-zero npub should decode to all-zero hex pubkey */
    for (int i = 0; i < 64; i++) {
        assert(info->pubkey[i] == '0');
    }
    hanami_grasp_info_free(info);
}

/* =========================================================================
 * 8. Info free safety
 * ========================================================================= */

static void test_info_free_null(void)
{
    hanami_grasp_info_free(NULL); /* Should not crash */
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void)
{
    printf("libhanami GRASP tests\n");
    printf("======================\n");

    /* 1. Detection */
    RUN_TEST(test_is_grasp_basic);
    RUN_TEST(test_is_grasp_http);
    RUN_TEST(test_is_grasp_relay_mismatch);
    RUN_TEST(test_is_grasp_not_npub);
    RUN_TEST(test_is_grasp_no_git_suffix);
    RUN_TEST(test_is_grasp_null_args);
    RUN_TEST(test_is_grasp_multiple_relays);
    RUN_TEST(test_is_grasp_case_insensitive_host);

    /* 2. URL parsing */
    RUN_TEST(test_parse_valid_url);
    RUN_TEST(test_parse_http_url);
    RUN_TEST(test_parse_invalid_scheme);
    RUN_TEST(test_parse_no_npub);
    RUN_TEST(test_parse_no_git_suffix);
    RUN_TEST(test_parse_null_args);
    RUN_TEST(test_parse_empty_repo);

    /* 3. URL builders */
    RUN_TEST(test_build_clone_url_https);
    RUN_TEST(test_build_clone_url_http);
    RUN_TEST(test_build_clone_url_null_args);
    RUN_TEST(test_build_relay_url_wss);
    RUN_TEST(test_build_relay_url_ws);
    RUN_TEST(test_build_relay_url_null);

    /* 4. PR refname */
    RUN_TEST(test_pr_refname);
    RUN_TEST(test_pr_refname_null_args);

    /* 5. Push validation */
    RUN_TEST(test_push_null_repo);
    RUN_TEST(test_push_null_opts);
    RUN_TEST(test_push_missing_signer);
    RUN_TEST(test_push_invalid_url);

    /* 6. Fetch validation */
    RUN_TEST(test_fetch_null_url);
    RUN_TEST(test_fetch_invalid_url);
    RUN_TEST(test_fetch_valid_url_no_repo);

    /* 7. Bech32 decode */
    RUN_TEST(test_npub_decode_known_vector);

    /* 8. Safety */
    RUN_TEST(test_info_free_null);

    printf("\n%d passed, %d failed\n", tests_run - tests_failed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
