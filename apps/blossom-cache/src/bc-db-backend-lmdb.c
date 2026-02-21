/*
 * bc-db-backend-lmdb.c - LMDB implementation of BcDbBackend
 *
 * SPDX-License-Identifier: MIT
 *
 * Uses LMDB (from nostrdb's vendored copy) for blob metadata storage.
 * This is the preferred backend when blossom-cache runs alongside gnostr,
 * since gnostr already uses nostrdb/LMDB — keeping the same storage engine
 * reduces dependencies and aligns with the project's data layer.
 *
 * Database layout (3 named databases within one LMDB environment):
 *
 *   "blobs"       - Primary store: key = sha256 (64 bytes), value = serialized BcDbBlobMeta
 *   "by_access"   - Index: key = last_accessed (8 bytes BE) || sha256, value = empty
 *   "by_created"  - Index: key = created_at (8 bytes BE) || sha256, value = empty
 *
 * Serialization format for blob metadata (all integers little-endian):
 *   [8: size] [8: created_at] [8: last_accessed] [4: access_count]
 *   [4: mime_type_len] [N: mime_type]
 */

#include "bc-db-backend.h"

#if __has_include("lmdb.h")
#include "lmdb.h"
#define HAVE_LMDB 1
#elif __has_include(<lmdb.h>)
#include <lmdb.h>
#define HAVE_LMDB 1
#else
#define HAVE_LMDB 0
#endif

#if !HAVE_LMDB

/* Stub when LMDB is not available at compile time */
BcDbBackend *
bc_db_backend_lmdb_new(const char *env_path, uint32_t map_size_mb, GError **error)
{
  (void)env_path; (void)map_size_mb;
  g_set_error_literal(error, g_quark_from_static_string("bc-db-lmdb"),
                      0, "LMDB support not available (lmdb.h not found)");
  return NULL;
}

#else /* HAVE_LMDB */

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* ── Error domain ──────────────────────────────────────────────────────── */

#define BC_DB_LMDB_ERROR (bc_db_lmdb_error_quark())
G_DEFINE_QUARK(bc-db-lmdb-error-quark, bc_db_lmdb_error)

enum {
  BC_DB_LMDB_ERROR_ENV,
  BC_DB_LMDB_ERROR_TXN,
  BC_DB_LMDB_ERROR_IO,
};

/* ── Internal context ──────────────────────────────────────────────────── */

#define LMDB_MAX_DBS 4

typedef struct {
  MDB_env *env;
  MDB_dbi  dbi_blobs;       /* sha256 → serialized meta */
  MDB_dbi  dbi_by_access;   /* (last_accessed || sha256) → empty */
  MDB_dbi  dbi_by_created;  /* (created_at || sha256) → empty */
} LmdbCtx;

/* ── Endianness helpers (big-endian for sorted keys) ───────────────────── */

static inline void
write_i64_be(uint8_t *p, int64_t v)
{
  uint64_t u = (uint64_t)v;
  for (int i = 7; i >= 0; i--) {
    p[7 - i] = (uint8_t)(u >> (i * 8));
  }
}

static inline int64_t
read_i64_be(const uint8_t *p)
{
  uint64_t u = 0;
  for (int i = 0; i < 8; i++) {
    u = (u << 8) | p[i];
  }
  return (int64_t)u;
}

/* Little-endian for value serialization */
static inline void
write_i64_le(uint8_t *p, int64_t v)
{
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)(u >> (i * 8));
  }
}

static inline int64_t
read_i64_le(const uint8_t *p)
{
  uint64_t u = 0;
  for (int i = 7; i >= 0; i--) {
    u = (u << 8) | p[i];
  }
  return (int64_t)u;
}

/* Note: read_i64_le iterates backwards (i=7..0) reading p[i], which correctly
 * reconstructs a little-endian value: p[7] becomes the MSB, p[0] the LSB.
 * This matches write_i64_le which stores LSB at p[0]. */

static inline void
write_u32_le(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t
read_u32_le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── Serialization ─────────────────────────────────────────────────────── */

/*
 * Format: [8:size][8:created_at][8:last_accessed][4:access_count][4:mime_len][N:mime]
 */
static uint8_t *
serialize_meta(const BcDbBlobMeta *meta, size_t *out_len)
{
  size_t mime_len = meta->mime_type ? strlen(meta->mime_type) : 0;
  size_t total = 8 + 8 + 8 + 4 + 4 + mime_len;

  uint8_t *buf = g_malloc(total);
  uint8_t *p = buf;

  write_i64_le(p, meta->size);          p += 8;
  write_i64_le(p, meta->created_at);    p += 8;
  write_i64_le(p, meta->last_accessed); p += 8;
  write_u32_le(p, meta->access_count);  p += 4;
  write_u32_le(p, (uint32_t)mime_len);  p += 4;
  if (mime_len > 0)
    memcpy(p, meta->mime_type, mime_len);

  *out_len = total;
  return buf;
}

static BcDbBlobMeta *
deserialize_meta(const char *sha256, const uint8_t *data, size_t data_len)
{
  if (data_len < 32) /* minimum: 8+8+8+4+4 */
    return NULL;

  BcDbBlobMeta *meta = g_new0(BcDbBlobMeta, 1);
  const uint8_t *p = data;

  meta->sha256        = g_strdup(sha256);
  meta->size          = read_i64_le(p);  p += 8;
  meta->created_at    = read_i64_le(p);  p += 8;
  meta->last_accessed = read_i64_le(p);  p += 8;
  meta->access_count  = read_u32_le(p);  p += 4;

  uint32_t mime_len = read_u32_le(p);    p += 4;
  if (mime_len > 0 && (size_t)(p - data) + mime_len <= data_len) {
    meta->mime_type = g_strndup((const char *)p, mime_len);
  }

  return meta;
}

/* ── Index key helpers ─────────────────────────────────────────────────── */

/* Access index key: 8 bytes big-endian timestamp + 64 bytes sha256 = 72 bytes */
static void
make_access_key(int64_t last_accessed, const char *sha256, uint8_t key[72])
{
  write_i64_be(key, last_accessed);
  memcpy(key + 8, sha256, 64);
}

/* Created index key: same layout */
static void
make_created_key(int64_t created_at, const char *sha256, uint8_t key[72])
{
  write_i64_be(key, created_at);
  memcpy(key + 8, sha256, 64);
}

/* ── Backend operations ────────────────────────────────────────────────── */

static gboolean
lmdb_contains(void *ctx, const char *sha256)
{
  LmdbCtx *lc = ctx;
  MDB_txn *txn;
  if (mdb_txn_begin(lc->env, NULL, MDB_RDONLY, &txn) != 0)
    return FALSE;

  MDB_val key = { .mv_size = 64, .mv_data = (void *)sha256 };
  MDB_val val;
  int rc = mdb_get(txn, lc->dbi_blobs, &key, &val);
  mdb_txn_abort(txn);

  return (rc == 0);
}

static BcDbBlobMeta *
lmdb_get_info(void *ctx, const char *sha256, GError **error)
{
  LmdbCtx *lc = ctx;

  /* First read the current record */
  MDB_txn *rtxn;
  if (mdb_txn_begin(lc->env, NULL, MDB_RDONLY, &rtxn) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                        "Failed to begin read transaction");
    return NULL;
  }

  MDB_val key = { .mv_size = 64, .mv_data = (void *)sha256 };
  MDB_val val;
  int rc = mdb_get(rtxn, lc->dbi_blobs, &key, &val);
  if (rc != 0) {
    mdb_txn_abort(rtxn);
    return NULL; /* not found */
  }

  BcDbBlobMeta *meta = deserialize_meta(sha256, val.mv_data, val.mv_size);
  int64_t old_accessed = meta ? meta->last_accessed : 0;
  mdb_txn_abort(rtxn);

  if (meta == NULL)
    return NULL;

  /* Now write-txn to touch access time */
  int64_t now = g_get_real_time() / G_USEC_PER_SEC;
  meta->last_accessed = now;
  meta->access_count += 1;

  MDB_txn *wtxn;
  if (mdb_txn_begin(lc->env, NULL, 0, &wtxn) == 0) {
    /* Update main record */
    size_t ser_len = 0;
    uint8_t *ser = serialize_meta(meta, &ser_len);
    MDB_val new_val = { .mv_size = ser_len, .mv_data = ser };
    mdb_put(wtxn, lc->dbi_blobs, &key, &new_val, 0);
    g_free(ser);

    /* Update access index: remove old key, add new */
    uint8_t old_ak[72], new_ak[72];
    make_access_key(old_accessed, sha256, old_ak);
    make_access_key(now, sha256, new_ak);

    MDB_val old_ak_key = { .mv_size = 72, .mv_data = old_ak };
    MDB_val new_ak_key = { .mv_size = 72, .mv_data = new_ak };
    MDB_val empty = { .mv_size = 0, .mv_data = NULL };

    mdb_del(wtxn, lc->dbi_by_access, &old_ak_key, NULL);
    mdb_put(wtxn, lc->dbi_by_access, &new_ak_key, &empty, 0);

    mdb_txn_commit(wtxn);
  }

  return meta;
}

static int64_t
lmdb_get_total_size(void *ctx)
{
  LmdbCtx *lc = ctx;
  MDB_txn *txn;
  if (mdb_txn_begin(lc->env, NULL, MDB_RDONLY, &txn) != 0)
    return 0;

  MDB_cursor *cursor;
  if (mdb_cursor_open(txn, lc->dbi_blobs, &cursor) != 0) {
    mdb_txn_abort(txn);
    return 0;
  }

  int64_t total = 0;
  MDB_val key, val;
  while (mdb_cursor_get(cursor, &key, &val, MDB_NEXT) == 0) {
    if (val.mv_size >= 8) {
      total += read_i64_le(val.mv_data);
    }
  }

  mdb_cursor_close(cursor);
  mdb_txn_abort(txn);
  return total;
}

static uint32_t
lmdb_get_blob_count(void *ctx)
{
  LmdbCtx *lc = ctx;
  MDB_txn *txn;
  if (mdb_txn_begin(lc->env, NULL, MDB_RDONLY, &txn) != 0)
    return 0;

  MDB_stat stat;
  if (mdb_stat(txn, lc->dbi_blobs, &stat) != 0) {
    mdb_txn_abort(txn);
    return 0;
  }

  mdb_txn_abort(txn);
  return (uint32_t)stat.ms_entries;
}

static gboolean
lmdb_put_meta(void *ctx, const BcDbBlobMeta *meta, GError **error)
{
  LmdbCtx *lc = ctx;

  MDB_txn *txn;
  if (mdb_txn_begin(lc->env, NULL, 0, &txn) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                        "Failed to begin write transaction");
    return FALSE;
  }

  /* Check if already exists */
  MDB_val key = { .mv_size = 64, .mv_data = (void *)meta->sha256 };
  MDB_val existing;
  if (mdb_get(txn, lc->dbi_blobs, &key, &existing) == 0) {
    mdb_txn_abort(txn);
    return TRUE; /* already exists, no-op */
  }

  /* Serialize and store main record */
  size_t ser_len = 0;
  uint8_t *ser = serialize_meta(meta, &ser_len);
  MDB_val val = { .mv_size = ser_len, .mv_data = ser };

  int rc = mdb_put(txn, lc->dbi_blobs, &key, &val, 0);
  g_free(ser);

  if (rc != 0) {
    g_set_error(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_IO,
                "LMDB put failed: %s", mdb_strerror(rc));
    mdb_txn_abort(txn);
    return FALSE;
  }

  /* Add index entries */
  uint8_t ak[72], ck[72];
  make_access_key(meta->last_accessed, meta->sha256, ak);
  make_created_key(meta->created_at, meta->sha256, ck);

  MDB_val ak_key = { .mv_size = 72, .mv_data = ak };
  MDB_val ck_key = { .mv_size = 72, .mv_data = ck };
  MDB_val empty = { .mv_size = 0, .mv_data = NULL };

  mdb_put(txn, lc->dbi_by_access, &ak_key, &empty, 0);
  mdb_put(txn, lc->dbi_by_created, &ck_key, &empty, 0);

  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    g_set_error(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                "LMDB commit failed: %s", mdb_strerror(rc));
    return FALSE;
  }

  return TRUE;
}

static gboolean
lmdb_delete_meta(void *ctx, const char *sha256, GError **error)
{
  LmdbCtx *lc = ctx;

  /* Read current meta to get index keys before deleting */
  MDB_txn *txn;
  if (mdb_txn_begin(lc->env, NULL, 0, &txn) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                        "Failed to begin write transaction");
    return FALSE;
  }

  MDB_val key = { .mv_size = 64, .mv_data = (void *)sha256 };
  MDB_val val;

  if (mdb_get(txn, lc->dbi_blobs, &key, &val) == 0) {
    BcDbBlobMeta *meta = deserialize_meta(sha256, val.mv_data, val.mv_size);
    if (meta != NULL) {
      /* Remove index entries */
      uint8_t ak[72], ck[72];
      make_access_key(meta->last_accessed, meta->sha256, ak);
      make_created_key(meta->created_at, meta->sha256, ck);

      MDB_val ak_key = { .mv_size = 72, .mv_data = ak };
      MDB_val ck_key = { .mv_size = 72, .mv_data = ck };

      mdb_del(txn, lc->dbi_by_access, &ak_key, NULL);
      mdb_del(txn, lc->dbi_by_created, &ck_key, NULL);
      bc_db_blob_meta_free(meta);
    }

    mdb_del(txn, lc->dbi_blobs, &key, NULL);
  }

  mdb_txn_commit(txn);
  return TRUE;
}

static GPtrArray *
lmdb_list_blobs(void *ctx, const char *cursor_sha256,
                uint32_t limit, GError **error)
{
  LmdbCtx *lc = ctx;
  if (limit == 0) limit = 100;

  MDB_txn *txn;
  if (mdb_txn_begin(lc->env, NULL, MDB_RDONLY, &txn) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                        "Failed to begin read transaction");
    return NULL;
  }

  MDB_cursor *cursor;
  if (mdb_cursor_open(txn, lc->dbi_by_created, &cursor) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_IO,
                        "Failed to open cursor");
    mdb_txn_abort(txn);
    return NULL;
  }

  GPtrArray *results = g_ptr_array_new_with_free_func((GDestroyNotify)bc_db_blob_meta_free);

  MDB_val ck, cv;
  int op;

  if (cursor_sha256 != NULL && cursor_sha256[0] != '\0') {
    /* Look up the cursor blob's created_at to position the cursor */
    MDB_val bk = { .mv_size = 64, .mv_data = (void *)cursor_sha256 };
    MDB_val bv;
    if (mdb_get(txn, lc->dbi_blobs, &bk, &bv) == 0 && bv.mv_size >= 16) {
      int64_t cursor_created = read_i64_le((const uint8_t *)bv.mv_data + 8);

      /* Position just before this entry */
      uint8_t start_key[72];
      make_created_key(cursor_created, cursor_sha256, start_key);
      ck.mv_size = 72;
      ck.mv_data = start_key;
      op = MDB_SET_RANGE;

      if (mdb_cursor_get(cursor, &ck, &cv, op) == 0) {
        /* Skip past the cursor entry, then iterate backwards (DESC order) */
        /* Actually, we need DESC order. LMDB iterates ASC by default.
         * Since keys are big-endian timestamps, we need to go PREV from the cursor. */
        /* First move past the cursor entry */
        op = MDB_PREV;
      } else {
        /* Cursor not found in index, start from the end */
        op = MDB_LAST;
      }
    } else {
      op = MDB_LAST;
    }
  } else {
    /* Start from the most recent (largest key = most recent created_at) */
    op = MDB_LAST;
  }

  uint32_t count = 0;
  while (count < limit && mdb_cursor_get(cursor, &ck, &cv, op) == 0) {
    op = MDB_PREV; /* subsequent iterations go backwards */

    if (ck.mv_size != 72) continue;

    /* Extract sha256 from the index key */
    char sha256[65];
    memcpy(sha256, (const uint8_t *)ck.mv_data + 8, 64);
    sha256[64] = '\0';

    /* Look up full metadata */
    MDB_val bk = { .mv_size = 64, .mv_data = sha256 };
    MDB_val bv;
    if (mdb_get(txn, lc->dbi_blobs, &bk, &bv) == 0) {
      BcDbBlobMeta *meta = deserialize_meta(sha256, bv.mv_data, bv.mv_size);
      if (meta != NULL) {
        g_ptr_array_add(results, meta);
        count++;
      }
    }
  }

  mdb_cursor_close(cursor);
  mdb_txn_abort(txn);
  return results;
}

static GPtrArray *
lmdb_evict_candidates(void *ctx, int64_t bytes_to_free, GError **error)
{
  LmdbCtx *lc = ctx;

  MDB_txn *txn;
  if (mdb_txn_begin(lc->env, NULL, MDB_RDONLY, &txn) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                        "Failed to begin read transaction");
    return NULL;
  }

  MDB_cursor *cursor;
  if (mdb_cursor_open(txn, lc->dbi_by_access, &cursor) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_IO,
                        "Failed to open cursor");
    mdb_txn_abort(txn);
    return NULL;
  }

  GPtrArray *candidates = g_ptr_array_new_with_free_func((GDestroyNotify)bc_db_blob_meta_free);
  int64_t freed = 0;
  MDB_val ak, av;

  /* Iterate from oldest access (smallest key = earliest timestamp) */
  while (freed < bytes_to_free &&
         mdb_cursor_get(cursor, &ak, &av, (freed == 0 ? MDB_FIRST : MDB_NEXT)) == 0) {
    if (ak.mv_size != 72) continue;

    char sha256[65];
    memcpy(sha256, (const uint8_t *)ak.mv_data + 8, 64);
    sha256[64] = '\0';

    /* Look up size from main record */
    MDB_val bk = { .mv_size = 64, .mv_data = sha256 };
    MDB_val bv;
    if (mdb_get(txn, lc->dbi_blobs, &bk, &bv) == 0 && bv.mv_size >= 8) {
      BcDbBlobMeta *meta = g_new0(BcDbBlobMeta, 1);
      meta->sha256 = g_strdup(sha256);
      meta->size = read_i64_le(bv.mv_data);
      g_ptr_array_add(candidates, meta);
      freed += meta->size;
    }
  }

  mdb_cursor_close(cursor);
  mdb_txn_abort(txn);
  return candidates;
}

static bool
lmdb_is_persistent(void *ctx)
{
  (void)ctx;
  return true;
}

static void
lmdb_destroy(void *ctx)
{
  LmdbCtx *lc = ctx;
  if (lc->env != NULL)
    mdb_env_close(lc->env);
  g_free(lc);
}

/* ── Public constructor ────────────────────────────────────────────────── */

BcDbBackend *
bc_db_backend_lmdb_new(const char *env_path, uint32_t map_size_mb, GError **error)
{
  g_return_val_if_fail(env_path != NULL, NULL);

  if (map_size_mb == 0)
    map_size_mb = 256;

  /* Ensure directory exists */
  if (g_mkdir_with_parents(env_path, 0755) != 0) {
    g_set_error(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_ENV,
                "Failed to create LMDB directory: %s", env_path);
    return NULL;
  }

  LmdbCtx *lc = g_new0(LmdbCtx, 1);

  int rc = mdb_env_create(&lc->env);
  if (rc != 0) {
    g_set_error(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_ENV,
                "mdb_env_create failed: %s", mdb_strerror(rc));
    g_free(lc);
    return NULL;
  }

  mdb_env_set_maxdbs(lc->env, LMDB_MAX_DBS);
  mdb_env_set_mapsize(lc->env, (size_t)map_size_mb * 1024ULL * 1024ULL);

  rc = mdb_env_open(lc->env, env_path, 0, 0644);
  if (rc != 0) {
    g_set_error(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_ENV,
                "mdb_env_open(%s) failed: %s", env_path, mdb_strerror(rc));
    mdb_env_close(lc->env);
    g_free(lc);
    return NULL;
  }

  /* Open named databases */
  MDB_txn *txn;
  rc = mdb_txn_begin(lc->env, NULL, 0, &txn);
  if (rc != 0) {
    g_set_error(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                "mdb_txn_begin failed: %s", mdb_strerror(rc));
    mdb_env_close(lc->env);
    g_free(lc);
    return NULL;
  }

  if (mdb_dbi_open(txn, "blobs", MDB_CREATE, &lc->dbi_blobs) != 0 ||
      mdb_dbi_open(txn, "by_access", MDB_CREATE, &lc->dbi_by_access) != 0 ||
      mdb_dbi_open(txn, "by_created", MDB_CREATE, &lc->dbi_by_created) != 0) {
    g_set_error_literal(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_ENV,
                        "Failed to open LMDB named databases");
    mdb_txn_abort(txn);
    mdb_env_close(lc->env);
    g_free(lc);
    return NULL;
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    g_set_error(error, BC_DB_LMDB_ERROR, BC_DB_LMDB_ERROR_TXN,
                "mdb_txn_commit failed: %s", mdb_strerror(rc));
    mdb_env_close(lc->env);
    g_free(lc);
    return NULL;
  }

  BcDbBackend *backend = g_new0(BcDbBackend, 1);
  backend->ctx              = lc;
  backend->contains         = lmdb_contains;
  backend->get_info         = lmdb_get_info;
  backend->get_total_size   = lmdb_get_total_size;
  backend->get_blob_count   = lmdb_get_blob_count;
  backend->put_meta         = lmdb_put_meta;
  backend->delete_meta      = lmdb_delete_meta;
  backend->list_blobs       = lmdb_list_blobs;
  backend->evict_candidates = lmdb_evict_candidates;
  backend->is_persistent    = lmdb_is_persistent;
  backend->destroy          = lmdb_destroy;

  return backend;
}

#endif /* HAVE_LMDB */
