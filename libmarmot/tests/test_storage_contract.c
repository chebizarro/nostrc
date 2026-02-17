/*
 * libmarmot - Storage contract tests
 *
 * Runs the same set of contract tests against every available storage
 * backend to verify they all satisfy the MarmotStorage interface contract.
 *
 * Backends tested:
 *   1. memory      — always available
 *   2. sqlite      — if MARMOT_HAVE_SQLITE is defined
 *   3. nostrdb     — if marmot_storage_nostrdb_new() returns non-NULL
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST(fn, backend_name, storage) do { \
    printf("  [%-8s] %-45s", backend_name, #fn); \
    fn(storage); \
    printf("PASS\n"); \
} while(0)

/* ──────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────── */

static char tmp_dir[256];

static void
make_tmp_dir(void)
{
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/marmot_test_XXXXXX");
    assert(mkdtemp(tmp_dir) != NULL);
}

static void
rm_rf(const char *path)
{
    /* Good enough for test cleanup */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static MarmotGroup *
make_test_group(const uint8_t *gid_data, size_t gid_len,
                const char *name, uint64_t epoch)
{
    MarmotGroup *g = marmot_group_new();
    assert(g != NULL);
    g->mls_group_id = marmot_group_id_new(gid_data, gid_len);
    memset(g->nostr_group_id, 0xBB, 32);
    g->name = strdup(name);
    g->description = strdup("test group description");
    g->state = MARMOT_GROUP_STATE_ACTIVE;
    g->epoch = epoch;
    return g;
}

static MarmotMessage *
make_test_message(const MarmotGroupId *gid, int index, int64_t created_at)
{
    MarmotMessage *m = marmot_message_new();
    assert(m != NULL);
    memset(m->id, (uint8_t)index, 32);
    memset(m->pubkey, 0x11, 32);
    m->kind = MARMOT_KIND_GROUP_MESSAGE;
    m->mls_group_id = marmot_group_id_new(gid->data, gid->len);
    m->created_at = created_at;
    m->processed_at = created_at + 1;
    m->content = malloc(64);
    snprintf(m->content, 64, "Message #%d", index);
    m->epoch = 1;
    m->state = MARMOT_MSG_STATE_CREATED;
    memset(m->wrapper_event_id, (uint8_t)(index + 0x80), 32);
    return m;
}

static MarmotWelcome *
make_test_welcome(const MarmotGroupId *gid, int index)
{
    MarmotWelcome *w = marmot_welcome_new();
    assert(w != NULL);
    memset(w->id, (uint8_t)(index + 0x50), 32);
    w->mls_group_id = marmot_group_id_new(gid->data, gid->len);
    memset(w->nostr_group_id, 0xCC, 32);
    w->group_name = strdup("Welcome Group");
    w->group_description = strdup("Welcome desc");
    w->state = MARMOT_WELCOME_STATE_PENDING;
    w->member_count = 5;
    memset(w->welcomer, 0x22, 32);
    memset(w->wrapper_event_id, (uint8_t)(index + 0xA0), 32);
    w->event_json = strdup("{\"kind\":444}");
    return w;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Contract tests — each receives a MarmotStorage* and exercises it
 * ──────────────────────────────────────────────────────────────────────── */

/* ── 1. Group CRUD ────────────────────────────────────────────────────── */

static void
test_group_save_and_find_by_mls_id(MarmotStorage *s)
{
    uint8_t gid[] = {10, 20, 30, 40};
    MarmotGroup *g = make_test_group(gid, sizeof(gid), "Alpha", 7);
    assert(s->save_group(s->ctx, g) == MARMOT_OK);

    MarmotGroup *found = NULL;
    assert(s->find_group_by_mls_id(s->ctx, &g->mls_group_id, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->name, "Alpha") == 0);
    assert(found->epoch == 7);
    assert(found->state == MARMOT_GROUP_STATE_ACTIVE);
    marmot_group_free(found);
    marmot_group_free(g);
}

static void
test_group_find_by_nostr_id(MarmotStorage *s)
{
    uint8_t gid[] = {11, 21, 31};
    MarmotGroup *g = make_test_group(gid, sizeof(gid), "Beta", 3);
    /* Set a distinctive nostr_group_id */
    memset(g->nostr_group_id, 0xDD, 32);
    assert(s->save_group(s->ctx, g) == MARMOT_OK);

    MarmotGroup *found = NULL;
    assert(s->find_group_by_nostr_id(s->ctx, g->nostr_group_id, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->name, "Beta") == 0);
    marmot_group_free(found);
    marmot_group_free(g);
}

static void
test_group_not_found(MarmotStorage *s)
{
    MarmotGroupId bad = marmot_group_id_new((uint8_t *)"nonexistent!!", 13);
    MarmotGroup *found = NULL;
    MarmotError err = s->find_group_by_mls_id(s->ctx, &bad, &found);
    assert(err == MARMOT_OK);
    assert(found == NULL);
    marmot_group_id_free(&bad);
}

static void
test_group_upsert(MarmotStorage *s)
{
    uint8_t gid[] = {50, 60, 70};
    MarmotGroup *g = make_test_group(gid, sizeof(gid), "Original", 1);
    assert(s->save_group(s->ctx, g) == MARMOT_OK);

    /* Update the group */
    free(g->name);
    g->name = strdup("Updated");
    g->epoch = 99;
    assert(s->save_group(s->ctx, g) == MARMOT_OK);

    MarmotGroup *found = NULL;
    assert(s->find_group_by_mls_id(s->ctx, &g->mls_group_id, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->name, "Updated") == 0);
    assert(found->epoch == 99);
    marmot_group_free(found);
    marmot_group_free(g);
}

static void
test_group_list_all(MarmotStorage *s)
{
    uint8_t gid1[] = {1, 1, 1};
    uint8_t gid2[] = {2, 2, 2};
    MarmotGroup *g1 = make_test_group(gid1, sizeof(gid1), "One", 1);
    MarmotGroup *g2 = make_test_group(gid2, sizeof(gid2), "Two", 2);
    assert(s->save_group(s->ctx, g1) == MARMOT_OK);
    assert(s->save_group(s->ctx, g2) == MARMOT_OK);

    MarmotGroup **groups = NULL;
    size_t count = 0;
    assert(s->all_groups(s->ctx, &groups, &count) == MARMOT_OK);
    /* At least 2 (could be more from prior tests in same backend instance) */
    assert(count >= 2);

    for (size_t i = 0; i < count; i++) marmot_group_free(groups[i]);
    free(groups);

    marmot_group_free(g1);
    marmot_group_free(g2);
}

/* ── 2. Message operations ─────────────────────────────────────────────── */

static void
test_message_save_and_find(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"msg_grp", 7);
    MarmotMessage *m = make_test_message(&gid, 1, 1000);
    assert(s->save_message(s->ctx, m) == MARMOT_OK);

    MarmotMessage *found = NULL;
    assert(s->find_message_by_id(s->ctx, m->id, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->content, "Message #1") == 0);
    assert(found->created_at == 1000);
    marmot_message_free(found);

    marmot_message_free(m);
    marmot_group_id_free(&gid);
}

static void
test_message_pagination(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"page_grp", 8);

    /* Insert 10 messages */
    for (int i = 0; i < 10; i++) {
        MarmotMessage *m = make_test_message(&gid, 100 + i, 2000 + i);
        assert(s->save_message(s->ctx, m) == MARMOT_OK);
        marmot_message_free(m);
    }

    /* Fetch all */
    MarmotPagination pg = marmot_pagination_default();
    MarmotMessage **msgs = NULL;
    size_t count = 0;
    assert(s->messages(s->ctx, &gid, &pg, &msgs, &count) == MARMOT_OK);
    assert(count == 10);
    for (size_t i = 0; i < count; i++) marmot_message_free(msgs[i]);
    free(msgs);

    /* Paginate: limit=3, offset=2 */
    pg.limit = 3;
    pg.offset = 2;
    assert(s->messages(s->ctx, &gid, &pg, &msgs, &count) == MARMOT_OK);
    assert(count == 3);
    for (size_t i = 0; i < count; i++) marmot_message_free(msgs[i]);
    free(msgs);

    marmot_group_id_free(&gid);
}

static void
test_message_last(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"last_grp", 8);

    for (int i = 0; i < 5; i++) {
        MarmotMessage *m = make_test_message(&gid, 200 + i, 3000 + i);
        assert(s->save_message(s->ctx, m) == MARMOT_OK);
        marmot_message_free(m);
    }

    MarmotMessage *last = NULL;
    assert(s->last_message(s->ctx, &gid, MARMOT_SORT_CREATED_AT_FIRST, &last) == MARMOT_OK);
    assert(last != NULL);
    assert(last->created_at == 3004);
    marmot_message_free(last);

    marmot_group_id_free(&gid);
}

static void
test_message_processed_tracking(MarmotStorage *s)
{
    uint8_t wrapper_id[32];
    memset(wrapper_id, 0xF1, 32);

    bool processed = true;
    assert(s->is_message_processed(s->ctx, wrapper_id, &processed) == MARMOT_OK);
    assert(processed == false);

    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"proc_grp", 8);
    uint8_t msg_id[32];
    memset(msg_id, 0xF2, 32);
    assert(s->save_processed_message(s->ctx, wrapper_id, msg_id,
                                      1234567890, 5, &gid,
                                      1 /* PROCESSED */, NULL) == MARMOT_OK);

    assert(s->is_message_processed(s->ctx, wrapper_id, &processed) == MARMOT_OK);
    assert(processed == true);

    marmot_group_id_free(&gid);
}

/* ── 3. Welcome operations ─────────────────────────────────────────────── */

static void
test_welcome_save_and_find(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"wel_grp", 7);
    MarmotWelcome *w = make_test_welcome(&gid, 1);
    assert(s->save_welcome(s->ctx, w) == MARMOT_OK);

    MarmotWelcome *found = NULL;
    assert(s->find_welcome_by_event_id(s->ctx, w->id, &found) == MARMOT_OK);
    assert(found != NULL);
    assert(strcmp(found->group_name, "Welcome Group") == 0);
    assert(found->member_count == 5);
    assert(found->state == MARMOT_WELCOME_STATE_PENDING);
    marmot_welcome_free(found);

    marmot_welcome_free(w);
    marmot_group_id_free(&gid);
}

static void
test_welcome_pending(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"pend_grp", 8);

    /* Insert 3 pending welcomes */
    for (int i = 0; i < 3; i++) {
        MarmotWelcome *w = make_test_welcome(&gid, 30 + i);
        assert(s->save_welcome(s->ctx, w) == MARMOT_OK);
        marmot_welcome_free(w);
    }

    MarmotPagination pg = marmot_pagination_default();
    MarmotWelcome **welcomes = NULL;
    size_t count = 0;
    assert(s->pending_welcomes(s->ctx, &pg, &welcomes, &count) == MARMOT_OK);
    /* At least 3 from this test */
    assert(count >= 3);

    for (size_t i = 0; i < count; i++) marmot_welcome_free(welcomes[i]);
    free(welcomes);

    marmot_group_id_free(&gid);
}

static void
test_welcome_processed_tracking(MarmotStorage *s)
{
    uint8_t wrapper_id[32];
    memset(wrapper_id, 0xE1, 32);

    bool found = true;
    int state = 0;
    char *reason = NULL;
    assert(s->find_processed_welcome(s->ctx, wrapper_id,
                                      &found, &state, &reason) == MARMOT_OK);
    assert(found == false);

    assert(s->save_processed_welcome(s->ctx, wrapper_id, NULL,
                                      1234567890,
                                      1 /* ACCEPTED */, NULL) == MARMOT_OK);

    assert(s->find_processed_welcome(s->ctx, wrapper_id,
                                      &found, &state, &reason) == MARMOT_OK);
    assert(found == true);
    assert(state == 1);
    free(reason);
}

/* ── 4. MLS key store ──────────────────────────────────────────────────── */

static void
test_mls_store_roundtrip(MarmotStorage *s)
{
    uint8_t key[] = {0xAA, 0xBB, 0xCC};
    uint8_t value[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x01, 0x02};

    assert(s->mls_store(s->ctx, "key_package", key, sizeof(key),
                         value, sizeof(value)) == MARMOT_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(s->mls_load(s->ctx, "key_package", key, sizeof(key),
                        &out, &out_len) == MARMOT_OK);
    assert(out_len == sizeof(value));
    assert(memcmp(out, value, sizeof(value)) == 0);
    free(out);
}

static void
test_mls_store_not_found(MarmotStorage *s)
{
    uint8_t key[] = {0xFF, 0xFE, 0xFD, 0xFC};
    uint8_t *out = NULL;
    size_t out_len = 0;

    MarmotError err = s->mls_load(s->ctx, "nonexistent_label", key, sizeof(key),
                                   &out, &out_len);
    assert(err == MARMOT_ERR_STORAGE_NOT_FOUND);
    assert(out == NULL);
}

static void
test_mls_store_upsert(MarmotStorage *s)
{
    uint8_t key[] = {0x01, 0x02};
    uint8_t v1[] = {0x10};
    uint8_t v2[] = {0x20, 0x30};

    assert(s->mls_store(s->ctx, "epoch_key", key, sizeof(key),
                         v1, sizeof(v1)) == MARMOT_OK);
    assert(s->mls_store(s->ctx, "epoch_key", key, sizeof(key),
                         v2, sizeof(v2)) == MARMOT_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(s->mls_load(s->ctx, "epoch_key", key, sizeof(key),
                        &out, &out_len) == MARMOT_OK);
    assert(out_len == 2);
    assert(out[0] == 0x20 && out[1] == 0x30);
    free(out);
}

static void
test_mls_store_delete(MarmotStorage *s)
{
    uint8_t key[] = {0xD0, 0xD1};
    uint8_t val[] = {0x42};

    assert(s->mls_store(s->ctx, "deleteme", key, sizeof(key),
                         val, sizeof(val)) == MARMOT_OK);
    assert(s->mls_delete(s->ctx, "deleteme", key, sizeof(key)) == MARMOT_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(s->mls_load(s->ctx, "deleteme", key, sizeof(key),
                        &out, &out_len) == MARMOT_ERR_STORAGE_NOT_FOUND);
}

static void
test_mls_store_label_isolation(MarmotStorage *s)
{
    uint8_t key[] = {0xAB};
    uint8_t v1[] = {0x01};
    uint8_t v2[] = {0x02};

    /* Same key, different labels — should be independent */
    assert(s->mls_store(s->ctx, "label_a", key, sizeof(key),
                         v1, sizeof(v1)) == MARMOT_OK);
    assert(s->mls_store(s->ctx, "label_b", key, sizeof(key),
                         v2, sizeof(v2)) == MARMOT_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;

    assert(s->mls_load(s->ctx, "label_a", key, sizeof(key),
                        &out, &out_len) == MARMOT_OK);
    assert(out_len == 1 && out[0] == 0x01);
    free(out);

    assert(s->mls_load(s->ctx, "label_b", key, sizeof(key),
                        &out, &out_len) == MARMOT_OK);
    assert(out_len == 1 && out[0] == 0x02);
    free(out);
}

/* ── 5. Exporter secrets ───────────────────────────────────────────────── */

static void
test_exporter_secret_roundtrip(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"exp_grp", 7);
    uint8_t secret[32];
    memset(secret, 0x77, 32);

    assert(s->save_exporter_secret(s->ctx, &gid, 5, secret) == MARMOT_OK);

    uint8_t out[32];
    assert(s->get_exporter_secret(s->ctx, &gid, 5, out) == MARMOT_OK);
    assert(memcmp(out, secret, 32) == 0);

    /* Wrong epoch → not found */
    assert(s->get_exporter_secret(s->ctx, &gid, 999, out)
           == MARMOT_ERR_STORAGE_NOT_FOUND);

    marmot_group_id_free(&gid);
}

static void
test_exporter_secret_overwrite(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"overwrite_grp", 13);
    uint8_t s1[32], s2[32];
    memset(s1, 0xAA, 32);
    memset(s2, 0xBB, 32);

    assert(s->save_exporter_secret(s->ctx, &gid, 10, s1) == MARMOT_OK);
    assert(s->save_exporter_secret(s->ctx, &gid, 10, s2) == MARMOT_OK);

    uint8_t out[32];
    assert(s->get_exporter_secret(s->ctx, &gid, 10, out) == MARMOT_OK);
    assert(memcmp(out, s2, 32) == 0);

    marmot_group_id_free(&gid);
}

/* ── 6. Relay operations ───────────────────────────────────────────────── */

static void
test_relay_replace_and_list(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"relay_grp", 9);

    const char *urls[] = {"wss://relay1.example.com", "wss://relay2.example.com"};
    assert(s->replace_group_relays(s->ctx, &gid, urls, 2) == MARMOT_OK);

    MarmotGroupRelay *relays = NULL;
    size_t count = 0;
    assert(s->group_relays(s->ctx, &gid, &relays, &count) == MARMOT_OK);
    assert(count == 2);

    /* Verify both relay URLs are present (order may vary) */
    bool found1 = false, found2 = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(relays[i].relay_url, "wss://relay1.example.com") == 0)
            found1 = true;
        if (strcmp(relays[i].relay_url, "wss://relay2.example.com") == 0)
            found2 = true;
        free(relays[i].relay_url);
        marmot_group_id_free(&relays[i].mls_group_id);
    }
    free(relays);
    assert(found1 && found2);

    /* Replace with a single relay */
    const char *new_urls[] = {"wss://relay3.example.com"};
    assert(s->replace_group_relays(s->ctx, &gid, new_urls, 1) == MARMOT_OK);

    assert(s->group_relays(s->ctx, &gid, &relays, &count) == MARMOT_OK);
    assert(count == 1);
    assert(strcmp(relays[0].relay_url, "wss://relay3.example.com") == 0);
    free(relays[0].relay_url);
    marmot_group_id_free(&relays[0].mls_group_id);
    free(relays);

    marmot_group_id_free(&gid);
}

/* ── 7. Persistence flag ──────────────────────────────────────────────── */

static void
test_is_persistent_memory(MarmotStorage *s)
{
    /* Memory backend should report not persistent */
    assert(s->is_persistent(s->ctx) == false);
}

static void
test_is_persistent_sqlite(MarmotStorage *s)
{
    assert(s->is_persistent(s->ctx) == true);
}

static void
test_is_persistent_nostrdb(MarmotStorage *s)
{
    assert(s->is_persistent(s->ctx) == true);
}

/* ── 8. Snapshot operations ─────────────────────────────────────────────── */

static void
test_snapshot_lifecycle(MarmotStorage *s)
{
    MarmotGroupId gid = marmot_group_id_new((uint8_t *)"snap_grp", 8);

    /* Create snapshot — should succeed even if group doesn't exist yet
     * (snapshots are just named save points) */
    MarmotError err = s->create_snapshot(s->ctx, &gid, "before_commit");
    assert(err == MARMOT_OK);

    /* Release without rollback */
    err = s->release_snapshot(s->ctx, &gid, "before_commit");
    assert(err == MARMOT_OK);

    /* Prune expired snapshots — should work even with none */
    size_t pruned = 0;
    err = s->prune_expired_snapshots(s->ctx, 0, &pruned);
    assert(err == MARMOT_OK);

    marmot_group_id_free(&gid);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Test runner — runs all contract tests against a given backend
 * ──────────────────────────────────────────────────────────────────────── */

static void
run_contract_tests(const char *backend_name, MarmotStorage *s,
                   bool is_persistent_expected)
{
    printf("\n── %s backend ──────────────────────────────────────────────\n",
           backend_name);

    /* Group CRUD */
    TEST(test_group_save_and_find_by_mls_id, backend_name, s);
    TEST(test_group_find_by_nostr_id, backend_name, s);
    TEST(test_group_not_found, backend_name, s);
    TEST(test_group_upsert, backend_name, s);
    TEST(test_group_list_all, backend_name, s);

    /* Messages */
    TEST(test_message_save_and_find, backend_name, s);
    TEST(test_message_pagination, backend_name, s);
    TEST(test_message_last, backend_name, s);
    TEST(test_message_processed_tracking, backend_name, s);

    /* Welcomes */
    TEST(test_welcome_save_and_find, backend_name, s);
    TEST(test_welcome_pending, backend_name, s);
    TEST(test_welcome_processed_tracking, backend_name, s);

    /* MLS key store */
    TEST(test_mls_store_roundtrip, backend_name, s);
    TEST(test_mls_store_not_found, backend_name, s);
    TEST(test_mls_store_upsert, backend_name, s);
    TEST(test_mls_store_delete, backend_name, s);
    TEST(test_mls_store_label_isolation, backend_name, s);

    /* Exporter secrets */
    TEST(test_exporter_secret_roundtrip, backend_name, s);
    TEST(test_exporter_secret_overwrite, backend_name, s);

    /* Relays */
    TEST(test_relay_replace_and_list, backend_name, s);

    /* Persistence */
    if (is_persistent_expected) {
        printf("  [%-8s] %-45sPASS (persistent)\n", backend_name, "is_persistent");
        assert(s->is_persistent(s->ctx) == true);
    } else {
        printf("  [%-8s] %-45sPASS (not persistent)\n", backend_name, "is_persistent");
        assert(s->is_persistent(s->ctx) == false);
    }

    /* Snapshots */
    TEST(test_snapshot_lifecycle, backend_name, s);
}

/* ──────────────────────────────────────────────────────────────────────────
 * main — instantiate and test each available backend
 * ──────────────────────────────────────────────────────────────────────── */

int main(void)
{
    int total_backends = 0;
    make_tmp_dir();

    printf("libmarmot: Storage contract tests\n");
    printf("  temp dir: %s\n", tmp_dir);

    /* ── 1. Memory backend (always available) ─────────────────────────── */
    {
        MarmotStorage *s = marmot_storage_memory_new();
        assert(s != NULL);
        run_contract_tests("memory", s, false);
        marmot_storage_free(s);
        total_backends++;
    }

    /* ── 2. SQLite backend (if compiled with SQLite3) ─────────────────── */
    {
        char db_path[512];
        snprintf(db_path, sizeof(db_path), "%s/contract_test.db", tmp_dir);

        MarmotStorage *s = marmot_storage_sqlite_new(db_path, NULL);
        if (s != NULL) {
            run_contract_tests("sqlite", s, true);
            marmot_storage_free(s);
            total_backends++;
        } else {
            printf("\n── sqlite backend ── SKIPPED (not available)\n");
        }
    }

    /* ── 3. nostrdb backend (if compiled with nostrdb) ────────────────── */
    {
        char ndb_dir[512];
        snprintf(ndb_dir, sizeof(ndb_dir), "%s/ndb_mls_state", tmp_dir);
        mkdir(ndb_dir, 0755);

        /* Pass NULL for ndb_handle — still tests the LMDB MLS state layer */
        MarmotStorage *s = marmot_storage_nostrdb_new(NULL, ndb_dir);
        if (s != NULL) {
            run_contract_tests("nostrdb", s, true);
            marmot_storage_free(s);
            total_backends++;
        } else {
            printf("\n── nostrdb backend ── SKIPPED (not available)\n");
        }
    }

    /* Cleanup */
    rm_rf(tmp_dir);

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("  Storage contract tests: %d backend(s) tested — ALL PASSED\n",
           total_backends);
    printf("════════════════════════════════════════════════════════════\n");

    return 0;
}
