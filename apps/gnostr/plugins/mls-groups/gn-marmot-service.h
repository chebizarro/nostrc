/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-marmot-service.h - Marmot Protocol Service
 *
 * Singleton service managing the MarmotGobjectClient lifecycle.
 * Provides the bridge between the gnostr plugin system and libmarmot.
 *
 * The service owns the MarmotGobjectClient and its SQLite storage backend.
 * It is created on plugin activation and destroyed on deactivation.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MARMOT_SERVICE_H
#define GN_MARMOT_SERVICE_H

#include <glib-object.h>
#include <gio/gio.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

G_BEGIN_DECLS

#define GN_TYPE_MARMOT_SERVICE (gn_marmot_service_get_type())
G_DECLARE_FINAL_TYPE(GnMarmotService, gn_marmot_service, GN, MARMOT_SERVICE, GObject)

/**
 * gn_marmot_service_get_default:
 *
 * Get the singleton Marmot service instance.
 *
 * Returns: (transfer none) (nullable): The service, or NULL if not initialized
 */
GnMarmotService *gn_marmot_service_get_default(void);

/**
 * gn_marmot_service_initialize:
 * @data_dir: Base data directory (e.g., ~/.local/share/gnostr)
 * @error: (nullable): Return location for a GError
 *
 * Initialize the singleton Marmot service. Creates the SQLite storage
 * backend and MarmotGobjectClient. Call once on plugin activation.
 *
 * Returns: (transfer none): The initialized service, or NULL on error
 */
GnMarmotService *gn_marmot_service_initialize(const gchar *data_dir,
                                               GError     **error);

/**
 * gn_marmot_service_shutdown:
 *
 * Shut down the singleton Marmot service. Releases the MarmotGobjectClient
 * and SQLite storage. Call on plugin deactivation.
 */
void gn_marmot_service_shutdown(void);

/**
 * gn_marmot_service_get_client:
 * @self: The service
 *
 * Get the underlying MarmotGobjectClient.
 *
 * Returns: (transfer none): The marmot client
 */
MarmotGobjectClient *gn_marmot_service_get_client(GnMarmotService *self);

/**
 * gn_marmot_service_get_user_pubkey_hex:
 * @self: The service
 *
 * Get the current user's public key as hex.
 *
 * Returns: (transfer none) (nullable): The user's pubkey hex, or NULL
 */
const gchar *gn_marmot_service_get_user_pubkey_hex(GnMarmotService *self);

/**
 * gn_marmot_service_set_user_identity:
 * @self: The service
 * @pubkey_hex: User's public key (64 hex chars)
 * @secret_key_hex: (nullable): User's secret key (64 hex chars), or NULL
 *
 * Set the current user identity. The secret key is needed for MLS
 * credential creation and message signing.
 */
void gn_marmot_service_set_user_identity(GnMarmotService *self,
                                          const gchar     *pubkey_hex,
                                          const gchar     *secret_key_hex);

/* ── Signals ──────────────────────────────────────────────────────── */

/**
 * GnMarmotService::group-created:
 * @service: The service
 * @group: (transfer none): The created MarmotGobjectGroup
 *
 * Emitted when a new group is created locally.
 */

/**
 * GnMarmotService::group-joined:
 * @service: The service
 * @group: (transfer none): The joined MarmotGobjectGroup
 *
 * Emitted when a group is joined via welcome acceptance.
 */

/**
 * GnMarmotService::message-received:
 * @service: The service
 * @group_id_hex: MLS group ID as hex
 * @inner_event_json: Decrypted inner event JSON
 *
 * Emitted when a group message is decrypted successfully.
 */

/**
 * GnMarmotService::welcome-received:
 * @service: The service
 * @welcome: (transfer none): The MarmotGobjectWelcome
 *
 * Emitted when a new welcome (group invitation) is received.
 */

/**
 * GnMarmotService::group-updated:
 * @service: The service
 * @group: (transfer none): The updated MarmotGobjectGroup
 *
 * Emitted when group metadata is updated (name, members, epoch, etc.).
 */

G_END_DECLS

#endif /* GN_MARMOT_SERVICE_H */
