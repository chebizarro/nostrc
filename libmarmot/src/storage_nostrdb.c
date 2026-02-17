/*
 * libmarmot - nostrdb storage backend
 *
 * Hybrid storage using nostrdb for Nostr events (kind 443/444/445)
 * and a separate LMDB environment for MLS internal state.
 *
 * This backend leverages nostrdb's native event indexing for messages
 * and welcomes, while keeping MLS state (group data, key packages,
 * exporter secrets, snapshots) in a dedicated LMDB database.
 *
 * Benefits over pure SQLite:
 * - Events are properly indexed by nostrdb (kind, author, tags)
 * - Subscriptions to new events via nostrdb's native pub/sub
 * - Shared nostrdb instance with the main app (no double storage)
 * - LMDB for MLS state is extremely fast for binary KV operations
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-storage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#if __has_include("nostrdb.h")
#include "nostrdb.h"
#include "lmdb.h"
#define HAVE_NOSTRDB 1
#else
#define HAVE_NOSTRDB 0
#endif

#if !HAVE_NOSTRDB

/* Stub when nostrdb is not available at compile time */
MarmotStorage *marmot_storage_nostrdb_new(void *ndb, const char *mls_state_dir)
{
    (void)ndb; (void)mls_state_dir;
    return NULL;
}

#else

/* ──────────────────────────────────────────────────────────────────────────
 * LMDB named databases for MLS state
 * ──────────────────────────────────────────────────────────────────────── */

/* We use 6 named databases within one LMDB environment: */
#define NDB_MLS_GROUPS       "marmot_groups"
#define NDB_MLS_MESSAGES     "marmot_messages"    /* message metadata index */
#define NDB_MLS_WELCOMES     "marmot_welcomes"
#define NDB_MLS_SECRETS      "marmot_secrets"     /* exporter secrets */
#define NDB_MLS_KV           "marmot_kv"          /* MLS internal state */
#define NDB_MLS_SNAPSHOTS    "marmot_snapshots"
#define NDB_MLS_PROCESSED    "marmot_processed"   /* processed message/welcome tracking */
#define NDB_MLS_RELAYS       "marmot_relays"      /* group relay URLs */
#define NDB_MLS_MAX_DBS      8

/* ──────────────────────────────────────────────────────────────────────────
 * Internal context
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    struct ndb *ndb;           /* borrowed reference — caller manages lifetime */
    MDB_env *mls_env;         /* our own LMDB environment for MLS state */
    MDB_dbi dbi_groups;
    MDB_dbi dbi_messages;
    MDB_dbi dbi_welcomes;
    MDB_dbi dbi_secrets;
    MDB_dbi dbi_kv;
    MDB_dbi dbi_snapshots;
    MDB_dbi dbi_processed;
    MDB_dbi dbi_relays;
    bool owns_ndb;            /* if true, we'll call ndb_destroy on cleanup */
} NdbCtx;

/* ── LMDB helpers ──────────────────────────────────────────────────────── */

/* Composite key for exporter secrets: group_id || epoch (big-endian) */
static size_t
make_secret_key(const MarmotGroupId *gid, uint64_t epoch,
                uint8_t *buf, size_t buf_len)
{
    if (!gid || !gid->data) return 0;
    size_t needed = gid->len + 8;
    if (needed > buf_len) return 0;
    memcpy(buf, gid->data, gid->len);
    /* Big-endian epoch for sorted ordering */
    for (int i = 7; i >= 0; i--) {
        buf[gid->len + (size_t)(7 - i)] = (uint8_t)(epoch >> (i * 8));
    }
    return needed;
}

/* Composite key for MLS key-value store: label\0 || key */
static size_t
make_kv_key(const char *label, const uint8_t *key, size_t key_len,
            uint8_t *buf, size_t buf_len)
{
    size_t label_len = strlen(label) + 1; /* include null terminator */
    size_t needed = label_len + key_len;
    if (needed > buf_len) return 0;
    memcpy(buf, label, label_len);
    if (key_len > 0) memcpy(buf + label_len, key, key_len);
    return needed;
}

/* ── Serialization: Group → binary blob ────────────────────────────────── */

/*
 * Group serialization format (version 1):
 * [1: version]
 * [4: nostr_group_id_len=32][32: nostr_group_id]
 * [4: name_len][N: name]
 * [4: description_len][N: description]
 * [1: has_image_hash][32?: image_hash]
 * [1: has_image_key][32?: image_key]
 * [1: has_image_nonce][12?: image_nonce]
 * [4: admin_count][admin_count*32: admin_pubkeys]
 * [4: last_message_id_len][N: last_message_id]
 * [8: last_message_at]
 * [8: last_message_processed_at]
 * [8: epoch]
 * [4: state]
 */

static uint8_t *
serialize_group(const MarmotGroup *g, size_t *out_len)
{
    /* Calculate size */
    size_t name_len = g->name ? strlen(g->name) : 0;
    size_t desc_len = g->description ? strlen(g->description) : 0;
    size_t lmid_len = g->last_message_id ? strlen(g->last_message_id) : 0;
    size_t size = 1 + 4 + 32 + 4 + name_len + 4 + desc_len
                + 3 + (g->image_hash ? 32 : 0) + (g->image_key ? 32 : 0) + (g->image_nonce ? 12 : 0)
                + 4 + g->admin_count * 32
                + 4 + lmid_len + 8 + 8 + 8 + 4;

    uint8_t *buf = malloc(size);
    if (!buf) return NULL;
    uint8_t *p = buf;

    *p++ = 1; /* version */

    /* nostr_group_id */
    uint32_t v32 = 32;
    memcpy(p, &v32, 4); p += 4;
    memcpy(p, g->nostr_group_id, 32); p += 32;

    /* name */
    v32 = (uint32_t)name_len;
    memcpy(p, &v32, 4); p += 4;
    if (name_len > 0) { memcpy(p, g->name, name_len); p += name_len; }

    /* description */
    v32 = (uint32_t)desc_len;
    memcpy(p, &v32, 4); p += 4;
    if (desc_len > 0) { memcpy(p, g->description, desc_len); p += desc_len; }

    /* image fields */
    *p++ = g->image_hash ? 1 : 0;
    if (g->image_hash) { memcpy(p, g->image_hash, 32); p += 32; }
    *p++ = g->image_key ? 1 : 0;
    if (g->image_key) { memcpy(p, g->image_key, 32); p += 32; }
    *p++ = g->image_nonce ? 1 : 0;
    if (g->image_nonce) { memcpy(p, g->image_nonce, 12); p += 12; }

    /* admins */
    v32 = (uint32_t)g->admin_count;
    memcpy(p, &v32, 4); p += 4;
    if (g->admin_count > 0 && g->admin_pubkeys) {
        memcpy(p, g->admin_pubkeys, g->admin_count * 32);
        p += g->admin_count * 32;
    }

    /* last_message_id */
    v32 = (uint32_t)lmid_len;
    memcpy(p, &v32, 4); p += 4;
    if (lmid_len > 0) { memcpy(p, g->last_message_id, lmid_len); p += lmid_len; }

    /* timestamps and state */
    int64_t i64;
    i64 = g->last_message_at; memcpy(p, &i64, 8); p += 8;
    i64 = g->last_message_processed_at; memcpy(p, &i64, 8); p += 8;
    uint64_t u64 = g->epoch; memcpy(p, &u64, 8); p += 8;
    int32_t i32 = (int32_t)g->state; memcpy(p, &i32, 4); p += 4;

    *out_len = (size_t)(p - buf);
    return buf;
}

static MarmotGroup *
deserialize_group(const MarmotGroupId *mls_gid, const uint8_t *data, size_t len)
{
    if (!data || len < 1) return NULL;
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    if (*p++ != 1) return NULL; /* version check */

    MarmotGroup *g = marmot_group_new();
    if (!g) return NULL;
    g->mls_group_id = marmot_group_id_new(mls_gid->data, mls_gid->len);

    /* nostr_group_id */
    if (p + 4 > end) goto fail;
    uint32_t v32; memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end || v32 != 32) goto fail;
    memcpy(g->nostr_group_id, p, 32); p += 32;

    /* name */
    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end) goto fail;
    if (v32 > 0) { g->name = strndup((const char *)p, v32); p += v32; }

    /* description */
    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end) goto fail;
    if (v32 > 0) { g->description = strndup((const char *)p, v32); p += v32; }

    /* image fields */
    if (p + 3 > end) goto fail;
    if (*p++) { if (p + 32 > end) goto fail; g->image_hash = malloc(32); if (g->image_hash) memcpy(g->image_hash, p, 32); p += 32; }
    if (*p++) { if (p + 32 > end) goto fail; g->image_key = malloc(32); if (g->image_key) memcpy(g->image_key, p, 32); p += 32; }
    if (*p++) { if (p + 12 > end) goto fail; g->image_nonce = malloc(12); if (g->image_nonce) memcpy(g->image_nonce, p, 12); p += 12; }

    /* admins */
    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (v32 > 0) {
        if (p + v32 * 32 > end) goto fail;
        g->admin_pubkeys = malloc(v32 * 32);
        if (g->admin_pubkeys) { memcpy(g->admin_pubkeys, p, v32 * 32); g->admin_count = v32; }
        p += v32 * 32;
    }

    /* last_message_id */
    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end) goto fail;
    if (v32 > 0) { g->last_message_id = strndup((const char *)p, v32); p += v32; }

    /* timestamps and state */
    if (p + 28 > end) goto fail;
    memcpy(&g->last_message_at, p, 8); p += 8;
    memcpy(&g->last_message_processed_at, p, 8); p += 8;
    memcpy(&g->epoch, p, 8); p += 8;
    int32_t state; memcpy(&state, p, 4); p += 4;
    g->state = state;

    return g;
fail:
    marmot_group_free(g);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Storage vtable implementations
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Group operations (via LMDB) ──────────────────────────────────────── */

static MarmotError
ndb_all_groups(void *ctx, MarmotGroup ***out, size_t *out_count)
{
    NdbCtx *nc = ctx;
    *out = NULL;
    *out_count = 0;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_cursor *cur;
    if (mdb_cursor_open(txn, nc->dbi_groups, &cur) != 0) {
        mdb_txn_abort(txn);
        return MARMOT_ERR_STORAGE;
    }

    /* First pass: count */
    size_t count = 0;
    MDB_val k, v;
    while (mdb_cursor_get(cur, &k, &v, count == 0 ? MDB_FIRST : MDB_NEXT) == 0)
        count++;
    mdb_cursor_close(cur);

    if (count == 0) {
        mdb_txn_abort(txn);
        return MARMOT_OK;
    }

    *out = calloc(count, sizeof(MarmotGroup *));
    if (!*out) { mdb_txn_abort(txn); return MARMOT_ERR_MEMORY; }

    /* Second pass: read */
    if (mdb_cursor_open(txn, nc->dbi_groups, &cur) != 0) {
        mdb_txn_abort(txn);
        free(*out);
        return MARMOT_ERR_STORAGE;
    }

    size_t i = 0;
    while (mdb_cursor_get(cur, &k, &v, i == 0 ? MDB_FIRST : MDB_NEXT) == 0 && i < count) {
        MarmotGroupId gid = marmot_group_id_new(k.mv_data, k.mv_size);
        (*out)[i] = deserialize_group(&gid, v.mv_data, v.mv_size);
        marmot_group_id_free(&gid);
        if (!(*out)[i]) {
            for (size_t j = 0; j < i; j++) marmot_group_free((*out)[j]);
            free(*out); *out = NULL;
            mdb_cursor_close(cur);
            mdb_txn_abort(txn);
            return MARMOT_ERR_MEMORY;
        }
        i++;
    }

    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    *out_count = i;
    return MARMOT_OK;
}

static MarmotError
ndb_find_group_by_mls_id(void *ctx, const MarmotGroupId *gid, MarmotGroup **out)
{
    NdbCtx *nc = ctx;
    *out = NULL;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = gid->len, .mv_data = (void *)gid->data };
    MDB_val v;
    int rc = mdb_get(txn, nc->dbi_groups, &k, &v);
    if (rc == 0) {
        *out = deserialize_group(gid, v.mv_data, v.mv_size);
    }
    mdb_txn_abort(txn);
    return MARMOT_OK; /* not found is not error */
}

static MarmotError
ndb_find_group_by_nostr_id(void *ctx, const uint8_t nostr_id[32], MarmotGroup **out)
{
    /* Linear scan — groups are few, this is fine */
    MarmotGroup **all = NULL;
    size_t count = 0;
    MarmotError err = ndb_all_groups(ctx, &all, &count);
    if (err != MARMOT_OK) return err;

    *out = NULL;
    for (size_t i = 0; i < count; i++) {
        if (memcmp(all[i]->nostr_group_id, nostr_id, 32) == 0) {
            *out = all[i];
            all[i] = NULL; /* don't free the match */
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (all[i]) marmot_group_free(all[i]);
    }
    free(all);
    return MARMOT_OK;
}

static MarmotError
ndb_save_group(void *ctx, const MarmotGroup *group)
{
    NdbCtx *nc = ctx;

    size_t blob_len;
    uint8_t *blob = serialize_group(group, &blob_len);
    if (!blob) return MARMOT_ERR_MEMORY;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0) {
        free(blob);
        return MARMOT_ERR_STORAGE;
    }

    MDB_val k = { .mv_size = group->mls_group_id.len, .mv_data = (void *)group->mls_group_id.data };
    MDB_val v = { .mv_size = blob_len, .mv_data = blob };

    int rc = mdb_put(txn, nc->dbi_groups, &k, &v, 0);
    free(blob);

    if (rc != 0) { mdb_txn_abort(txn); return MARMOT_ERR_STORAGE; }
    mdb_txn_commit(txn);
    return MARMOT_OK;
}

/* ── Message operations (via nostrdb for events + LMDB for metadata) ──── */

/*
 * Messages: We store MarmotMessage metadata in LMDB (keyed by event_id),
 * and the actual Nostr events in nostrdb. On retrieval, we pull from LMDB.
 * The full event JSON is also stored in the metadata blob.
 */

static uint8_t *
serialize_message(const MarmotMessage *m, size_t *out_len)
{
    size_t content_len = m->content ? strlen(m->content) : 0;
    size_t tags_len = m->tags_json ? strlen(m->tags_json) : 0;
    size_t event_len = m->event_json ? strlen(m->event_json) : 0;

    size_t size = 1 /* version */ + 32 /* pubkey */ + 4 /* kind */
                + 4 + m->mls_group_id.len /* mls_group_id */
                + 8 /* created_at */ + 8 /* processed_at */
                + 4 + content_len + 4 + tags_len + 4 + event_len
                + 32 /* wrapper_event_id */ + 8 /* epoch */ + 4 /* state */;

    uint8_t *buf = malloc(size);
    if (!buf) return NULL;
    uint8_t *p = buf;

    *p++ = 1;
    memcpy(p, m->pubkey, 32); p += 32;
    uint32_t v32 = m->kind; memcpy(p, &v32, 4); p += 4;

    v32 = (uint32_t)m->mls_group_id.len; memcpy(p, &v32, 4); p += 4;
    if (m->mls_group_id.len > 0) { memcpy(p, m->mls_group_id.data, m->mls_group_id.len); p += m->mls_group_id.len; }

    int64_t i64 = m->created_at; memcpy(p, &i64, 8); p += 8;
    i64 = m->processed_at; memcpy(p, &i64, 8); p += 8;

    v32 = (uint32_t)content_len; memcpy(p, &v32, 4); p += 4;
    if (content_len > 0) { memcpy(p, m->content, content_len); p += content_len; }
    v32 = (uint32_t)tags_len; memcpy(p, &v32, 4); p += 4;
    if (tags_len > 0) { memcpy(p, m->tags_json, tags_len); p += tags_len; }
    v32 = (uint32_t)event_len; memcpy(p, &v32, 4); p += 4;
    if (event_len > 0) { memcpy(p, m->event_json, event_len); p += event_len; }

    memcpy(p, m->wrapper_event_id, 32); p += 32;
    uint64_t u64 = m->epoch; memcpy(p, &u64, 8); p += 8;
    int32_t state = (int32_t)m->state; memcpy(p, &state, 4); p += 4;

    *out_len = (size_t)(p - buf);
    return buf;
}

static MarmotMessage *
deserialize_message(const uint8_t id[32], const uint8_t *data, size_t len)
{
    if (!data || len < 1 || data[0] != 1) return NULL;
    const uint8_t *p = data + 1;
    const uint8_t *end = data + len;

    MarmotMessage *m = marmot_message_new();
    if (!m) return NULL;
    memcpy(m->id, id, 32);

    if (p + 32 + 4 > end) goto fail;
    memcpy(m->pubkey, p, 32); p += 32;
    uint32_t v32; memcpy(&v32, p, 4); p += 4;
    m->kind = v32;

    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end) goto fail;
    m->mls_group_id = marmot_group_id_new(p, v32); p += v32;

    if (p + 16 > end) goto fail;
    memcpy(&m->created_at, p, 8); p += 8;
    memcpy(&m->processed_at, p, 8); p += 8;

    /* content */
    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end) goto fail;
    if (v32 > 0) { m->content = strndup((const char *)p, v32); p += v32; }

    /* tags_json */
    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end) goto fail;
    if (v32 > 0) { m->tags_json = strndup((const char *)p, v32); p += v32; }

    /* event_json */
    if (p + 4 > end) goto fail;
    memcpy(&v32, p, 4); p += 4;
    if (p + v32 > end) goto fail;
    if (v32 > 0) { m->event_json = strndup((const char *)p, v32); p += v32; }

    if (p + 32 + 8 + 4 > end) goto fail;
    memcpy(m->wrapper_event_id, p, 32); p += 32;
    memcpy(&m->epoch, p, 8); p += 8;
    int32_t state; memcpy(&state, p, 4); p += 4;
    m->state = state;

    return m;
fail:
    marmot_message_free(m);
    return NULL;
}

static MarmotError
ndb_save_message(void *ctx, const MarmotMessage *msg)
{
    NdbCtx *nc = ctx;

    /* Store metadata in LMDB */
    size_t blob_len;
    uint8_t *blob = serialize_message(msg, &blob_len);
    if (!blob) return MARMOT_ERR_MEMORY;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0) {
        free(blob); return MARMOT_ERR_STORAGE;
    }

    MDB_val k = { .mv_size = 32, .mv_data = (void *)msg->id };
    MDB_val v = { .mv_size = blob_len, .mv_data = blob };
    int rc = mdb_put(txn, nc->dbi_messages, &k, &v, 0);
    free(blob);

    if (rc != 0) { mdb_txn_abort(txn); return MARMOT_ERR_STORAGE; }

    /* Also ingest the event JSON into nostrdb if available */
    if (nc->ndb && msg->event_json) {
        ndb_process_event(nc->ndb, msg->event_json, (int)strlen(msg->event_json));
    }

    mdb_txn_commit(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_find_message_by_id(void *ctx, const uint8_t event_id[32], MarmotMessage **out)
{
    NdbCtx *nc = ctx;
    *out = NULL;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = 32, .mv_data = (void *)event_id };
    MDB_val v;
    int rc = mdb_get(txn, nc->dbi_messages, &k, &v);
    if (rc == 0) {
        *out = deserialize_message(event_id, v.mv_data, v.mv_size);
    }
    mdb_txn_abort(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_messages(void *ctx, const MarmotGroupId *gid,
             const MarmotPagination *pg,
             MarmotMessage ***out, size_t *out_count)
{
    NdbCtx *nc = ctx;
    *out = NULL;
    *out_count = 0;

    /* Scan all messages in LMDB, filter by group_id */
    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_cursor *cur;
    if (mdb_cursor_open(txn, nc->dbi_messages, &cur) != 0) {
        mdb_txn_abort(txn);
        return MARMOT_ERR_STORAGE;
    }

    size_t skip = pg ? pg->offset : 0;
    size_t limit = pg ? pg->limit : 1000;

    size_t cap = 64;
    MarmotMessage **arr = calloc(cap, sizeof(MarmotMessage *));
    if (!arr) { mdb_cursor_close(cur); mdb_txn_abort(txn); return MARMOT_ERR_MEMORY; }

    size_t found = 0, added = 0;
    MDB_val k, v;
    int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
    while (rc == 0 && added < limit) {
        if (k.mv_size == 32) {
            MarmotMessage *m = deserialize_message(k.mv_data, v.mv_data, v.mv_size);
            if (m && marmot_group_id_equal(&m->mls_group_id, gid)) {
                if (found >= skip) {
                    if (added >= cap) {
                        cap *= 2;
                        MarmotMessage **new_arr = realloc(arr, cap * sizeof(MarmotMessage *));
                        if (!new_arr) { marmot_message_free(m); break; }
                        arr = new_arr;
                    }
                    arr[added++] = m;
                } else {
                    marmot_message_free(m);
                }
                found++;
            } else if (m) {
                marmot_message_free(m);
            }
        }
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    *out = arr;
    *out_count = added;
    return MARMOT_OK;
}

static MarmotError
ndb_last_message(void *ctx, const MarmotGroupId *gid,
                  MarmotSortOrder order, MarmotMessage **out)
{
    /* Get all messages for group, find the latest */
    MarmotMessage **msgs = NULL;
    size_t count = 0;
    MarmotPagination pg = { .offset = 0, .limit = 10000 };
    MarmotError err = ndb_messages(ctx, gid, &pg, &msgs, &count);
    if (err != MARMOT_OK) return err;

    *out = NULL;
    MarmotMessage *best = NULL;
    size_t best_idx = 0;
    for (size_t i = 0; i < count; i++) {
        if (!best) { best = msgs[i]; best_idx = i; continue; }
        int64_t b_ts = (order == MARMOT_SORT_PROCESSED_AT_FIRST) ? best->processed_at : best->created_at;
        int64_t m_ts = (order == MARMOT_SORT_PROCESSED_AT_FIRST) ? msgs[i]->processed_at : msgs[i]->created_at;
        if (m_ts > b_ts) { best = msgs[i]; best_idx = i; }
    }

    if (best) {
        *out = best;
        msgs[best_idx] = NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (msgs[i]) marmot_message_free(msgs[i]);
    }
    free(msgs);
    return MARMOT_OK;
}

static MarmotError
ndb_is_message_processed(void *ctx, const uint8_t wrapper_id[32], bool *out)
{
    NdbCtx *nc = ctx;
    *out = false;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = 32, .mv_data = (void *)wrapper_id };
    MDB_val v;
    if (mdb_get(txn, nc->dbi_processed, &k, &v) == 0)
        *out = true;
    mdb_txn_abort(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_save_processed_message(void *ctx, const uint8_t wrapper_id[32],
                            const uint8_t *msg_id, int64_t processed_at,
                            uint64_t epoch, const MarmotGroupId *gid,
                            int state, const char *reason)
{
    NdbCtx *nc = ctx;
    (void)msg_id; (void)processed_at; (void)epoch; (void)gid; (void)reason;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    uint8_t val = (uint8_t)state;
    MDB_val k = { .mv_size = 32, .mv_data = (void *)wrapper_id };
    MDB_val v = { .mv_size = 1, .mv_data = &val };
    mdb_put(txn, nc->dbi_processed, &k, &v, 0);
    mdb_txn_commit(txn);
    return MARMOT_OK;
}

/* ── Welcome operations ────────────────────────────────────────────────── */
/* Welcomes use LMDB with similar serialize/deserialize pattern */
/* For brevity, we delegate to the same pattern as messages */

static MarmotError
ndb_save_welcome(void *ctx, const MarmotWelcome *w)
{
    NdbCtx *nc = ctx;

    /* Simple: store event_json as the value, keyed by welcome id */
    const char *json = w->event_json ? w->event_json : "";

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = 32, .mv_data = (void *)w->id };
    MDB_val v = { .mv_size = strlen(json) + 1, .mv_data = (void *)json };
    int rc = mdb_put(txn, nc->dbi_welcomes, &k, &v, 0);

    if (rc != 0) { mdb_txn_abort(txn); return MARMOT_ERR_STORAGE; }
    mdb_txn_commit(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_find_welcome_by_event_id(void *ctx, const uint8_t event_id[32], MarmotWelcome **out)
{
    NdbCtx *nc = ctx;
    *out = NULL;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = 32, .mv_data = (void *)event_id };
    MDB_val v;
    if (mdb_get(txn, nc->dbi_welcomes, &k, &v) == 0) {
        MarmotWelcome *w = marmot_welcome_new();
        if (w) {
            memcpy(w->id, event_id, 32);
            w->event_json = strndup(v.mv_data, v.mv_size);
        }
        *out = w;
    }
    mdb_txn_abort(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_pending_welcomes(void *ctx, const MarmotPagination *pg,
                      MarmotWelcome ***out, size_t *out_count)
{
    (void)ctx; (void)pg;
    *out = NULL;
    *out_count = 0;
    return MARMOT_OK; /* TODO: track welcome state in LMDB */
}

static MarmotError
ndb_find_processed_welcome(void *ctx, const uint8_t wrapper_id[32],
                            bool *found, int *state, char **reason)
{
    (void)ctx; (void)wrapper_id;
    *found = false; *state = 0; *reason = NULL;
    return MARMOT_OK;
}

static MarmotError
ndb_save_processed_welcome(void *ctx, const uint8_t wrapper_id[32],
                            const uint8_t *welcome_id, int64_t processed_at,
                            int state, const char *reason)
{
    (void)ctx; (void)wrapper_id; (void)welcome_id;
    (void)processed_at; (void)state; (void)reason;
    return MARMOT_OK;
}

/* ── Relay operations (via LMDB) ──────────────────────────────────────── */

static MarmotError
ndb_group_relays(void *ctx, const MarmotGroupId *gid,
                  MarmotGroupRelay **out, size_t *out_count)
{
    NdbCtx *nc = ctx;
    *out = NULL;
    *out_count = 0;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = gid->len, .mv_data = (void *)gid->data };
    MDB_val v;
    if (mdb_get(txn, nc->dbi_relays, &k, &v) == 0 && v.mv_size > 0) {
        /* Tab-separated relay URLs */
        char *str = strndup(v.mv_data, v.mv_size);
        if (str) {
            size_t count = 1;
            for (char *p = str; *p; p++) if (*p == '\t') count++;

            *out = calloc(count, sizeof(MarmotGroupRelay));
            if (*out) {
                size_t idx = 0;
                char *saveptr = NULL;
                char *tok = strtok_r(str, "\t", &saveptr);
                while (tok && idx < count) {
                    (*out)[idx].url = strdup(tok);
                    idx++;
                    tok = strtok_r(NULL, "\t", &saveptr);
                }
                *out_count = idx;
            }
            free(str);
        }
    }
    mdb_txn_abort(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_replace_group_relays(void *ctx, const MarmotGroupId *gid,
                          const char **urls, size_t count)
{
    NdbCtx *nc = ctx;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = gid->len, .mv_data = (void *)gid->data };

    if (count == 0 || !urls) {
        mdb_del(txn, nc->dbi_relays, &k, NULL);
    } else {
        size_t total = 0;
        for (size_t i = 0; i < count; i++) {
            if (urls[i]) total += strlen(urls[i]) + 1;
        }
        char *str = malloc(total);
        if (str) {
            str[0] = '\0';
            for (size_t i = 0; i < count; i++) {
                if (i > 0) strcat(str, "\t");
                if (urls[i]) strcat(str, urls[i]);
            }
            MDB_val v = { .mv_size = strlen(str), .mv_data = str };
            mdb_put(txn, nc->dbi_relays, &k, &v, 0);
            free(str);
        }
    }

    mdb_txn_commit(txn);
    return MARMOT_OK;
}

/* ── Exporter secret operations (via LMDB) ─────────────────────────────── */

static MarmotError
ndb_get_exporter_secret(void *ctx, const MarmotGroupId *gid,
                         uint64_t epoch, uint8_t out[32])
{
    NdbCtx *nc = ctx;

    uint8_t key_buf[256];
    size_t key_len = make_secret_key(gid, epoch, key_buf, sizeof(key_buf));
    if (key_len == 0) return MARMOT_ERR_INVALID_ARG;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = key_len, .mv_data = key_buf };
    MDB_val v;
    int rc = mdb_get(txn, nc->dbi_secrets, &k, &v);
    mdb_txn_abort(txn);

    if (rc == 0 && v.mv_size >= 32) {
        memcpy(out, v.mv_data, 32);
        return MARMOT_OK;
    }
    return MARMOT_ERR_STORAGE_NOT_FOUND;
}

static MarmotError
ndb_save_exporter_secret(void *ctx, const MarmotGroupId *gid,
                          uint64_t epoch, const uint8_t secret[32])
{
    NdbCtx *nc = ctx;

    uint8_t key_buf[256];
    size_t key_len = make_secret_key(gid, epoch, key_buf, sizeof(key_buf));
    if (key_len == 0) return MARMOT_ERR_INVALID_ARG;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = key_len, .mv_data = key_buf };
    MDB_val v = { .mv_size = 32, .mv_data = (void *)secret };
    int rc = mdb_put(txn, nc->dbi_secrets, &k, &v, 0);

    if (rc != 0) { mdb_txn_abort(txn); return MARMOT_ERR_STORAGE; }
    mdb_txn_commit(txn);
    return MARMOT_OK;
}

/* ── Snapshot operations (via LMDB) ────────────────────────────────────── */

static MarmotError
ndb_create_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{
    NdbCtx *nc = ctx;
    uint8_t key_buf[256];
    size_t kl = make_kv_key("snap", (const uint8_t *)name, strlen(name), key_buf, sizeof(key_buf));
    if (kl == 0) return MARMOT_ERR_INVALID_ARG;

    /* Prepend group_id */
    uint8_t full_key[512];
    if (gid->len + kl > sizeof(full_key)) return MARMOT_ERR_INVALID_ARG;
    memcpy(full_key, gid->data, gid->len);
    memcpy(full_key + gid->len, key_buf, kl);

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    int64_t now = (int64_t)time(NULL);
    MDB_val k = { .mv_size = gid->len + kl, .mv_data = full_key };
    MDB_val v = { .mv_size = sizeof(now), .mv_data = &now };
    mdb_put(txn, nc->dbi_snapshots, &k, &v, 0);
    mdb_txn_commit(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_rollback_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{
    NdbCtx *nc = ctx;
    uint8_t key_buf[256];
    size_t kl = make_kv_key("snap", (const uint8_t *)name, strlen(name), key_buf, sizeof(key_buf));
    if (kl == 0) return MARMOT_ERR_INVALID_ARG;

    uint8_t full_key[512];
    if (gid->len + kl > sizeof(full_key)) return MARMOT_ERR_INVALID_ARG;
    memcpy(full_key, gid->data, gid->len);
    memcpy(full_key + gid->len, key_buf, kl);

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = gid->len + kl, .mv_data = full_key };
    mdb_del(txn, nc->dbi_snapshots, &k, NULL);
    mdb_txn_commit(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_release_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{
    return ndb_rollback_snapshot(ctx, gid, name);
}

static MarmotError
ndb_prune_expired(void *ctx, uint64_t min_ts, size_t *out)
{
    (void)ctx; (void)min_ts;
    *out = 0;
    return MARMOT_OK; /* TODO: iterate snapshots and prune by timestamp */
}

/* ── MLS key store (via LMDB) ──────────────────────────────────────────── */

static MarmotError
ndb_mls_store(void *ctx, const char *label,
               const uint8_t *key, size_t key_len,
               const uint8_t *value, size_t value_len)
{
    NdbCtx *nc = ctx;

    uint8_t kbuf[512];
    size_t kl = make_kv_key(label, key, key_len, kbuf, sizeof(kbuf));
    if (kl == 0) return MARMOT_ERR_INVALID_ARG;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = kl, .mv_data = kbuf };
    MDB_val v = { .mv_size = value_len, .mv_data = (void *)value };
    int rc = mdb_put(txn, nc->dbi_kv, &k, &v, 0);

    if (rc != 0) { mdb_txn_abort(txn); return MARMOT_ERR_STORAGE; }
    mdb_txn_commit(txn);
    return MARMOT_OK;
}

static MarmotError
ndb_mls_load(void *ctx, const char *label,
              const uint8_t *key, size_t key_len,
              uint8_t **out, size_t *out_len)
{
    NdbCtx *nc = ctx;
    *out = NULL;
    *out_len = 0;

    uint8_t kbuf[512];
    size_t kl = make_kv_key(label, key, key_len, kbuf, sizeof(kbuf));
    if (kl == 0) return MARMOT_ERR_INVALID_ARG;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, MDB_RDONLY, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = kl, .mv_data = kbuf };
    MDB_val v;
    int rc = mdb_get(txn, nc->dbi_kv, &k, &v);
    if (rc == 0 && v.mv_size > 0) {
        *out = malloc(v.mv_size);
        if (*out) {
            memcpy(*out, v.mv_data, v.mv_size);
            *out_len = v.mv_size;
        }
    }
    mdb_txn_abort(txn);
    return (*out) ? MARMOT_OK : MARMOT_ERR_STORAGE_NOT_FOUND;
}

static MarmotError
ndb_mls_delete(void *ctx, const char *label,
                const uint8_t *key, size_t key_len)
{
    NdbCtx *nc = ctx;

    uint8_t kbuf[512];
    size_t kl = make_kv_key(label, key, key_len, kbuf, sizeof(kbuf));
    if (kl == 0) return MARMOT_ERR_INVALID_ARG;

    MDB_txn *txn;
    if (mdb_txn_begin(nc->mls_env, NULL, 0, &txn) != 0)
        return MARMOT_ERR_STORAGE;

    MDB_val k = { .mv_size = kl, .mv_data = kbuf };
    int rc = mdb_del(txn, nc->dbi_kv, &k, NULL);
    mdb_txn_commit(txn);
    return (rc == 0) ? MARMOT_OK : MARMOT_ERR_STORAGE_NOT_FOUND;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

static bool ndb_is_persistent(void *ctx) { (void)ctx; return true; }

static void
ndb_destroy(void *ctx)
{
    NdbCtx *nc = ctx;
    if (nc->mls_env) mdb_env_close(nc->mls_env);
    if (nc->owns_ndb && nc->ndb) ndb_destroy(nc->ndb);
    free(nc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public constructor
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotStorage *
marmot_storage_nostrdb_new(void *ndb_handle, const char *mls_state_dir)
{
    if (!mls_state_dir) return NULL;

    NdbCtx *nc = calloc(1, sizeof(NdbCtx));
    if (!nc) return NULL;

    nc->ndb = (struct ndb *)ndb_handle; /* borrowed, may be NULL */
    nc->owns_ndb = false;

    /* Create our own LMDB environment for MLS state */
    int rc = mdb_env_create(&nc->mls_env);
    if (rc != 0) { free(nc); return NULL; }

    mdb_env_set_maxdbs(nc->mls_env, NDB_MLS_MAX_DBS);
    mdb_env_set_mapsize(nc->mls_env, 256ULL * 1024 * 1024); /* 256 MB */

    rc = mdb_env_open(nc->mls_env, mls_state_dir, MDB_NOSUBDIR, 0644);
    if (rc != 0) {
        fprintf(stderr, "[marmot-nostrdb] mdb_env_open(%s) failed: %s\n",
                mls_state_dir, mdb_strerror(rc));
        mdb_env_close(nc->mls_env);
        free(nc);
        return NULL;
    }

    /* Open named databases */
    MDB_txn *txn;
    rc = mdb_txn_begin(nc->mls_env, NULL, 0, &txn);
    if (rc != 0) { mdb_env_close(nc->mls_env); free(nc); return NULL; }

    if (mdb_dbi_open(txn, NDB_MLS_GROUPS, MDB_CREATE, &nc->dbi_groups) != 0 ||
        mdb_dbi_open(txn, NDB_MLS_MESSAGES, MDB_CREATE, &nc->dbi_messages) != 0 ||
        mdb_dbi_open(txn, NDB_MLS_WELCOMES, MDB_CREATE, &nc->dbi_welcomes) != 0 ||
        mdb_dbi_open(txn, NDB_MLS_SECRETS, MDB_CREATE, &nc->dbi_secrets) != 0 ||
        mdb_dbi_open(txn, NDB_MLS_KV, MDB_CREATE, &nc->dbi_kv) != 0 ||
        mdb_dbi_open(txn, NDB_MLS_SNAPSHOTS, MDB_CREATE, &nc->dbi_snapshots) != 0 ||
        mdb_dbi_open(txn, NDB_MLS_PROCESSED, MDB_CREATE, &nc->dbi_processed) != 0 ||
        mdb_dbi_open(txn, NDB_MLS_RELAYS, MDB_CREATE, &nc->dbi_relays) != 0) {
        mdb_txn_abort(txn);
        mdb_env_close(nc->mls_env);
        free(nc);
        return NULL;
    }
    mdb_txn_commit(txn);

    MarmotStorage *s = calloc(1, sizeof(MarmotStorage));
    if (!s) { mdb_env_close(nc->mls_env); free(nc); return NULL; }

    s->ctx = nc;

    s->all_groups = ndb_all_groups;
    s->find_group_by_mls_id = ndb_find_group_by_mls_id;
    s->find_group_by_nostr_id = ndb_find_group_by_nostr_id;
    s->save_group = ndb_save_group;
    s->messages = ndb_messages;
    s->last_message = ndb_last_message;
    s->save_message = ndb_save_message;
    s->find_message_by_id = ndb_find_message_by_id;
    s->is_message_processed = ndb_is_message_processed;
    s->save_processed_message = ndb_save_processed_message;
    s->save_welcome = ndb_save_welcome;
    s->find_welcome_by_event_id = ndb_find_welcome_by_event_id;
    s->pending_welcomes = ndb_pending_welcomes;
    s->find_processed_welcome = ndb_find_processed_welcome;
    s->save_processed_welcome = ndb_save_processed_welcome;
    s->group_relays = ndb_group_relays;
    s->replace_group_relays = ndb_replace_group_relays;
    s->get_exporter_secret = ndb_get_exporter_secret;
    s->save_exporter_secret = ndb_save_exporter_secret;
    s->create_snapshot = ndb_create_snapshot;
    s->rollback_snapshot = ndb_rollback_snapshot;
    s->release_snapshot = ndb_release_snapshot;
    s->prune_expired_snapshots = ndb_prune_expired;
    s->mls_store = ndb_mls_store;
    s->mls_load = ndb_mls_load;
    s->mls_delete = ndb_mls_delete;
    s->is_persistent = ndb_is_persistent;
    s->destroy = ndb_destroy;

    return s;
}

#endif /* HAVE_NOSTRDB */
