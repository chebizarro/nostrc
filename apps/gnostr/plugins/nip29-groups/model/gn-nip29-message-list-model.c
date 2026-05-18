/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-message-list-model.c - GListModel for NIP-29 group messages
 */

#include "gn-nip29-message-list-model.h"
#include "gn-nip29-message-item.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct _GnNip29MessageListModel
{
  GObject parent_instance;

  GnNip29GroupService *service;
  gchar               *group_key;
  GPtrArray           *items;   /* GnNip29MessageItem* */
  gulong               sig_group_updated;
};

static void g_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnNip29MessageListModel, gn_nip29_message_list_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, g_list_model_iface_init))

static GType
gn_nip29_message_list_model_get_item_type(GListModel *model)
{
  return GN_TYPE_NIP29_MESSAGE_ITEM;
}

static guint
gn_nip29_message_list_model_get_n_items(GListModel *model)
{
  GnNip29MessageListModel *self = GN_NIP29_MESSAGE_LIST_MODEL(model);
  return self->items ? self->items->len : 0;
}

static gpointer
gn_nip29_message_list_model_get_item(GListModel *model, guint position)
{
  GnNip29MessageListModel *self = GN_NIP29_MESSAGE_LIST_MODEL(model);
  if (self->items == NULL || position >= self->items->len)
    return NULL;
  return g_object_ref(g_ptr_array_index(self->items, position));
}

static void
g_list_model_iface_init(GListModelInterface *iface)
{
  iface->get_item_type = gn_nip29_message_list_model_get_item_type;
  iface->get_n_items = gn_nip29_message_list_model_get_n_items;
  iface->get_item = gn_nip29_message_list_model_get_item;
}

/* ── Event JSON parsing helpers ──────────────────────────────────── */

static gchar *
extract_json_string(const char *json, const char *field)
{
  if (json == NULL)
    return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, NULL))
    return NULL;

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root))
    return NULL;

  JsonObject *obj = json_node_get_object(root);
  const char *val = json_object_get_string_member_with_default(obj, field, NULL);
  return val ? g_strdup(val) : NULL;
}

/* ── Reload logic ────────────────────────────────────────────────── */

static void
reload_messages(GnNip29MessageListModel *self)
{
  guint old_len = self->items ? self->items->len : 0;

  if (self->items != NULL)
    g_ptr_array_set_size(self->items, 0);
  else
    self->items = g_ptr_array_new_with_free_func(g_object_unref);

  if (self->service == NULL || self->group_key == NULL)
    {
      if (old_len > 0)
        g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, 0);
      return;
    }

  guint count = gn_nip29_group_service_get_message_count_for_key(
    self->service, self->group_key);

  for (guint i = 0; i < count; i++)
    {
      GnNip29MessageRef ref = {0};
      if (!gn_nip29_group_service_get_message_at(self->service, self->group_key,
                                                  i, &ref))
        continue;

      g_autofree gchar *pubkey = extract_json_string(ref.event_json, "pubkey");
      g_autofree gchar *content = extract_json_string(ref.event_json, "content");

      GnNip29MessageItem *item = gn_nip29_message_item_new(
        ref.id, ref.event_json, ref.created_at, ref.kind,
        pubkey, content);
      g_ptr_array_add(self->items, item);
    }

  guint new_len = self->items->len;
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
}

static void
on_group_updated(GnNip29GroupService     *service,
                 const char              *group_key,
                 GnNip29MessageListModel *self)
{
  if (g_strcmp0(group_key, self->group_key) != 0)
    return;

  /* Only reload if message count changed (avoid full rebuild on metadata) */
  guint svc_count = gn_nip29_group_service_get_message_count_for_key(
    service, self->group_key);
  guint cur_count = self->items ? self->items->len : 0;

  if (svc_count != cur_count)
    reload_messages(self);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_nip29_message_list_model_dispose(GObject *object)
{
  GnNip29MessageListModel *self = GN_NIP29_MESSAGE_LIST_MODEL(object);

  if (self->service != NULL && self->sig_group_updated > 0)
    {
      g_signal_handler_disconnect(self->service, self->sig_group_updated);
      self->sig_group_updated = 0;
    }

  g_clear_object(&self->service);
  g_clear_pointer(&self->items, g_ptr_array_unref);

  G_OBJECT_CLASS(gn_nip29_message_list_model_parent_class)->dispose(object);
}

static void
gn_nip29_message_list_model_finalize(GObject *object)
{
  GnNip29MessageListModel *self = GN_NIP29_MESSAGE_LIST_MODEL(object);
  g_free(self->group_key);
  G_OBJECT_CLASS(gn_nip29_message_list_model_parent_class)->finalize(object);
}

static void
gn_nip29_message_list_model_class_init(GnNip29MessageListModelClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_nip29_message_list_model_dispose;
  oc->finalize = gn_nip29_message_list_model_finalize;
}

static void
gn_nip29_message_list_model_init(GnNip29MessageListModel *self)
{
  self->items = g_ptr_array_new_with_free_func(g_object_unref);
}

GnNip29MessageListModel *
gn_nip29_message_list_model_new(GnNip29GroupService *service,
                                 const char          *group_key)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(service), NULL);
  g_return_val_if_fail(group_key != NULL, NULL);

  GnNip29MessageListModel *self = g_object_new(GN_TYPE_NIP29_MESSAGE_LIST_MODEL, NULL);
  self->service = g_object_ref(service);
  self->group_key = g_strdup(group_key);

  self->sig_group_updated = g_signal_connect(service, "group-updated",
                                             G_CALLBACK(on_group_updated), self);

  reload_messages(self);
  return self;
}

const char *
gn_nip29_message_list_model_get_group_key(GnNip29MessageListModel *self)
{
  g_return_val_if_fail(GN_IS_NIP29_MESSAGE_LIST_MODEL(self), NULL);
  return self->group_key;
}
