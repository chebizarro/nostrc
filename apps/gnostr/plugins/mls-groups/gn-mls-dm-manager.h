/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-dm-manager.h - MLS Direct Message Manager
 *
 * Manages 1-on-1 MLS direct messages using the Whitenoise DirectMessage
 * group type. Each DM conversation is a 2-person MLS group, providing
 * forward secrecy unlike NIP-17.
 *
 * Flow:
 *   1. Caller requests a DM with a pubkey
 *   2. Manager checks if a DirectMessage group already exists for that peer
 *   3. If not: fetch peer's key package, create 2-person MLS group (type: DM)
 *   4. Send welcome to peer via NIP-59 gift wrap
 *   5. Return the group for use with the normal chat UI
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MLS_DM_MANAGER_H
#define GN_MLS_DM_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>
#include "gn-marmot-service.h"
#include "gn-mls-event-router.h"

/* Forward declaration */
#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_MLS_DM_MANAGER (gn_mls_dm_manager_get_type())
G_DECLARE_FINAL_TYPE(GnMlsDmManager, gn_mls_dm_manager,
                     GN, MLS_DM_MANAGER, GObject)

/**
 * gn_mls_dm_manager_new:
 * @service: The marmot service
 * @router: The MLS event router
 * @plugin_context: The plugin context (borrowed)
 *
 * Returns: (transfer full): A new #GnMlsDmManager
 */
GnMlsDmManager *gn_mls_dm_manager_new(GnMarmotService     *service,
                                        GnMlsEventRouter   *router,
                                        GnostrPluginContext *plugin_context);

/**
 * gn_mls_dm_manager_open_dm_async:
 * @self: The manager
 * @peer_pubkey_hex: Peer's Nostr public key (64 hex chars)
 * @cancellable: (nullable): a #GCancellable
 * @callback: Callback when complete
 * @user_data: User data
 *
 * Opens (or creates) a DirectMessage MLS group with the given peer.
 * If a DM group already exists, returns it immediately.
 * If not, fetches the peer's key package and creates a new 2-person group.
 */
void gn_mls_dm_manager_open_dm_async(GnMlsDmManager      *self,
                                       const gchar          *peer_pubkey_hex,
                                       GCancellable         *cancellable,
                                       GAsyncReadyCallback   callback,
                                       gpointer              user_data);

/**
 * gn_mls_dm_manager_open_dm_finish:
 * @self: The manager
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Returns: (transfer full) (nullable): The DM #MarmotGobjectGroup, or NULL on error
 */
MarmotGobjectGroup *gn_mls_dm_manager_open_dm_finish(GnMlsDmManager *self,
                                                       GAsyncResult    *result,
                                                       GError         **error);

/**
 * gn_mls_dm_manager_get_dm_groups:
 * @self: The manager
 * @error: (nullable): return location for a #GError
 *
 * Returns all existing DirectMessage groups synchronously.
 *
 * Returns: (transfer full) (element-type MarmotGobjectGroup) (nullable):
 *   Array of DM groups, or NULL on error
 */
GPtrArray *gn_mls_dm_manager_get_dm_groups(GnMlsDmManager *self,
                                             GError         **error);

G_END_DECLS

#endif /* GN_MLS_DM_MANAGER_H */
