/* nd-dav-server.h - Localhost DAV server for Nostr ↔ GNOME bridging
 *
 * SPDX-License-Identifier: MIT
 *
 * CalDAV/CardDAV/WebDAV server over SQLite-backed stores.
 *
 * Fail-closed: nd_dav_server_start() refuses to listen until an account
 * is configured and its token is loaded in the NdTokenStore, and refuses
 * any non-loopback address. Every request except OPTIONS and the
 * .well-known redirects must carry HTTP Basic auth whose password is the
 * bearer token (username is ignored).
 */
#ifndef ND_DAV_SERVER_H
#define ND_DAV_SERVER_H

#include <glib.h>
#include <gio/gio.h>
#include "nd-token-store.h"
#include "nd-store-db.h"

G_BEGIN_DECLS

#define ND_DAV_SERVER_ERROR (nd_dav_server_error_quark())
GQuark nd_dav_server_error_quark(void);

typedef enum {
  ND_DAV_SERVER_ERROR_BIND = 1,
  ND_DAV_SERVER_ERROR_ALREADY_RUNNING,
  ND_DAV_SERVER_ERROR_NOT_CONFIGURED,
  ND_DAV_SERVER_ERROR_NOT_LOOPBACK
} NdDavServerError;

#define ND_TYPE_DAV_SERVER (nd_dav_server_get_type())
G_DECLARE_FINAL_TYPE(NdDavServer, nd_dav_server, ND, DAV_SERVER, GObject)

/**
 * nd_dav_server_new:
 * @token_store: (transfer none): token store for auth validation; must
 *   outlive the server
 * @db: (transfer none): store database; the server takes a reference
 *
 * Returns: (transfer full): a new DAV server instance.
 */
NdDavServer *nd_dav_server_new(NdTokenStore *token_store,
                               NdStoreDb    *db);

/**
 * nd_dav_server_set_account_id:
 * @self: the server
 * @account_id: account whose token authorizes requests
 */
void nd_dav_server_set_account_id(NdDavServer *self,
                                  const gchar *account_id);

/**
 * nd_dav_server_start:
 * @self: the server
 * @address: loopback listen address (e.g. "127.0.0.1")
 * @port: listen port (e.g. 7680); 0 picks an ephemeral port
 * @error: (out) (optional): location for error
 *
 * Fails with %ND_DAV_SERVER_ERROR_NOT_CONFIGURED if no account is set or
 * its token is not loaded, and with %ND_DAV_SERVER_ERROR_NOT_LOOPBACK for
 * a non-loopback @address. Binding is the last step.
 *
 * Returns: TRUE on success.
 */
gboolean nd_dav_server_start(NdDavServer *self,
                             const gchar *address,
                             guint        port,
                             GError     **error);

/**
 * nd_dav_server_stop:
 * @self: the server
 */
void nd_dav_server_stop(NdDavServer *self);

/**
 * nd_dav_server_is_running:
 * @self: the server
 *
 * Returns: TRUE if listening.
 */
gboolean nd_dav_server_is_running(NdDavServer *self);

/**
 * nd_dav_server_get_port:
 *
 * Returns: the bound port while running, else 0.
 */
guint nd_dav_server_get_port(NdDavServer *self);

G_END_DECLS
#endif /* ND_DAV_SERVER_H */
