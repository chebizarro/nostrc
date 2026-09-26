/* nd-store-db.h - SQLite persistence shared by the nostr-dav stores
 *
 * SPDX-License-Identifier: MIT
 *
 * One database file per user (default
 * $XDG_DATA_HOME/nostr-dav/store.sqlite, mode 0600, WAL) backs the
 * calendar, contact, and file stores. The schema carries the publish
 * outbox columns (publish_state, publish_attempts, publish_next_ts,
 * signed_event_json) and the publish_log table up front so the relay
 * publisher can land without a schema migration.
 *
 * Each collection has a monotonically increasing generation counter
 * (table store_generation) that is bumped inside the same transaction
 * as every mutation; it is exposed as the collection's DAV ctag and is
 * therefore stable across daemon restarts.
 */
#ifndef ND_STORE_DB_H
#define ND_STORE_DB_H

#include <glib.h>

G_BEGIN_DECLS

#define ND_STORE_DB_SCHEMA_VERSION 3

#define ND_STORE_DB_ERROR (nd_store_db_error_quark())
GQuark nd_store_db_error_quark(void);

typedef enum {
  ND_STORE_DB_ERROR_OPEN = 1,
  ND_STORE_DB_ERROR_PERMISSIONS,
  ND_STORE_DB_ERROR_CORRUPT,
  ND_STORE_DB_ERROR_SCHEMA,
  ND_STORE_DB_ERROR_SQL,
  ND_STORE_DB_ERROR_DATA
} NdStoreDbError;

typedef enum {
  ND_STORE_COLLECTION_EVENTS,
  ND_STORE_COLLECTION_CONTACTS,
  ND_STORE_COLLECTION_FILES
} NdStoreCollection;

typedef struct _NdStoreDb NdStoreDb;
struct sqlite3;

/**
 * nd_store_db_default_path:
 *
 * Returns: (transfer full): $XDG_DATA_HOME/nostr-dav/store.sqlite
 */
gchar *nd_store_db_default_path(void);

/**
 * nd_store_db_open:
 * @path: database file path; parent directory is created 0700
 * @error: (out) (optional): location for error
 *
 * Opens (creating if needed) the store database, enables WAL, and
 * migrates the schema to %ND_STORE_DB_SCHEMA_VERSION. A database that
 * fails its integrity check is renamed to `<path>.corrupt-<unix-ts>`
 * and a fresh one is created in its place (the relays are the durable
 * copy; this file is a cache plus the pending-publish queue).
 *
 * Returns: (transfer full) (nullable): the database, or NULL on error.
 */
NdStoreDb *nd_store_db_open(const gchar *path, GError **error);

NdStoreDb *nd_store_db_ref(NdStoreDb *db);
void       nd_store_db_unref(NdStoreDb *db);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdStoreDb, nd_store_db_unref)

/* ---- Helpers for the store adapters (not for DAV handlers) ---- */

struct sqlite3 *nd_store_db_get_handle(NdStoreDb *db);

/** Starts a write transaction (BEGIN IMMEDIATE). */
gboolean nd_store_db_begin(NdStoreDb *db, GError **error);
gboolean nd_store_db_commit(NdStoreDb *db, GError **error);
void     nd_store_db_rollback(NdStoreDb *db);

/** Bumps @collection's generation; call inside a transaction. */
gboolean nd_store_db_bump_generation(NdStoreDb        *db,
                                     NdStoreCollection collection,
                                     GError          **error);

gboolean nd_store_db_get_generation(NdStoreDb        *db,
                                    NdStoreCollection collection,
                                    gint64           *out_generation,
                                    GError          **error);

/** Collection generation formatted as a DAV ctag string. */
gchar *nd_store_db_get_ctag(NdStoreDb        *db,
                            NdStoreCollection collection,
                            GError          **error);

/** Whether a row keyed by @key exists (call inside a transaction when
 *  the answer feeds a subsequent write). */
gboolean nd_store_db_row_exists(NdStoreDb        *db,
                                NdStoreCollection collection,
                                const gchar      *key,
                                gboolean         *out_exists,
                                GError          **error);

/** Deletes the row keyed by @key and, if one was removed, bumps the
 *  collection generation — atomically. */
gboolean nd_store_db_delete_row(NdStoreDb        *db,
                                NdStoreCollection collection,
                                const gchar      *key,
                                gboolean         *out_removed,
                                GError          **error);

gboolean nd_store_db_count_rows(NdStoreDb        *db,
                                NdStoreCollection collection,
                                guint            *out_count,
                                GError          **error);

/**
 * nd_store_db_set_sql_error:
 *
 * Sets %ND_STORE_DB_ERROR_SQL on @error as "@context: <sqlite message>".
 * Returns: FALSE, for tail-calling from failure paths.
 */
gboolean nd_store_db_set_sql_error(NdStoreDb   *db,
                                   GError     **error,
                                   const gchar *context);

/* ---- Relay subscription cursor (Track 2 D4) ---- */

/**
 * nd_store_db_get_relay_cursor:
 * @relay_url: relay URL used as the primary key
 * @out_since_ts: (out) (nullable): last processed created_at, or 0 if the
 *   relay has no persisted cursor. Unset on error.
 *
 * Returns: TRUE on success (including "no row"); FALSE with @error set on
 *   a SQLite failure.
 */
gboolean nd_store_db_get_relay_cursor(NdStoreDb   *db,
                                      const gchar *relay_url,
                                      gint64      *out_since_ts,
                                      GError     **error);

/**
 * nd_store_db_set_relay_cursor:
 * @relay_url: relay URL used as the primary key
 * @since_ts: newest created_at successfully ingested
 *
 * UPSERTs the cursor row and its updated_at wall-clock timestamp.
 */
gboolean nd_store_db_set_relay_cursor(NdStoreDb   *db,
                                      const gchar *relay_url,
                                      gint64       since_ts,
                                      GError     **error);

G_END_DECLS
#endif /* ND_STORE_DB_H */
