/*
 * test_hanami_config.c - Tests for configuration system
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-config.h"
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
 * Lifecycle
 * ========================================================================= */

static void test_new_default(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);
    assert(c != NULL);

    /* Check defaults */
    assert(hanami_config_get_endpoint(c) == NULL);
    assert(hanami_config_get_relay_count(c) == 0);
    assert(strcmp(hanami_config_get_cache_dir(c), "~/.cache/hanami") == 0);
    assert(strcmp(hanami_config_get_index_backend(c), "sqlite") == 0);
    assert(hanami_config_get_upload_threshold(c) == 0);
    assert(hanami_config_get_prefetch_concurrency(c) == 4);
    assert(hanami_config_get_verify_on_read(c) == true);

    hanami_config_free(c);
}

static void test_new_null_out(void)
{
    assert(hanami_config_new(NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_free_null(void)
{
    hanami_config_free(NULL); /* must not crash */
}

/* =========================================================================
 * Setters / Getters
 * ========================================================================= */

static void test_set_endpoint(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);

    assert(hanami_config_set_endpoint(c, "https://blossom.example.com") == HANAMI_OK);
    assert(strcmp(hanami_config_get_endpoint(c), "https://blossom.example.com") == 0);

    /* Override */
    assert(hanami_config_set_endpoint(c, "https://other.com") == HANAMI_OK);
    assert(strcmp(hanami_config_get_endpoint(c), "https://other.com") == 0);

    /* Set to NULL */
    assert(hanami_config_set_endpoint(c, NULL) == HANAMI_OK);
    assert(hanami_config_get_endpoint(c) == NULL);

    hanami_config_free(c);
}

static void test_set_relays(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);

    const char *relays[] = { "wss://relay1.com", "wss://relay2.com" };
    assert(hanami_config_set_relays(c, relays, 2) == HANAMI_OK);
    assert(hanami_config_get_relay_count(c) == 2);

    const char *const *got = hanami_config_get_relays(c);
    assert(got != NULL);
    assert(strcmp(got[0], "wss://relay1.com") == 0);
    assert(strcmp(got[1], "wss://relay2.com") == 0);

    /* Clear relays */
    assert(hanami_config_set_relays(c, NULL, 0) == HANAMI_OK);
    assert(hanami_config_get_relay_count(c) == 0);

    hanami_config_free(c);
}

static void test_set_cache_dir(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);

    assert(hanami_config_set_cache_dir(c, "/tmp/hanami-cache") == HANAMI_OK);
    assert(strcmp(hanami_config_get_cache_dir(c), "/tmp/hanami-cache") == 0);

    hanami_config_free(c);
}

static void test_set_index_backend(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);

    assert(hanami_config_set_index_backend(c, "sqlite") == HANAMI_OK);
    assert(strcmp(hanami_config_get_index_backend(c), "sqlite") == 0);

    assert(hanami_config_set_index_backend(c, "lmdb") == HANAMI_OK);
    assert(strcmp(hanami_config_get_index_backend(c), "lmdb") == 0);

    /* Invalid backend */
    assert(hanami_config_set_index_backend(c, "redis") == HANAMI_ERR_INVALID_ARG);
    /* Should still be "lmdb" */
    assert(strcmp(hanami_config_get_index_backend(c), "lmdb") == 0);

    hanami_config_free(c);
}

static void test_set_numeric_options(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);

    hanami_config_set_upload_threshold(c, 1024);
    assert(hanami_config_get_upload_threshold(c) == 1024);

    hanami_config_set_prefetch_concurrency(c, 8);
    assert(hanami_config_get_prefetch_concurrency(c) == 8);

    /* Zero concurrency is ignored */
    hanami_config_set_prefetch_concurrency(c, 0);
    assert(hanami_config_get_prefetch_concurrency(c) == 8);

    hanami_config_set_verify_on_read(c, false);
    assert(hanami_config_get_verify_on_read(c) == false);

    hanami_config_set_verify_on_read(c, true);
    assert(hanami_config_get_verify_on_read(c) == true);

    hanami_config_free(c);
}

/* =========================================================================
 * Null guards for setters
 * ========================================================================= */

static void test_setter_null_config(void)
{
    assert(hanami_config_set_endpoint(NULL, "x") == HANAMI_ERR_INVALID_ARG);
    assert(hanami_config_set_relays(NULL, NULL, 0) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_config_set_cache_dir(NULL, "x") == HANAMI_ERR_INVALID_ARG);
    assert(hanami_config_set_index_backend(NULL, "sqlite") == HANAMI_ERR_INVALID_ARG);

    /* Void setters just silently ignore NULL */
    hanami_config_set_upload_threshold(NULL, 100);
    hanami_config_set_prefetch_concurrency(NULL, 8);
    hanami_config_set_verify_on_read(NULL, false);
}

/* =========================================================================
 * Getters on NULL
 * ========================================================================= */

static void test_getter_null_config(void)
{
    assert(hanami_config_get_endpoint(NULL) == NULL);
    assert(hanami_config_get_relays(NULL) == NULL);
    assert(hanami_config_get_relay_count(NULL) == 0);
    assert(hanami_config_get_cache_dir(NULL) == NULL);
    assert(hanami_config_get_index_backend(NULL) == NULL);
    assert(hanami_config_get_upload_threshold(NULL) == 0);
    assert(hanami_config_get_prefetch_concurrency(NULL) == 4);
    assert(hanami_config_get_verify_on_read(NULL) == true);
}

/* =========================================================================
 * Environment loading
 * ========================================================================= */

static void test_load_env(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);

    /* Set env vars */
    setenv("HANAMI_ENDPOINT", "https://env.blossom.com", 1);
    setenv("HANAMI_RELAYS", "wss://r1.com, wss://r2.com", 1);
    setenv("HANAMI_CACHE_DIR", "/tmp/hanami-env", 1);
    setenv("HANAMI_INDEX_BACKEND", "lmdb", 1);
    setenv("HANAMI_UPLOAD_THRESHOLD", "2048", 1);
    setenv("HANAMI_PREFETCH_CONCURRENCY", "16", 1);
    setenv("HANAMI_VERIFY_ON_READ", "false", 1);

    assert(hanami_config_load_env(c) == HANAMI_OK);

    assert(strcmp(hanami_config_get_endpoint(c), "https://env.blossom.com") == 0);
    assert(hanami_config_get_relay_count(c) == 2);
    const char *const *relays = hanami_config_get_relays(c);
    assert(strcmp(relays[0], "wss://r1.com") == 0);
    assert(strcmp(relays[1], "wss://r2.com") == 0);
    assert(strcmp(hanami_config_get_cache_dir(c), "/tmp/hanami-env") == 0);
    assert(strcmp(hanami_config_get_index_backend(c), "lmdb") == 0);
    assert(hanami_config_get_upload_threshold(c) == 2048);
    assert(hanami_config_get_prefetch_concurrency(c) == 16);
    assert(hanami_config_get_verify_on_read(c) == false);

    /* Clean up env */
    unsetenv("HANAMI_ENDPOINT");
    unsetenv("HANAMI_RELAYS");
    unsetenv("HANAMI_CACHE_DIR");
    unsetenv("HANAMI_INDEX_BACKEND");
    unsetenv("HANAMI_UPLOAD_THRESHOLD");
    unsetenv("HANAMI_PREFETCH_CONCURRENCY");
    unsetenv("HANAMI_VERIFY_ON_READ");

    hanami_config_free(c);
}

static void test_load_env_null(void)
{
    assert(hanami_config_load_env(NULL) == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * Gitconfig loading
 * ========================================================================= */

static void test_load_gitconfig_null_repo(void)
{
    hanami_config_t *c = NULL;
    assert(hanami_config_new(&c) == HANAMI_OK);

    /* Loading from default gitconfig with NULL repo should not crash */
    hanami_error_t err = hanami_config_load_gitconfig(c, NULL);
    assert(err == HANAMI_OK);

    hanami_config_free(c);
}

static void test_load_gitconfig_null_config(void)
{
    assert(hanami_config_load_gitconfig(NULL, NULL) == HANAMI_ERR_INVALID_ARG);
}

/* ---- Main ---- */

int main(void)
{
    git_libgit2_init();

    printf("libhanami configuration tests\n");
    printf("==============================\n");

    /* Lifecycle */
    TEST(new_default);
    TEST(new_null_out);
    TEST(free_null);

    /* Setters / Getters */
    TEST(set_endpoint);
    TEST(set_relays);
    TEST(set_cache_dir);
    TEST(set_index_backend);
    TEST(set_numeric_options);

    /* Null guards */
    TEST(setter_null_config);
    TEST(getter_null_config);

    /* Environment */
    TEST(load_env);
    TEST(load_env_null);

    /* Gitconfig */
    TEST(load_gitconfig_null_repo);
    TEST(load_gitconfig_null_config);

    printf("\n%d passed, 0 failed\n", tests_passed);

    git_libgit2_shutdown();
    return 0;
}
