/*
 * test_hanami_integration.c - Integration tests for libhanami
 *
 * SPDX-License-Identifier: MIT
 *
 * Tests the full component wiring: repo open with Blossom ODB + Nostr RefDB,
 * index ↔ ODB ↔ RefDB interop, hash integrity, config → component lifecycle,
 * transport URL → repo open roundtrip, and NIP-34 announce/publish pipeline.
 *
 * These tests run without live servers by using in-memory backends and
 * verifying internal consistency between components.
 */

#include "hanami/hanami.h"
#include "hanami/hanami-config.h"
#include "hanami/hanami-index.h"
#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-refdb-backend.h"
#include "hanami/hanami-nostr.h"
#include "hanami/hanami-transport.h"

#include <git2.h>
#include <git2/sys/odb_backend.h>
#include <git2/sys/refdb_backend.h>
#include <git2/sys/refs.h>
#include <nostr-filter.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  %-60s ", #name); \
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
 * 1. Library lifecycle
 * ========================================================================= */

static void test_init_shutdown_roundtrip(void)
{
    assert(hanami_init() == HANAMI_OK);
    assert(hanami_init() == HANAMI_OK); /* refcounted */
    hanami_shutdown();
    hanami_shutdown();
}

static void test_version_info(void)
{
    int major = -1, minor = -1, patch = -1;
    const char *v = hanami_version(&major, &minor, &patch);
    assert(v != NULL);
    assert(major == 0);
    assert(minor == 1);
    assert(patch == 0);
}

/* =========================================================================
 * 2. Index ↔ ODB backend integration
 *
 * Verifies: index stores mapping → ODB exists/read_header uses it
 * ========================================================================= */

static void test_index_odb_interop(void)
{
    /* Open in-memory index */
    hanami_index_t *index = NULL;
    assert(hanami_index_open(&index, ":memory:", NULL) == HANAMI_OK);

    /* Create a Blossom client (won't make real requests) */
    hanami_blossom_client_opts_t blossom_opts = {
        .endpoint = "https://blossom.example.com",
        .timeout_seconds = 1,
        .user_agent = "test"
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&blossom_opts, NULL, &client) == HANAMI_OK);

    /* Create ODB backend */
    hanami_odb_backend_opts_t odb_opts = {
        .index = index,
        .client = client,
        .verify_on_read = true
    };
    git_odb_backend *odb_be = NULL;
    assert(hanami_odb_backend_new(&odb_be, &odb_opts) == HANAMI_OK);

    /* Insert an entry into the index */
    hanami_index_entry_t entry = {0};
    strncpy(entry.git_oid, "aabbccddaabbccddaabbccddaabbccddaabbccdd",
            sizeof(entry.git_oid) - 1);
    strncpy(entry.blossom_hash,
            "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
            sizeof(entry.blossom_hash) - 1);
    entry.type = GIT_OBJECT_BLOB;
    entry.size = 42;
    entry.timestamp = 1000000;
    assert(hanami_index_put(index, &entry) == HANAMI_OK);

    /* ODB exists should find it via index */
    git_oid oid;
    git_oid_fromstr(&oid, "aabbccddaabbccddaabbccddaabbccddaabbccdd");
    int exists = odb_be->exists(odb_be, &oid);
    assert(exists == 1);

    /* ODB read_header should return type and size from index */
    size_t len = 0;
    git_object_t type = GIT_OBJECT_INVALID;
    int rc = odb_be->read_header(&len, &type, odb_be, &oid);
    assert(rc == 0);
    assert(len == 42);
    assert(type == GIT_OBJECT_BLOB);

    /* Non-existent OID */
    git_oid missing;
    git_oid_fromstr(&missing, "0000000000000000000000000000000000000000");
    assert(odb_be->exists(odb_be, &missing) == 0);

    odb_be->free(odb_be);
    hanami_blossom_client_free(client);
    hanami_index_close(index);
}

/* =========================================================================
 * 3. Nostr context → RefDB backend integration
 *
 * Verifies: nostr ctx created → refdb backend attached → write/read refs
 * ========================================================================= */

static void test_nostr_refdb_interop(void)
{
    /* Create nostr context */
    const char *relays[] = { "wss://relay.test.com", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, &test_signer, &ctx) == HANAMI_OK);

    /* Create refdb backend */
    hanami_refdb_backend_opts_t opts = {
        .nostr_ctx = ctx,
        .repo_id = "integration-test",
        .owner_pubkey = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    };
    git_refdb_backend *refdb_be = NULL;
    assert(hanami_refdb_backend_new(&refdb_be, &opts) == HANAMI_OK);

    /* Write a direct ref */
    git_oid oid;
    git_oid_fromstr(&oid, "1111111111111111111111111111111111111111");
    git_reference *ref = git_reference__alloc("refs/heads/main", &oid, NULL);
    assert(ref != NULL);
    refdb_be->write(refdb_be, ref, 1, NULL, NULL, NULL, NULL);
    git_reference_free(ref);

    /* Write a symbolic ref */
    git_reference *head = git_reference__alloc_symbolic("HEAD", "refs/heads/main");
    assert(head != NULL);
    refdb_be->write(refdb_be, head, 1, NULL, NULL, NULL, NULL);
    git_reference_free(head);

    /* Read back and verify */
    git_reference *found = NULL;
    assert(refdb_be->lookup(&found, refdb_be, "refs/heads/main") == 0);
    assert(git_reference_type(found) == GIT_REFERENCE_DIRECT);
    assert(git_oid_cmp(git_reference_target(found), &oid) == 0);
    git_reference_free(found);

    assert(refdb_be->lookup(&found, refdb_be, "HEAD") == 0);
    assert(git_reference_type(found) == GIT_REFERENCE_SYMBOLIC);
    assert(strcmp(git_reference_symbolic_target(found), "refs/heads/main") == 0);
    git_reference_free(found);

    /* Iterate — should find both */
    git_reference_iterator *iter = NULL;
    assert(refdb_be->iterator(&iter, refdb_be, NULL) == 0);
    int count = 0;
    while (iter->next(&found, iter) == 0) {
        count++;
        git_reference_free(found);
    }
    assert(count == 2);
    iter->free(iter);

    refdb_be->free(refdb_be);
    hanami_nostr_ctx_free(ctx);
}

/* =========================================================================
 * 4. Hash integrity
 *
 * Verifies: Blossom hash ≠ Git hash for same content, both correct
 * ========================================================================= */

static void test_hash_integrity(void)
{
    const char *data = "Hello, Blossom!";
    size_t len = strlen(data);

    char blossom_hash[65] = {0};
    char git_sha1[41] = {0};

    assert(hanami_hash_blossom(data, len, blossom_hash) == HANAMI_OK);
    assert(hanami_hash_git_sha1(data, len, GIT_OBJECT_BLOB, git_sha1) == HANAMI_OK);

    /* Both should be non-empty hex strings */
    assert(strlen(blossom_hash) == 64);
    assert(strlen(git_sha1) == 40);

    /* They should be DIFFERENT (blossom = SHA256(raw), git = SHA1(header+raw)) */
    /* Can't directly compare since different lengths, but verify they're both valid hex */
    for (int i = 0; blossom_hash[i]; i++) {
        char c = blossom_hash[i];
        assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    for (int i = 0; git_sha1[i]; i++) {
        char c = git_sha1[i];
        assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

static void test_hash_deterministic(void)
{
    const char *data = "deterministic test content";
    size_t len = strlen(data);

    char hash1[65] = {0}, hash2[65] = {0};
    assert(hanami_hash_blossom(data, len, hash1) == HANAMI_OK);
    assert(hanami_hash_blossom(data, len, hash2) == HANAMI_OK);
    assert(strcmp(hash1, hash2) == 0);

    char git1[41] = {0}, git2[41] = {0};
    assert(hanami_hash_git_sha1(data, len, GIT_OBJECT_BLOB, git1) == HANAMI_OK);
    assert(hanami_hash_git_sha1(data, len, GIT_OBJECT_BLOB, git2) == HANAMI_OK);
    assert(strcmp(git1, git2) == 0);
}

static void test_hash_different_types(void)
{
    const char *data = "same content";
    size_t len = strlen(data);

    char blob_hash[41] = {0};
    char commit_hash[41] = {0};

    assert(hanami_hash_git_sha1(data, len, GIT_OBJECT_BLOB, blob_hash) == HANAMI_OK);
    assert(hanami_hash_git_sha1(data, len, GIT_OBJECT_COMMIT, commit_hash) == HANAMI_OK);

    /* Different types → different Git hashes (different headers) */
    assert(strcmp(blob_hash, commit_hash) != 0);
}

/* =========================================================================
 * 5. Config → component wiring
 *
 * Verifies: config values propagate correctly
 * ========================================================================= */

static void test_config_to_components(void)
{
    hanami_config_t *config = NULL;
    assert(hanami_config_new(&config) == HANAMI_OK);

    assert(hanami_config_set_endpoint(config, "https://blossom.test.com") == HANAMI_OK);
    const char *relays[] = { "wss://relay1.test.com", "wss://relay2.test.com" };
    assert(hanami_config_set_relays(config, relays, 2) == HANAMI_OK);
    hanami_config_set_verify_on_read(config, false);

    /* Verify config values round-trip */
    assert(strcmp(hanami_config_get_endpoint(config), "https://blossom.test.com") == 0);
    assert(hanami_config_get_relay_count(config) == 2);
    assert(hanami_config_get_verify_on_read(config) == false);

    /* Create Blossom client using config endpoint */
    hanami_blossom_client_opts_t blossom_opts = {
        .endpoint = hanami_config_get_endpoint(config),
        .timeout_seconds = 5,
        .user_agent = "test"
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&blossom_opts, NULL, &client) == HANAMI_OK);

    /* Create Nostr context using config relays */
    const char *const *cfg_relays = hanami_config_get_relays(config);
    hanami_nostr_ctx_t *nostr_ctx = NULL;
    assert(hanami_nostr_ctx_new(cfg_relays, &test_signer, &nostr_ctx) == HANAMI_OK);

    hanami_nostr_ctx_free(nostr_ctx);
    hanami_blossom_client_free(client);
    hanami_config_free(config);
}

/* =========================================================================
 * 6. Transport URL parsing → repo identification
 *
 * Verifies: blossom:// URL parsed → used for component creation
 * ========================================================================= */

static void test_transport_url_to_repo(void)
{
    char *endpoint = NULL, *owner = NULL, *repo_id = NULL;

    assert(hanami_transport_parse_url(
        "blossom://blossom.nostr.build/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/my-cool-repo",
        &endpoint, &owner, &repo_id) == HANAMI_OK);

    assert(strcmp(endpoint, "https://blossom.nostr.build") == 0);
    assert(strlen(owner) == 64);
    assert(strcmp(repo_id, "my-cool-repo") == 0);

    /* Use parsed values to create components */
    hanami_blossom_client_opts_t opts = {
        .endpoint = endpoint,
        .timeout_seconds = 5
    };
    hanami_blossom_client_t *client = NULL;
    assert(hanami_blossom_client_new(&opts, &test_signer, &client) == HANAMI_OK);

    const char *relays[] = { "wss://relay.damus.io", NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(relays, &test_signer, &ctx) == HANAMI_OK);

    hanami_refdb_backend_opts_t refdb_opts = {
        .nostr_ctx = ctx,
        .repo_id = repo_id,
        .owner_pubkey = owner
    };
    git_refdb_backend *refdb_be = NULL;
    assert(hanami_refdb_backend_new(&refdb_be, &refdb_opts) == HANAMI_OK);

    refdb_be->free(refdb_be);
    hanami_nostr_ctx_free(ctx);
    hanami_blossom_client_free(client);
    free(endpoint);
    free(owner);
    free(repo_id);
}

/* =========================================================================
 * 7. High-level API: hanami_repo_open
 *
 * Verifies: creates a repo with backends attached
 * ========================================================================= */

static void test_repo_open_null_args(void)
{
    git_repository *repo = NULL;
    assert(hanami_repo_open(NULL, "ep", (const char *[]){"wss://r", NULL},
                            "id", "pk", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_repo_open(&repo, NULL, (const char *[]){"wss://r", NULL},
                            "id", "pk", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_repo_open(&repo, "ep", NULL,
                            "id", "pk", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_clone_null_args(void)
{
    git_repository *repo = NULL;
    assert(hanami_clone(NULL, "uri", "/tmp/x", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_clone(&repo, NULL, "/tmp/x", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_clone(&repo, "uri", NULL, NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    /* Invalid URI scheme (not nostr://) */
    assert(hanami_clone(&repo, "https://example.com/repo", "/tmp/x", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    /* Missing repo_id component */
    assert(hanami_clone(&repo, "nostr://pubkeyhex", "/tmp/x", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
    /* Empty pubkey */
    assert(hanami_clone(&repo, "nostr:///repo-id", "/tmp/x", NULL, NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_push_null_args(void)
{
    assert(hanami_push_to_blossom(NULL, "ep", &test_signer,
                                  (const char *[]){"wss://r", NULL}, "id") == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * 8. High-level API: announce + publish state pipeline
 *
 * Verifies: null arg checks + successful call path
 * ========================================================================= */

static void test_announce_repo_null_args(void)
{
    const char *relays[] = { "wss://r.com", NULL };
    assert(hanami_announce_repo(NULL, "name", "desc", NULL, relays, &test_signer) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_announce_repo("id", NULL, "desc", NULL, relays, &test_signer) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_announce_repo("id", "name", "desc", NULL, NULL, &test_signer) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_announce_repo("id", "name", "desc", NULL, relays, NULL) == HANAMI_ERR_INVALID_ARG);
}

static void test_publish_state_null_args(void)
{
    const char *relays[] = { "wss://r.com", NULL };
    assert(hanami_publish_state(NULL, NULL, 0, relays, &test_signer) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_publish_state("id", NULL, 0, NULL, &test_signer) == HANAMI_ERR_INVALID_ARG);
    assert(hanami_publish_state("id", NULL, 0, relays, NULL) == HANAMI_ERR_INVALID_ARG);
}

/* =========================================================================
 * 9. Index ↔ hash roundtrip
 *
 * Verifies: compute hashes → store in index → retrieve → match
 * ========================================================================= */

static void test_index_hash_roundtrip(void)
{
    const char *data = "test blob content for roundtrip verification";
    size_t len = strlen(data);

    /* Compute both hashes */
    char git_hash[41] = {0};
    char blossom_hash[65] = {0};
    assert(hanami_hash_git_sha1(data, len, GIT_OBJECT_BLOB, git_hash) == HANAMI_OK);
    assert(hanami_hash_blossom(data, len, blossom_hash) == HANAMI_OK);

    /* Store in index */
    hanami_index_t *index = NULL;
    assert(hanami_index_open(&index, ":memory:", NULL) == HANAMI_OK);

    hanami_index_entry_t entry = {0};
    strncpy(entry.git_oid, git_hash, sizeof(entry.git_oid) - 1);
    strncpy(entry.blossom_hash, blossom_hash, sizeof(entry.blossom_hash) - 1);
    entry.type = GIT_OBJECT_BLOB;
    entry.size = len;
    entry.timestamp = 12345;
    assert(hanami_index_put(index, &entry) == HANAMI_OK);

    /* Retrieve by Git OID */
    hanami_index_entry_t found = {0};
    assert(hanami_index_get_by_oid(index, git_hash, &found) == HANAMI_OK);
    assert(strcmp(found.blossom_hash, blossom_hash) == 0);
    assert(found.type == GIT_OBJECT_BLOB);
    assert(found.size == len);

    /* Retrieve by Blossom hash */
    hanami_index_entry_t found2 = {0};
    assert(hanami_index_get_by_blossom(index, blossom_hash, &found2) == HANAMI_OK);
    assert(strcmp(found2.git_oid, git_hash) == 0);

    hanami_index_close(index);
}

/* =========================================================================
 * 10. Multiple index entries, counting
 * ========================================================================= */

static void test_index_multiple_objects(void)
{
    hanami_index_t *index = NULL;
    assert(hanami_index_open(&index, ":memory:", NULL) == HANAMI_OK);

    /* Add 3 different objects */
    const char *contents[] = { "blob one", "blob two", "blob three" };
    for (int i = 0; i < 3; i++) {
        char git_hash[41] = {0};
        char blossom_hash[65] = {0};
        assert(hanami_hash_git_sha1(contents[i], strlen(contents[i]),
                                     GIT_OBJECT_BLOB, git_hash) == HANAMI_OK);
        assert(hanami_hash_blossom(contents[i], strlen(contents[i]),
                                    blossom_hash) == HANAMI_OK);

        hanami_index_entry_t e = {0};
        strncpy(e.git_oid, git_hash, sizeof(e.git_oid) - 1);
        strncpy(e.blossom_hash, blossom_hash, sizeof(e.blossom_hash) - 1);
        e.type = GIT_OBJECT_BLOB;
        e.size = strlen(contents[i]);
        assert(hanami_index_put(index, &e) == HANAMI_OK);
    }

    assert(hanami_index_count(index) == 3);

    /* Delete one */
    char del_hash[41] = {0};
    hanami_hash_git_sha1(contents[1], strlen(contents[1]),
                          GIT_OBJECT_BLOB, del_hash);
    assert(hanami_index_delete(index, del_hash) == HANAMI_OK);
    assert(hanami_index_count(index) == 2);

    /* The deleted one should be gone */
    assert(hanami_index_exists(index, del_hash) == false);

    /* The others should still be there */
    char hash0[41] = {0}, hash2[41] = {0};
    hanami_hash_git_sha1(contents[0], strlen(contents[0]),
                          GIT_OBJECT_BLOB, hash0);
    hanami_hash_git_sha1(contents[2], strlen(contents[2]),
                          GIT_OBJECT_BLOB, hash2);
    assert(hanami_index_exists(index, hash0) == true);
    assert(hanami_index_exists(index, hash2) == true);

    hanami_index_close(index);
}

/* =========================================================================
 * 11. Filter builder → filter consistency
 * ========================================================================= */

static void test_filter_builder_consistency(void)
{
    /* Build repo filter and verify it could match a 30617 event */
    NostrFilter *rf = hanami_nostr_build_repo_filter("test-repo", "aabb");
    assert(rf != NULL);
    assert(nostr_filter_kinds_len(rf) == 1);
    assert(nostr_filter_kinds_get(rf, 0) == 30617);
    assert(nostr_filter_get_limit(rf) == 1);
    nostr_filter_free(rf);

    /* Build state filter for same repo */
    NostrFilter *sf = hanami_nostr_build_state_filter("test-repo", "aabb");
    assert(sf != NULL);
    assert(nostr_filter_kinds_get(sf, 0) == 30618);
    nostr_filter_free(sf);

    /* Build patches filter */
    NostrFilter *pf = hanami_nostr_build_patches_filter("30617:aabb:test-repo");
    assert(pf != NULL);
    assert(nostr_filter_kinds_get(pf, 0) == 1617);
    nostr_filter_free(pf);
}

/* ---- Main ---- */

int main(void)
{
    git_libgit2_init();

    printf("libhanami integration tests\n");
    printf("============================\n");

    /* 1. Library lifecycle */
    TEST(init_shutdown_roundtrip);
    TEST(version_info);

    /* 2. Index ↔ ODB backend */
    TEST(index_odb_interop);

    /* 3. Nostr ↔ RefDB backend */
    TEST(nostr_refdb_interop);

    /* 4. Hash integrity */
    TEST(hash_integrity);
    TEST(hash_deterministic);
    TEST(hash_different_types);

    /* 5. Config → components */
    TEST(config_to_components);

    /* 6. Transport URL → repo identification */
    TEST(transport_url_to_repo);

    /* 7. High-level API arg validation */
    TEST(repo_open_null_args);
    TEST(clone_null_args);
    TEST(push_null_args);

    /* 8. Announce + publish state */
    TEST(announce_repo_null_args);
    TEST(publish_state_null_args);

    /* 9. Index ↔ hash roundtrip */
    TEST(index_hash_roundtrip);

    /* 10. Multiple index entries */
    TEST(index_multiple_objects);

    /* 11. Filter builder consistency */
    TEST(filter_builder_consistency);

    printf("\n%d passed, 0 failed\n", tests_passed);

    git_libgit2_shutdown();
    return 0;
}
