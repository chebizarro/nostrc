/*
 * hanami-index.c - OID ↔ Blossom SHA-256 mapping index (SQLite backend)
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-index.h"
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

struct hanami_index {
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
    "  timestamp    INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_blossom_hash ON oid_map(blossom_hash);";

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
    int is_memory = (strcmp(path, ":memory:") == 0);
    char *db_path = NULL;

    if (is_memory) {
        db_path = strdup(":memory:");
    } else {
        if (ensure_directory(path) != 0 && errno != EEXIST) {
            /* Try anyway — maybe the directory already exists */
        }
        size_t path_len = strlen(path);
        db_path = malloc(path_len + 20);
        if (!db_path)
            return HANAMI_ERR_NOMEM;
        snprintf(db_path, path_len + 20, "%s/hanami_index.db", path);
    }

    hanami_index_t *idx = calloc(1, sizeof(*idx));
    if (!idx) {
        free(db_path);
        return HANAMI_ERR_NOMEM;
    }

    int rc = sqlite3_open(db_path, &idx->db);
    free(db_path);
    if (rc != SQLITE_OK) {
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

    /* Prepare statements */
    const char *sql_put =
        "INSERT OR REPLACE INTO oid_map "
        "(git_oid, blossom_hash, obj_type, obj_size, timestamp) "
        "VALUES (?, ?, ?, ?, ?);";
    const char *sql_get_oid =
        "SELECT git_oid, blossom_hash, obj_type, obj_size, timestamp "
        "FROM oid_map WHERE git_oid = ?;";
    const char *sql_get_blossom =
        "SELECT git_oid, blossom_hash, obj_type, obj_size, timestamp "
        "FROM oid_map WHERE blossom_hash = ? LIMIT 1;";
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
    return HANAMI_ERR_NOT_FOUND;
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

    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        hanami_error_t err = fill_entry_from_row(s, out);
        sqlite3_reset(s);
        return err;
    }
    sqlite3_reset(s);
    return HANAMI_ERR_NOT_FOUND;
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

/* =========================================================================
 * Hash computation
 * ========================================================================= */

hanami_error_t hanami_hash_blossom(const void *data, size_t len,
                                   char out_hex[65])
{
    if (!data || !out_hex)
        return HANAMI_ERR_INVALID_ARG;

    unsigned char digest[32];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return HANAMI_ERR_NOMEM;

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
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
