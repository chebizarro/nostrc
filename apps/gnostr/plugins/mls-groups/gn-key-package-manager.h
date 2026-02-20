/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-key-package-manager.h - MLS Key Package Lifecycle Manager
 *
 * Manages the creation, publication, and rotation of MLS key packages
 * (kind:443 events) and key package relay lists (kind:10051).
 *
 * Key packages are the entry point for MLS group membership — other users
 * fetch our key package from relays to add us to a group.
 *
 * Lifecycle:
 *   1. On login: check if a valid key package exists on relays
 *   2. If missing/expired: create via marmot and sign via D-Bus signer
 *   3. Publish to user's relays
 *   4. Publish kind:10051 relay list for key package discovery
 *   5. Rotate when epoch changes or after a configurable interval
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_KEY_PACKAGE_MANAGER_H
#define GN_KEY_PACKAGE_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>
#include "gn-marmot-service.h"

/* Forward declaration — full definition in gnostr-plugin-api.h */
#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_KEY_PACKAGE_MANAGER (gn_key_package_manager_get_type())
G_DECLARE_FINAL_TYPE(GnKeyPackageManager, gn_key_package_manager,
                     GN, KEY_PACKAGE_MANAGER, GObject)

/**
 * gn_key_package_manager_new:
 * @service: The marmot service
 * @plugin_context: The plugin context (for signing and publishing)
 *
 * Creates a new key package manager.
 *
 * Returns: (transfer full): A new #GnKeyPackageManager
 */
GnKeyPackageManager *gn_key_package_manager_new(GnMarmotService     *service,
                                                 GnostrPluginContext *plugin_context);

/**
 * gn_key_package_manager_ensure_key_package_async:
 * @self: The manager
 * @cancellable: (nullable): a GCancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Ensure a valid key package is published for the current user.
 * Creates and publishes a new one if needed.
 */
void gn_key_package_manager_ensure_key_package_async(GnKeyPackageManager *self,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);

/**
 * gn_key_package_manager_ensure_key_package_finish:
 * @self: The manager
 * @result: A GAsyncResult
 * @error: (nullable): Return location for a GError
 *
 * Returns: %TRUE if a valid key package is published
 */
gboolean gn_key_package_manager_ensure_key_package_finish(GnKeyPackageManager *self,
                                                           GAsyncResult        *result,
                                                           GError             **error);

/**
 * gn_key_package_manager_rotate_async:
 * @self: The manager
 * @cancellable: (nullable): a GCancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Force rotate the key package — create a new one and publish it.
 */
void gn_key_package_manager_rotate_async(GnKeyPackageManager *self,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data);

gboolean gn_key_package_manager_rotate_finish(GnKeyPackageManager *self,
                                               GAsyncResult        *result,
                                               GError             **error);

/**
 * gn_key_package_manager_publish_relay_list_async:
 * @self: The manager
 * @relay_urls: (array zero-terminated=1): Relay URLs for key package discovery
 * @cancellable: (nullable): a GCancellable
 * @callback: Callback
 * @user_data: User data
 *
 * Publish a kind:10051 relay list for key package discovery.
 */
void gn_key_package_manager_publish_relay_list_async(GnKeyPackageManager  *self,
                                                      const gchar * const  *relay_urls,
                                                      GCancellable         *cancellable,
                                                      GAsyncReadyCallback   callback,
                                                      gpointer              user_data);

gboolean gn_key_package_manager_publish_relay_list_finish(GnKeyPackageManager *self,
                                                           GAsyncResult        *result,
                                                           GError             **error);

G_END_DECLS

#endif /* GN_KEY_PACKAGE_MANAGER_H */
