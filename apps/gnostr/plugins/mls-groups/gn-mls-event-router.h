/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-event-router.h - MLS Event Routing
 *
 * Routes incoming Nostr events to the appropriate marmot processing:
 *
 * - kind:1059 gift wraps → NIP-59 unwrap → kind:444 welcome or kind:445 message
 * - kind:445 direct → group message processing
 *
 * The router handles the NIP-59 unwrapping, NIP-44 decryption, and
 * dispatching to the MarmotGobjectClient for MLS processing.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_MLS_EVENT_ROUTER_H
#define GN_MLS_EVENT_ROUTER_H

#include <glib-object.h>
#include <gio/gio.h>
#include "gn-marmot-service.h"

/* Forward declaration — full definition in gnostr-plugin-api.h */
#ifndef GNOSTR_PLUGIN_API_H
typedef struct _GnostrPluginContext GnostrPluginContext;
#endif

G_BEGIN_DECLS

#define GN_TYPE_MLS_EVENT_ROUTER (gn_mls_event_router_get_type())
G_DECLARE_FINAL_TYPE(GnMlsEventRouter, gn_mls_event_router,
                     GN, MLS_EVENT_ROUTER, GObject)

/**
 * gn_mls_event_router_new:
 * @service: The marmot service
 * @plugin_context: The plugin context
 *
 * Creates a new MLS event router.
 *
 * Returns: (transfer full): A new #GnMlsEventRouter
 */
GnMlsEventRouter *gn_mls_event_router_new(GnMarmotService     *service,
                                            GnostrPluginContext *plugin_context);

/**
 * gn_mls_event_router_process_gift_wrap:
 * @self: The router
 * @gift_wrap_json: JSON of the kind:1059 event
 *
 * Process an incoming gift-wrapped event. Unwraps via NIP-59 and routes
 * the inner event (kind:444 welcome or kind:445 message) to marmot.
 *
 * This is async — results are delivered via GnMarmotService signals.
 */
void gn_mls_event_router_process_gift_wrap(GnMlsEventRouter *self,
                                            const gchar       *gift_wrap_json);

/**
 * gn_mls_event_router_process_group_message:
 * @self: The router
 * @event_json: JSON of the kind:445 event
 *
 * Process an incoming group message event. Extracts the MLS ciphertext
 * and routes to marmot for decryption.
 *
 * Results are delivered via GnMarmotService::message-received signal.
 */
void gn_mls_event_router_process_group_message(GnMlsEventRouter *self,
                                                const gchar       *event_json);

/**
 * gn_mls_event_router_send_message_async:
 * @self: The router
 * @group_id_hex: MLS group ID as hex
 * @content: Message content (UTF-8)
 * @kind: Inner event kind (typically 9 for chat message)
 * @cancellable: (nullable): a GCancellable
 * @callback: Callback when complete
 * @user_data: User data
 *
 * Send a message to a group. Creates the inner event, encrypts via marmot,
 * signs with ephemeral key, and publishes to group relays.
 */
void gn_mls_event_router_send_message_async(GnMlsEventRouter   *self,
                                             const gchar         *group_id_hex,
                                             const gchar         *content,
                                             guint16              kind,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);

gboolean gn_mls_event_router_send_message_finish(GnMlsEventRouter *self,
                                                   GAsyncResult      *result,
                                                   GError           **error);

/**
 * gn_mls_event_router_send_welcome_async:
 * @self: The router
 * @recipient_pubkey_hex: Recipient's public key
 * @welcome_rumor_json: JSON of the unsigned kind:444 welcome event
 * @cancellable: (nullable): a GCancellable
 * @callback: Callback
 * @user_data: User data
 *
 * Gift-wrap and send a welcome message to a recipient.
 */
void gn_mls_event_router_send_welcome_async(GnMlsEventRouter   *self,
                                              const gchar         *recipient_pubkey_hex,
                                              const gchar         *welcome_rumor_json,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);

gboolean gn_mls_event_router_send_welcome_finish(GnMlsEventRouter *self,
                                                   GAsyncResult      *result,
                                                   GError           **error);

G_END_DECLS

#endif /* GN_MLS_EVENT_ROUTER_H */
