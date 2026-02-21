/*
 * bc-cache-manager.h - BcCacheManager: cache policy, eviction, and upstream orchestration
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef BC_CACHE_MANAGER_H
#define BC_CACHE_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>
#include "bc-blob-store.h"

G_BEGIN_DECLS

typedef struct _BcUpstreamClient BcUpstreamClient;

#define BC_TYPE_CACHE_MANAGER (bc_cache_manager_get_type())
G_DECLARE_FINAL_TYPE(BcCacheManager, bc_cache_manager, BC, CACHE_MANAGER, GObject)

typedef enum {
  BC_CACHE_MANAGER_ERROR_UPSTREAM_FAILED,
  BC_CACHE_MANAGER_ERROR_EVICTION_FAILED,
} BcCacheManagerError;

#define BC_CACHE_MANAGER_ERROR (bc_cache_manager_error_quark())
GQuark bc_cache_manager_error_quark(void);

BcCacheManager *bc_cache_manager_new(BcBlobStore *store, BcUpstreamClient *upstream,
                                     gint64 max_cache_bytes, gint64 max_blob_bytes,
                                     gboolean verify_hash);
GBytes         *bc_cache_manager_get_blob(BcCacheManager *self, const gchar *sha256,
                                          BcBlobInfo **out_info, GError **error);
GBytes         *bc_cache_manager_get_blob_with_hints(BcCacheManager *self,
                                                     const gchar *sha256,
                                                     const gchar * const *server_hints,
                                                     BcBlobInfo **out_info,
                                                     GError **error);
gboolean        bc_cache_manager_put_blob(BcCacheManager *self, const gchar *sha256,
                                          GBytes *data, const gchar *mime_type,
                                          GError **error);
gint            bc_cache_manager_run_eviction(BcCacheManager *self, GError **error);

G_END_DECLS

#endif /* BC_CACHE_MANAGER_H */
