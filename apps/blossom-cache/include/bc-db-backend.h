/*
 * bc-db-backend.h - BcDbBackend: abstract metadata storage interface
 *
 * SPDX-License-Identifier: MIT
 *
 * Vtable pattern following libmarmot's MarmotStorage approach.
 * Implementations: SQLite (bc_db_backend_sqlite_new) and LMDB (bc_db_backend_lmdb_new).
 *
 * The BcBlobStore delegates all metadata operations through this interface,
 * keeping blob content on the filesystem regardless of backend choice.
 */

#ifndef BC_DB_BACKEND_H
#define BC_DB_BACKEND_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BcDbBlobMeta:
 *
 * Metadata record for a cached blob. Backends serialize/deserialize this
 * as needed for their storage format.
 */
typedef struct {
  char    *sha256;        /* 64-char hex hash (owned) */
  int64_t  size;          /* blob size in bytes */
  char    *mime_type;     /* MIME type (owned, may be NULL) */
  int64_t  created_at;    /* unix timestamp of first cache */
  int64_t  last_accessed; /* unix timestamp of most recent access */
  uint32_t access_count;  /* number of times served */
} BcDbBlobMeta;

BcDbBlobMeta *bc_db_blob_meta_copy(const BcDbBlobMeta *meta);
void          bc_db_blob_meta_free(BcDbBlobMeta *meta);

/**
 * BcDbBackend:
 *
 * Abstract metadata storage vtable. All function pointers receive @ctx
 * as their first argument (the backend's private data).
 *
 * Memory ownership:
 * - Functions returning pointers transfer ownership to the caller.
 * - Functions accepting pointers do NOT take ownership — the backend
 *   must copy if it needs to retain data.
 */
typedef struct BcDbBackend {
  /* ── Opaque backend context ──────────────────────────────────────── */
  void *ctx;

  /* ── Query operations ────────────────────────────────────────────── */

  /** Check if a blob exists by SHA-256 hash. */
  gboolean (*contains)(void *ctx, const char *sha256);

  /** Get metadata for a blob. Returns NULL if not found.
   *  Also touches last_accessed and increments access_count. */
  BcDbBlobMeta *(*get_info)(void *ctx, const char *sha256, GError **error);

  /** Get total size of all blobs in bytes. */
  int64_t (*get_total_size)(void *ctx);

  /** Get total number of blobs. */
  uint32_t (*get_blob_count)(void *ctx);

  /* ── Mutation operations ─────────────────────────────────────────── */

  /** Insert metadata for a new blob. Returns FALSE on failure. */
  gboolean (*put_meta)(void *ctx, const BcDbBlobMeta *meta, GError **error);

  /** Delete metadata for a blob by hash. */
  gboolean (*delete_meta)(void *ctx, const char *sha256, GError **error);

  /* ── List / eviction operations ──────────────────────────────────── */

  /**
   * List blobs sorted by created_at DESC with cursor-based pagination.
   * @cursor_sha256 (nullable): hash of the last blob from the previous page.
   * @limit: max results to return (0 → default 100).
   *
   * Returns: (element-type BcDbBlobMeta) (transfer full): array of metadata
   */
  GPtrArray *(*list_blobs)(void *ctx, const char *cursor_sha256,
                           uint32_t limit, GError **error);

  /**
   * Get candidates for LRU eviction — blobs ordered by last_accessed ASC.
   * Returns up to enough hashes+sizes to free @bytes_to_free bytes.
   *
   * Each element is a BcDbBlobMeta with at least sha256 and size populated.
   *
   * Returns: (element-type BcDbBlobMeta) (transfer full): eviction candidates
   */
  GPtrArray *(*evict_candidates)(void *ctx, int64_t bytes_to_free, GError **error);

  /* ── Lifecycle ───────────────────────────────────────────────────── */

  /** Whether this is a persistent backend. */
  bool (*is_persistent)(void *ctx);

  /** Destroy the backend and free all resources. */
  void (*destroy)(void *ctx);
} BcDbBackend;

/* ──────────────────────────────────────────────────────────────────────────
 * Built-in backends
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * bc_db_backend_sqlite_new:
 * @db_path: path to the SQLite database file (created if it doesn't exist)
 *
 * Create a SQLite-backed metadata store (default backend).
 *
 * Returns: (transfer full) (nullable): a new BcDbBackend, or NULL on error
 */
BcDbBackend *bc_db_backend_sqlite_new(const char *db_path, GError **error);

/**
 * bc_db_backend_lmdb_new:
 * @env_path: path to the LMDB environment directory (created if needed)
 * @map_size_mb: LMDB map size in MB (0 → default 256 MB)
 *
 * Create an LMDB-backed metadata store.
 * Uses nostrdb's bundled LMDB library for compatibility.
 *
 * Returns: (transfer full) (nullable): a new BcDbBackend, or NULL on error
 */
BcDbBackend *bc_db_backend_lmdb_new(const char *env_path,
                                     uint32_t map_size_mb,
                                     GError **error);

/**
 * bc_db_backend_free:
 * @backend: (transfer full) (nullable): backend to destroy
 */
void bc_db_backend_free(BcDbBackend *backend);

#ifdef __cplusplus
}
#endif

#endif /* BC_DB_BACKEND_H */
