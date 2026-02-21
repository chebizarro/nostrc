/*
 * bc-http-server.h - BcHttpServer: local Blossom-compatible HTTP server
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef BC_HTTP_SERVER_H
#define BC_HTTP_SERVER_H

#include <glib-object.h>
#include <gio/gio.h>
#include "bc-blob-store.h"
#include "bc-cache-manager.h"

G_BEGIN_DECLS

#define BC_TYPE_HTTP_SERVER (bc_http_server_get_type())
G_DECLARE_FINAL_TYPE(BcHttpServer, bc_http_server, BC, HTTP_SERVER, GObject)

typedef enum {
  BC_HTTP_SERVER_ERROR_BIND,
  BC_HTTP_SERVER_ERROR_ALREADY_RUNNING,
} BcHttpServerError;

#define BC_HTTP_SERVER_ERROR (bc_http_server_error_quark())
GQuark bc_http_server_error_quark(void);

BcHttpServer *bc_http_server_new(BcBlobStore *store, BcCacheManager *cache_mgr);
gboolean      bc_http_server_start(BcHttpServer *self, const gchar *address,
                                   guint port, GError **error);
void          bc_http_server_stop(BcHttpServer *self);
gboolean      bc_http_server_is_running(BcHttpServer *self);

G_END_DECLS

#endif /* BC_HTTP_SERVER_H */
