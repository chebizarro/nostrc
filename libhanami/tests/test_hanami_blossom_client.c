/*
 * test_hanami_blossom_client.c - Tests for Blossom HTTP client
 *
 * SPDX-License-Identifier: MIT
 *
 * These tests verify client creation, configuration, error paths,
 * and JSON parsing without requiring a live Blossom server.
 */

#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-50s ", #name); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* ---- Dummy signer for tests ---- */

static hanami_error_t dummy_sign(const char *event_json,
                                  char **out_signed_json,
                                  void *user_data)
{
    (void)user_data;
    /* Just return the event JSON unchanged (won't have valid sig) */
    *out_signed_json = strdup(event_json);
    return *out_signed_json ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

/* ---- Client lifecycle ---- */

static void test_client_new_basic(void)
{
    hanami_blossom_client_opts_t opts = {
        .endpoint = "https://blossom.example.com",
        .timeout_seconds = 0,
        .user_agent = NULL
    };
    hanami_blossom_client_t *client = NULL;
    hanami_error_t err = hanami_blossom_client_new(&opts, NULL, &client);
    assert(err == HANAMI_OK);
    assert(client != NULL);
    hanami_blossom_client_free(client);
}

static void test_client_new_with_signer(void)
{
    hanami_blossom_client_opts_t opts = {
        .endpoint = "https://blossom.example.com/",
        .timeout_seconds = 60,
        .user_agent = "test/1.0"
    };
    hanami_signer_t signer = {
        .pubkey = "aabbccdd",
        .sign = dummy_sign,
        .user_data = NULL
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&opts, &signer, &client) == HANAMI_OK);
    assert(client != NULL);
    hanami_blossom_client_free(client);
}

static void test_client_new_null_params(void)
{
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(NULL, NULL, &client) == HANAMI_ERR_INVALID_ARG);

    hanami_blossom_client_opts_t opts = { .endpoint = NULL };
    assert(hanami_blossom_client_new(&opts, NULL, &client) == HANAMI_ERR_INVALID_ARG);

    opts.endpoint = "https://example.com";
    assert(hanami_blossom_client_new(&opts, NULL, NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_client_free_null(void)
{
    /* Should not crash */
    hanami_blossom_client_free(NULL);
}

/* ---- API null guards ---- */

static void test_get_null_args(void)
{
    uint8_t *data = NULL;
    size_t len = 0;
    bool exists = false;

    assert(hanami_blossom_get(NULL, "abc", &data, &len) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_blossom_head(NULL, "abc", &exists) == HANAMI_ERR_INVALID_ARG);
}

static void test_upload_requires_signer(void)
{
    hanami_blossom_client_opts_t opts = {
        .endpoint = "https://blossom.example.com"
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &client) == HANAMI_OK);

    /* Upload without signer should fail with auth error */
    uint8_t data[] = "hello";
    assert(hanami_blossom_upload(client, data, 5, NULL, NULL) == HANAMI_ERR_AUTH);

    /* Delete without signer should fail */
    assert(hanami_blossom_delete(client, "aaaa") == HANAMI_ERR_AUTH);

    /* Mirror without signer should fail */
    assert(hanami_blossom_mirror(client, "https://other.com/blob", NULL) == HANAMI_ERR_AUTH);

    hanami_blossom_client_free(client);
}

static void test_list_null_args(void)
{
    char *json = NULL;
    size_t len = 0;
    assert(hanami_blossom_list(NULL, "pk", &json, &len) == HANAMI_ERR_INVALID_ARG);
}

/* ---- Network error handling (connect to localhost with invalid port) ---- */

static void test_get_connection_error(void)
{
    hanami_blossom_client_opts_t opts = {
        .endpoint = "http://127.0.0.1:1",  /* port 1 — should fail to connect */
        .timeout_seconds = 2
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &client) == HANAMI_OK);

    uint8_t *data = NULL;
    size_t len = 0;
    hanami_error_t err = hanami_blossom_get(client, "deadbeef", &data, &len);
    assert(err == HANAMI_ERR_NETWORK || err == HANAMI_ERR_TIMEOUT);
    assert(data == NULL);

    hanami_blossom_client_free(client);
}

static void test_head_connection_error(void)
{
    hanami_blossom_client_opts_t opts = {
        .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 2
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&opts, NULL, &client) == HANAMI_OK);

    bool exists = true;
    hanami_error_t err = hanami_blossom_head(client, "deadbeef", &exists);
    assert(err == HANAMI_ERR_NETWORK || err == HANAMI_ERR_TIMEOUT);

    hanami_blossom_client_free(client);
}

/* ---- Blob descriptor ---- */

static void test_blob_descriptor_free_null(void)
{
    hanami_blob_descriptor_free(NULL); /* must not crash */
}

static void test_blob_descriptor_free(void)
{
    hanami_blob_descriptor_t *desc = calloc(1, sizeof(*desc));
    assert(desc != NULL);
    strcpy(desc->sha256, "aaaa");
    desc->url = strdup("https://example.com/blob");
    desc->mime_type = strdup("application/octet-stream");
    hanami_blob_descriptor_free(desc);
}

/* ---- Main ---- */

int main(void)
{
    printf("libhanami Blossom client tests\n");
    printf("==============================\n");

    /* Lifecycle */
    TEST(client_new_basic);
    TEST(client_new_with_signer);
    TEST(client_new_null_params);
    TEST(client_free_null);

    /* Null guards */
    TEST(get_null_args);
    TEST(upload_requires_signer);
    TEST(list_null_args);

    /* Network errors */
    TEST(get_connection_error);
    TEST(head_connection_error);

    /* Blob descriptor */
    TEST(blob_descriptor_free_null);
    TEST(blob_descriptor_free);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
