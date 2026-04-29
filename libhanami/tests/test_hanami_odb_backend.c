/*
 * test_hanami_odb_backend.c - Tests for custom git_odb_backend
 *
 * SPDX-License-Identifier: MIT
 *
 * Tests the ODB backend constructor, vtable wiring, and integration
 * with the OID index. Network-dependent operations (actual Blossom
 * fetches) are tested at the integration level.
 */

#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-index.h"

#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <git2/version.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-55s ", #name); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* Helper: temp dir */
static char tmp_dir[256];

static void setup_tmpdir(void)
{
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hanami_odb_test_XXXXXX");
    assert(mkdtemp(tmp_dir) != NULL);
}

static void rm_rf(const char *path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

/* ---- Constructor tests ---- */

static void test_backend_new_null_args(void)
{
    git_odb_backend *be = NULL;

    assert(hanami_odb_backend_new(NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_odb_backend_new(&be, NULL) == HANAMI_ERR_INVALID_ARG);

    hanami_odb_backend_opts_t opts = {0};
    assert(hanami_odb_backend_new(&be, &opts) == HANAMI_ERR_INVALID_ARG);
    assert(be == NULL);
}

static void test_backend_new_success(void)
{
    setup_tmpdir();

    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    /* Create a client (won't actually connect) */
    hanami_blossom_client_opts_t client_opts = {
        .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 1
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&client_opts, NULL, &client) == HANAMI_OK);

    /* Create backend */
    hanami_odb_backend_opts_t opts = {
        .index = idx,
        .client = client,
        .verify_on_read = true
    };
    git_odb_backend *be = NULL;
    assert(hanami_odb_backend_new(&be, &opts) == HANAMI_OK);
    assert(be != NULL);

    /* Verify vtable is populated */
    assert(be->version == GIT_ODB_BACKEND_VERSION);
    assert(be->read != NULL);
    assert(be->read_header != NULL);
    assert(be->write != NULL);
    assert(be->exists != NULL);
    assert(be->exists_prefix != NULL);
    assert(be->freshen != NULL);
    assert(be->free != NULL);

    /* Free the backend */
    be->free(be);
    hanami_blossom_client_free(client);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

/* ---- exists() with index ---- */

static void test_backend_exists_with_index(void)
{
    setup_tmpdir();

    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_blossom_client_opts_t client_opts = {
        .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 1
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&client_opts, NULL, &client) == HANAMI_OK);

    hanami_odb_backend_opts_t opts = {
        .index = idx,
        .client = client,
        .verify_on_read = false
    };
    git_odb_backend *be = NULL;
    assert(hanami_odb_backend_new(&be, &opts) == HANAMI_OK);

    /* Create a known OID */
    git_oid oid;
    git_oid_fromstr(&oid, "95d09f2b10159347eece71399a7e2e907ea3df4f");

    /* Not in index yet — should not exist */
    assert(be->exists(be, &oid) == 0);

    /* Add to index */
    hanami_index_entry_t entry = {0};
    strcpy(entry.git_oid, "95d09f2b10159347eece71399a7e2e907ea3df4f");
    strcpy(entry.blossom_hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    entry.type = GIT_OBJECT_BLOB;
    entry.size = 11;
    assert(hanami_index_put(idx, &entry) == HANAMI_OK);

    /* Now it should exist */
    assert(be->exists(be, &oid) == 1);

    be->free(be);
    hanami_blossom_client_free(client);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

/* ---- read_header() with index ---- */

static void test_backend_read_header_from_index(void)
{
    setup_tmpdir();

    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_blossom_client_opts_t client_opts = {
        .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 1
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&client_opts, NULL, &client) == HANAMI_OK);

    hanami_odb_backend_opts_t opts = {
        .index = idx,
        .client = client,
        .verify_on_read = false
    };
    git_odb_backend *be = NULL;
    assert(hanami_odb_backend_new(&be, &opts) == HANAMI_OK);

    /* Add entry to index */
    hanami_index_entry_t entry = {0};
    strcpy(entry.git_oid, "95d09f2b10159347eece71399a7e2e907ea3df4f");
    strcpy(entry.blossom_hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    entry.type = GIT_OBJECT_BLOB;
    entry.size = 11;
    assert(hanami_index_put(idx, &entry) == HANAMI_OK);

    /* Read header */
    git_oid oid;
    git_oid_fromstr(&oid, "95d09f2b10159347eece71399a7e2e907ea3df4f");

    size_t len = 0;
    git_object_t type = GIT_OBJECT_INVALID;
    int rc = be->read_header(&len, &type, be, &oid);
    assert(rc == 0);
    assert(len == 11);
    assert(type == GIT_OBJECT_BLOB);

    /* Non-existent OID */
    git_oid missing;
    git_oid_fromstr(&missing, "0000000000000000000000000000000000000000");
    rc = be->read_header(&len, &type, be, &missing);
    assert(rc == GIT_ENOTFOUND);

    be->free(be);
    hanami_blossom_client_free(client);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

/* ---- read() returns GIT_ENOTFOUND for unknown OID ---- */

static void test_backend_read_not_found(void)
{
    setup_tmpdir();

    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_blossom_client_opts_t client_opts = {
        .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 1
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&client_opts, NULL, &client) == HANAMI_OK);

    hanami_odb_backend_opts_t opts = {
        .index = idx,
        .client = client,
        .verify_on_read = false
    };
    git_odb_backend *be = NULL;
    assert(hanami_odb_backend_new(&be, &opts) == HANAMI_OK);

    git_oid oid;
    git_oid_fromstr(&oid, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef");

    void *data = NULL;
    size_t len = 0;
    git_object_t type = GIT_OBJECT_INVALID;
    int rc = be->read(&data, &len, &type, be, &oid);
    assert(rc == GIT_ENOTFOUND);
    assert(data == NULL);

    be->free(be);
    hanami_blossom_client_free(client);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

/* ---- exists_prefix returns ENOTFOUND ---- */

static void test_backend_exists_prefix_not_supported(void)
{
    setup_tmpdir();

    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_blossom_client_opts_t client_opts = {
        .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 1
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&client_opts, NULL, &client) == HANAMI_OK);

    hanami_odb_backend_opts_t opts = {
        .index = idx,
        .client = client,
        .verify_on_read = false
    };
    git_odb_backend *be = NULL;
    assert(hanami_odb_backend_new(&be, &opts) == HANAMI_OK);

    git_oid out, prefix;
    git_oid_fromstr(&prefix, "95d09f2b00000000000000000000000000000000");
    assert(be->exists_prefix(&out, be, &prefix, 8) == GIT_ENOTFOUND);

    be->free(be);
    hanami_blossom_client_free(client);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

/* ---- read() with indexed OID and network error ---- */

static void test_backend_read_indexed_oid_network_error(void)
{
    setup_tmpdir();

    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    /* Point client to nonexistent endpoint — will fail with network error */
    hanami_blossom_client_opts_t client_opts = {
        .endpoint = "http://127.0.0.1:1",
        .timeout_seconds = 1
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&client_opts, NULL, &client) == HANAMI_OK);

    hanami_odb_backend_opts_t opts = {
        .index = idx,
        .client = client,
        .verify_on_read = false
    };
    git_odb_backend *be = NULL;
    assert(hanami_odb_backend_new(&be, &opts) == HANAMI_OK);

    /* Add entry to index with known OID → Blossom hash mapping */
    hanami_index_entry_t entry = {0};
    strcpy(entry.git_oid, "95d09f2b10159347eece71399a7e2e907ea3df4f");
    strcpy(entry.blossom_hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    entry.type = GIT_OBJECT_BLOB;
    entry.size = 11;
    assert(hanami_index_put(idx, &entry) == HANAMI_OK);

    /* Try to read — should exercise the read() path:
     * 1. Look up OID in index ✓
     * 2. Get Blossom hash ✓
     * 3. Attempt HTTP fetch (will fail with network error, not "not found")
     * This exercises the core read path past parameter validation. */
    git_oid oid;
    git_oid_fromstr(&oid, "95d09f2b10159347eece71399a7e2e907ea3df4f");

    void *data = NULL;
    size_t len = 0;
    git_object_t type = GIT_OBJECT_INVALID;
    int rc = be->read(&data, &len, &type, be, &oid);

    /* Should fail (network error), but NOT with GIT_ENOTFOUND.
     * GIT_ENOTFOUND means we didn't find it in the index.
     * Any other error means we found it and tried to fetch. */
    assert(rc != 0); /* Should fail */
    assert(rc != GIT_ENOTFOUND); /* But not "not found" — we DID find it in index */
    assert(data == NULL);

    be->free(be);
    hanami_blossom_client_free(client);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

/* ---- Main ---- */

int main(void)
{
    git_libgit2_init();

    printf("libhanami ODB backend tests\n");
    printf("============================\n");

    TEST(backend_new_null_args);
    TEST(backend_new_success);
    TEST(backend_exists_with_index);
    TEST(backend_read_header_from_index);
    TEST(backend_read_not_found);
    TEST(backend_exists_prefix_not_supported);
    TEST(backend_read_indexed_oid_network_error);

    printf("\n%d passed, 0 failed\n", tests_passed);

    git_libgit2_shutdown();
    return 0;
}
