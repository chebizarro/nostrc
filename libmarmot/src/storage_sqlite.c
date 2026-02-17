/*
 * libmarmot - SQLite storage backend
 *
 * Persistent storage implementation using SQLite3.
 * Optionally supports SQLCipher encryption via the encryption_key parameter.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-storage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sqlite3.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Schema version + migration
 * ──────────────────────────────────────────────────────────────────────── */

#define CURRENT_SCHEMA_VERSION 1

static const char *SCHEMA_V1 =
    /* Groups */
    "CREATE TABLE IF NOT EXISTS groups ("
    "  mls_group_id     BLOB PRIMARY KEY,"
    "  nostr_group_id   BLOB NOT NULL,"
    "  name             TEXT,"
    "  description      TEXT,"
    "  image_hash       BLOB,"
    "  image_key        BLOB,"
    "  image_nonce      BLOB,"
    "  admin_pubkeys    BLOB,"        /* concatenated 32-byte pubkeys */
    "  admin_count      INTEGER DEFAULT 0,"
    "  last_message_id  TEXT,"
    "  last_message_at  INTEGER DEFAULT 0,"
    "  last_message_processed_at INTEGER DEFAULT 0,"
    "  epoch            INTEGER DEFAULT 0,"
    "  state            INTEGER DEFAULT 0"
    ");"

    /* Messages */
    "CREATE TABLE IF NOT EXISTS messages ("
    "  id               BLOB PRIMARY KEY,"
    "  pubkey           BLOB NOT NULL,"
    "  kind             INTEGER NOT NULL,"
    "  mls_group_id     BLOB NOT NULL,"
    "  created_at       INTEGER NOT NULL,"
    "  processed_at     INTEGER NOT NULL,"
    "  content          TEXT,"
    "  tags_json        TEXT,"
    "  event_json       TEXT,"
    "  wrapper_event_id BLOB,"
    "  epoch            INTEGER DEFAULT 0,"
    "  state            INTEGER DEFAULT 0,"
    "  FOREIGN KEY (mls_group_id) REFERENCES groups(mls_group_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_group ON messages(mls_group_id, created_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_messages_wrapper ON messages(wrapper_event_id);"

    /* Welcomes */
    "CREATE TABLE IF NOT EXISTS welcomes ("
    "  id               BLOB PRIMARY KEY,"
    "  event_json       TEXT,"
    "  mls_group_id     BLOB,"
    "  nostr_group_id   BLOB,"
    "  group_name       TEXT,"
    "  group_description TEXT,"
    "  group_image_hash BLOB,"
    "  group_admin_pubkeys BLOB,"
    "  group_admin_count INTEGER DEFAULT 0,"
    "  group_relays     TEXT,"       /* JSON array of relay URLs */
    "  welcomer         BLOB,"
    "  member_count     INTEGER DEFAULT 0,"
    "  state            INTEGER DEFAULT 0,"
    "  wrapper_event_id BLOB"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_welcomes_state ON welcomes(state);"
    "CREATE INDEX IF NOT EXISTS idx_welcomes_wrapper ON welcomes(wrapper_event_id);"

    /* Processed messages/welcomes tracking */
    "CREATE TABLE IF NOT EXISTS processed_messages ("
    "  wrapper_event_id BLOB PRIMARY KEY,"
    "  message_event_id BLOB,"
    "  processed_at     INTEGER NOT NULL,"
    "  epoch            INTEGER DEFAULT 0,"
    "  mls_group_id     BLOB,"
    "  state            INTEGER DEFAULT 0,"
    "  failure_reason   TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS processed_welcomes ("
    "  wrapper_event_id BLOB PRIMARY KEY,"
    "  welcome_event_id BLOB,"
    "  processed_at     INTEGER NOT NULL,"
    "  state            INTEGER DEFAULT 0,"
    "  failure_reason   TEXT"
    ");"

    /* Group relays */
    "CREATE TABLE IF NOT EXISTS group_relays ("
    "  mls_group_id     BLOB NOT NULL,"
    "  relay_url        TEXT NOT NULL,"
    "  PRIMARY KEY (mls_group_id, relay_url),"
    "  FOREIGN KEY (mls_group_id) REFERENCES groups(mls_group_id)"
    ");"

    /* Exporter secrets */
    "CREATE TABLE IF NOT EXISTS exporter_secrets ("
    "  mls_group_id     BLOB NOT NULL,"
    "  epoch            INTEGER NOT NULL,"
    "  secret           BLOB NOT NULL,"
    "  PRIMARY KEY (mls_group_id, epoch)"
    ");"

    /* MLS key-value store (for internal MLS state) */
    "CREATE TABLE IF NOT EXISTS mls_store ("
    "  label            TEXT NOT NULL,"
    "  key              BLOB NOT NULL,"
    "  value            BLOB NOT NULL,"
    "  PRIMARY KEY (label, key)"
    ");"

    /* Snapshots (for commit race resolution) */
    "CREATE TABLE IF NOT EXISTS snapshots ("
    "  mls_group_id     BLOB NOT NULL,"
    "  name             TEXT NOT NULL,"
    "  data             BLOB NOT NULL,"
    "  created_at       INTEGER NOT NULL,"
    "  PRIMARY KEY (mls_group_id, name)"
    ");"

    /* Schema version tracking */
    "CREATE TABLE IF NOT EXISTS schema_version ("
    "  version INTEGER PRIMARY KEY"
    ");"
    "INSERT OR IGNORE INTO schema_version (version) VALUES (1);";

/* ──────────────────────────────────────────────────────────────────────────
 * Internal context
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    sqlite3 *db;
    char *path;
} SqliteCtx;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int
ensure_schema(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, SCHEMA_V1, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[marmot-sqlite] Schema init failed: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* Bind a MarmotGroupId as a BLOB */
static void
bind_group_id(sqlite3_stmt *stmt, int idx, const MarmotGroupId *gid)
{
    if (gid && gid->data && gid->len > 0)
        sqlite3_bind_blob(stmt, idx, gid->data, (int)gid->len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, idx);
}

/* Read a MarmotGroupId from a column */
static MarmotGroupId
read_group_id(sqlite3_stmt *stmt, int col)
{
    const void *data = sqlite3_column_blob(stmt, col);
    int len = sqlite3_column_bytes(stmt, col);
    if (!data || len <= 0) {
        MarmotGroupId empty = {0};
        return empty;
    }
    return marmot_group_id_new(data, (size_t)len);
}

/* Read a 32-byte fixed blob into a buffer */
static void
read_fixed32(sqlite3_stmt *stmt, int col, uint8_t out[32])
{
    const void *data = sqlite3_column_blob(stmt, col);
    int len = sqlite3_column_bytes(stmt, col);
    if (data && len >= 32)
        memcpy(out, data, 32);
    else
        memset(out, 0, 32);
}

static char *
read_text(sqlite3_stmt *stmt, int col)
{
    const char *txt = (const char *)sqlite3_column_text(stmt, col);
    return txt ? strdup(txt) : NULL;
}

/* ── Row → Group ───────────────────────────────────────────────────────── */

static MarmotGroup *
group_from_row(sqlite3_stmt *stmt)
{
    MarmotGroup *g = marmot_group_new();
    if (!g) return NULL;

    g->mls_group_id = read_group_id(stmt, 0);
    read_fixed32(stmt, 1, g->nostr_group_id);
    g->name = read_text(stmt, 2);
    g->description = read_text(stmt, 3);

    /* image_hash (col 4), image_key (5), image_nonce (6) */
    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        g->image_hash = malloc(32);
        if (g->image_hash) read_fixed32(stmt, 4, g->image_hash);
    }
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
        g->image_key = malloc(32);
        if (g->image_key) read_fixed32(stmt, 5, g->image_key);
    }
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
        g->image_nonce = malloc(12);
        if (g->image_nonce) {
            const void *d = sqlite3_column_blob(stmt, 6);
            int l = sqlite3_column_bytes(stmt, 6);
            if (d && l >= 12) memcpy(g->image_nonce, d, 12);
        }
    }

    /* admin_pubkeys (col 7), admin_count (8) */
    int admin_count = sqlite3_column_int(stmt, 8);
    if (admin_count > 0 && sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        const void *pk = sqlite3_column_blob(stmt, 7);
        int pk_len = sqlite3_column_bytes(stmt, 7);
        if (pk && pk_len >= admin_count * 32) {
            g->admin_pubkeys = malloc((size_t)admin_count * 32);
            if (g->admin_pubkeys) {
                memcpy(g->admin_pubkeys, pk, (size_t)admin_count * 32);
                g->admin_count = (size_t)admin_count;
            }
        }
    }

    g->last_message_id = read_text(stmt, 9);
    g->last_message_at = sqlite3_column_int64(stmt, 10);
    g->last_message_processed_at = sqlite3_column_int64(stmt, 11);
    g->epoch = (uint64_t)sqlite3_column_int64(stmt, 12);
    g->state = sqlite3_column_int(stmt, 13);
    return g;
}

/* ── Row → Message ─────────────────────────────────────────────────────── */

static MarmotMessage *
message_from_row(sqlite3_stmt *stmt)
{
    MarmotMessage *m = marmot_message_new();
    if (!m) return NULL;

    read_fixed32(stmt, 0, m->id);
    read_fixed32(stmt, 1, m->pubkey);
    m->kind = (uint32_t)sqlite3_column_int(stmt, 2);
    m->mls_group_id = read_group_id(stmt, 3);
    m->created_at = sqlite3_column_int64(stmt, 4);
    m->processed_at = sqlite3_column_int64(stmt, 5);
    m->content = read_text(stmt, 6);
    m->tags_json = read_text(stmt, 7);
    m->event_json = read_text(stmt, 8);
    read_fixed32(stmt, 9, m->wrapper_event_id);
    m->epoch = (uint64_t)sqlite3_column_int64(stmt, 10);
    m->state = sqlite3_column_int(stmt, 11);
    return m;
}

/* ── Row → Welcome ─────────────────────────────────────────────────────── */

static MarmotWelcome *
welcome_from_row(sqlite3_stmt *stmt)
{
    MarmotWelcome *w = marmot_welcome_new();
    if (!w) return NULL;

    read_fixed32(stmt, 0, w->id);
    w->event_json = read_text(stmt, 1);
    w->mls_group_id = read_group_id(stmt, 2);
    read_fixed32(stmt, 3, w->nostr_group_id);
    w->group_name = read_text(stmt, 4);
    w->group_description = read_text(stmt, 5);

    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
        w->group_image_hash = malloc(32);
        if (w->group_image_hash) read_fixed32(stmt, 6, w->group_image_hash);
    }

    int ac = sqlite3_column_int(stmt, 8);
    if (ac > 0 && sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        const void *pk = sqlite3_column_blob(stmt, 7);
        int pk_len = sqlite3_column_bytes(stmt, 7);
        if (pk && pk_len >= ac * 32) {
            w->group_admin_pubkeys = malloc((size_t)ac * 32);
            if (w->group_admin_pubkeys) {
                memcpy(w->group_admin_pubkeys, pk, (size_t)ac * 32);
                w->group_admin_count = (size_t)ac;
            }
        }
    }

    /* Relay URLs stored as tab-separated string */
    const char *relays_txt = (const char *)sqlite3_column_text(stmt, 9);
    if (relays_txt && *relays_txt) {
        /* Count tabs to determine relay count */
        size_t count = 1;
        for (const char *p = relays_txt; *p; p++) {
            if (*p == '\t') count++;
        }
        w->group_relays = calloc(count, sizeof(char *));
        if (w->group_relays) {
            char *copy = strdup(relays_txt);
            if (copy) {
                size_t idx = 0;
                char *saveptr = NULL;
                char *tok = strtok_r(copy, "\t", &saveptr);
                while (tok && idx < count) {
                    w->group_relays[idx++] = strdup(tok);
                    tok = strtok_r(NULL, "\t", &saveptr);
                }
                w->group_relay_count = idx;
                free(copy);
            }
        }
    }

    read_fixed32(stmt, 10, w->welcomer);
    w->member_count = (size_t)sqlite3_column_int(stmt, 11);
    w->state = sqlite3_column_int(stmt, 12);
    read_fixed32(stmt, 13, w->wrapper_event_id);
    return w;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Storage vtable implementations
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Group operations ──────────────────────────────────────────────────── */

static MarmotError
sql_all_groups(void *ctx, MarmotGroup ***out, size_t *out_count)
{
    SqliteCtx *sc = ctx;
    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db, "SELECT * FROM groups", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    /* First pass: count rows */
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) count++;
    sqlite3_reset(stmt);

    if (count == 0) {
        sqlite3_finalize(stmt);
        return MARMOT_OK;
    }

    *out = calloc(count, sizeof(MarmotGroup *));
    if (!*out) { sqlite3_finalize(stmt); return MARMOT_ERR_MEMORY; }

    size_t i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < count) {
        (*out)[i] = group_from_row(stmt);
        if (!(*out)[i]) {
            for (size_t j = 0; j < i; j++) marmot_group_free((*out)[j]);
            free(*out); *out = NULL;
            sqlite3_finalize(stmt);
            return MARMOT_ERR_MEMORY;
        }
        i++;
    }
    *out_count = i;
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_find_group_by_mls_id(void *ctx, const MarmotGroupId *gid, MarmotGroup **out)
{
    SqliteCtx *sc = ctx;
    *out = NULL;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT * FROM groups WHERE mls_group_id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = group_from_row(stmt);
    }
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_find_group_by_nostr_id(void *ctx, const uint8_t nostr_id[32], MarmotGroup **out)
{
    SqliteCtx *sc = ctx;
    *out = NULL;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT * FROM groups WHERE nostr_group_id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, nostr_id, 32, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = group_from_row(stmt);
    }
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_save_group(void *ctx, const MarmotGroup *group)
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO groups "
        "(mls_group_id, nostr_group_id, name, description, "
        " image_hash, image_key, image_nonce, "
        " admin_pubkeys, admin_count, "
        " last_message_id, last_message_at, last_message_processed_at, "
        " epoch, state) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, &group->mls_group_id);
    sqlite3_bind_blob(stmt, 2, group->nostr_group_id, 32, SQLITE_TRANSIENT);
    if (group->name) sqlite3_bind_text(stmt, 3, group->name, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 3);
    if (group->description) sqlite3_bind_text(stmt, 4, group->description, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 4);

    if (group->image_hash) sqlite3_bind_blob(stmt, 5, group->image_hash, 32, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 5);
    if (group->image_key) sqlite3_bind_blob(stmt, 6, group->image_key, 32, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 6);
    if (group->image_nonce) sqlite3_bind_blob(stmt, 7, group->image_nonce, 12, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 7);

    if (group->admin_count > 0 && group->admin_pubkeys)
        sqlite3_bind_blob(stmt, 8, group->admin_pubkeys, (int)(group->admin_count * 32), SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int(stmt, 9, (int)group->admin_count);

    if (group->last_message_id) sqlite3_bind_text(stmt, 10, group->last_message_id, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 10);
    sqlite3_bind_int64(stmt, 11, group->last_message_at);
    sqlite3_bind_int64(stmt, 12, group->last_message_processed_at);
    sqlite3_bind_int64(stmt, 13, (int64_t)group->epoch);
    sqlite3_bind_int(stmt, 14, group->state);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

/* ── Message operations ────────────────────────────────────────────────── */

static MarmotError
sql_messages(void *ctx, const MarmotGroupId *gid,
             const MarmotPagination *pg,
             MarmotMessage ***out, size_t *out_count)
{
    SqliteCtx *sc = ctx;
    *out = NULL;
    *out_count = 0;

    size_t limit = pg ? pg->limit : 1000;
    size_t offset = pg ? pg->offset : 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT * FROM messages WHERE mls_group_id = ? "
        "ORDER BY created_at DESC LIMIT ? OFFSET ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);
    sqlite3_bind_int64(stmt, 2, (int64_t)limit);
    sqlite3_bind_int64(stmt, 3, (int64_t)offset);

    /* Collect results */
    size_t cap = 64;
    MarmotMessage **arr = calloc(cap, sizeof(MarmotMessage *));
    if (!arr) { sqlite3_finalize(stmt); return MARMOT_ERR_MEMORY; }

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            MarmotMessage **new_arr = realloc(arr, cap * sizeof(MarmotMessage *));
            if (!new_arr) goto oom;
            arr = new_arr;
        }
        arr[count] = message_from_row(stmt);
        if (!arr[count]) goto oom;
        count++;
    }

    sqlite3_finalize(stmt);
    *out = arr;
    *out_count = count;
    return MARMOT_OK;

oom:
    for (size_t i = 0; i < count; i++) marmot_message_free(arr[i]);
    free(arr);
    sqlite3_finalize(stmt);
    return MARMOT_ERR_MEMORY;
}

static MarmotError
sql_last_message(void *ctx, const MarmotGroupId *gid,
                  MarmotSortOrder order, MarmotMessage **out)
{
    SqliteCtx *sc = ctx;
    *out = NULL;

    const char *sql = (order == MARMOT_SORT_PROCESSED_AT_FIRST)
        ? "SELECT * FROM messages WHERE mls_group_id = ? ORDER BY processed_at DESC LIMIT 1"
        : "SELECT * FROM messages WHERE mls_group_id = ? ORDER BY created_at DESC LIMIT 1";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = message_from_row(stmt);
    }
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_save_message(void *ctx, const MarmotMessage *msg)
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO messages "
        "(id, pubkey, kind, mls_group_id, created_at, processed_at, "
        " content, tags_json, event_json, wrapper_event_id, epoch, state) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, msg->id, 32, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, msg->pubkey, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)msg->kind);
    bind_group_id(stmt, 4, &msg->mls_group_id);
    sqlite3_bind_int64(stmt, 5, msg->created_at);
    sqlite3_bind_int64(stmt, 6, msg->processed_at);
    if (msg->content) sqlite3_bind_text(stmt, 7, msg->content, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 7);
    if (msg->tags_json) sqlite3_bind_text(stmt, 8, msg->tags_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 8);
    if (msg->event_json) sqlite3_bind_text(stmt, 9, msg->event_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 9);
    sqlite3_bind_blob(stmt, 10, msg->wrapper_event_id, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 11, (int64_t)msg->epoch);
    sqlite3_bind_int(stmt, 12, msg->state);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

static MarmotError
sql_find_message_by_id(void *ctx, const uint8_t event_id[32], MarmotMessage **out)
{
    SqliteCtx *sc = ctx;
    *out = NULL;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT * FROM messages WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, event_id, 32, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = message_from_row(stmt);
    }
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_is_message_processed(void *ctx, const uint8_t wrapper_id[32], bool *out)
{
    SqliteCtx *sc = ctx;
    *out = false;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT 1 FROM processed_messages WHERE wrapper_event_id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, wrapper_id, 32, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) *out = true;
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_save_processed_message(void *ctx, const uint8_t wrapper_id[32],
                            const uint8_t *msg_id, int64_t processed_at,
                            uint64_t epoch, const MarmotGroupId *gid,
                            int state, const char *reason)
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO processed_messages "
        "(wrapper_event_id, message_event_id, processed_at, epoch, mls_group_id, state, failure_reason) "
        "VALUES (?,?,?,?,?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, wrapper_id, 32, SQLITE_TRANSIENT);
    if (msg_id) sqlite3_bind_blob(stmt, 2, msg_id, 32, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, processed_at);
    sqlite3_bind_int64(stmt, 4, (int64_t)epoch);
    bind_group_id(stmt, 5, gid);
    sqlite3_bind_int(stmt, 6, state);
    if (reason) sqlite3_bind_text(stmt, 7, reason, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 7);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

/* ── Welcome operations ────────────────────────────────────────────────── */

static MarmotError
sql_save_welcome(void *ctx, const MarmotWelcome *w)
{
    SqliteCtx *sc = ctx;

    /* Serialize relays as tab-separated string */
    char *relay_str = NULL;
    if (w->group_relay_count > 0 && w->group_relays) {
        size_t total = 0;
        for (size_t i = 0; i < w->group_relay_count; i++) {
            if (w->group_relays[i]) total += strlen(w->group_relays[i]) + 1;
        }
        relay_str = malloc(total + 1);
        if (relay_str) {
            char *p = relay_str;
            for (size_t i = 0; i < w->group_relay_count; i++) {
                if (w->group_relays[i]) {
                    if (p != relay_str) *p++ = '\t';
                    size_t len = strlen(w->group_relays[i]);
                    memcpy(p, w->group_relays[i], len);
                    p += len;
                }
            }
            *p = '\0';
        }
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO welcomes "
        "(id, event_json, mls_group_id, nostr_group_id, "
        " group_name, group_description, group_image_hash, "
        " group_admin_pubkeys, group_admin_count, group_relays, "
        " welcomer, member_count, state, wrapper_event_id) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) { free(relay_str); return MARMOT_ERR_STORAGE; }

    sqlite3_bind_blob(stmt, 1, w->id, 32, SQLITE_TRANSIENT);
    if (w->event_json) sqlite3_bind_text(stmt, 2, w->event_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 2);
    bind_group_id(stmt, 3, &w->mls_group_id);
    sqlite3_bind_blob(stmt, 4, w->nostr_group_id, 32, SQLITE_TRANSIENT);
    if (w->group_name) sqlite3_bind_text(stmt, 5, w->group_name, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 5);
    if (w->group_description) sqlite3_bind_text(stmt, 6, w->group_description, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 6);
    if (w->group_image_hash) sqlite3_bind_blob(stmt, 7, w->group_image_hash, 32, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 7);
    if (w->group_admin_count > 0 && w->group_admin_pubkeys)
        sqlite3_bind_blob(stmt, 8, w->group_admin_pubkeys, (int)(w->group_admin_count * 32), SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int(stmt, 9, (int)w->group_admin_count);
    if (relay_str) sqlite3_bind_text(stmt, 10, relay_str, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 10);
    sqlite3_bind_blob(stmt, 11, w->welcomer, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 12, (int)w->member_count);
    sqlite3_bind_int(stmt, 13, w->state);
    sqlite3_bind_blob(stmt, 14, w->wrapper_event_id, 32, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(relay_str);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

static MarmotError
sql_find_welcome_by_event_id(void *ctx, const uint8_t event_id[32], MarmotWelcome **out)
{
    SqliteCtx *sc = ctx;
    *out = NULL;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT * FROM welcomes WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, event_id, 32, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = welcome_from_row(stmt);
    }
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_pending_welcomes(void *ctx, const MarmotPagination *pg,
                      MarmotWelcome ***out, size_t *out_count)
{
    SqliteCtx *sc = ctx;
    *out = NULL;
    *out_count = 0;

    size_t limit = pg ? pg->limit : 1000;
    size_t offset = pg ? pg->offset : 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT * FROM welcomes WHERE state = ? LIMIT ? OFFSET ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_int(stmt, 1, MARMOT_WELCOME_STATE_PENDING);
    sqlite3_bind_int64(stmt, 2, (int64_t)limit);
    sqlite3_bind_int64(stmt, 3, (int64_t)offset);

    size_t cap = 16;
    MarmotWelcome **arr = calloc(cap, sizeof(MarmotWelcome *));
    if (!arr) { sqlite3_finalize(stmt); return MARMOT_ERR_MEMORY; }

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            MarmotWelcome **new_arr = realloc(arr, cap * sizeof(MarmotWelcome *));
            if (!new_arr) goto oom;
            arr = new_arr;
        }
        arr[count] = welcome_from_row(stmt);
        if (!arr[count]) goto oom;
        count++;
    }

    sqlite3_finalize(stmt);
    *out = arr;
    *out_count = count;
    return MARMOT_OK;

oom:
    for (size_t i = 0; i < count; i++) marmot_welcome_free(arr[i]);
    free(arr);
    sqlite3_finalize(stmt);
    return MARMOT_ERR_MEMORY;
}

static MarmotError
sql_find_processed_welcome(void *ctx, const uint8_t wrapper_id[32],
                            bool *found, int *state, char **reason)
{
    SqliteCtx *sc = ctx;
    *found = false;
    *state = 0;
    *reason = NULL;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT state, failure_reason FROM processed_welcomes WHERE wrapper_event_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, wrapper_id, 32, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *found = true;
        *state = sqlite3_column_int(stmt, 0);
        *reason = read_text(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_save_processed_welcome(void *ctx, const uint8_t wrapper_id[32],
                            const uint8_t *welcome_id, int64_t processed_at,
                            int state, const char *reason)
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO processed_welcomes "
        "(wrapper_event_id, welcome_event_id, processed_at, state, failure_reason) "
        "VALUES (?,?,?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_blob(stmt, 1, wrapper_id, 32, SQLITE_TRANSIENT);
    if (welcome_id) sqlite3_bind_blob(stmt, 2, welcome_id, 32, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, processed_at);
    sqlite3_bind_int(stmt, 4, state);
    if (reason) sqlite3_bind_text(stmt, 5, reason, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 5);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

/* ── Relay operations ──────────────────────────────────────────────────── */

static MarmotError
sql_group_relays(void *ctx, const MarmotGroupId *gid,
                  MarmotGroupRelay **out, size_t *out_count)
{
    SqliteCtx *sc = ctx;
    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT relay_url FROM group_relays WHERE mls_group_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);

    size_t cap = 8;
    MarmotGroupRelay *arr = calloc(cap, sizeof(MarmotGroupRelay));
    if (!arr) { sqlite3_finalize(stmt); return MARMOT_ERR_MEMORY; }

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            MarmotGroupRelay *new_arr = realloc(arr, cap * sizeof(MarmotGroupRelay));
            if (!new_arr) { free(arr); sqlite3_finalize(stmt); return MARMOT_ERR_MEMORY; }
            arr = new_arr;
        }
        const char *url = (const char *)sqlite3_column_text(stmt, 0);
        arr[count].relay_url = url ? strdup(url) : NULL;
        count++;
    }

    sqlite3_finalize(stmt);
    *out = arr;
    *out_count = count;
    return MARMOT_OK;
}

static MarmotError
sql_replace_group_relays(void *ctx, const MarmotGroupId *gid,
                          const char **urls, size_t count)
{
    SqliteCtx *sc = ctx;

    /* Delete existing */
    sqlite3_stmt *del;
    int rc = sqlite3_prepare_v2(sc->db,
        "DELETE FROM group_relays WHERE mls_group_id = ?", -1, &del, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;
    bind_group_id(del, 1, gid);
    sqlite3_step(del);
    sqlite3_finalize(del);

    /* Insert new */
    if (count > 0 && urls) {
        sqlite3_stmt *ins;
        rc = sqlite3_prepare_v2(sc->db,
            "INSERT INTO group_relays (mls_group_id, relay_url) VALUES (?,?)",
            -1, &ins, NULL);
        if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

        for (size_t i = 0; i < count; i++) {
            bind_group_id(ins, 1, gid);
            sqlite3_bind_text(ins, 2, urls[i], -1, SQLITE_TRANSIENT);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
    }
    return MARMOT_OK;
}

/* ── Exporter secret operations ────────────────────────────────────────── */

static MarmotError
sql_get_exporter_secret(void *ctx, const MarmotGroupId *gid,
                         uint64_t epoch, uint8_t out[32])
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT secret FROM exporter_secrets WHERE mls_group_id = ? AND epoch = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);
    sqlite3_bind_int64(stmt, 2, (int64_t)epoch);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(stmt, 0);
        int len = sqlite3_column_bytes(stmt, 0);
        if (data && len >= 32) {
            memcpy(out, data, 32);
            sqlite3_finalize(stmt);
            return MARMOT_OK;
        }
    }

    sqlite3_finalize(stmt);
    return MARMOT_ERR_STORAGE_NOT_FOUND;
}

static MarmotError
sql_save_exporter_secret(void *ctx, const MarmotGroupId *gid,
                          uint64_t epoch, const uint8_t secret[32])
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO exporter_secrets (mls_group_id, epoch, secret) "
        "VALUES (?,?,?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);
    sqlite3_bind_int64(stmt, 2, (int64_t)epoch);
    sqlite3_bind_blob(stmt, 3, secret, 32, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

/* ── Snapshot operations ───────────────────────────────────────────────── */

static MarmotError
sql_create_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{
    SqliteCtx *sc = ctx;

    /* Serialize group state (MLS key store entries for this group) */
    /* For now, just mark the snapshot with timestamp */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO snapshots (mls_group_id, name, data, created_at) "
        "VALUES (?, ?, X'00', strftime('%s','now'))", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

static MarmotError
sql_rollback_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{
    SqliteCtx *sc = ctx;

    /* Delete the snapshot after use */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "DELETE FROM snapshots WHERE mls_group_id = ? AND name = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    bind_group_id(stmt, 1, gid);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

static MarmotError
sql_release_snapshot(void *ctx, const MarmotGroupId *gid, const char *name)
{
    return sql_rollback_snapshot(ctx, gid, name);
}

static MarmotError
sql_prune_expired(void *ctx, uint64_t min_ts, size_t *out)
{
    SqliteCtx *sc = ctx;
    *out = 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "DELETE FROM snapshots WHERE created_at < ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_int64(stmt, 1, (int64_t)min_ts);
    sqlite3_step(stmt);
    *out = (size_t)sqlite3_changes(sc->db);
    sqlite3_finalize(stmt);
    return MARMOT_OK;
}

/* ── MLS key store ─────────────────────────────────────────────────────── */

static MarmotError
sql_mls_store(void *ctx, const char *label,
               const uint8_t *key, size_t key_len,
               const uint8_t *value, size_t value_len)
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "INSERT OR REPLACE INTO mls_store (label, key, value) VALUES (?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_text(stmt, 1, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, key, (int)key_len, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, value, (int)value_len, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

static MarmotError
sql_mls_load(void *ctx, const char *label,
              const uint8_t *key, size_t key_len,
              uint8_t **out, size_t *out_len)
{
    SqliteCtx *sc = ctx;
    *out = NULL;
    *out_len = 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "SELECT value FROM mls_store WHERE label = ? AND key = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_text(stmt, 1, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, key, (int)key_len, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(stmt, 0);
        int len = sqlite3_column_bytes(stmt, 0);
        if (data && len > 0) {
            *out = malloc((size_t)len);
            if (*out) {
                memcpy(*out, data, (size_t)len);
                *out_len = (size_t)len;
            } else {
                sqlite3_finalize(stmt);
                return MARMOT_ERR_MEMORY;
            }
        }
        sqlite3_finalize(stmt);
        return MARMOT_OK;
    }

    sqlite3_finalize(stmt);
    return MARMOT_ERR_STORAGE_NOT_FOUND;
}

static MarmotError
sql_mls_delete(void *ctx, const char *label,
                const uint8_t *key, size_t key_len)
{
    SqliteCtx *sc = ctx;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sc->db,
        "DELETE FROM mls_store WHERE label = ? AND key = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return MARMOT_ERR_STORAGE;

    sqlite3_bind_text(stmt, 1, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, key, (int)key_len, SQLITE_TRANSIENT);
    sqlite3_step(stmt);

    int changes = sqlite3_changes(sc->db);
    sqlite3_finalize(stmt);
    return (changes > 0) ? MARMOT_OK : MARMOT_ERR_STORAGE_NOT_FOUND;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

static bool sql_is_persistent(void *ctx) { (void)ctx; return true; }

static void
sql_destroy(void *ctx)
{
    SqliteCtx *sc = ctx;
    if (sc->db) sqlite3_close(sc->db);
    free(sc->path);
    free(sc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public constructor
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotStorage *
marmot_storage_sqlite_new(const char *path, const char *encryption_key)
{
    if (!path) return NULL;

    SqliteCtx *sc = calloc(1, sizeof(SqliteCtx));
    if (!sc) return NULL;
    sc->path = strdup(path);

    int rc = sqlite3_open(path, &sc->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[marmot-sqlite] sqlite3_open(%s) failed: %s\n",
                path, sqlite3_errmsg(sc->db));
        sqlite3_close(sc->db);
        free(sc->path);
        free(sc);
        return NULL;
    }

    /* Enable WAL mode for better concurrency */
    sqlite3_exec(sc->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(sc->db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);

    /* SQLCipher encryption (if key provided and SQLCipher is linked) */
    if (encryption_key && *encryption_key) {
#ifdef SQLITE_HAS_CODEC
        rc = sqlite3_key(sc->db, encryption_key, (int)strlen(encryption_key));
        if (rc != SQLITE_OK) {
            fprintf(stderr, "[marmot-sqlite] Encryption key failed\n");
            sqlite3_close(sc->db);
            free(sc->path);
            free(sc);
            return NULL;
        }
#else
        fprintf(stderr, "[marmot-sqlite] Encryption requested but SQLCipher not linked\n");
#endif
    }

    /* Initialize schema */
    if (ensure_schema(sc->db) != 0) {
        sqlite3_close(sc->db);
        free(sc->path);
        free(sc);
        return NULL;
    }

    MarmotStorage *s = calloc(1, sizeof(MarmotStorage));
    if (!s) {
        sqlite3_close(sc->db);
        free(sc->path);
        free(sc);
        return NULL;
    }

    s->ctx = sc;

    /* Group ops */
    s->all_groups = sql_all_groups;
    s->find_group_by_mls_id = sql_find_group_by_mls_id;
    s->find_group_by_nostr_id = sql_find_group_by_nostr_id;
    s->save_group = sql_save_group;
    s->messages = sql_messages;
    s->last_message = sql_last_message;

    /* Message ops */
    s->save_message = sql_save_message;
    s->find_message_by_id = sql_find_message_by_id;
    s->is_message_processed = sql_is_message_processed;
    s->save_processed_message = sql_save_processed_message;

    /* Welcome ops */
    s->save_welcome = sql_save_welcome;
    s->find_welcome_by_event_id = sql_find_welcome_by_event_id;
    s->pending_welcomes = sql_pending_welcomes;
    s->find_processed_welcome = sql_find_processed_welcome;
    s->save_processed_welcome = sql_save_processed_welcome;

    /* Relay ops */
    s->group_relays = sql_group_relays;
    s->replace_group_relays = sql_replace_group_relays;

    /* Exporter secret ops */
    s->get_exporter_secret = sql_get_exporter_secret;
    s->save_exporter_secret = sql_save_exporter_secret;

    /* Snapshot ops */
    s->create_snapshot = sql_create_snapshot;
    s->rollback_snapshot = sql_rollback_snapshot;
    s->release_snapshot = sql_release_snapshot;
    s->prune_expired_snapshots = sql_prune_expired;

    /* MLS key store */
    s->mls_store = sql_mls_store;
    s->mls_load = sql_mls_load;
    s->mls_delete = sql_mls_delete;

    /* Lifecycle */
    s->is_persistent = sql_is_persistent;
    s->destroy = sql_destroy;

    return s;
}
