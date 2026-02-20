/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-message-model.c - GListModel adapter for group messages
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-message-model.h"
#include <marmot-gobject-1.0/marmot-gobject.h>

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
   * TODO: Parse inner_event_json into a MarmotGobjectMessage.
   * For now, we just reload from storage after a short delay
   * to let marmot persist the message.
   *
   * In the future, the marmot service should emit the actual
   * MarmotGobjectMessage object directly.
   */

  /* Reload messages from storage to pick up the new one */
  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    return;

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) fresh =
    marmot_gobject_client_get_messages(client, self->mls_group_id_hex,
                                        MESSAGES_PAGE_SIZE, 0, &error);
  if (fresh == NULL)
    return;

  guint old_count = self->messages->len;

  g_ptr_array_set_size(self->messages, 0);
  for (guint i = 0; i < fresh->len; i++)
    g_ptr_array_add(self->messages,
                    g_object_ref(g_ptr_array_index(fresh, i)));

  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, self->messages->len);
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
