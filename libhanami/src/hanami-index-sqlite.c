/*
 * hanami-index.c - OID ↔ Blossom SHA-256 mapping index (SQLite backend)
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-index.h"
#include "hanami-index-private.h"
#include <porthome/crypto.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

/* =========================================================================
 * Internal structures
 * ========================================================================= */

static const porthome_index_ops index_ops;

struct hanami_index {
    porthome_index portable;
    sqlite3 *db;
    /* Prepared statements */
    sqlite3_stmt *stmt_put;
    sqlite3_stmt *stmt_get_by_oid;
    sqlite3_stmt *stmt_get_by_blossom;
    sqlite3_stmt *stmt_exists;
    sqlite3_stmt *stmt_delete;
    sqlite3_stmt *stmt_count;
};

/* =========================================================================
 * Schema
 * ========================================================================= */

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS oid_map ("
    "  git_oid      TEXT PRIMARY KEY NOT NULL,"
    "  blossom_hash TEXT NOT NULL,"
    "  obj_type     INTEGER NOT NULL,"
    "  obj_size     INTEGER NOT NULL,"
    "  timestamp    INTEGER NOT NULL,"
    "  chunk_key_id INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_blossom_hash ON oid_map(blossom_hash);"
    "CREATE TABLE IF NOT EXISTS hanami_meta ("
    "  key   TEXT PRIMARY KEY NOT NULL,"
    "  value TEXT NOT NULL"
    ");"
    /* OIDs covered by the published manifest chain (vs local-only rows). */
    "CREATE TABLE IF NOT EXISTS manifest_rows ("
    "  git_oid TEXT PRIMARY KEY NOT NULL"
    ");";

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
#ifdef _WIN32
    return mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static const char *git_type_str(git_object_t type)
{
    switch (type) {
    case GIT_OBJECT_BLOB:   return "blob";
    case GIT_OBJECT_TREE:   return "tree";
    case GIT_OBJECT_COMMIT: return "commit";
    case GIT_OBJECT_TAG:    return "tag";
    default:                return "blob";
    }
}

static void bytes_to_hex(const unsigned char *bytes, size_t len,
                         char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[(bytes[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[bytes[i] & 0xf];
    }
    out[len * 2] = '\0';
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

hanami_error_t hanami_index_open(hanami_index_t **out,
                                 const char *path,
                                 const char *backend)
{
    if (!out || !path)
        return HANAMI_ERR_INVALID_ARG;

    /* Only sqlite backend is supported (LMDB is gated by HANAMI_HAVE_LMDB) */
    if (backend && strcmp(backend, "sqlite") != 0 && strcmp(backend, "") != 0) {
#if HANAMI_HAVE_LMDB
        if (strcmp(backend, "lmdb") == 0) {
            /* LMDB backend is a compile-time option not yet wired */
            return HANAMI_ERR_INVALID_ARG;
        }
#endif
        return HANAMI_ERR_INVALID_ARG;
    }

    *out = NULL;

    /* Determine actual DB path — handle :memory: specially */
    if (strcmp(path, ":memory:") == 0)
        return hanami_index_open_file(out, ":memory:");

    if (ensure_directory(path) != 0 && errno != EEXIST) {
        /* Try anyway — maybe the directory already exists */
    }
    size_t path_len = strlen(path);
    char *db_path = malloc(path_len + 20);
    if (!db_path)
        return HANAMI_ERR_NOMEM;
    snprintf(db_path, path_len + 20, "%s/hanami_index.db", path);
    hanami_error_t err = hanami_index_open_file(out, db_path);
    free(db_path);
    return err;
}

/* Schema version 1 adds hanami_meta (schema_version, manifest head). Older
 * unversioned databases have the same oid_map and are stamped on open;
 * newer versions are refused rather than misread. */
static hanami_error_t check_schema_version(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM hanami_meta WHERE key = 'schema_version'",
                           -1, &s, NULL) != SQLITE_OK)
        return HANAMI_ERR_INDEX;
    int rc = sqlite3_step(s);
    long version = rc == SQLITE_ROW
        ? strtol((const char *)sqlite3_column_text(s, 0), NULL, 10) : 0;
    sqlite3_finalize(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return HANAMI_ERR_INDEX;
    if (version > HANAMI_INDEX_SCHEMA_VERSION) return HANAMI_ERR_INDEX;
    if (version == HANAMI_INDEX_SCHEMA_VERSION) return HANAMI_OK;
    char sql[128];
    snprintf(sql, sizeof(sql),
             "INSERT OR REPLACE INTO hanami_meta (key, value) VALUES ('schema_version', '%d')",
             HANAMI_INDEX_SCHEMA_VERSION);
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? HANAMI_OK : HANAMI_ERR_INDEX;
}

hanami_error_t hanami_index_open_file(hanami_index_t **out, const char *db_path)
{
    if (!out || !db_path)
        return HANAMI_ERR_INVALID_ARG;
    *out = NULL;

    hanami_index_t *idx = calloc(1, sizeof(*idx));
    if (!idx)
        return HANAMI_ERR_NOMEM;

    int rc = sqlite3_open(db_path, &idx->db);
    if (rc != SQLITE_OK) {
        sqlite3_close(idx->db);
        free(idx);
        return HANAMI_ERR_INDEX;
    }

    /* Enable WAL mode for concurrency */
    sqlite3_exec(idx->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(idx->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    /* Create schema */
    char *err_msg = NULL;
    rc = sqlite3_exec(idx->db, SCHEMA_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        sqlite3_free(err_msg);
        sqlite3_close(idx->db);
        free(idx);
        return HANAMI_ERR_INDEX;
    }

    /* Preserve databases created before the storage-independent index type.
     * Only the added key id is migrated; raw-object rows keep key id zero. */
    sqlite3_stmt *columns = NULL;
    int has_key_id = 0;
    rc = sqlite3_prepare_v2(idx->db, "PRAGMA table_info(oid_map)", -1, &columns, NULL);
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(columns)) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(columns, 1);
            if (name && strcmp(name, "chunk_key_id") == 0) has_key_id = 1;
        }
    }
    sqlite3_finalize(columns);
    if (rc != SQLITE_DONE || (!has_key_id && sqlite3_exec(idx->db,
        "ALTER TABLE oid_map ADD COLUMN chunk_key_id INTEGER NOT NULL DEFAULT 0",
        NULL, NULL, NULL) != SQLITE_OK)) {
        hanami_index_close(idx);
        return HANAMI_ERR_INDEX;
    }
    if (check_schema_version(idx->db) != HANAMI_OK) {
        hanami_index_close(idx);
        return HANAMI_ERR_INDEX;
    }
    idx->portable = (porthome_index){ .ctx = idx, .ops = &index_ops };

    /* Prepare statements */
    const char *sql_put =
        "INSERT OR REPLACE INTO oid_map "
        "(git_oid, blossom_hash, obj_type, obj_size, timestamp, chunk_key_id) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    const char *sql_get_oid =
        "SELECT git_oid, blossom_hash, obj_type, obj_size, timestamp, chunk_key_id "
        "FROM oid_map WHERE git_oid = ?;";
    const char *sql_get_blossom =
        "SELECT git_oid, blossom_hash, obj_type, obj_size, timestamp, chunk_key_id "
        "FROM oid_map WHERE blossom_hash = ? ORDER BY git_oid LIMIT 2;";
    const char *sql_exists =
        "SELECT 1 FROM oid_map WHERE git_oid = ?;";
    const char *sql_delete =
        "DELETE FROM oid_map WHERE git_oid = ?;";
    const char *sql_count =
        "SELECT COUNT(*) FROM oid_map;";

    if (sqlite3_prepare_v2(idx->db, sql_put, -1, &idx->stmt_put, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(idx->db, sql_get_oid, -1, &idx->stmt_get_by_oid, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(idx->db, sql_get_blossom, -1, &idx->stmt_get_by_blossom, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(idx->db, sql_exists, -1, &idx->stmt_exists, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(idx->db, sql_delete, -1, &idx->stmt_delete, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(idx->db, sql_count, -1, &idx->stmt_count, NULL) != SQLITE_OK)
    {
        hanami_index_close(idx);
        return HANAMI_ERR_INDEX;
    }

    *out = idx;
    return HANAMI_OK;
}

void hanami_index_close(hanami_index_t *idx)
{
    if (!idx)
        return;

    if (idx->stmt_put) sqlite3_finalize(idx->stmt_put);
    if (idx->stmt_get_by_oid) sqlite3_finalize(idx->stmt_get_by_oid);
    if (idx->stmt_get_by_blossom) sqlite3_finalize(idx->stmt_get_by_blossom);
    if (idx->stmt_exists) sqlite3_finalize(idx->stmt_exists);
    if (idx->stmt_delete) sqlite3_finalize(idx->stmt_delete);
    if (idx->stmt_count) sqlite3_finalize(idx->stmt_count);

    if (idx->db)
        sqlite3_close(idx->db);

    free(idx);
}

/* =========================================================================
 * CRUD
 * ========================================================================= */

hanami_error_t hanami_index_put(hanami_index_t *idx,
                                const hanami_index_entry_t *entry)
{
    if (!idx || !entry)
        return HANAMI_ERR_INVALID_ARG;
    if (entry->git_oid[0] == '\0' || entry->blossom_hash[0] == '\0')
        return HANAMI_ERR_INVALID_ARG;

    sqlite3_stmt *s = idx->stmt_put;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, entry->git_oid, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, entry->blossom_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 3, (int)entry->type);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)entry->size);
    sqlite3_bind_int64(s, 5, entry->timestamp);
    sqlite3_bind_int64(s, 6, entry->chunk_key_id);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    return (rc == SQLITE_DONE) ? HANAMI_OK : HANAMI_ERR_INDEX;
}

static hanami_error_t fill_entry_from_row(sqlite3_stmt *s,
                                          hanami_index_entry_t *out)
{
    const char *oid = (const char *)sqlite3_column_text(s, 0);
    const char *bh  = (const char *)sqlite3_column_text(s, 1);
    if (!oid || !bh)
        return HANAMI_ERR_INDEX;

    memset(out, 0, sizeof(*out));
    strncpy(out->git_oid, oid, sizeof(out->git_oid) - 1);
    strncpy(out->blossom_hash, bh, sizeof(out->blossom_hash) - 1);
    out->type = (git_object_t)sqlite3_column_int(s, 2);
    out->size = (size_t)sqlite3_column_int64(s, 3);
    out->timestamp = sqlite3_column_int64(s, 4);
    sqlite3_int64 key_id = sqlite3_column_int64(s, 5);
    if (key_id < 0 || key_id > UINT32_MAX) return HANAMI_ERR_INDEX;
    out->chunk_key_id = (uint32_t)key_id;
    return HANAMI_OK;
}

hanami_error_t hanami_index_get_by_oid(hanami_index_t *idx,
                                       const char *git_oid_hex,
                                       hanami_index_entry_t *out)
{
    if (!idx || !git_oid_hex || !out)
        return HANAMI_ERR_INVALID_ARG;

    sqlite3_stmt *s = idx->stmt_get_by_oid;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, git_oid_hex, -1, SQLITE_STATIC);

    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        hanami_error_t err = fill_entry_from_row(s, out);
        sqlite3_reset(s);
        return err;
    }
    sqlite3_reset(s);
    return rc == SQLITE_DONE ? HANAMI_ERR_NOT_FOUND : HANAMI_ERR_INDEX;
}

hanami_error_t hanami_index_get_by_blossom(hanami_index_t *idx,
                                           const char *blossom_hash_hex,
                                           hanami_index_entry_t *out)
{
    if (!idx || !blossom_hash_hex || !out)
        return HANAMI_ERR_INVALID_ARG;

    sqlite3_stmt *s = idx->stmt_get_by_blossom;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, blossom_hash_hex, -1, SQLITE_STATIC);

    /* Raw-payload addresses are not unique object identifiers (the empty
     * blob and empty tree share one). Never pick an arbitrary row. */
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        hanami_error_t err = fill_entry_from_row(s, out);
        if (err == HANAMI_OK && sqlite3_step(s) != SQLITE_DONE)
            err = HANAMI_ERR_INDEX;
        sqlite3_reset(s);
        return err;
    }
    sqlite3_reset(s);
    return rc == SQLITE_DONE ? HANAMI_ERR_NOT_FOUND : HANAMI_ERR_INDEX;
}

bool hanami_index_exists(hanami_index_t *idx, const char *git_oid_hex)
{
    if (!idx || !git_oid_hex)
        return false;

    sqlite3_stmt *s = idx->stmt_exists;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, git_oid_hex, -1, SQLITE_STATIC);

    bool found = (sqlite3_step(s) == SQLITE_ROW);
    sqlite3_reset(s);
    return found;
}

hanami_error_t hanami_index_delete(hanami_index_t *idx,
                                   const char *git_oid_hex)
{
    if (!idx || !git_oid_hex)
        return HANAMI_ERR_INVALID_ARG;

    sqlite3_stmt *s = idx->stmt_delete;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, git_oid_hex, -1, SQLITE_STATIC);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    if (rc != SQLITE_DONE)
        return HANAMI_ERR_INDEX;

    return (sqlite3_changes(idx->db) > 0) ? HANAMI_OK : HANAMI_ERR_NOT_FOUND;
}

hanami_error_t hanami_index_meta_get(hanami_index_t *idx, const char *key, char **out)
{
    if (!idx || !key || !out) return HANAMI_ERR_INVALID_ARG;
    *out = NULL;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(idx->db, "SELECT value FROM hanami_meta WHERE key = ?",
                           -1, &s, NULL) != SQLITE_OK)
        return HANAMI_ERR_INDEX;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    hanami_error_t err = rc == SQLITE_DONE ? HANAMI_ERR_NOT_FOUND : HANAMI_ERR_INDEX;
    if (rc == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(s, 0);
        *out = value ? strdup(value) : NULL;
        err = *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
    }
    sqlite3_finalize(s);
    return err;
}

hanami_error_t hanami_index_meta_set(hanami_index_t *idx, const char *key, const char *value)
{
    if (!idx || !key || !value) return HANAMI_ERR_INVALID_ARG;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(idx->db,
            "INSERT OR REPLACE INTO hanami_meta (key, value) VALUES (?, ?)",
            -1, &s, NULL) != SQLITE_OK)
        return HANAMI_ERR_INDEX;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? HANAMI_OK : HANAMI_ERR_INDEX;
}

hanami_error_t hanami_index_mark_published(hanami_index_t *idx, const char *git_oid_hex)
{
    if (!idx || !git_oid_hex) return HANAMI_ERR_INVALID_ARG;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(idx->db, "INSERT OR IGNORE INTO manifest_rows (git_oid) VALUES (?)",
                           -1, &s, NULL) != SQLITE_OK)
        return HANAMI_ERR_INDEX;
    sqlite3_bind_text(s, 1, git_oid_hex, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? HANAMI_OK : HANAMI_ERR_INDEX;
}

hanami_error_t hanami_index_collect(hanami_index_t *idx, bool unpublished_only,
    size_t max_rows, hanami_index_entry_t **rows, size_t *count)
{
    if (!idx || !rows || !count) return HANAMI_ERR_INVALID_ARG;
    *rows = NULL; *count = 0;
    const char *sql = unpublished_only
        ? "SELECT git_oid, blossom_hash, obj_type, obj_size, timestamp, chunk_key_id "
          "FROM oid_map WHERE git_oid NOT IN (SELECT git_oid FROM manifest_rows) ORDER BY git_oid"
        : "SELECT git_oid, blossom_hash, obj_type, obj_size, timestamp, chunk_key_id "
          "FROM oid_map ORDER BY git_oid";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(idx->db, sql, -1, &s, NULL) != SQLITE_OK) return HANAMI_ERR_INDEX;
    hanami_error_t err = HANAMI_OK;
    size_t cap = 0;
    int rc;
    while (err == HANAMI_OK && (rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (*count == max_rows) { err = HANAMI_ERR_INDEX; break; }
        if (*count == cap) {
            size_t grown_cap = cap ? cap * 2 : 64;
            hanami_index_entry_t *grown = realloc(*rows, grown_cap * sizeof(**rows));
            if (!grown) { err = HANAMI_ERR_NOMEM; break; }
            *rows = grown; cap = grown_cap;
        }
        err = fill_entry_from_row(s, &(*rows)[*count]);
        if (err == HANAMI_OK) (*count)++;
    }
    if (err == HANAMI_OK && rc != SQLITE_DONE) err = HANAMI_ERR_INDEX;
    sqlite3_finalize(s);
    if (err != HANAMI_OK) { free(*rows); *rows = NULL; *count = 0; }
    return err;
}

static hanami_error_t exec_simple(hanami_index_t *idx, const char *sql)
{
    if (!idx) return HANAMI_ERR_INVALID_ARG;
    return sqlite3_exec(idx->db, sql, NULL, NULL, NULL) == SQLITE_OK
        ? HANAMI_OK : HANAMI_ERR_INDEX;
}

static hanami_error_t exec_oid(hanami_index_t *idx, const char *sql, const char *oid)
{
    if (!idx || !oid) return HANAMI_ERR_INVALID_ARG;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(idx->db, sql, -1, &s, NULL) != SQLITE_OK) return HANAMI_ERR_INDEX;
    sqlite3_bind_text(s, 1, oid, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? HANAMI_OK : HANAMI_ERR_INDEX;
}

hanami_error_t hanami_index_unmark_published(hanami_index_t *idx, const char *git_oid_hex)
{
    return exec_oid(idx, "DELETE FROM manifest_rows WHERE git_oid = ?", git_oid_hex);
}

hanami_error_t hanami_index_sync_begin(hanami_index_t *idx)
{
    hanami_error_t err = exec_simple(idx,
        "CREATE TEMP TABLE IF NOT EXISTS sync_rows (git_oid TEXT PRIMARY KEY NOT NULL)");
    return err == HANAMI_OK ? exec_simple(idx, "DELETE FROM sync_rows") : err;
}

hanami_error_t hanami_index_sync_note(hanami_index_t *idx, const char *git_oid_hex)
{
    return exec_oid(idx, "INSERT OR IGNORE INTO sync_rows (git_oid) VALUES (?)", git_oid_hex);
}

hanami_error_t hanami_index_sync_prune(hanami_index_t *idx)
{
    return exec_simple(idx,
        "DELETE FROM manifest_rows WHERE git_oid NOT IN (SELECT git_oid FROM sync_rows)");
}
hanami_error_t hanami_index_begin(hanami_index_t *idx) { return exec_simple(idx, "BEGIN IMMEDIATE"); }
hanami_error_t hanami_index_commit(hanami_index_t *idx) { return exec_simple(idx, "COMMIT"); }
void hanami_index_rollback(hanami_index_t *idx) { (void)exec_simple(idx, "ROLLBACK"); }

size_t hanami_index_count(hanami_index_t *idx)
{
    if (!idx)
        return 0;

    sqlite3_stmt *s = idx->stmt_count;
    sqlite3_reset(s);

    size_t count = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
        count = (size_t)sqlite3_column_int64(s, 0);
    sqlite3_reset(s);
    return count;
}

/* Storage adapter for the shared row; Git-only metadata stays in SQLite. */
static porthome_index_status index_status(hanami_error_t err)
{
    if (err == HANAMI_OK) return PORTHOME_INDEX_OK;
    if (err == HANAMI_ERR_NOT_FOUND) return PORTHOME_INDEX_NOT_FOUND;
    if (err == HANAMI_ERR_INVALID_ARG) return PORTHOME_INDEX_ERR_ARG;
    if (err == HANAMI_ERR_NOMEM) return PORTHOME_INDEX_ERR_OOM;
    return PORTHOME_INDEX_ERR_STORAGE;
}

static porthome_index_status portable_entry(const hanami_index_entry_t *entry,
                                           porthome_index_entry *out)
{
    porthome_index_entry row = { .chunk_key_id = entry->chunk_key_id };
    memcpy(row.git_oid, entry->git_oid, sizeof(row.git_oid));
    if (porthome_from_hex64(entry->blossom_hash, row.sealed_sha256) != PORTHOME_OK)
        return PORTHOME_INDEX_ERR_STORAGE;
    *out = row;
    return PORTHOME_INDEX_OK;
}

static porthome_index_status index_lookup(void *ctx, const char *oid,
                                         porthome_index_entry *out)
{
    hanami_index_entry_t entry;
    hanami_error_t err = hanami_index_get_by_oid(ctx, oid, &entry);
    return err == HANAMI_OK ? portable_entry(&entry, out) : index_status(err);
}

static porthome_index_status index_insert(void *ctx, const porthome_index_entry *row)
{
    hanami_index_entry_t entry = {0};
    hanami_error_t err = hanami_index_get_by_oid(ctx, row->git_oid, &entry);
    if (err != HANAMI_OK && err != HANAMI_ERR_NOT_FOUND) return index_status(err);
    memcpy(entry.git_oid, row->git_oid, sizeof(entry.git_oid));
    porthome_hex64(row->sealed_sha256, entry.blossom_hash);
    entry.chunk_key_id = row->chunk_key_id;
    return index_status(hanami_index_put(ctx, &entry));
}

static porthome_index_status index_iterate(void *ctx, porthome_index_visit_fn visit,
                                          void *visit_ctx)
{
    hanami_index_t *idx = ctx;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT git_oid, blossom_hash, obj_type, obj_size, timestamp, "
                      "chunk_key_id FROM oid_map ORDER BY git_oid";
    if (sqlite3_prepare_v2(idx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return PORTHOME_INDEX_ERR_STORAGE;
    porthome_index_status status = PORTHOME_INDEX_OK;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        hanami_index_entry_t entry;
        porthome_index_entry row;
        status = index_status(fill_entry_from_row(stmt, &entry));
        if (status == PORTHOME_INDEX_OK) status = portable_entry(&entry, &row);
        if (status == PORTHOME_INDEX_OK) status = visit(visit_ctx, &row);
        if (status != PORTHOME_INDEX_OK) break;
    }
    if (status == PORTHOME_INDEX_OK && rc != SQLITE_DONE)
        status = PORTHOME_INDEX_ERR_STORAGE;
    sqlite3_finalize(stmt);
    return status;
}

static const porthome_index_ops index_ops = {
    .lookup = index_lookup, .insert = index_insert, .iterate = index_iterate
};

porthome_index *hanami_index_porthome(hanami_index_t *idx)
{
    return idx ? &idx->portable : NULL;
}

/* =========================================================================
 * Hash computation
 * ========================================================================= */

hanami_error_t hanami_hash_blossom(const void *data, size_t len,
                                   char out_hex[65])
{
    if (!data || !out_hex)
        return HANAMI_ERR_INVALID_ARG;

    uint8_t digest[32];
    if (porthome_sha256(data, len, digest) != PORTHOME_OK)
        return HANAMI_ERR_IO;
    porthome_hex64(digest, out_hex);
    return HANAMI_OK;
}

hanami_error_t hanami_hash_git_sha1(const void *data, size_t len,
                                    git_object_t type,
                                    char out_hex[41])
{
    if (!data || !out_hex)
        return HANAMI_ERR_INVALID_ARG;

    const char *type_str = git_type_str(type);
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0)
        return HANAMI_ERR_IO;
    header_len++; /* include the NUL byte */

    unsigned char digest[20];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return HANAMI_ERR_NOMEM;

    if (EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, header, (size_t)header_len) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, NULL) != 1)
    {
        EVP_MD_CTX_free(ctx);
        return HANAMI_ERR_IO;
    }
    EVP_MD_CTX_free(ctx);

    bytes_to_hex(digest, 20, out_hex);
    return HANAMI_OK;
}

hanami_error_t hanami_hash_git_sha256(const void *data, size_t len,
                                      git_object_t type,
                                      char out_hex[65])
{
    if (!data || !out_hex)
        return HANAMI_ERR_INVALID_ARG;

    const char *type_str = git_type_str(type);
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0)
        return HANAMI_ERR_IO;
    header_len++; /* include the NUL byte */

    unsigned char digest[32];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return HANAMI_ERR_NOMEM;

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, header, (size_t)header_len) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, NULL) != 1)
    {
        EVP_MD_CTX_free(ctx);
        return HANAMI_ERR_IO;
    }
    EVP_MD_CTX_free(ctx);

    bytes_to_hex(digest, 32, out_hex);
    return HANAMI_OK;
}
