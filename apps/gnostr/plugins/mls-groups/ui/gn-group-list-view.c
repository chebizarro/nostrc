/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-list-view.c - Group list panel
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-list-view.h"
#include "gn-group-list-row.h"
#include "gn-group-chat-view.h"
#include "../model/gn-group-list-model.h"
#include <marmot-gobject-1.0/marmot-gobject.h>

struct _GnGroupListView
{
  GtkBox parent_instance;

  /* Dependencies */
  GnMarmotService    *service;        /* strong ref */
  GnMlsEventRouter  *router;         /* strong ref */
  AdwNavigationView  *nav_view;       /* weak — host owns it */

  /* Widgets */
  GtkListView        *list_view;
  GtkWidget          *empty_page;     /* Shown when no groups */
  GtkStack           *stack;

  /* Model */
  GnGroupListModel   *model;
};

G_DEFINE_TYPE(GnGroupListView, gn_group_list_view, GTK_TYPE_BOX)

/* ── Factory callbacks ───────────────────────────────────────────── */

static void
on_factory_setup(GtkSignalListItemFactory *factory,
                 GtkListItem              *list_item,
                 gpointer                  user_data)
{
  GnGroupListRow *row = gn_group_list_row_new();
  gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
on_factory_bind(GtkSignalListItemFactory *factory,
                GtkListItem              *list_item,
                gpointer                  user_data)
{
  GnGroupListRow *row = GN_GROUP_LIST_ROW(gtk_list_item_get_child(list_item));
  MarmotGobjectGroup *group = gtk_list_item_get_item(list_item);
  gn_group_list_row_bind(row, group);
}

static void
on_factory_unbind(GtkSignalListItemFactory *factory,
                  GtkListItem              *list_item,
                  gpointer                  user_data)
{
  GnGroupListRow *row = GN_GROUP_LIST_ROW(gtk_list_item_get_child(list_item));
  gn_group_list_row_unbind(row);
}

/* ── Selection / activation ──────────────────────────────────────── */

static void
on_group_activated(GtkListView *list_view,
                   guint        position,
                   gpointer     user_data)
{
  GnGroupListView *self = GN_GROUP_LIST_VIEW(user_data);

  g_autoptr(MarmotGobjectGroup) group =
    g_list_model_get_item(G_LIST_MODEL(self->model), position);
  if (group == NULL)
    return;

  const gchar *mls_id = marmot_gobject_group_get_mls_group_id_hex(group);
  const gchar *name   = marmot_gobject_group_get_name(group);

  g_debug("GroupListView: activated group %s (%s)", name, mls_id);

  if (self->nav_view == NULL)
    return;

  /* Create and push the chat view */
  GnGroupChatView *chat = gn_group_chat_view_new(
    self->service, self->router, group);

  AdwNavigationPage *page = adw_navigation_page_new(
    GTK_WIDGET(chat),
    (name && *name) ? name : "Group Chat");
  adw_navigation_page_set_tag(page, mls_id);

  adw_navigation_view_push(self->nav_view, page);
}

/* ── Model change listener ───────────────────────────────────────── */

static void
on_items_changed(GListModel *model,
                 guint       position,
                 guint       removed,
                 guint       added,
                 gpointer    user_data)
{
  GnGroupListView *self = GN_GROUP_LIST_VIEW(user_data);
  guint n = g_list_model_get_n_items(model);

  if (n == 0)
    gtk_stack_set_visible_child(self->stack, self->empty_page);
  else
    gtk_stack_set_visible_child(self->stack, GTK_WIDGET(self->list_view));
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_group_list_view_dispose(GObject *object)
{
  GnGroupListView *self = GN_GROUP_LIST_VIEW(object);

  g_clear_object(&self->service);
  g_clear_object(&self->router);
  g_clear_object(&self->model);
  self->nav_view = NULL;

  G_OBJECT_CLASS(gn_group_list_view_parent_class)->dispose(object);
}

static void
gn_group_list_view_class_init(GnGroupListViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_group_list_view_dispose;
}

static void
gn_group_list_view_init(GnGroupListView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);

  /* Stack to switch between list and empty state */
  self->stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(self->stack), TRUE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->stack));

  /* Empty page (placeholder) */
  self->empty_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(self->empty_page, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(self->empty_page, GTK_ALIGN_CENTER);

  GtkWidget *empty_icon = gtk_image_new_from_icon_name("chat-bubble-text-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 64);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_page), empty_icon);

  GtkWidget *empty_title = gtk_label_new("No Groups Yet");
  gtk_widget_add_css_class(empty_title, "title-2");
  gtk_box_append(GTK_BOX(self->empty_page), empty_title);

  GtkWidget *empty_desc = gtk_label_new(
    "Join a group via an invitation, or create one to get started.");
  gtk_widget_add_css_class(empty_desc, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(empty_desc), TRUE);
  gtk_label_set_justify(GTK_LABEL(empty_desc), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(self->empty_page), empty_desc);

  gtk_stack_add_named(self->stack, self->empty_page, "empty");

  /* Scrolled list view (added after model is set in _new) */
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupListView *
gn_group_list_view_new(GnMarmotService    *service,
                        GnMlsEventRouter  *router,
                        AdwNavigationView  *navigation_view)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(router), NULL);

  GnGroupListView *self = g_object_new(GN_TYPE_GROUP_LIST_VIEW, NULL);
  self->service  = g_object_ref(service);
  self->router   = g_object_ref(router);
  self->nav_view = navigation_view;   /* weak ref */

  /* Create model */
  self->model = gn_group_list_model_new(service);

  /* Create factory */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), NULL);
  g_signal_connect(factory, "bind",  G_CALLBACK(on_factory_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), NULL);

  /* Selection model (single selection) */
  GtkSingleSelection *selection =
    gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->model)));
  gtk_single_selection_set_autoselect(selection, FALSE);
  gtk_single_selection_set_can_unselect(selection, TRUE);

  /* List view */
  self->list_view = GTK_LIST_VIEW(
    gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory));
  gtk_list_view_set_single_click_activate(self->list_view, TRUE);
  g_signal_connect(self->list_view, "activate",
                   G_CALLBACK(on_group_activated), self);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_WIDGET(self->list_view));
  gtk_widget_set_vexpand(scroll, TRUE);

  gtk_stack_add_named(self->stack, scroll, "list");

  /* Listen for model changes to switch stack */
  g_signal_connect(self->model, "items-changed",
                   G_CALLBACK(on_items_changed), self);

  /* Set initial visible child */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->model));
  if (n == 0)
    gtk_stack_set_visible_child(self->stack, self->empty_page);
  else
    gtk_stack_set_visible_child(self->stack, GTK_WIDGET(self->list_view));

  return self;
}
