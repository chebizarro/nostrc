/*
 * libmarmot - In-memory storage backend tests
 *
 * Tests the memory storage implementation via the MarmotStorage vtable.
 */

#include <marmot/marmot.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── Test-only storage helpers ─────────────────────────────────────────── */

typedef struct {
    int save_group_calls;
    int mls_store_calls;
    int save_exporter_secret_calls;
} FailingStorageCtx;

static void test_destroy(void *ctx)
{
    free(ctx);
}

static MarmotError test_save_group_fails(void *ctx, const MarmotGroup *group)
{
    (void)group;
    ((FailingStorageCtx *)ctx)->save_group_calls++;
    return MARMOT_ERR_STORAGE;
}

static MarmotError test_mls_store_ok(void *ctx, const char *label,
                                      const uint8_t *key, size_t key_len,
                                      const uint8_t *value, size_t value_len)
{
    (void)label; (void)key; (void)key_len; (void)value; (void)value_len;
    ((FailingStorageCtx *)ctx)->mls_store_calls++;
    return MARMOT_OK;
}

static MarmotError test_mls_store_fails(void *ctx, const char *label,
                                         const uint8_t *key, size_t key_len,
                                         const uint8_t *value, size_t value_len)
{
    (void)label; (void)key; (void)key_len; (void)value; (void)value_len;
    ((FailingStorageCtx *)ctx)->mls_store_calls++;
    return MARMOT_ERR_STORAGE;
}

static MarmotError test_mls_delete_ok(void *ctx, const char *label,
                                       const uint8_t *key, size_t key_len)
{
    (void)ctx; (void)label; (void)key; (void)key_len;
    return MARMOT_OK;
}

static MarmotError test_save_exporter_secret_ok(void *ctx,
                                                 const MarmotGroupId *gid,
                                                 uint64_t epoch,
                                                 const uint8_t secret[32])
{
    (void)gid; (void)epoch; (void)secret;
    ((FailingStorageCtx *)ctx)->save_exporter_secret_calls++;
    return MARMOT_OK;
}

static MarmotError test_save_key_package_info_ok(void *ctx,
                                                  const MarmotKeyPackageInfo *info)
{
    (void)ctx; (void)info;
    return MARMOT_OK;
}

static MarmotError test_deactivate_key_packages_ok(void *ctx,
                                                    const uint8_t pubkey[32])
{
    (void)ctx; (void)pubkey;
    return MARMOT_OK;
}

static MarmotStorage *make_test_storage(FailingStorageCtx **out_ctx)
{
    FailingStorageCtx *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != NULL);
    MarmotStorage *s = calloc(1, sizeof(*s));
    assert(s != NULL);
    s->ctx = ctx;
    s->destroy = test_destroy;
    if (out_ctx) *out_ctx = ctx;
    return s;
}

/* ── Group CRUD ────────────────────────────────────────────────────────── */

static void test_storage_group_roundtrip(void)
{
    MarmotStorage *s = marmot_storage_memory_new();
    assert(s != NULL);

    /* Save a group */
    MarmotGroup *g = marmot_group_new();
    uint8_t gid_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    g->mls_group_id = marmot_group_id_new(gid_bytes, sizeof(gid_bytes));
    memset(g->nostr_group_id, 0xAA, 32);
    g->name = strdup("Test Group");
    g->description = strdup("Description");
    g->state = MARMOT_GROUP_STATE_ACTIVE;
    g->epoch = 42;

    assert(s->save_group(s->ctx, g) == MARMOT_OK);

    /* Find by MLS group ID */
    MarmotGroup *found = NULL;
    assert(s->find_group_by_mls_id(s->ctx, &g->mls_group_id, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->name, "Test Group") == 0);
    assert(found->epoch == 42);
    assert(found->state == MARMOT_GROUP_STATE_ACTIVE);
    marmot_group_free(found);

    /* Find by Nostr group ID */
    found = NULL;
    assert(s->find_group_by_nostr_id(s->ctx, g->nostr_group_id, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->description, "Description") == 0);
    marmot_group_free(found);

    /* Not found */
    MarmotGroupId bad = marmot_group_id_new((uint8_t *)"nonexistent", 11);
    found = NULL;
    assert(s->find_group_by_mls_id(s->ctx, &bad, &found) == MARMOT_OK);
    assert(found == NULL);
    marmot_group_id_free(&bad);

    /* List all groups */
    MarmotGroup **groups = NULL;
    size_t count = 0;
    assert(s->all_groups(s->ctx, &groups, &count) == MARMOT_OK);
    assert(count == 1);
    assert(strcmp(groups[0]->name, "Test Group") == 0);
    marmot_group_free(groups[0]);
    free(groups);

    marmot_group_free(g);
    marmot_storage_free(s);
}

static void test_storage_group_upsert(void)
{
    MarmotStorage *s = marmot_storage_memory_new();

    MarmotGroup *g = marmot_group_new();
    uint8_t gid[] = {10, 20, 30};
    g->mls_group_id = marmot_group_id_new(gid, sizeof(gid));
    g->name = strdup("Original");
    g->epoch = 1;
    assert(s->save_group(s->ctx, g) == MARMOT_OK);

    /* Update */
    free(g->name);
    g->name = strdup("Updated");
    g->epoch = 5;
    assert(s->save_group(s->ctx, g) == MARMOT_OK);

    /* Verify only one group, with updated name */
    MarmotGroup **all = NULL;
    size_t count = 0;
    assert(s->all_groups(s->ctx, &all, &count) == MARMOT_OK);
    assert(count == 1);
    assert(strcmp(all[0]->name, "Updated") == 0);
    assert(all[0]->epoch == 5);
    marmot_group_free(all[0]);
    free(all);

    marmot_group_free(g);
    marmot_storage_free(s);
}

/* ── Message operations ────────────────────────────────────────────────── */

static void test_storage_messages(void)
{
    MarmotStorage *s = marmot_storage_memory_new();

    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"grp1", 4);

    /* Save messages */
    for (int i = 0; i < 5; i++) {
        MarmotMessage *m = marmot_message_new();
        memset(m->id, (uint8_t)i, 32);
        memset(m->pubkey, 0x11, 32);
        m->mls_group_id = marmot_group_id_new(gid.data, gid.len);
        m->created_at = 1000 + i;
        m->content = malloc(32);
        snprintf(m->content, 32, "Message %d", i);
        assert(s->save_message(s->ctx, m) == MARMOT_OK);
        marmot_message_free(m);
    }

    /* Query messages */
    MarmotPagination pg = marmot_pagination_default();
    MarmotMessage **msgs = NULL;
    size_t count = 0;
    assert(s->messages(s->ctx, &gid, &pg, &msgs, &count) == MARMOT_OK);
    assert(count == 5);
    assert(strcmp(msgs[0]->content, "Message 0") == 0);
    assert(strcmp(msgs[4]->content, "Message 4") == 0);

    for (size_t i = 0; i < count; i++) marmot_message_free(msgs[i]);
    free(msgs);

    /* Pagination: limit=2, offset=1 */
    pg.limit = 2;
    pg.offset = 1;
    assert(s->messages(s->ctx, &gid, &pg, &msgs, &count) == MARMOT_OK);
    assert(count == 2);
    assert(strcmp(msgs[0]->content, "Message 1") == 0);
    assert(strcmp(msgs[1]->content, "Message 2") == 0);

    for (size_t i = 0; i < count; i++) marmot_message_free(msgs[i]);
    free(msgs);

    /* Find by ID */
    uint8_t id2[32];
    memset(id2, 2, 32);
    MarmotMessage *found = NULL;
    assert(s->find_message_by_id(s->ctx, id2, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->content, "Message 2") == 0);
    marmot_message_free(found);

    /* Last message */
    MarmotMessage *last = NULL;
    assert(s->last_message(s->ctx, &gid, MARMOT_SORT_CREATED_AT_FIRST, &last) == MARMOT_OK);
    assert(last != NULL);
    assert(last->created_at == 1004);
    marmot_message_free(last);

    marmot_group_id_free(&gid);
    marmot_storage_free(s);
}

/* ── MLS key store ─────────────────────────────────────────────────────── */

static void test_storage_mls_kv(void)
{
    MarmotStorage *s = marmot_storage_memory_new();

    uint8_t key[] = {1, 2, 3, 4};
    uint8_t value[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

    /* Store */
    assert(s->mls_store(s->ctx, "key_package", key, sizeof(key),
                         value, sizeof(value)) == MARMOT_OK);

    /* Load */
    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(s->mls_load(s->ctx, "key_package", key, sizeof(key),
                        &out, &out_len) == MARMOT_OK);
    assert(out_len == sizeof(value));
    assert(memcmp(out, value, sizeof(value)) == 0);
    free(out);

    /* Load with wrong key → not found */
    uint8_t wrong_key[] = {5, 6, 7, 8};
    assert(s->mls_load(s->ctx, "key_package", wrong_key, sizeof(wrong_key),
                        &out, &out_len) == MARMOT_ERR_STORAGE_NOT_FOUND);

    /* Upsert */
    uint8_t new_value[] = {0xFF};
    assert(s->mls_store(s->ctx, "key_package", key, sizeof(key),
                         new_value, sizeof(new_value)) == MARMOT_OK);
    assert(s->mls_load(s->ctx, "key_package", key, sizeof(key),
                        &out, &out_len) == MARMOT_OK);
    assert(out_len == 1 && out[0] == 0xFF);
    free(out);

    /* Delete */
    assert(s->mls_delete(s->ctx, "key_package", key, sizeof(key)) == MARMOT_OK);
    assert(s->mls_load(s->ctx, "key_package", key, sizeof(key),
                        &out, &out_len) == MARMOT_ERR_STORAGE_NOT_FOUND);

    marmot_storage_free(s);
}

/* ── Exporter secret ───────────────────────────────────────────────────── */

static void test_storage_exporter_secret(void)
{
    MarmotStorage *s = marmot_storage_memory_new();

    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"group", 5);
    uint8_t secret[32];
    memset(secret, 0x77, 32);

    assert(s->save_exporter_secret(s->ctx, &gid, 3, secret) == MARMOT_OK);

    uint8_t out[32];
    assert(s->get_exporter_secret(s->ctx, &gid, 3, out) == MARMOT_OK);
    assert(memcmp(out, secret, 32) == 0);

    /* Wrong epoch */
    assert(s->get_exporter_secret(s->ctx, &gid, 99, out) == MARMOT_ERR_STORAGE_NOT_FOUND);

    marmot_group_id_free(&gid);
    marmot_storage_free(s);
}

/* ── Snapshot support ─────────────────────────────────────────────────── */

static void test_storage_memory_snapshots_unsupported(void)
{
    MarmotStorage *s = marmot_storage_memory_new();
    assert(s != NULL);

    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"snap", 4);
    size_t pruned = 123;

    assert(s->create_snapshot(s->ctx, &gid, "before") == MARMOT_ERR_UNSUPPORTED);
    assert(s->rollback_snapshot(s->ctx, &gid, "before") == MARMOT_ERR_UNSUPPORTED);
    assert(s->release_snapshot(s->ctx, &gid, "before") == MARMOT_ERR_UNSUPPORTED);
    assert(s->prune_expired_snapshots(s->ctx, 0, &pruned) == MARMOT_ERR_UNSUPPORTED);
    assert(pruned == 0);

    marmot_group_id_free(&gid);
    marmot_storage_free(s);
}

/* ── Public API storage-failure behavior ───────────────────────────────── */

static void test_public_apis_reject_missing_storage_methods(void)
{
    MarmotStorage *s = make_test_storage(NULL);
    Marmot *m = marmot_new(s);
    assert(m != NULL);

    MarmotWelcome **welcomes = NULL;
    size_t count = 0;
    assert(marmot_get_pending_welcomes(m, NULL, &welcomes, &count) == MARMOT_ERR_STORAGE);

    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"gid", 3);
    MarmotGroup *group = NULL;
    assert(marmot_get_group(m, &gid, &group) == MARMOT_ERR_STORAGE);

    MarmotGroup **groups = NULL;
    assert(marmot_get_all_groups(m, &groups, &count) == MARMOT_ERR_STORAGE);

    MarmotMessage **messages = NULL;
    assert(marmot_get_messages(m, &gid, NULL, &messages, &count) == MARMOT_ERR_STORAGE);

    const uint8_t data[] = { 1, 2, 3 };
    MarmotEncryptedMedia media;
    assert(marmot_encrypt_media(m, &gid, data, sizeof(data), "application/octet-stream",
                                 NULL, &media) == MARMOT_ERR_STORAGE);

    uint8_t enc[16] = { 0 };
    MarmotImetaInfo imeta;
    memset(&imeta, 0, sizeof(imeta));
    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(marmot_decrypt_media(m, &gid, enc, sizeof(enc), &imeta,
                                 &out, &out_len) == MARMOT_ERR_STORAGE);

    marmot_group_id_free(&gid);
    marmot_free(m);
}

static void test_create_group_reports_save_group_failure(void)
{
    FailingStorageCtx *ctx = NULL;
    MarmotStorage *s = make_test_storage(&ctx);
    s->mls_store = test_mls_store_ok;
    s->mls_delete = test_mls_delete_ok;
    s->save_exporter_secret = test_save_exporter_secret_ok;
    s->save_group = test_save_group_fails;

    Marmot *m = marmot_new(s);
    assert(m != NULL);

    uint8_t creator_pk[32];
    memset(creator_pk, 0x42, sizeof(creator_pk));
    MarmotGroupConfig config;
    memset(&config, 0, sizeof(config));
    config.name = "storage failure group";

    MarmotCreateGroupResult result;
    memset(&result, 0, sizeof(result));
    MarmotError err = marmot_create_group(m, creator_pk, NULL, 0, &config, &result);
    assert(err == MARMOT_ERR_STORAGE);
    assert(ctx->mls_store_calls == 1);
    assert(ctx->save_exporter_secret_calls == 1);
    assert(ctx->save_group_calls == 1);
    assert(result.group == NULL);

    marmot_create_group_result_free(&result);
    marmot_free(m);
}

static void test_key_package_reports_private_store_failure(void)
{
    FailingStorageCtx *ctx = NULL;
    MarmotStorage *s = make_test_storage(&ctx);
    s->mls_store = test_mls_store_fails;
    s->mls_delete = test_mls_delete_ok;
    s->save_key_package_info = test_save_key_package_info_ok;
    s->deactivate_key_packages = test_deactivate_key_packages_ok;

    Marmot *m = marmot_new(s);
    assert(m != NULL);

    uint8_t pubkey[32];
    memset(pubkey, 0x24, sizeof(pubkey));
    MarmotKeyPackageResult result;
    memset(&result, 0, sizeof(result));

    MarmotError err = marmot_create_key_package_unsigned(m, pubkey, NULL, 0, &result);
    assert(err == MARMOT_ERR_STORAGE);
    assert(ctx->mls_store_calls == 1);
    assert(result.event_json == NULL);

    marmot_key_package_result_free(&result);
    marmot_free(m);
}

/* ── is_persistent ─────────────────────────────────────────────────────── */

static void test_storage_not_persistent(void)
{
    MarmotStorage *s = marmot_storage_memory_new();
    assert(s->is_persistent(s->ctx) == false);
    marmot_storage_free(s);
}

int main(void)
{
    printf("libmarmot: Storage backend tests\n");
    TEST(test_storage_group_roundtrip);
    TEST(test_storage_group_upsert);
    TEST(test_storage_messages);
    TEST(test_storage_mls_kv);
    TEST(test_storage_exporter_secret);
    TEST(test_storage_memory_snapshots_unsupported);
    TEST(test_public_apis_reject_missing_storage_methods);
    TEST(test_create_group_reports_save_group_failure);
    TEST(test_key_package_reports_private_store_failure);
    TEST(test_storage_not_persistent);
    printf("All storage tests passed.\n");
    return 0;
}
