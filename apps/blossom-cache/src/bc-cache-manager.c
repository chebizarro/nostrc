/*
 * bc-cache-manager.c - BcCacheManager: cache policy and upstream orchestration
 *
 * SPDX-License-Identifier: MIT
 */

#include "bc-cache-manager.h"
#include "bc-upstream-client.h"

#include <gio/gio.h>

/* ---- Error quark ---- */

G_DEFINE_QUARK(bc-cache-manager-error-quark, bc_cache_manager_error)

/* ---- Private structure ---- */

struct _BcCacheManager {
  GObject           parent_instance;

  BcBlobStore      *store;       /* not owned */
  BcUpstreamClient *upstream;    /* not owned */
  gint64            max_cache_bytes;
  gint64            max_blob_bytes;
  gboolean          verify_hash;
};

G_DEFINE_TYPE(BcCacheManager, bc_cache_manager, G_TYPE_OBJECT)

/* ---- GObject lifecycle ---- */

static void
bc_cache_manager_dispose(GObject *obj)
{
  BcCacheManager *self = BC_CACHE_MANAGER(obj);
  self->store    = NULL;
  self->upstream = NULL;
  G_OBJECT_CLASS(bc_cache_manager_parent_class)->dispose(obj);
}

static void
bc_cache_manager_class_init(BcCacheManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = bc_cache_manager_dispose;
}

static void
bc_cache_manager_init(BcCacheManager *self)
{
  (void)self;
}

/* ---- Public API ---- */

BcCacheManager *
bc_cache_manager_new(BcBlobStore      *store,
                     BcUpstreamClient *upstream,
                     gint64            max_cache_bytes,
                     gint64            max_blob_bytes,
                     gboolean          verify_hash)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(store), NULL);
  g_return_val_if_fail(BC_IS_UPSTREAM_CLIENT(upstream), NULL);

  BcCacheManager *self = g_object_new(BC_TYPE_CACHE_MANAGER, NULL);
  self->store           = store;
  self->upstream        = upstream;
  self->max_cache_bytes = max_cache_bytes;
  self->max_blob_bytes  = max_blob_bytes;
  self->verify_hash     = verify_hash;

  return self;
}

static GBytes *
get_blob_internal(BcCacheManager      *self,
                  const gchar         *sha256,
                  const gchar * const *server_hints,
                  BcBlobInfo         **out_info,
                  GError             **error)
{
  /* Try local store first */
  if (bc_blob_store_contains(self->store, sha256)) {
    g_autoptr(BcBlobInfo) info = bc_blob_store_get_info(self->store, sha256, error);
    if (info == NULL)
      return NULL;

    g_autofree gchar *path = bc_blob_store_get_content_path(self->store, sha256);
    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, error))
      return NULL;

    if (out_info != NULL)
      *out_info = g_steal_pointer(&info);

    g_debug("cache HIT: %s (%" G_GSIZE_FORMAT " bytes)", sha256, length);
    return g_bytes_new_take(contents, length);
  }

  /* Cache miss — fetch from upstream (with optional proxy hints) */
  g_debug("cache MISS: %s — fetching from upstream", sha256);

  GError *fetch_err = NULL;
  g_autoptr(BcFetchResult) result = NULL;

  if (server_hints != NULL && server_hints[0] != NULL) {
    result = bc_upstream_client_fetch_with_hints(self->upstream, sha256,
                                                 server_hints, NULL, &fetch_err);
  } else {
    result = bc_upstream_client_fetch(self->upstream, sha256, NULL, &fetch_err);
  }

  if (result == NULL) {
    g_propagate_error(error, fetch_err);
    return NULL;
  }

  gsize blob_size = g_bytes_get_size(result->data);

  /* Check single-blob size limit before caching */
  if (self->max_blob_bytes > 0 && (gint64)blob_size > self->max_blob_bytes) {
    g_debug("blob %s too large (%" G_GSIZE_FORMAT " > %" G_GINT64_FORMAT
            ") — serving without caching",
            sha256, blob_size, self->max_blob_bytes);

    if (out_info != NULL) {
      BcBlobInfo *info = g_new0(BcBlobInfo, 1);
      info->sha256    = g_strdup(sha256);
      info->size      = (gint64)blob_size;
      info->mime_type = g_strdup(result->mime_type);
      *out_info = info;
    }
    return g_bytes_ref(result->data);
  }

  /* Make room if needed */
  if (self->max_cache_bytes > 0) {
    gint64 current = bc_blob_store_get_total_size(self->store);
    gint64 needed = current + (gint64)blob_size - self->max_cache_bytes;

    if (needed > 0) {
      GError *evict_err = NULL;
      gint evicted = bc_blob_store_evict_lru(self->store, needed, &evict_err);
      if (evicted < 0) {
        g_warning("eviction failed while caching %s: %s",
                  sha256, evict_err ? evict_err->message : "unknown");
        g_clear_error(&evict_err);
      }
    }
  }

  /* Store in local cache */
  GError *store_err = NULL;
  gboolean stored = bc_blob_store_put(self->store, sha256, result->data,
                                      result->mime_type, self->verify_hash,
                                      &store_err);
  if (!stored) {
    g_warning("failed to cache blob %s: %s",
              sha256, store_err ? store_err->message : "unknown");
    g_clear_error(&store_err);
  }

  if (out_info != NULL) {
    if (stored) {
      *out_info = bc_blob_store_get_info(self->store, sha256, NULL);
    } else {
      BcBlobInfo *info = g_new0(BcBlobInfo, 1);
      info->sha256    = g_strdup(sha256);
      info->size      = (gint64)blob_size;
      info->mime_type = g_strdup(result->mime_type);
      *out_info = info;
    }
  }

  return g_bytes_ref(result->data);
}

GBytes *
bc_cache_manager_get_blob(BcCacheManager  *self,
                          const gchar     *sha256,
                          BcBlobInfo     **out_info,
                          GError         **error)
{
  g_return_val_if_fail(BC_IS_CACHE_MANAGER(self), NULL);
  g_return_val_if_fail(sha256 != NULL, NULL);

  return get_blob_internal(self, sha256, NULL, out_info, error);
}

GBytes *
bc_cache_manager_get_blob_with_hints(BcCacheManager      *self,
                                     const gchar         *sha256,
                                     const gchar * const *server_hints,
                                     BcBlobInfo         **out_info,
                                     GError             **error)
{
  g_return_val_if_fail(BC_IS_CACHE_MANAGER(self), NULL);
  g_return_val_if_fail(sha256 != NULL, NULL);

  return get_blob_internal(self, sha256, server_hints, out_info, error);
}

gboolean
bc_cache_manager_put_blob(BcCacheManager *self,
                          const gchar    *sha256,
                          GBytes         *data,
                          const gchar    *mime_type,
                          GError        **error)
{
  g_return_val_if_fail(BC_IS_CACHE_MANAGER(self), FALSE);
  g_return_val_if_fail(sha256 != NULL, FALSE);
  g_return_val_if_fail(data != NULL, FALSE);

  gsize blob_size = g_bytes_get_size(data);

  if (self->max_blob_bytes > 0 && (gint64)blob_size > self->max_blob_bytes) {
    g_set_error(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_TOO_LARGE,
                "Blob %s is too large (%" G_GSIZE_FORMAT " bytes, max %"
                G_GINT64_FORMAT ")",
                sha256, blob_size, self->max_blob_bytes);
    return FALSE;
  }

  if (self->max_cache_bytes > 0) {
    gint64 current = bc_blob_store_get_total_size(self->store);
    gint64 needed = current + (gint64)blob_size - self->max_cache_bytes;

    if (needed > 0) {
      GError *evict_err = NULL;
      gint evicted = bc_blob_store_evict_lru(self->store, needed, &evict_err);
      if (evicted < 0) {
        g_propagate_prefixed_error(error, evict_err,
                                   "Eviction failed before storing %s: ", sha256);
        return FALSE;
      }
    }
  }

  return bc_blob_store_put(self->store, sha256, data, mime_type,
                           self->verify_hash, error);
}

gint
bc_cache_manager_run_eviction(BcCacheManager *self, GError **error)
{
  g_return_val_if_fail(BC_IS_CACHE_MANAGER(self), -1);

  if (self->max_cache_bytes <= 0)
    return 0;

  gint64 current = bc_blob_store_get_total_size(self->store);
  gint64 overage = current - self->max_cache_bytes;

  if (overage <= 0)
    return 0;

  return bc_blob_store_evict_lru(self->store, overage, error);
}
