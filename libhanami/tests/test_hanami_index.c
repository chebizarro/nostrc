/*
 * test_hanami_index.c - Tests for OID ↔ Blossom hash index
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-index.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-50s ", #name); \
        test_##name(); \
        printf("OK\n"); \
        tests_passed++; \
    } while (0)

/* Helper: create a temp directory */
static char tmp_dir[256];

static void setup_tmpdir(void)
{
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hanami_test_XXXXXX");
    assert(mkdtemp(tmp_dir) != NULL);
}

static void rm_rf(const char *path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

/* ---- Hash computation tests ---- */

static void test_hash_blossom(void)
{
    const char *data = "hello world";
    char hex[65];
    hanami_error_t err = hanami_hash_blossom(data, strlen(data), hex);
    assert(err == HANAMI_OK);
    assert(strlen(hex) == 64);
    /* Known SHA-256 of "hello world" */
    assert(strcmp(hex,
        "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9") == 0);
}

static void test_hash_git_sha1(void)
{
    const char *data = "hello world";
    char hex[41];
    hanami_error_t err = hanami_hash_git_sha1(data, strlen(data),
                                              GIT_OBJECT_BLOB, hex);
    assert(err == HANAMI_OK);
    assert(strlen(hex) == 40);
    /* git hash-object: echo -n "hello world" | git hash-object --stdin */
    /* = 95d09f2b10159347eece71399a7e2e907ea3df4f */
    assert(strcmp(hex,
        "95d09f2b10159347eece71399a7e2e907ea3df4f") == 0);
}

static void test_hash_git_sha256(void)
{
    const char *data = "hello world";
    char hex[65];
    hanami_error_t err = hanami_hash_git_sha256(data, strlen(data),
                                                GIT_OBJECT_BLOB, hex);
    assert(err == HANAMI_OK);
    assert(strlen(hex) == 64);
    /* SHA-256 of "blob 11\0hello world" — different from raw content hash */
    /* Verify it's different from Blossom hash */
    char blossom_hex[65];
    hanami_hash_blossom(data, strlen(data), blossom_hex);
    assert(strcmp(hex, blossom_hex) != 0);
}

static void test_hash_null_args(void)
{
    char hex[65];
    assert(hanami_hash_blossom(NULL, 0, hex) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_hash_blossom("x", 1, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_hash_git_sha1(NULL, 0, GIT_OBJECT_BLOB, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_hash_git_sha256(NULL, 0, GIT_OBJECT_BLOB, NULL) == HANAMI_ERR_INVALID_ARG);
}

/* ---- Index CRUD tests ---- */

static void test_index_open_close(void)
{
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    hanami_error_t err = hanami_index_open(&idx, tmp_dir, "sqlite");
    assert(err == HANAMI_OK);
    assert(idx != NULL);
    assert(hanami_index_count(idx) == 0);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

static void test_index_open_null_defaults(void)
{
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    /* NULL backend should default to sqlite */
    hanami_error_t err = hanami_index_open(&idx, tmp_dir, NULL);
    assert(err == HANAMI_OK);
    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

static void test_index_put_get_roundtrip(void)
{
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_index_entry_t entry = {0};
    strcpy(entry.git_oid, "95d09f2b10159347eece71399a7e2e907ea3df4f");
    strcpy(entry.blossom_hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    entry.type = GIT_OBJECT_BLOB;
    entry.size = 11;
    entry.timestamp = 1700000000;

    assert(hanami_index_put(idx, &entry) == HANAMI_OK);
    assert(hanami_index_count(idx) == 1);

    /* Get by OID */
    hanami_index_entry_t out = {0};
    assert(hanami_index_get_by_oid(idx, entry.git_oid, &out) == HANAMI_OK);
    assert(strcmp(out.git_oid, entry.git_oid) == 0);
    assert(strcmp(out.blossom_hash, entry.blossom_hash) == 0);
    assert(out.type == GIT_OBJECT_BLOB);
    assert(out.size == 11);
    assert(out.timestamp == 1700000000);

    /* Get by Blossom hash */
    memset(&out, 0, sizeof(out));
    assert(hanami_index_get_by_blossom(idx, entry.blossom_hash, &out) == HANAMI_OK);
    assert(strcmp(out.git_oid, entry.git_oid) == 0);

    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

static void test_index_exists(void)
{
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    assert(hanami_index_exists(idx, "nonexistent") == false);

    hanami_index_entry_t entry = {0};
    strcpy(entry.git_oid, "aaaa");
    strcpy(entry.blossom_hash, "bbbb");
    entry.type = GIT_OBJECT_BLOB;
    entry.size = 1;
    assert(hanami_index_put(idx, &entry) == HANAMI_OK);

    assert(hanami_index_exists(idx, "aaaa") == true);
    assert(hanami_index_exists(idx, "cccc") == false);

    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

static void test_index_delete(void)
{
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_index_entry_t entry = {0};
    strcpy(entry.git_oid, "deadbeef");
    strcpy(entry.blossom_hash, "cafebabe");
    entry.type = GIT_OBJECT_TREE;
    entry.size = 100;
    assert(hanami_index_put(idx, &entry) == HANAMI_OK);
    assert(hanami_index_count(idx) == 1);

    assert(hanami_index_delete(idx, "deadbeef") == HANAMI_OK);
    assert(hanami_index_count(idx) == 0);
    assert(hanami_index_exists(idx, "deadbeef") == false);

    /* Delete nonexistent */
    assert(hanami_index_delete(idx, "deadbeef") == HANAMI_ERR_NOT_FOUND);

    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

static void test_index_update(void)
{
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_index_entry_t entry = {0};
    strcpy(entry.git_oid, "abcd1234");
    strcpy(entry.blossom_hash, "1111");
    entry.type = GIT_OBJECT_BLOB;
    entry.size = 10;
    entry.timestamp = 100;
    assert(hanami_index_put(idx, &entry) == HANAMI_OK);

    /* Update the same OID with new blossom hash */
    strcpy(entry.blossom_hash, "2222");
    entry.size = 20;
    entry.timestamp = 200;
    assert(hanami_index_put(idx, &entry) == HANAMI_OK);

    /* Should still have 1 entry (upsert) */
    assert(hanami_index_count(idx) == 1);

    hanami_index_entry_t out = {0};
    assert(hanami_index_get_by_oid(idx, "abcd1234", &out) == HANAMI_OK);
    assert(strcmp(out.blossom_hash, "2222") == 0);
    assert(out.size == 20);

    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

static void test_index_not_found(void)
{
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    hanami_index_entry_t out;
    assert(hanami_index_get_by_oid(idx, "nonexistent", &out) == HANAMI_ERR_NOT_FOUND);
    assert(hanami_index_get_by_blossom(idx, "nonexistent", &out) == HANAMI_ERR_NOT_FOUND);

    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

static void test_index_null_args(void)
{
    assert(hanami_index_open(NULL, "/tmp", NULL) == HANAMI_ERR_INVALID_ARG);

    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, NULL, NULL) == HANAMI_ERR_INVALID_ARG);

    assert(hanami_index_put(NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_index_get_by_oid(NULL, "x", NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_index_get_by_blossom(NULL, "x", NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_index_exists(NULL, "x") == false);
    assert(hanami_index_delete(NULL, "x") == HANAMI_ERR_INVALID_ARG);
    assert(hanami_index_count(NULL) == 0);

    /* Close NULL should not crash */
    hanami_index_close(NULL);
}

static void test_index_sha1_sha256_both_modes(void)
{
    /* Verify that the same content produces different Git SHA-1 vs SHA-256 OIDs
     * and that both can be stored independently in the index */
    setup_tmpdir();
    hanami_index_t *idx = NULL;
    assert(hanami_index_open(&idx, tmp_dir, NULL) == HANAMI_OK);

    const char *data = "test content";
    char sha1_hex[41], sha256_hex[65], blossom_hex[65];

    assert(hanami_hash_git_sha1(data, strlen(data), GIT_OBJECT_BLOB, sha1_hex) == HANAMI_OK);
    assert(hanami_hash_git_sha256(data, strlen(data), GIT_OBJECT_BLOB, sha256_hex) == HANAMI_OK);
    assert(hanami_hash_blossom(data, strlen(data), blossom_hex) == HANAMI_OK);

    /* All three should be different */
    assert(strcmp(sha1_hex, sha256_hex) != 0);
    assert(strcmp(sha256_hex, blossom_hex) != 0);

    /* Store both mappings */
    hanami_index_entry_t e1 = {0};
    strcpy(e1.git_oid, sha1_hex);
    strcpy(e1.blossom_hash, blossom_hex);
    e1.type = GIT_OBJECT_BLOB;
    e1.size = strlen(data);
    assert(hanami_index_put(idx, &e1) == HANAMI_OK);

    hanami_index_entry_t e2 = {0};
    strcpy(e2.git_oid, sha256_hex);
    strcpy(e2.blossom_hash, blossom_hex);
    e2.type = GIT_OBJECT_BLOB;
    e2.size = strlen(data);
    assert(hanami_index_put(idx, &e2) == HANAMI_OK);

    assert(hanami_index_count(idx) == 2);

    /* Both should resolve to same Blossom hash */
    hanami_index_entry_t out1 = {0}, out2 = {0};
    assert(hanami_index_get_by_oid(idx, sha1_hex, &out1) == HANAMI_OK);
    assert(hanami_index_get_by_oid(idx, sha256_hex, &out2) == HANAMI_OK);
    assert(strcmp(out1.blossom_hash, out2.blossom_hash) == 0);

    /* Reverse lookup by blossom hash returns one of them */
    hanami_index_entry_t out3 = {0};
    assert(hanami_index_get_by_blossom(idx, blossom_hex, &out3) == HANAMI_OK);

    hanami_index_close(idx);
    rm_rf(tmp_dir);
}

/* ---- Main ---- */

int main(void)
{
    printf("libhanami OID index tests\n");
    printf("=========================\n");

    /* Hash tests */
    TEST(hash_blossom);
    TEST(hash_git_sha1);
    TEST(hash_git_sha256);
    TEST(hash_null_args);

    /* Index CRUD tests */
    TEST(index_open_close);
    TEST(index_open_null_defaults);
    TEST(index_put_get_roundtrip);
    TEST(index_exists);
    TEST(index_delete);
    TEST(index_update);
    TEST(index_not_found);
    TEST(index_null_args);
    TEST(index_sha1_sha256_both_modes);

    printf("\n%d passed, 0 failed\n", tests_passed);
    return 0;
}
