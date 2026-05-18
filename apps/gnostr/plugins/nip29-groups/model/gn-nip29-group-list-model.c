/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-list-model.c - GListModel adapter for NIP-29 groups
 */

#include "gn-nip29-group-list-model.h"
#include "gn-nip29-group-item.h"
#include <nip29.h>

struct _GnNip29GroupListModel
{
  GObject parent_instance;

  GnNip29GroupService *service;
  GPtrArray           *items;   /* GnNip29GroupItem* */
  gulong               sig_groups_changed;
  gulong               sig_group_updated;
};

static void g_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnNip29GroupListModel, gn_nip29_group_list_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, g_list_model_iface_init))

static GType
gn_nip29_group_list_model_get_item_type(GListModel *model)
{
  return GN_TYPE_NIP29_GROUP_ITEM;
}

static guint
gn_nip29_group_list_model_get_n_items(GListModel *model)
{
  GnNip29GroupListModel *self = GN_NIP29_GROUP_LIST_MODEL(model);
  return self->items ? self->items->len : 0;
}

static gpointer
gn_nip29_group_list_model_get_item(GListModel *model, guint position)
{
  GnNip29GroupListModel *self = GN_NIP29_GROUP_LIST_MODEL(model);
  if (self->items == NULL || position >= self->items->len)
    return NULL;
  return g_object_ref(g_ptr_array_index(self->items, position));
}

static void
g_list_model_iface_init(GListModelInterface *iface)
{
  iface->get_item_type = gn_nip29_group_list_model_get_item_type;
  iface->get_n_items = gn_nip29_group_list_model_get_n_items;
  iface->get_item = gn_nip29_group_list_model_get_item;
}

static GnNip29GroupItem *
snapshot_group_item(GnNip29GroupService *service, const char *key)
{
  const char *relay_url = gn_nip29_group_service_get_group_relay_url(service, key);
  const char *group_id = gn_nip29_group_service_get_group_group_id(service, key);
  const char *alias = gn_nip29_group_service_get_group_alias(service, key);
  const nostr_group_t *nip29 = gn_nip29_group_service_get_group_data(service, key);
  guint msg_count = gn_nip29_group_service_get_message_count_for_key(service, key);

  return gn_nip29_group_item_new(
    key, relay_url, group_id, alias,
    nip29 ? nip29->name : NULL,
    nip29 ? nip29->picture : NULL,
    nip29 ? nip29->about : NULL,
    nip29 ? nip29->is_private : FALSE,
    nip29 ? nip29->is_restricted : FALSE,
    nip29 ? nip29->is_hidden : FALSE,
    nip29 ? nip29->is_closed : FALSE,
    nip29 ? nip29->admins_loaded : FALSE,
    nip29 ? nip29->members_loaded : FALSE,
    nip29 ? nip29->members_may_be_partial : FALSE,
    nip29 ? nip29->roles_loaded : FALSE,
    nip29 ? (guint)nip29->admins_len : 0,
    nip29 ? (guint)nip29->members_len : 0,
    msg_count);
}

void
gn_nip29_group_list_model_reload(GnNip29GroupListModel *self)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_LIST_MODEL(self));

  guint old_len = self->items ? self->items->len : 0;

  if (self->items != NULL)
    g_ptr_array_set_size(self->items, 0);
  else
    self->items = g_ptr_array_new_with_free_func(g_object_unref);

  if (self->service != NULL)
    {
      GList *keys = gn_nip29_group_service_list_group_keys(self->service);
      for (GList *l = keys; l != NULL; l = l->next)
        {
          GnNip29GroupItem *item = snapshot_group_item(self->service, l->data);
          if (item != NULL)
            g_ptr_array_add(self->items, item);
        }
      g_list_free(keys);
    }

  guint new_len = self->items->len;
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
}

static void
on_groups_changed(GnNip29GroupService   *service,
                  GnNip29GroupListModel *self)
{
  gn_nip29_group_list_model_reload(self);
}

static void
on_group_updated(GnNip29GroupService   *service,
                 const char            *group_key,
                 GnNip29GroupListModel *self)
{
  /* Find and update the specific item, or reload fully */
  if (self->items == NULL || self->service == NULL)
    return;

  for (guint i = 0; i < self->items->len; i++)
    {
      GnNip29GroupItem *item = g_ptr_array_index(self->items, i);
      if (g_strcmp0(gn_nip29_group_item_get_key(item), group_key) == 0)
        {
          GnNip29GroupItem *updated = snapshot_group_item(self->service, group_key);
          if (updated != NULL)
            {
              self->items->pdata[i] = updated;
              g_object_unref(item);
              g_list_model_items_changed(G_LIST_MODEL(self), i, 1, 1);
            }
          return;
        }
    }

  /* Not found — full reload (group may have been added) */
  gn_nip29_group_list_model_reload(self);
}

static void
gn_nip29_group_list_model_dispose(GObject *object)
{
  GnNip29GroupListModel *self = GN_NIP29_GROUP_LIST_MODEL(object);

  if (self->service != NULL)
    {
      if (self->sig_groups_changed > 0)
        g_signal_handler_disconnect(self->service, self->sig_groups_changed);
      if (self->sig_group_updated > 0)
        g_signal_handler_disconnect(self->service, self->sig_group_updated);
      self->sig_groups_changed = 0;
      self->sig_group_updated = 0;
    }

  g_clear_object(&self->service);
  g_clear_pointer(&self->items, g_ptr_array_unref);

  G_OBJECT_CLASS(gn_nip29_group_list_model_parent_class)->dispose(object);
}

static void
gn_nip29_group_list_model_class_init(GnNip29GroupListModelClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_nip29_group_list_model_dispose;
}

static void
gn_nip29_group_list_model_init(GnNip29GroupListModel *self)
{
  self->items = g_ptr_array_new_with_free_func(g_object_unref);
}

GnNip29GroupListModel *
gn_nip29_group_list_model_new(GnNip29GroupService *service)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(service), NULL);

  GnNip29GroupListModel *self = g_object_new(GN_TYPE_NIP29_GROUP_LIST_MODEL, NULL);
  self->service = g_object_ref(service);

  self->sig_groups_changed = g_signal_connect(service, "groups-changed",
                                              G_CALLBACK(on_groups_changed), self);
  self->sig_group_updated = g_signal_connect(service, "group-updated",
                                             G_CALLBACK(on_group_updated), self);

  gn_nip29_group_list_model_reload(self);
  return self;
}
