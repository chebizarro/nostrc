/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-event-router.c - MLS Event Routing
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-event-router.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <marmot-gobject-1.0/marmot-gobject.h>
#include "nip59_giftwrap.h"
#include "nostr-event.h"

/* NIP-C7: Chat message kind */
#define KIND_CHAT_MESSAGE 9

struct _GnMlsEventRouter
{
  GObject parent_instance;

  GnMarmotService     *service;    /* strong ref */
  GnostrPluginContext *context;    /* borrowed — only accessed on main thread */
};

G_DEFINE_TYPE(GnMlsEventRouter, gn_mls_event_router, G_TYPE_OBJECT)

static void
gn_mls_event_router_dispose(GObject *object)
{
  GnMlsEventRouter *self = GN_MLS_EVENT_ROUTER(object);
  g_clear_object(&self->service);
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
  g_autoptr(GnMlsEventRouter) self = GN_MLS_EVENT_ROUTER(user_data);
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

  /* Emit signal on service (if still alive) */
  if (self->service != NULL)
    g_signal_emit_by_name(self->service, "welcome-received", welcome);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: Process group message after NIP-44 decryption
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  GnMlsEventRouter *router;       /* strong ref */
  gchar            *group_id_hex; /* extracted from h tag */
} ProcessMsgData;

static void
process_msg_data_free(ProcessMsgData *data)
{
  g_clear_object(&data->router);
  g_free(data->group_id_hex);
  g_free(data);
}

/**
 * extract_h_tag_from_event:
 * @event_json: JSON string of the Nostr event
 *
 * Extracts the value of the first "h" tag from a Nostr event.
 * The "h" tag contains the MLS group ID for kind:445 events.
 *
 * Returns: (transfer full) (nullable): The h tag value, or NULL if not found
 */
static gchar *
extract_h_tag_from_event(const gchar *event_json)
{
  if (event_json == NULL)
    return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error))
    {
      g_debug("extract_h_tag: failed to parse JSON: %s", error->message);
      return NULL;
    }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root))
    return NULL;

  JsonObject *obj = json_node_get_object(root);
  if (!json_object_has_member(obj, "tags"))
    return NULL;

  JsonArray *tags = json_object_get_array_member(obj, "tags");
  if (tags == NULL)
    return NULL;

  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++)
    {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (tag == NULL || json_array_get_length(tag) < 2)
        continue;

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      if (g_strcmp0(tag_name, "h") == 0)
        {
          const gchar *value = json_array_get_string_element(tag, 1);
          if (value != NULL)
            return g_strdup(value);
        }
    }

  return NULL;
}

static void
on_message_processed(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  ProcessMsgData *data = user_data;
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
      process_msg_data_free(data);
      return;
    }

  if (data->router->service == NULL)
    {
      process_msg_data_free(data);
      return;
    }

  switch (result_type)
    {
    case MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION:
      if (inner_json != NULL)
        {
          g_debug("MLS EventRouter: application message decrypted for group %s",
                  data->group_id_hex ? data->group_id_hex : "(unknown)");

          g_signal_emit_by_name(data->router->service, "message-received",
                                data->group_id_hex ? data->group_id_hex : "",
                                inner_json);
        }
      break;

    case MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT:
      {
        g_debug("MLS EventRouter: commit processed, group state updated");

        /* Refresh group from storage and notify listeners */
        if (data->group_id_hex != NULL && data->router->service != NULL)
          {
            MarmotGobjectClient *grp_client =
              gn_marmot_service_get_client(data->router->service);
            if (grp_client != NULL)
              {
                g_autoptr(GError) grp_error = NULL;
                g_autoptr(MarmotGobjectGroup) updated_group =
                  marmot_gobject_client_get_group(grp_client,
                                                   data->group_id_hex,
                                                   &grp_error);
                if (updated_group != NULL)
                  {
                    g_info("MLS EventRouter: emitting group-updated for %s",
                           data->group_id_hex);
                    g_signal_emit_by_name(data->router->service,
                                          "group-updated", updated_group);
                  }
                else
                  {
                    g_warning("MLS EventRouter: could not fetch group %s after commit: %s",
                              data->group_id_hex,
                              grp_error ? grp_error->message : "unknown");
                  }
              }
          }
      }
      break;

    case MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE:
      g_debug("MLS EventRouter: skipping own message");
      break;

    default:
      g_debug("MLS EventRouter: unhandled result type %d", result_type);
      break;
    }

  process_msg_data_free(data);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: NIP-59 unwrap callback
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
  GnMlsEventRouter *router;          /* strong ref */
  gchar            *gift_wrap_event_id; /* hex id of outer gift wrap */
} UnwrapData;

static void
unwrap_data_free(UnwrapData *data)
{
  g_clear_object(&data->router);
  g_free(data->gift_wrap_event_id);
  g_free(data);
}

/*
 * After NIP-59 gift wrap unwrapping, we get the inner rumor event.
 * Route it based on kind:
 *   - 444 → marmot welcome processing
 *   - 445 → marmot message processing
 */
static void
on_gift_wrap_unwrapped(GnostrUnwrapResult *unwrap_result, gpointer user_data)
{
  UnwrapData *data = user_data;

  if (!unwrap_result->success)
    {
      g_warning("MLS EventRouter: gift wrap unwrap failed: %s",
                unwrap_result->error_message ? unwrap_result->error_message : "unknown");
      gnostr_unwrap_result_free(unwrap_result);
      unwrap_data_free(data);
      return;
    }

  NostrEvent *rumor = unwrap_result->rumor;
  int kind = nostr_event_get_kind(rumor);

  MarmotGobjectClient *client = gn_marmot_service_get_client(data->router->service);
  if (client == NULL)
    {
      g_warning("MLS EventRouter: marmot client not available after unwrap");
      gnostr_unwrap_result_free(unwrap_result);
      unwrap_data_free(data);
      return;
    }

  g_debug("MLS EventRouter: gift wrap unwrapped — routing kind:%d from sender %.8s",
          kind, unwrap_result->sender_pubkey ? unwrap_result->sender_pubkey : "?");

  if (kind == 444)
    {
      /* Welcome message — route to marmot for MLS state initialization */
      g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);
      if (rumor_json != NULL)
        {
          marmot_gobject_client_process_welcome_async(
            client,
            data->gift_wrap_event_id,
            rumor_json,
            NULL,
            on_welcome_processed,
            g_object_ref(data->router));
        }
    }
  else if (kind == 445)
    {
      /* Group message — route to marmot for MLS decryption */
      g_autofree gchar *event_json = nostr_event_serialize_compact(rumor);
      if (event_json != NULL)
        {
          g_autofree gchar *group_id_hex = extract_h_tag_from_event(event_json);

          ProcessMsgData *msg_data = g_new0(ProcessMsgData, 1);
          msg_data->router       = g_object_ref(data->router);
          msg_data->group_id_hex = g_steal_pointer(&group_id_hex);

          marmot_gobject_client_process_message_async(
            client,
            event_json,
            NULL,
            on_message_processed,
            msg_data);
        }
    }
  else
    {
      g_debug("MLS EventRouter: ignoring unwrapped event with kind:%d", kind);
    }

  gnostr_unwrap_result_free(unwrap_result);
  unwrap_data_free(data);
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
  GnMlsEventRouter *router;       /* strong ref */
  GTask             *task;
  gchar            *group_id_hex; /* for relay lookup */
} SendMsgData;

static void
send_msg_data_free(SendMsgData *data)
{
  g_clear_object(&data->router);
  g_free(data->group_id_hex);
  g_free(data);
}

static void
on_msg_published(GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  SendMsgData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (data->router->context == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Plugin deactivated");
      g_object_unref(data->task);
      send_msg_data_free(data);
      return;
    }

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
  send_msg_data_free(data);
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
      send_msg_data_free(data);
      return;
    }

  if (data->router->context == NULL)
    {
      g_task_return_new_error(data->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Plugin deactivated");
      g_object_unref(data->task);
      send_msg_data_free(data);
      return;
    }

  g_debug("MLS EventRouter: message encrypted, publishing kind:445 event");

  /*
   * Publish to the group's designated relays for proper routing.
   * Falls back to user's default write relays if no group relays are configured.
   */
  g_auto(GStrv) group_relays = NULL;
  if (data->group_id_hex != NULL)
    {
      MarmotGobjectClient *client = gn_marmot_service_get_client(data->router->service);
      if (client != NULL)
        {
          gsize relay_count = 0;
          group_relays = marmot_gobject_client_get_group_relay_urls(
            client, data->group_id_hex, &relay_count);
          if (relay_count > 0)
            g_debug("MLS EventRouter: publishing to %zu group-specific relay(s)",
                    relay_count);
        }
    }

  gnostr_plugin_context_publish_event_to_relays_async(
    data->router->context,
    event_json,
    (const char * const *)group_relays,
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
  self->service = g_object_ref(service);   /* strong ref */
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

  /* Parse gift_wrap_json into NostrEvent */
  NostrEvent *gift_wrap = nostr_event_new();
  if (!nostr_event_deserialize_compact(gift_wrap, gift_wrap_json, NULL))
    {
      g_warning("MLS EventRouter: failed to parse gift wrap JSON");
      nostr_event_free(gift_wrap);
      return;
    }

  /* Track the gift wrap event ID — needed by marmot for welcome processing */
  UnwrapData *data = g_new0(UnwrapData, 1);
  data->router = g_object_ref(self);
  data->gift_wrap_event_id = nostr_event_get_id(gift_wrap);

  /* Decrypt seal → rumor via D-Bus NIP-44 signer, then route by kind */
  gnostr_nip59_unwrap_async(gift_wrap, user_pubkey, NULL,
                             on_gift_wrap_unwrapped, data);

  nostr_event_free(gift_wrap);
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

  /* Extract the MLS group ID from the h tag before processing */
  g_autofree gchar *group_id_hex = extract_h_tag_from_event(event_json);

  g_debug("MLS EventRouter: processing kind:445 group message for group %s",
          group_id_hex ? group_id_hex : "(unknown)");

  /* Create callback data with extracted group ID */
  ProcessMsgData *data = g_new0(ProcessMsgData, 1);
  data->router       = g_object_ref(self);
  data->group_id_hex = g_steal_pointer(&group_id_hex);

  marmot_gobject_client_process_message_async(
    client,
    event_json,
    NULL,  /* cancellable */
    on_message_processed,
    data);
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
   * Build the unsigned inner event (the "rumor") using json-glib:
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

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "pubkey");
  json_builder_add_string_value(builder, user_pubkey);

  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, inner_kind);

  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, g_get_real_time() / G_USEC_PER_SEC);

  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, content);

  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);
  json_builder_end_array(builder);

  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  g_autofree gchar *inner_event_json = json_generator_to_data(gen, NULL);

  SendMsgData *data = g_new0(SendMsgData, 1);
  data->router       = g_object_ref(self);   /* strong ref for async safety */
  data->task         = task;
  data->group_id_hex = g_strdup(group_id_hex);

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

/* ══════════════════════════════════════════════════════════════════════════
 * Internal: Welcome gift-wrap → publish chain
 * ══════════════════════════════════════════════════════════════════════════ */

static void
on_welcome_published(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GTask *task = G_TASK(user_data);
  g_autoptr(GError) error = NULL;

  GnMlsEventRouter *self = GN_MLS_EVENT_ROUTER(g_task_get_source_object(task));

  gboolean ok = gnostr_plugin_context_publish_event_finish(
    self->context, result, &error);

  if (ok)
    {
      g_info("MLS EventRouter: welcome gift wrap published");
      g_task_return_boolean(task, TRUE);
    }
  else
    {
      g_warning("MLS EventRouter: failed to publish welcome: %s",
                error ? error->message : "unknown");
      g_task_return_error(task, g_steal_pointer(&error));
    }

  g_object_unref(task);
}

static void
on_welcome_gift_wrapped(GnostrGiftWrapResult *wrap_result, gpointer user_data)
{
  GTask *task = G_TASK(user_data);
  GnMlsEventRouter *self = GN_MLS_EVENT_ROUTER(g_task_get_source_object(task));

  if (!wrap_result->success)
    {
      g_warning("MLS EventRouter: gift wrap creation failed: %s",
                wrap_result->error_message ? wrap_result->error_message : "unknown");
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Gift wrap failed: %s",
                              wrap_result->error_message ? wrap_result->error_message : "unknown");
      g_object_unref(task);
      gnostr_gift_wrap_result_free(wrap_result);
      return;
    }

  if (self->context == NULL)
    {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Plugin deactivated");
      g_object_unref(task);
      gnostr_gift_wrap_result_free(wrap_result);
      return;
    }

  g_debug("MLS EventRouter: welcome gift-wrapped, publishing kind:1059 event");

  /* Publish the gift-wrapped welcome to relays */
  gnostr_plugin_context_publish_event_async(
    self->context,
    wrap_result->gift_wrap_json,
    g_task_get_cancellable(task),
    on_welcome_published,
    task);

  gnostr_gift_wrap_result_free(wrap_result);
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

  /* Parse welcome_rumor_json into a NostrEvent */
  NostrEvent *rumor = nostr_event_new();
  if (!nostr_event_deserialize_compact(rumor, welcome_rumor_json, NULL))
    {
      g_warning("MLS EventRouter: failed to parse welcome rumor JSON");
      nostr_event_free(rumor);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "Failed to parse welcome rumor JSON");
      g_object_unref(task);
      return;
    }

  /* Gift-wrap the welcome rumor and publish on completion */
  gnostr_nip59_create_gift_wrap_async(
    rumor,
    recipient_pubkey_hex,
    sender_pubkey,
    g_task_get_cancellable(task),
    on_welcome_gift_wrapped,
    task);   /* task ownership transferred to callback */

  nostr_event_free(rumor);
}

gboolean
gn_mls_event_router_send_welcome_finish(GnMlsEventRouter *self,
                                          GAsyncResult      *result,
                                          GError           **error)
{
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
