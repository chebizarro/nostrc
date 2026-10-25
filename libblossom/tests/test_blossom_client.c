/*
 * test_blossom_client.c - Tests for Blossom HTTP client
 *
 * SPDX-License-Identifier: MIT
 *
 * These tests verify client creation, configuration, error paths,
 * and JSON parsing without requiring a live Blossom server.
 */

#include "blossom/client.h"
#include <blossom/types.h>

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

static blossom_error_t dummy_sign(const char *event_json,
                                  char **out_signed_json,
                                  void *user_data)
{
    (void)user_data;
    /* Just return the event JSON unchanged (won't have valid sig) */
    *out_signed_json = strdup(event_json);
    return *out_signed_json ? BLOSSOM_OK : BLOSSOM_ERR_NOMEM;
}

/* ---- Client lifecycle ---- */

static void test_client_new_basic(void)
{
    blossom_client_opts_t opts = {
        .endpoint = "https://blossom.example.com",
        .timeout_seconds = 0,
        .user_agent = NULL
    };
    blossom_client_t *client = NULL;
    blossom_error_t err = blossom_client_new(&opts, NULL, &client);
    assert(err == BLOSSOM_OK);
    assert(client != NULL);
    blossom_client_free(client);
}

static void test_client_new_with_signer(void)
{
    blossom_client_opts_t opts = {
        .endpoint = "https://blossom.example.com/",
        .timeout_seconds = 60,
        .user_agent = "test/1.0"
    };
    blossom_signer_t signer = {
        .pubkey = "aabbccdd",
        .sign = dummy_sign,
        .user_data = NULL
    };
    blossom_client_t *client = NULL;
    assert(blossom_client_new(&opts, &signer, &client) == BLOSSOM_OK);
    assert(client != NULL);
    blossom_client_free(client);
}

static void test_client_new_null_params(void)
{
    blossom_client_t *client = NULL;
    assert(blossom_client_new(NULL, NULL, &client) == BLOSSOM_ERR_INVALID_ARG);

    blossom_client_opts_t opts = { .endpoint = NULL };
    assert(blossom_client_new(&opts, NULL, &client) == BLOSSOM_ERR_INVALID_ARG);

    opts.endpoint = "https://example.com";
    assert(blossom_client_new(&opts, NULL, NULL) == BLOSSOM_ERR_INVALID_ARG);
}

static void test_client_free_null(void)
{
    /* Should not crash */
    blossom_client_free(NULL);
}

/* ---- API null guards ---- */

static void test_get_null_args(void)
{
    uint8_t *data = NULL;
    size_t len = 0;
    bool exists = false;

    assert(blossom_client_get(NULL, "abc", &data, &len) == BLOSSOM_ERR_INVALID_ARG);
    assert(blossom_client_head(NULL, "abc", &exists) == BLOSSOM_ERR_INVALID_ARG);
}

static void test_upload_requires_signer(void)
{
    blossom_client_opts_t opts = {
        .endpoint = "https://blossom.example.com"
    };
    blossom_client_t *client = NULL;
    assert(blossom_client_new(&opts, NULL, &client) == BLOSSOM_OK);

    /* Upload without signer should fail with auth error */
    uint8_t data[] = "hello";
    assert(blossom_client_put(client, data, 5, NULL, NULL) == BLOSSOM_ERR_AUTH);

    /* Delete without signer should fail */
    assert(blossom_client_delete(client, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == BLOSSOM_ERR_AUTH);

    /* Mirror without signer should fail */
    assert(blossom_client_mirror(client, "https://other.com/blob", NULL) == BLOSSOM_ERR_AUTH);

    blossom_client_free(client);
}

static void test_list_null_args(void)
{
    char *json = NULL;
    size_t len = 0;
    assert(blossom_client_list(NULL, "pk", &json, &len) == BLOSSOM_ERR_INVALID_ARG);
}

/* ---- Network error handling (connect to localhost with invalid port) ---- */

static void test_get_connection_error(void)
{
    blossom_client_opts_t opts = {
        .allow_insecure = true, .endpoint = "http://127.0.0.1:1",  /* port 1 — should fail to connect */
        .timeout_seconds = 2
    };
    blossom_client_t *client = NULL;
    assert(blossom_client_new(&opts, NULL, &client) == BLOSSOM_OK);

    uint8_t *data = NULL;
    size_t len = 0;
    blossom_error_t err = blossom_client_get(client, "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", &data, &len);
    assert(err == BLOSSOM_ERR_NETWORK || err == BLOSSOM_ERR_TIMEOUT);
    assert(data == NULL);

    blossom_client_free(client);
}

static void test_head_connection_error(void)
{
    blossom_client_opts_t opts = {
        .allow_insecure = true, .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 2
    };
    blossom_client_t *client = NULL;
    assert(blossom_client_new(&opts, NULL, &client) == BLOSSOM_OK);

    bool exists = true;
    blossom_error_t err = blossom_client_head(client, "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", &exists);
    assert(err == BLOSSOM_ERR_NETWORK || err == BLOSSOM_ERR_TIMEOUT);

    blossom_client_free(client);
}

/* ---- Blob descriptor ---- */

static void test_blob_descriptor_free_null(void)
{
    blossom_blob_descriptor_free(NULL); /* must not crash */
}

static void test_blob_descriptor_free(void)
{
    blossom_blob_descriptor_t *desc = calloc(1, sizeof(*desc));
    assert(desc != NULL);
    strcpy(desc->sha256, "aaaa");
    desc->url = strdup("https://example.com/blob");
    desc->mime_type = strdup("application/octet-stream");
    blossom_blob_descriptor_free(desc);
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
