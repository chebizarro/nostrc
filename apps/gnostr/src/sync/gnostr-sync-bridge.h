/**
 * @file gnostr-sync-bridge.h
 * @brief Bridge between negentropy sync events and UI data refresh
 *
 * Subscribes to negentropy EventBus topics and triggers refresh of
 * affected data consumers (follow list, mute list) when sync detects changes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GNOSTR_SYNC_BRIDGE_H
#define GNOSTR_SYNC_BRIDGE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gnostr_sync_bridge_init:
 * @user_pubkey_hex: (nullable): current user's pubkey for follow list refresh
 *
 * Initialize the sync bridge. Subscribes to negentropy EventBus topics
 * and routes change notifications to the appropriate data services.
 *
 * Call after storage_ndb_init and before starting the sync service.
 * Safe to call with NULL pubkey; set later with _set_user_pubkey().
 */
void gnostr_sync_bridge_init(const char *user_pubkey_hex);

/**
 * gnostr_sync_bridge_set_user_pubkey:
 * @pubkey_hex: (nullable): 64-char hex pubkey, or NULL to clear
 *
 * Updates the user pubkey used for follow list refresh.
 * Called after login or when user changes.
 */
void gnostr_sync_bridge_set_user_pubkey(const char *pubkey_hex);

/**
 * gnostr_sync_bridge_shutdown:
 *
 * Unsubscribes from EventBus and cleans up. Call at app shutdown
 * before the EventBus is destroyed.
 */
void gnostr_sync_bridge_shutdown(void);

G_END_DECLS

#endif /* GNOSTR_SYNC_BRIDGE_H */
