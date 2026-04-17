/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-message-model.c - GListModel adapter for group messages
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-message-model.h"
#include <marmot-gobject-1.0/marmot-gobject.h>
#include <json-glib/json-glib.h>

#define MESSAGES_PAGE_SIZE 50

struct _GnGroupMessageModel
{
  GObject parent_instance;

  GnMarmotService *service;          /* strong ref */
  gchar           *mls_group_id_hex;
  GPtrArray       *messages;         /* (element-type MarmotGobjectMessage) */
  guint            offset;           /* pagination offset */
  gboolean         has_more;         /* TRUE if more pages exist */

  /* Signal handler for live message append */
  gulong sig_message_received;
};

static void gn_group_message_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnGroupMessageModel, gn_group_message_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              gn_group_message_model_iface_init))

/* ── GListModel interface ────────────────────────────────────────── */

static GType
gn_group_message_model_get_item_type(GListModel *model)
{
  return MARMOT_GOBJECT_TYPE_MESSAGE;
}

static guint
gn_group_message_model_get_n_items(GListModel *model)
{
  GnGroupMessageModel *self = GN_GROUP_MESSAGE_MODEL(model);
  return self->messages ? self->messages->len : 0;
}

static gpointer
gn_group_message_model_get_item(GListModel *model, guint position)
{
  GnGroupMessageModel *self = GN_GROUP_MESSAGE_MODEL(model);
  if (!self->messages || position >= self->messages->len)
    return NULL;

  return g_object_ref(g_ptr_array_index(self->messages, position));
}

static void
gn_group_message_model_iface_init(GListModelInterface *iface)
{
  iface->get_item_type = gn_group_message_model_get_item_type;
  iface->get_n_items   = gn_group_message_model_get_n_items;
  iface->get_item      = gn_group_message_model_get_item;
}

/* ── Live message handler ────────────────────────────────────────── */

static void
on_message_received(GnMarmotService *service,
                    const gchar     *group_id_hex,
                    const gchar     *inner_event_json,
                    gpointer         user_data)
{
  GnGroupMessageModel *self = GN_GROUP_MESSAGE_MODEL(user_data);

  /* Only accept messages for our group */
  if (g_strcmp0(group_id_hex, self->mls_group_id_hex) != 0)
    return;

  /*
   * Parse the inner event JSON directly into a MarmotGobjectMessage
   * and append to the model for immediate display.
   *
   * Inner event shape (built by gn-mls-event-router.c):
   *   {"pubkey":"<hex>","kind":9,"created_at":<ts>,"content":"<text>","tags":[]}
   */
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, inner_event_json, -1, NULL))
    return;

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root))
    return;

  JsonObject *obj = json_node_get_object(root);

  const gchar *pubkey  = json_object_get_string_member_with_default(obj, "pubkey", NULL);
  const gchar *content = json_object_get_string_member_with_default(obj, "content", NULL);
  gint64 kind          = json_object_get_int_member_with_default(obj, "kind", 9);
  gint64 created_at    = json_object_get_int_member_with_default(obj, "created_at", 0);
  const gchar *event_id = json_object_get_string_member_with_default(obj, "id", NULL);

  if (pubkey == NULL)
    return;

  /* Use event id if present, otherwise generate a placeholder from content hash */
  g_autofree gchar *placeholder_id = NULL;
  if (event_id == NULL) {
    g_autofree gchar *hash_input = g_strdup_printf("%s:%"G_GINT64_FORMAT":%s",
                                                    pubkey, created_at,
                                                    content ? content : "");
    placeholder_id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, hash_input, -1);
    event_id = placeholder_id;
  }

  MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
    event_id, pubkey, content, (guint)kind, created_at, self->mls_group_id_hex);

  guint position = self->messages->len;
  g_ptr_array_add(self->messages, msg); /* takes ownership */

  g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_group_message_model_dispose(GObject *object)
{
  GnGroupMessageModel *self = GN_GROUP_MESSAGE_MODEL(object);

  if (self->service && self->sig_message_received > 0)
    {
      g_signal_handler_disconnect(self->service, self->sig_message_received);
      self->sig_message_received = 0;
    }

  g_clear_object(&self->service);
  g_clear_pointer(&self->messages, g_ptr_array_unref);

  G_OBJECT_CLASS(gn_group_message_model_parent_class)->dispose(object);
}

static void
gn_group_message_model_finalize(GObject *object)
{
  GnGroupMessageModel *self = GN_GROUP_MESSAGE_MODEL(object);
  g_clear_pointer(&self->mls_group_id_hex, g_free);
  G_OBJECT_CLASS(gn_group_message_model_parent_class)->finalize(object);
}

static void
gn_group_message_model_class_init(GnGroupMessageModelClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose  = gn_group_message_model_dispose;
  oc->finalize = gn_group_message_model_finalize;
}

static void
gn_group_message_model_init(GnGroupMessageModel *self)
{
  self->messages = g_ptr_array_new_with_free_func(g_object_unref);
  self->has_more = TRUE;
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupMessageModel *
gn_group_message_model_new(GnMarmotService *service,
                            const gchar     *mls_group_id_hex)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(mls_group_id_hex != NULL, NULL);

  GnGroupMessageModel *self = g_object_new(GN_TYPE_GROUP_MESSAGE_MODEL, NULL);
  self->service          = g_object_ref(service);
  self->mls_group_id_hex = g_strdup(mls_group_id_hex);
  self->offset           = 0;

  /* Connect live message signal */
  self->sig_message_received = g_signal_connect(
    service, "message-received",
    G_CALLBACK(on_message_received), self);

  /* Initial load */
  MarmotGobjectClient *client = gn_marmot_service_get_client(service);
  if (client != NULL)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GPtrArray) initial =
        marmot_gobject_client_get_messages(client, mls_group_id_hex,
                                            MESSAGES_PAGE_SIZE, 0, &error);
      if (initial != NULL)
        {
          for (guint i = 0; i < initial->len; i++)
            g_ptr_array_add(self->messages,
                            g_object_ref(g_ptr_array_index(initial, i)));

          self->offset  = initial->len;
          self->has_more = (initial->len >= MESSAGES_PAGE_SIZE);

          if (self->messages->len > 0)
            g_list_model_items_changed(G_LIST_MODEL(self), 0, 0,
                                        self->messages->len);
        }
      else if (error)
        {
          g_warning("GroupMessageModel: initial load failed: %s", error->message);
        }
    }

  return self;
}

void
gn_group_message_model_load_more(GnGroupMessageModel *self)
{
  g_return_if_fail(GN_IS_GROUP_MESSAGE_MODEL(self));

  if (!self->has_more)
    return;

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    return;

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) page =
    marmot_gobject_client_get_messages(client, self->mls_group_id_hex,
                                        MESSAGES_PAGE_SIZE, self->offset, &error);
  if (page == NULL || page->len == 0)
    {
      self->has_more = FALSE;
      return;
    }

  /* Prepend older messages at position 0 */
  guint insert_count = page->len;
  for (guint i = 0; i < page->len; i++)
    g_ptr_array_insert(self->messages, i,
                       g_object_ref(g_ptr_array_index(page, i)));

  self->offset  += page->len;
  self->has_more = (page->len >= MESSAGES_PAGE_SIZE);

  g_list_model_items_changed(G_LIST_MODEL(self), 0, 0, insert_count);
}

const gchar *
gn_group_message_model_get_group_id_hex(GnGroupMessageModel *self)
{
  g_return_val_if_fail(GN_IS_GROUP_MESSAGE_MODEL(self), NULL);
  return self->mls_group_id_hex;
}
