/*
 * test_hanami_types.c - Basic tests for libhanami types and lifecycle
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami.h"
#include "hanami/hanami-config.h"
#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-refdb-backend.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        printf("  %-50s ", #name); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* ---- Tests ---- */

static void test_version(void)
{
    int major = -1, minor = -1, patch = -1;
    const char *v = hanami_version(&major, &minor, &patch);
    assert(v != NULL);
    assert(strlen(v) > 0);
    
    /* Version components should be non-negative */
    assert(major >= 0);
    assert(minor >= 0);
    assert(patch >= 0);
    
    /* Version string should have format like "X.Y.Z" or "X.Y.Z-suffix"
     * Check that it starts with digits and contains dots */
    assert(v[0] >= '0' && v[0] <= '9'); /* First char is a digit */
    assert(strchr(v, '.') != NULL);     /* Contains at least one dot */
    
    /* Verify the version string matches the components (at least the major version) */
    char expected_start[16];
    snprintf(expected_start, sizeof(expected_start), "%d.", major);
    assert(strncmp(v, expected_start, strlen(expected_start)) == 0);
}

static void test_init_shutdown(void)
{
    hanami_error_t err = hanami_init();
    assert(err == HANAMI_OK);

    /* Double init should be fine (refcounted) */
    err = hanami_init();
    assert(err == HANAMI_OK);

    hanami_shutdown();
    hanami_shutdown();
}

static void test_strerror(void)
{
    const char *msg = hanami_strerror(HANAMI_OK);
    assert(msg != NULL);
    assert(strcmp(msg, "success") == 0);

    msg = hanami_strerror(HANAMI_ERR_NOMEM);
    assert(msg != NULL);
    assert(strlen(msg) > 0);

    /* Unknown error */
    msg = hanami_strerror((hanami_error_t)9999);
    assert(msg != NULL);
    assert(strcmp(msg, "unknown error") == 0);
}

static void test_config_default(void)
{
    hanami_config_t *config = NULL;
    hanami_error_t err = hanami_config_new(&config);
    assert(err == HANAMI_OK);
    assert(config != NULL);

    assert(hanami_config_get_endpoint(config) == NULL);
    assert(hanami_config_get_relay_count(config) == 0);
    assert(strcmp(hanami_config_get_cache_dir(config), "~/.cache/hanami") == 0);
    assert(strcmp(hanami_config_get_index_backend(config), "sqlite") == 0);
    assert(hanami_config_get_upload_threshold(config) == 0);
    assert(hanami_config_get_prefetch_concurrency(config) == 4);
    assert(hanami_config_get_verify_on_read(config) == true);

    hanami_config_free(config);
}

static void test_config_load(void)
{
    hanami_config_t *config = NULL;
    hanami_error_t err = hanami_config_new(&config);
    assert(err == HANAMI_OK);
    err = hanami_config_load_env(config);
    assert(err == HANAMI_OK);
    /* Defaults should still be set (no env vars) */
    assert(hanami_config_get_prefetch_concurrency(config) == 4);
    hanami_config_free(config);
}

static void test_config_null(void)
{
    /* Should not crash */
    hanami_config_free(NULL);

    hanami_error_t err = hanami_config_new(NULL);
    assert(err == HANAMI_ERR_INVALID_ARG);

    err = hanami_config_load_env(NULL);
    assert(err == HANAMI_ERR_INVALID_ARG);
}

static void test_blob_descriptor_free_null(void)
{
    /* Should not crash */
    hanami_blob_descriptor_free(NULL);
}

static void test_odb_backend_null_out(void)
{
    hanami_error_t err = hanami_odb_backend_new(NULL, NULL);
    assert(err == HANAMI_ERR_INVALID_ARG);
}

static void test_refdb_backend_null_out(void)
{
    hanami_error_t err = hanami_refdb_backend_new(NULL, NULL);
    assert(err == HANAMI_ERR_INVALID_ARG);
}

static void test_repo_open_null_out(void)
{
    hanami_error_t err = hanami_repo_open(NULL, "https://example.com",
                                          NULL, "repo", "pubkey", NULL, NULL);
    assert(err == HANAMI_ERR_INVALID_ARG);
}

static void test_clone_null_out(void)
{
    hanami_error_t err = hanami_clone(NULL, "nostr://npub1.../repo",
                                      "/tmp/test", NULL, NULL);
    assert(err == HANAMI_ERR_INVALID_ARG);
}

/* ---- Main ---- */

int main(void)
{
    printf("libhanami type and lifecycle tests\n");
    printf("==================================\n");

    TEST(version);
    TEST(init_shutdown);
    TEST(strerror);
    TEST(config_default);
    TEST(config_load);
    TEST(config_null);
    TEST(blob_descriptor_free_null);
    TEST(odb_backend_null_out);
    TEST(refdb_backend_null_out);
    TEST(repo_open_null_out);
    TEST(clone_null_out);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
