/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-event-router.c - MLS Event Routing
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-event-router.h"
#include <gnostr-plugin-api.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

/* NIP-C7: Chat message kind */
#define KIND_CHAT_MESSAGE 9

struct _GnMlsEventRouter
{
  GObject parent_instance;

  GnMarmotService     *service;    /* weak ref */
  GnostrPluginContext *context;    /* borrowed */
};

G_DEFINE_TYPE(GnMlsEventRouter, gn_mls_event_router, G_TYPE_OBJECT)

static void
gn_mls_event_router_dispose(GObject *object)
{
  GnMlsEventRouter *self = GN_MLS_EVENT_ROUTER(object);
  self->service = NULL;
  self->context = NULL;
  G_OBJECT_CLASS(gn_mls_event_router_parent_class)->dispose(object);
}

static void
gn_mls_event_router_class_init(GnMlsEventRouterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_mls_event_router_dispose;
}

static void
gn_mls_event_router_init(GnMlsEventRouter *self)
{
  self->service = NULL;
  self->context = NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: Process welcome after NIP-59 unwrap
 * ══════════════════════════════════════════════════════════════════════════ */

static void
on_welcome_processed(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GnMlsEventRouter *self = GN_MLS_EVENT_ROUTER(user_data);
  g_autoptr(GError) error = NULL;

  MarmotGobjectClient *client = MARMOT_GOBJECT_CLIENT(source);
  g_autoptr(MarmotGobjectWelcome) welcome =
    marmot_gobject_client_process_welcome_finish(client, result, &error);

  if (welcome == NULL)
    {
      g_warning("MLS EventRouter: failed to process welcome: %s",
                error ? error->message : "unknown");
      return;
    }

  g_info("MLS EventRouter: welcome processed successfully");

  /* Emit signal on service */
  GnMarmotService *service = gn_marmot_service_get_default();
  if (service != NULL)
    g_signal_emit_by_name(service, "welcome-received", welcome);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: Process group message after NIP-44 decryption
 * ══════════════════════════════════════════════════════════════════════════ */

static void
on_message_processed(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GnMlsEventRouter *self = GN_MLS_EVENT_ROUTER(user_data);
  g_autoptr(GError) error = NULL;

  MarmotGobjectClient *client = MARMOT_GOBJECT_CLIENT(source);
  MarmotGobjectMessageResultType result_type;

  g_autofree gchar *inner_json =
    marmot_gobject_client_process_message_finish(client, result,
                                                  &result_type, &error);

  if (error != NULL)
    {
      g_warning("MLS EventRouter: failed to process group message: %s",
                error->message);
      return;
    }

  GnMarmotService *service = gn_marmot_service_get_default();
  if (service == NULL)
    return;

  switch (result_type)
    {
    case MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION:
      if (inner_json != NULL)
        {
          g_debug("MLS EventRouter: application message decrypted");

          /*
           * TODO: Extract group_id_hex from the event's h tag.
           * For now, pass empty string — will be fixed when we add
           * proper event JSON parsing.
           */
          g_signal_emit_by_name(service, "message-received",
                                "", inner_json);
        }
      break;

    case MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT:
      g_debug("MLS EventRouter: commit processed, group state updated");
      /* TODO: Fetch updated group and emit group-updated */
      break;

    case MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE:
      g_debug("MLS EventRouter: skipping own message");
      break;

    default:
      g_debug("MLS EventRouter: unhandled result type %d", result_type);
      break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: NIP-59 unwrap callback
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * After NIP-59 gift wrap unwrapping, we get the inner rumor event.
 * Route it based on kind:
 *   - 444 → marmot welcome processing
 *   - 445 → marmot message processing
 */
static void
on_gift_wrap_unwrapped(gpointer result_ptr, gpointer user_data)
{
  GnMlsEventRouter *self = GN_MLS_EVENT_ROUTER(user_data);

  /*
   * NOTE: This uses the GnostrUnwrapResult from nip59_giftwrap.h.
   * The actual integration requires including the NIP-59 headers and
   * casting the result properly. Shown here conceptually:
   *
   * GnostrUnwrapResult *unwrap = (GnostrUnwrapResult *)result_ptr;
   * if (!unwrap->success) { g_warning(...); return; }
   *
   * NostrEvent *rumor = unwrap->rumor;
   * int kind = nostr_event_get_kind(rumor);
   *
   * if (kind == 444) { // Welcome
   *   char *rumor_json = nostr_event_to_json(rumor);
   *   char *wrapper_id = unwrap->wrapper_event_id_hex;
   *   marmot_gobject_client_process_welcome_async(client,
   *     wrapper_id, rumor_json, NULL, on_welcome_processed, self);
   * } else if (kind == 445) { // Group message
   *   char *event_json = nostr_event_to_json(rumor);
   *   marmot_gobject_client_process_message_async(client,
   *     event_json, NULL, on_message_processed, self);
   * }
   */

  g_debug("MLS EventRouter: gift wrap unwrapped — routing inner event");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: Send message flow
 *
 * 1. Build unsigned inner event (kind:9 chat message)
 * 2. marmot_gobject_client_send_message_async() → kind:445 event JSON
 * 3. The kind:445 event uses an ephemeral pubkey (marmot handles this)
 * 4. Publish to group relays
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  GnMlsEventRouter *router;
  GTask             *task;
} SendMsgData;

static void
on_msg_published(GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  SendMsgData *data = user_data;
  g_autoptr(GError) error = NULL;

  gboolean ok = gnostr_plugin_context_publish_event_finish(
    data->router->context, result, &error);

  if (ok)
    {
      g_info("MLS EventRouter: group message published");
      g_task_return_boolean(data->task, TRUE);
    }
  else
    {
      g_warning("MLS EventRouter: failed to publish group message: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
    }

  g_object_unref(data->task);
  g_free(data);
}

static void
on_msg_encrypted(GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  SendMsgData *data = user_data;
  g_autoptr(GError) error = NULL;

  MarmotGobjectClient *client = MARMOT_GOBJECT_CLIENT(source);
  g_autofree gchar *event_json =
    marmot_gobject_client_send_message_finish(client, result, &error);

  if (event_json == NULL)
    {
      g_warning("MLS EventRouter: failed to create encrypted message: %s",
                error ? error->message : "unknown");
      g_task_return_error(data->task, g_steal_pointer(&error));
      g_object_unref(data->task);
      g_free(data);
      return;
    }

  g_debug("MLS EventRouter: message encrypted, publishing kind:445 event");

  /*
   * The kind:445 event is already signed with an ephemeral key by marmot.
   * We just need to publish it directly to the group's relays.
   *
   * TODO: Publish to group-specific relays rather than user's default relays.
   * For now, use the default publish which goes to user's write relays.
   */
  gnostr_plugin_context_publish_event_async(
    data->router->context,
    event_json,
    g_task_get_cancellable(data->task),
    on_msg_published,
    data);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

GnMlsEventRouter *
gn_mls_event_router_new(GnMarmotService     *service,
                         GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnMlsEventRouter *self = g_object_new(GN_TYPE_MLS_EVENT_ROUTER, NULL);
  self->service = service;
  self->context = plugin_context;

  return self;
}

void
gn_mls_event_router_process_gift_wrap(GnMlsEventRouter *self,
                                       const gchar       *gift_wrap_json)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(self));
  g_return_if_fail(gift_wrap_json != NULL);

  const gchar *user_pubkey = gn_marmot_service_get_user_pubkey_hex(self->service);
  if (user_pubkey == NULL)
    {
      g_warning("MLS EventRouter: cannot unwrap — no user identity");
      return;
    }

  g_debug("MLS EventRouter: unwrapping gift wrap for MLS processing");

  /*
   * TODO: Parse gift_wrap_json into NostrEvent, then call
   * gnostr_nip59_unwrap_async(event, user_pubkey, NULL,
   *                            on_gift_wrap_unwrapped, self);
   *
   * The NIP-59 unwrap will decrypt the seal and rumor using the
   * D-Bus signer for NIP-44 decryption. Once unwrapped, the inner
   * event is routed by kind (444 or 445).
   */
}

void
gn_mls_event_router_process_group_message(GnMlsEventRouter *self,
                                           const gchar       *event_json)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(self));
  g_return_if_fail(event_json != NULL);

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    {
      g_warning("MLS EventRouter: marmot client not available");
      return;
    }

  g_debug("MLS EventRouter: processing kind:445 group message");

  marmot_gobject_client_process_message_async(
    client,
    event_json,
    NULL,  /* cancellable */
    on_message_processed,
    self);
}

void
gn_mls_event_router_send_message_async(GnMlsEventRouter   *self,
                                        const gchar         *group_id_hex,
                                        const gchar         *content,
                                        guint16              kind,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(self));
  g_return_if_fail(group_id_hex != NULL);
  g_return_if_fail(content != NULL);

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_mls_event_router_send_message_async);

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "Marmot client not available");
      g_object_unref(task);
      return;
    }

  const gchar *user_pubkey = gn_marmot_service_get_user_pubkey_hex(self->service);
  if (user_pubkey == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "User identity not set");
      g_object_unref(task);
      return;
    }

  /*
   * Build the unsigned inner event (the "rumor"):
   * {
   *   "pubkey": "<user_pubkey>",
   *   "kind": 9,       // NIP-C7 chat message
   *   "created_at": <now>,
   *   "content": "<content>",
   *   "tags": []
   * }
   *
   * The inner event kind follows whitenoise convention:
   * - kind:9  for regular chat messages (NIP-C7)
   * - kind:5  for deletions (NIP-09)
   * - kind:7  for reactions (NIP-25)
   */
  guint16 inner_kind = (kind > 0) ? kind : KIND_CHAT_MESSAGE;

  g_autofree gchar *inner_event_json = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"kind\":%u,\"created_at\":%lld,"
    "\"content\":\"%s\",\"tags\":[]}",
    user_pubkey,
    inner_kind,
    (long long)g_get_real_time() / G_USEC_PER_SEC,
    content);  /* TODO: Properly escape JSON content */

  SendMsgData *data = g_new0(SendMsgData, 1);
  data->router = self;
  data->task   = task;

  marmot_gobject_client_send_message_async(
    client,
    group_id_hex,
    inner_event_json,
    cancellable,
    on_msg_encrypted,
    data);
}

gboolean
gn_mls_event_router_send_message_finish(GnMlsEventRouter *self,
                                          GAsyncResult      *result,
                                          GError           **error)
{
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gn_mls_event_router_send_welcome_async(GnMlsEventRouter   *self,
                                         const gchar         *recipient_pubkey_hex,
                                         const gchar         *welcome_rumor_json,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  g_return_if_fail(GN_IS_MLS_EVENT_ROUTER(self));
  g_return_if_fail(recipient_pubkey_hex != NULL);
  g_return_if_fail(welcome_rumor_json != NULL);

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_mls_event_router_send_welcome_async);

  const gchar *sender_pubkey = gn_marmot_service_get_user_pubkey_hex(self->service);
  if (sender_pubkey == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                              "User identity not set");
      g_object_unref(task);
      return;
    }

  g_info("MLS EventRouter: gift-wrapping welcome for %s", recipient_pubkey_hex);

  /*
   * TODO: Full welcome send flow:
   *
   * 1. Parse welcome_rumor_json into a NostrEvent
   * 2. gnostr_nip59_create_gift_wrap_async(rumor, recipient, sender, ...)
   * 3. On gift wrap result → publish the kind:1059 event
   *
   * For now, placeholder:
   */
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

gboolean
gn_mls_event_router_send_welcome_finish(GnMlsEventRouter *self,
                                          GAsyncResult      *result,
                                          GError           **error)
{
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
