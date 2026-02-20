/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-list-model.c - GListModel adapter for MLS groups
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-list-model.h"
#include <marmot-gobject-1.0/marmot-gobject.h>

struct _GnGroupListModel
{
  GObject parent_instance;

  GnMarmotService *service;   /* strong ref */
  GPtrArray       *groups;    /* (element-type MarmotGobjectGroup) */

  /* Signal handler IDs for auto-reload */
  gulong sig_group_created;
  gulong sig_group_joined;
  gulong sig_group_updated;
};

static void gn_group_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnGroupListModel, gn_group_list_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              gn_group_list_model_iface_init))

/* ── GListModel interface ────────────────────────────────────────── */

static GType
gn_group_list_model_get_item_type(GListModel *model)
{
  return MARMOT_GOBJECT_TYPE_GROUP;
}

static guint
gn_group_list_model_get_n_items(GListModel *model)
{
  GnGroupListModel *self = GN_GROUP_LIST_MODEL(model);
  return self->groups ? self->groups->len : 0;
}

static gpointer
gn_group_list_model_get_item(GListModel *model, guint position)
{
  GnGroupListModel *self = GN_GROUP_LIST_MODEL(model);
  if (!self->groups || position >= self->groups->len)
    return NULL;

  return g_object_ref(g_ptr_array_index(self->groups, position));
}

static void
gn_group_list_model_iface_init(GListModelInterface *iface)
{
  iface->get_item_type = gn_group_list_model_get_item_type;
  iface->get_n_items   = gn_group_list_model_get_n_items;
  iface->get_item      = gn_group_list_model_get_item;
}

/* ── Auto-reload signal handlers ─────────────────────────────────── */

static void
on_service_groups_changed(GnMarmotService *service,
                          gpointer         unused,
                          gpointer         user_data)
{
  GnGroupListModel *self = GN_GROUP_LIST_MODEL(user_data);
  gn_group_list_model_reload(self);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_group_list_model_dispose(GObject *object)
{
  GnGroupListModel *self = GN_GROUP_LIST_MODEL(object);

  if (self->service)
    {
      if (self->sig_group_created > 0)
        g_signal_handler_disconnect(self->service, self->sig_group_created);
      if (self->sig_group_joined > 0)
        g_signal_handler_disconnect(self->service, self->sig_group_joined);
      if (self->sig_group_updated > 0)
        g_signal_handler_disconnect(self->service, self->sig_group_updated);
      self->sig_group_created = 0;
      self->sig_group_joined  = 0;
      self->sig_group_updated = 0;
    }

  g_clear_object(&self->service);
  g_clear_pointer(&self->groups, g_ptr_array_unref);

  G_OBJECT_CLASS(gn_group_list_model_parent_class)->dispose(object);
}

static void
gn_group_list_model_class_init(GnGroupListModelClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_group_list_model_dispose;
}

static void
gn_group_list_model_init(GnGroupListModel *self)
{
  self->groups = g_ptr_array_new_with_free_func(g_object_unref);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupListModel *
gn_group_list_model_new(GnMarmotService *service)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);

  GnGroupListModel *self = g_object_new(GN_TYPE_GROUP_LIST_MODEL, NULL);
  self->service = g_object_ref(service);

  /* Connect auto-reload signals */
  self->sig_group_created = g_signal_connect(
    service, "group-created",
    G_CALLBACK(on_service_groups_changed), self);
  self->sig_group_joined = g_signal_connect(
    service, "group-joined",
    G_CALLBACK(on_service_groups_changed), self);
  self->sig_group_updated = g_signal_connect(
    service, "group-updated",
    G_CALLBACK(on_service_groups_changed), self);

  /* Initial load */
  gn_group_list_model_reload(self);

  return self;
}

void
gn_group_list_model_reload(GnGroupListModel *self)
{
  g_return_if_fail(GN_IS_GROUP_LIST_MODEL(self));

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    return;

  guint old_count = self->groups->len;

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) new_groups =
    marmot_gobject_client_get_all_groups(client, &error);

  if (new_groups == NULL)
    {
      if (error)
        g_warning("GroupListModel: failed to reload groups: %s", error->message);
      return;
    }

  /* Replace all: clear old, add new */
  g_ptr_array_set_size(self->groups, 0);
  for (guint i = 0; i < new_groups->len; i++)
    g_ptr_array_add(self->groups,
                    g_object_ref(g_ptr_array_index(new_groups, i)));

  /* Emit items-changed with "replace all" semantics */
  g_list_model_items_changed(G_LIST_MODEL(self), 0, old_count, self->groups->len);
}
