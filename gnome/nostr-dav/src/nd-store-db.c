/* nd-store-db.c - SQLite persistence shared by the nostr-dav stores
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-store-db.h"

#include <sqlite3.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

G_DEFINE_QUARK(nd-store-db-error-quark, nd_store_db_error)

struct _NdStoreDb {
  gint     ref_count;
  sqlite3 *handle;
  gchar   *path;
};

/* Schema v1. The publish_* / signed_event_json columns and publish_log
 * are the durable outbox for the relay publisher (plan Track 2 D3/D5);
 * rows default to publish_state='idle' until that worker exists. */
static const char SCHEMA_V1[] =
  "CREATE TABLE store_generation ("
  "  collection TEXT PRIMARY KEY NOT NULL,"
  "  generation INTEGER NOT NULL DEFAULT 0"
  ");"
  "INSERT INTO store_generation(collection, generation)"
  "  VALUES ('events', 0), ('contacts', 0), ('files', 0);"

  "CREATE TABLE events ("
  "  uid               TEXT PRIMARY KEY NOT NULL,"
  "  ical              TEXT NOT NULL,"
  "  etag              TEXT NOT NULL,"
  "  kind              INTEGER NOT NULL,"
  "  pubkey            TEXT,"
  "  created_at        INTEGER NOT NULL DEFAULT 0,"
  "  nostr_event_id    TEXT,"
  "  updated_at        INTEGER NOT NULL,"
  "  publish_state     TEXT NOT NULL DEFAULT 'idle' CHECK (publish_state IN"
  "    ('idle', 'pending', 'published', 'failed_permanent', 'superseded')),"
  "  publish_attempts  INTEGER NOT NULL DEFAULT 0,"
  "  publish_next_ts   INTEGER,"
  "  signed_event_json TEXT"
  ");"
  "CREATE INDEX events_publish_queue ON events(publish_state, publish_next_ts);"

  "CREATE TABLE contacts ("
  "  uid               TEXT PRIMARY KEY NOT NULL,"
  "  vcard             TEXT NOT NULL,"
  "  etag              TEXT NOT NULL,"
  "  pubkey            TEXT,"
  "  npub              TEXT,"
  "  created_at        INTEGER NOT NULL DEFAULT 0,"
  "  nostr_event_id    TEXT,"
  "  updated_at        INTEGER NOT NULL,"
  "  publish_state     TEXT NOT NULL DEFAULT 'idle' CHECK (publish_state IN"
  "    ('idle', 'pending', 'published', 'failed_permanent', 'superseded')),"
  "  publish_attempts  INTEGER NOT NULL DEFAULT 0,"
  "  publish_next_ts   INTEGER,"
  "  signed_event_json TEXT"
  ");"
  "CREATE INDEX contacts_publish_queue ON contacts(publish_state, publish_next_ts);"

  "CREATE TABLE files ("
  "  path              TEXT PRIMARY KEY NOT NULL,"
  "  content           BLOB NOT NULL,"
  "  mime              TEXT NOT NULL,"
  "  etag              TEXT NOT NULL,"
  "  sha256            TEXT NOT NULL,"
  "  size              INTEGER NOT NULL,"
  "  blossom_url       TEXT,"
  "  pubkey            TEXT,"
  "  created_at        INTEGER NOT NULL DEFAULT 0,"
  "  nostr_event_id    TEXT,"
  "  modified_at       INTEGER NOT NULL,"
  "  publish_state     TEXT NOT NULL DEFAULT 'idle' CHECK (publish_state IN"
  "    ('idle', 'pending', 'published', 'failed_permanent', 'superseded')),"
  "  publish_attempts  INTEGER NOT NULL DEFAULT 0,"
  "  publish_next_ts   INTEGER,"
  "  signed_event_json TEXT"
  ");"
  "CREATE INDEX files_publish_queue ON files(publish_state, publish_next_ts);"

  "CREATE TABLE publish_log ("
  "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  target_kind  INTEGER NOT NULL,"
  "  target_uid   TEXT NOT NULL,"
  "  relay_url    TEXT,"
  "  attempted_at INTEGER NOT NULL,"
  "  http_status  INTEGER,"
  "  error        TEXT"
  ");"
  "CREATE INDEX publish_log_target ON publish_log(target_kind, target_uid);";

/* Schema v2 — relay subscription cursor (plan Track 2 D4). One row per
 * upstream relay URL; since_ts is the newest created_at that has already
 * been ingested, so REQ resume filters can use `since_ts + 1`. */
static const char SCHEMA_V2[] =
  "CREATE TABLE relay_cursor ("
  "  relay_url  TEXT PRIMARY KEY NOT NULL,"
  "  since_ts   INTEGER NOT NULL DEFAULT 0,"
  "  updated_at INTEGER NOT NULL DEFAULT 0"
  ");";

/* Schema v3 — publish target set per outbox row. Stored as a JSON
 * array of relay URLs so a NIP-65 change between attempts does not
 * silently retarget a retry (plan Track 2 D5). */
static const char SCHEMA_V3[] =
  "ALTER TABLE events   ADD COLUMN publish_targets TEXT;"
  "ALTER TABLE contacts ADD COLUMN publish_targets TEXT;"
  "ALTER TABLE files    ADD COLUMN publish_targets TEXT;";

static const gchar *
collection_name(NdStoreCollection collection)
{
  switch (collection) {
  case ND_STORE_COLLECTION_EVENTS:   return "events";
  case ND_STORE_COLLECTION_CONTACTS: return "contacts";
  case ND_STORE_COLLECTION_FILES:    return "files";
  default:                           break;
  }
  g_return_val_if_reached("events");
}

/* Table names equal collection names; files are keyed by path. */
static const gchar *
collection_key_column(NdStoreCollection collection)
{
  return collection == ND_STORE_COLLECTION_FILES ? "path" : "uid";
}

/* ---- Low-level helpers ---- */

static gboolean
db_exec(NdStoreDb *db, const char *sql, GError **error)
{
  char *errmsg = NULL;
  if (sqlite3_exec(db->handle, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_SQL,
                "%s", errmsg ? errmsg : sqlite3_errmsg(db->handle));
    sqlite3_free(errmsg);
    return FALSE;
  }
  return TRUE;
}

static gboolean
is_corruption_code(int rc)
{
  int primary = rc & 0xff;
  return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

gboolean
nd_store_db_set_sql_error(NdStoreDb   *db,
                          GError     **error,
                          const gchar *context)
{
  g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_SQL,
              "%s: %s", context, sqlite3_errmsg(db->handle));
  return FALSE;
}

/* Pre-create the database file with 0600 so SQLite (and the -wal/-shm
 * files it derives permissions from) never exists world-readable. */
static gboolean
ensure_db_file(const gchar *path, GError **error)
{
  int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    int saved = errno;
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_OPEN,
                "Cannot open store %s: %s", path, g_strerror(saved));
    return FALSE;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    int saved = errno;
    close(fd);
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_OPEN,
                "Cannot stat store %s: %s", path, g_strerror(saved));
    return FALSE;
  }

  if (!S_ISREG(st.st_mode)) {
    close(fd);
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_PERMISSIONS,
                "Store %s is not a regular file", path);
    return FALSE;
  }

  if (st.st_uid != getuid()) {
    close(fd);
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_PERMISSIONS,
                "Store %s is owned by uid %u, not the current user",
                path, (guint)st.st_uid);
    return FALSE;
  }

  if ((st.st_mode & 077) != 0) {
    if (fchmod(fd, 0600) != 0) {
      int saved = errno;
      close(fd);
      g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_PERMISSIONS,
                  "Cannot restrict permissions on %s: %s",
                  path, g_strerror(saved));
      return FALSE;
    }
    g_warning("nostr-dav: tightened permissions on %s from %04o to 0600",
              path, (guint)(st.st_mode & 07777));
  }

  close(fd);
  return TRUE;
}

static gboolean
check_integrity(NdStoreDb *db, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db->handle, "PRAGMA quick_check", -1, &stmt, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(stmt);

  gboolean ok = FALSE;
  if (rc == SQLITE_ROW) {
    const unsigned char *res = sqlite3_column_text(stmt, 0);
    ok = (res != NULL && strcmp((const char *)res, "ok") == 0);
    if (!ok)
      g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_CORRUPT,
                  "Integrity check failed for %s: %s", db->path,
                  res ? (const char *)res : "(no result)");
  } else if (is_corruption_code(rc)) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_CORRUPT,
                "Integrity check failed for %s: %s", db->path,
                sqlite3_errmsg(db->handle));
  } else {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_OPEN,
                "Cannot read store %s: %s", db->path,
                sqlite3_errmsg(db->handle));
  }

  sqlite3_finalize(stmt);
  return ok;
}

static gboolean
query_int64(NdStoreDb *db, const char *sql, gint64 *out, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(db, error, sql);

  gboolean ok = FALSE;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    *out = sqlite3_column_int64(stmt, 0);
    ok = TRUE;
  } else {
    nd_store_db_set_sql_error(db, error, sql);
  }
  sqlite3_finalize(stmt);
  return ok;
}

static void
enable_wal(NdStoreDb *db)
{
  sqlite3_stmt *stmt = NULL;
  const char *mode = NULL;
  if (sqlite3_prepare_v2(db->handle, "PRAGMA journal_mode=WAL", -1,
                         &stmt, NULL) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW)
    mode = (const char *)sqlite3_column_text(stmt, 0);

  if (mode == NULL || g_ascii_strcasecmp(mode, "wal") != 0)
    g_warning("nostr-dav: WAL unavailable for %s (journal_mode=%s); "
              "continuing with rollback journal",
              db->path, mode ? mode : "unknown");
  sqlite3_finalize(stmt);
}

static gboolean
migrate(NdStoreDb *db, GError **error)
{
  gint64 version = 0;
  if (!query_int64(db, "PRAGMA user_version", &version, error))
    return FALSE;

  if (version == ND_STORE_DB_SCHEMA_VERSION)
    return TRUE;

  if (version > ND_STORE_DB_SCHEMA_VERSION) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_SCHEMA,
                "Store %s has schema version %" G_GINT64_FORMAT
                "; this nostr-dav supports up to %d",
                db->path, version, ND_STORE_DB_SCHEMA_VERSION);
    return FALSE;
  }

  g_autofree gchar *set_version =
    g_strdup_printf("PRAGMA user_version = %d", ND_STORE_DB_SCHEMA_VERSION);

  if (!nd_store_db_begin(db, error))
    return FALSE;

  /* user_version is transactional: a crash mid-migration leaves the
   * previous version (and schema) intact. */
  if ((version < 1 && !db_exec(db, SCHEMA_V1, error)) ||
      (version < 2 && !db_exec(db, SCHEMA_V2, error)) ||
      (version < 3 && !db_exec(db, SCHEMA_V3, error)) ||
      !db_exec(db, set_version, error)) {
    nd_store_db_rollback(db);
    return FALSE;
  }

  return nd_store_db_commit(db, error);
}

static NdStoreDb *
open_once(const gchar *path, GError **error)
{
  if (!ensure_db_file(path, error))
    return NULL;

  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
  flags |= SQLITE_OPEN_NOFOLLOW;
#endif

  sqlite3 *handle = NULL;
  int rc = sqlite3_open_v2(path, &handle, flags, NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_OPEN,
                "Cannot open store %s: %s", path,
                handle ? sqlite3_errmsg(handle) : sqlite3_errstr(rc));
    sqlite3_close_v2(handle);
    return NULL;
  }

  NdStoreDb *db = g_new0(NdStoreDb, 1);
  db->ref_count = 1;
  db->handle = handle;
  db->path = g_strdup(path);

  sqlite3_busy_timeout(handle, 5000);

  if (!check_integrity(db, error))
    goto fail;

  enable_wal(db);

  if (!db_exec(db, "PRAGMA synchronous=NORMAL", error))
    goto fail;

  if (!migrate(db, error))
    goto fail;

  return db;

fail:
  nd_store_db_unref(db);
  return NULL;
}

static gboolean
quarantine(const gchar *path, GError **error)
{
  gint64 ts = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *dest = g_strdup_printf("%s.corrupt-%" G_GINT64_FORMAT,
                                           path, ts);

  if (g_rename(path, dest) != 0) {
    int saved = errno;
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_OPEN,
                "Cannot move corrupt store %s aside: %s",
                path, g_strerror(saved));
    return FALSE;
  }

  static const gchar *const sidecars[] = { "-wal", "-shm" };
  for (gsize i = 0; i < G_N_ELEMENTS(sidecars); i++) {
    g_autofree gchar *src = g_strconcat(path, sidecars[i], NULL);
    g_autofree gchar *dst = g_strconcat(dest, sidecars[i], NULL);
    if (g_rename(src, dst) != 0 && errno != ENOENT)
      g_warning("nostr-dav: could not move %s aside: %s",
                src, g_strerror(errno));
  }

  g_warning("nostr-dav: store %s failed its integrity check; moved to %s "
            "and starting empty (relays hold the durable copy)", path, dest);
  return TRUE;
}

/* ---- Public API ---- */

gchar *
nd_store_db_default_path(void)
{
  return g_build_filename(g_get_user_data_dir(), "nostr-dav",
                          "store.sqlite", NULL);
}

NdStoreDb *
nd_store_db_open(const gchar *path, GError **error)
{
  g_return_val_if_fail(path != NULL, NULL);

  g_autofree gchar *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    int saved = errno;
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_OPEN,
                "Cannot create %s: %s", dir, g_strerror(saved));
    return NULL;
  }

  GError *local_err = NULL;
  NdStoreDb *db = open_once(path, &local_err);
  if (db != NULL)
    return db;

  if (!g_error_matches(local_err, ND_STORE_DB_ERROR,
                       ND_STORE_DB_ERROR_CORRUPT)) {
    g_propagate_error(error, local_err);
    return NULL;
  }

  g_warning("nostr-dav: %s", local_err->message);
  g_clear_error(&local_err);

  if (!quarantine(path, error))
    return NULL;

  return open_once(path, error);
}

NdStoreDb *
nd_store_db_ref(NdStoreDb *db)
{
  g_return_val_if_fail(db != NULL, NULL);
  g_atomic_int_inc(&db->ref_count);
  return db;
}

void
nd_store_db_unref(NdStoreDb *db)
{
  if (db == NULL)
    return;
  if (!g_atomic_int_dec_and_test(&db->ref_count))
    return;

  if (db->handle != NULL && sqlite3_close_v2(db->handle) != SQLITE_OK)
    g_warning("nostr-dav: closing store %s: %s",
              db->path, sqlite3_errmsg(db->handle));
  g_free(db->path);
  g_free(db);
}

struct sqlite3 *
nd_store_db_get_handle(NdStoreDb *db)
{
  g_return_val_if_fail(db != NULL, NULL);
  return db->handle;
}

gboolean
nd_store_db_begin(NdStoreDb *db, GError **error)
{
  return db_exec(db, "BEGIN IMMEDIATE", error);
}

gboolean
nd_store_db_commit(NdStoreDb *db, GError **error)
{
  if (db_exec(db, "COMMIT", error))
    return TRUE;
  nd_store_db_rollback(db);
  return FALSE;
}

void
nd_store_db_rollback(NdStoreDb *db)
{
  if (!sqlite3_get_autocommit(db->handle))
    sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
}

gboolean
nd_store_db_bump_generation(NdStoreDb        *db,
                            NdStoreCollection collection,
                            GError          **error)
{
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db->handle,
        "UPDATE store_generation SET generation = generation + 1 "
        "WHERE collection = ?1", -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(db, error, "prepare generation bump");

  sqlite3_bind_text(stmt, 1, collection_name(collection), -1, SQLITE_STATIC);

  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE &&
                 sqlite3_changes(db->handle) == 1);
  if (!ok)
    nd_store_db_set_sql_error(db, error, "bump generation");
  sqlite3_finalize(stmt);
  return ok;
}

gchar *
nd_store_db_get_ctag(NdStoreDb        *db,
                     NdStoreCollection collection,
                     GError          **error)
{
  gint64 generation = 0;
  if (!nd_store_db_get_generation(db, collection, &generation, error))
    return NULL;
  return g_strdup_printf("%" G_GINT64_FORMAT, generation);
}

gboolean
nd_store_db_row_exists(NdStoreDb        *db,
                       NdStoreCollection collection,
                       const gchar      *key,
                       gboolean         *out_exists,
                       GError          **error)
{
  g_return_val_if_fail(key != NULL && out_exists != NULL, FALSE);

  g_autofree gchar *sql = g_strdup_printf("SELECT 1 FROM %s WHERE %s = ?1",
                                          collection_name(collection),
                                          collection_key_column(collection));
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(db, error, "prepare exists");

  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  gboolean ok = (rc == SQLITE_ROW || rc == SQLITE_DONE);
  if (ok)
    *out_exists = (rc == SQLITE_ROW);
  else
    nd_store_db_set_sql_error(db, error, "exists");
  sqlite3_finalize(stmt);
  return ok;
}

gboolean
nd_store_db_delete_row(NdStoreDb        *db,
                       NdStoreCollection collection,
                       const gchar      *key,
                       gboolean         *out_removed,
                       GError          **error)
{
  g_return_val_if_fail(key != NULL, FALSE);

  g_autofree gchar *sql = g_strdup_printf("DELETE FROM %s WHERE %s = ?1",
                                          collection_name(collection),
                                          collection_key_column(collection));
  if (!nd_store_db_begin(db, error))
    return FALSE;

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(db, error, "prepare delete");
    nd_store_db_rollback(db);
    return FALSE;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  gboolean removed = (sqlite3_changes(db->handle) > 0);
  if (rc != SQLITE_DONE)
    nd_store_db_set_sql_error(db, error, "delete");
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE ||
      (removed && !nd_store_db_bump_generation(db, collection, error))) {
    nd_store_db_rollback(db);
    return FALSE;
  }

  if (!nd_store_db_commit(db, error))
    return FALSE;

  if (out_removed)
    *out_removed = removed;
  return TRUE;
}

gboolean
nd_store_db_count_rows(NdStoreDb        *db,
                       NdStoreCollection collection,
                       guint            *out_count,
                       GError          **error)
{
  g_return_val_if_fail(out_count != NULL, FALSE);

  g_autofree gchar *sql = g_strdup_printf("SELECT COUNT(*) FROM %s",
                                          collection_name(collection));
  gint64 count = 0;
  if (!query_int64(db, sql, &count, error))
    return FALSE;
  *out_count = (guint)count;
  return TRUE;
}

gboolean
nd_store_db_get_relay_cursor(NdStoreDb   *db,
                             const gchar *relay_url,
                             gint64      *out_since_ts,
                             GError     **error)
{
  g_return_val_if_fail(db != NULL, FALSE);
  g_return_val_if_fail(relay_url != NULL, FALSE);

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db->handle,
        "SELECT since_ts FROM relay_cursor WHERE relay_url = ?1",
        -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(db, error, "prepare cursor read");

  sqlite3_bind_text(stmt, 1, relay_url, -1, SQLITE_TRANSIENT);

  gint64 value = 0;
  gboolean ok = TRUE;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    value = sqlite3_column_int64(stmt, 0);
  else if (rc != SQLITE_DONE)
    ok = nd_store_db_set_sql_error(db, error, "cursor read");

  sqlite3_finalize(stmt);
  if (ok && out_since_ts != NULL)
    *out_since_ts = value;
  return ok;
}

gboolean
nd_store_db_set_relay_cursor(NdStoreDb   *db,
                             const gchar *relay_url,
                             gint64       since_ts,
                             GError     **error)
{
  g_return_val_if_fail(db != NULL, FALSE);
  g_return_val_if_fail(relay_url != NULL, FALSE);

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db->handle,
        "INSERT INTO relay_cursor(relay_url, since_ts, updated_at)"
        "  VALUES (?1, ?2, ?3)"
        "  ON CONFLICT(relay_url) DO UPDATE SET"
        "    since_ts = MAX(relay_cursor.since_ts, excluded.since_ts),"
        "    updated_at = excluded.updated_at",
        -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(db, error, "prepare cursor upsert");

  sqlite3_bind_text (stmt, 1, relay_url, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, since_ts);
  sqlite3_bind_int64(stmt, 3, g_get_real_time() / G_USEC_PER_SEC);

  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok)
    nd_store_db_set_sql_error(db, error, "cursor upsert");
  sqlite3_finalize(stmt);
  return ok;
}

gboolean
nd_store_db_get_generation(NdStoreDb        *db,
                           NdStoreCollection collection,
                           gint64           *out_generation,
                           GError          **error)
{
  g_return_val_if_fail(out_generation != NULL, FALSE);

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db->handle,
        "SELECT generation FROM store_generation WHERE collection = ?1",
        -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(db, error, "prepare generation read");

  sqlite3_bind_text(stmt, 1, collection_name(collection), -1, SQLITE_STATIC);

  gboolean ok = FALSE;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    *out_generation = sqlite3_column_int64(stmt, 0);
    ok = TRUE;
  } else {
    nd_store_db_set_sql_error(db, error, "read generation");
  }
  sqlite3_finalize(stmt);
  return ok;
}
