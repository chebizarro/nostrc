/*
 * bc-upstream-client.h - BcUpstreamClient: fetches blobs from remote Blossom servers
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef BC_UPSTREAM_CLIENT_H
#define BC_UPSTREAM_CLIENT_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define BC_TYPE_UPSTREAM_CLIENT (bc_upstream_client_get_type())
G_DECLARE_FINAL_TYPE(BcUpstreamClient, bc_upstream_client, BC, UPSTREAM_CLIENT, GObject)

typedef enum {
  BC_UPSTREAM_CLIENT_ERROR_ALL_FAILED,
  BC_UPSTREAM_CLIENT_ERROR_NOT_FOUND,
  BC_UPSTREAM_CLIENT_ERROR_IO,
} BcUpstreamClientError;

#define BC_UPSTREAM_CLIENT_ERROR (bc_upstream_client_error_quark())
GQuark bc_upstream_client_error_quark(void);

typedef struct {
  GBytes *data;
  gchar  *mime_type;
  gchar  *server_url;
} BcFetchResult;

BcFetchResult *bc_fetch_result_copy(const BcFetchResult *result);
void           bc_fetch_result_free(BcFetchResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BcFetchResult, bc_fetch_result_free)

BcUpstreamClient *bc_upstream_client_new(const gchar * const *server_urls);
BcFetchResult    *bc_upstream_client_fetch(BcUpstreamClient *self, const gchar *sha256,
                                           GCancellable *cancellable, GError **error);
void              bc_upstream_client_set_servers(BcUpstreamClient *self,
                                                const gchar * const *server_urls);

/**
 * bc_upstream_client_fetch_from_servers:
 * @self: the upstream client
 * @sha256: blob hash to fetch
 * @server_hints: (nullable): NULL-terminated array of server URLs to try first (xs hints)
 * @cancellable: (nullable): optional cancellable
 * @error: return location for error
 *
 * Tries the server_hints first, then falls back to the configured upstream servers.
 * Used for BUD-10 proxy hint support.
 *
 * Returns: (transfer full) (nullable): fetch result, or %NULL on failure
 */
BcFetchResult    *bc_upstream_client_fetch_with_hints(BcUpstreamClient *self,
                                                      const gchar *sha256,
                                                      const gchar * const *server_hints,
                                                      GCancellable *cancellable,
                                                      GError **error);

G_END_DECLS

#endif /* BC_UPSTREAM_CLIENT_H */
