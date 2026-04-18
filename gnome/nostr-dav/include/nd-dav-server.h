/* nd-dav-server.h - Localhost DAV server for Nostr ↔ GNOME bridging
 *
 * SPDX-License-Identifier: MIT
 *
 * Implements a minimal DAV server that responds to OPTIONS, PROPFIND,
 * and well-known discovery requests. v1 scaffold returns empty
 * calendar and address book collections — subsequent beads wire real
 * NIP-52/NIP-contacts content.
 *
 * All requests are authenticated via HTTP Basic with the bearer token
 * from NdTokenStore as the password (username is ignored).
 */
#ifndef ND_DAV_SERVER_H
#define ND_DAV_SERVER_H

#include <glib.h>
#include <gio/gio.h>
#include "nd-token-store.h"

G_BEGIN_DECLS

#define ND_TYPE_DAV_SERVER (nd_dav_server_get_type())
G_DECLARE_FINAL_TYPE(NdDavServer, nd_dav_server, ND, DAV_SERVER, GObject)

/**
 * nd_dav_server_new:
 * @token_store: (transfer none): token store for auth validation
 *
 * Returns: (transfer full): a new DAV server instance.
 */
NdDavServer *nd_dav_server_new(NdTokenStore *token_store);

/**
 * nd_dav_server_start:
 * @self: the server
 * @address: listen address (e.g. "127.0.0.1")
 * @port: listen port (e.g. 7680)
 * @error: (out) (optional): location for error
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

G_END_DECLS
#endif /* ND_DAV_SERVER_H */
