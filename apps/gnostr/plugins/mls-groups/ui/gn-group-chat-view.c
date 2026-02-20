/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-chat-view.c - Group conversation view
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-chat-view.h"
#include "gn-group-message-row.h"
#include "gn-group-composer.h"
#include "../model/gn-group-message-model.h"
#include <marmot-gobject-1.0/marmot-gobject.h>

struct _GnGroupChatView
{
  GtkBox parent_instance;

  /* Dependencies */
  GnMarmotService      *service;     /* strong ref */
  GnMlsEventRouter    *router;      /* strong ref */
  MarmotGobjectGroup   *group;       /* strong ref */

  /* Child widgets */
  GtkListView          *message_list;
  GtkScrolledWindow    *scroll;
  GnGroupComposer      *composer;

  /* Model */
  GnGroupMessageModel  *msg_model;
};

G_DEFINE_TYPE(GnGroupChatView, gn_group_chat_view, GTK_TYPE_BOX)

/* ── Factory callbacks ───────────────────────────────────────────── */

static void
on_msg_factory_setup(GtkSignalListItemFactory *factory,
                     GtkListItem              *list_item,
                     gpointer                  user_data)
{
  GnGroupMessageRow *row = gn_group_message_row_new();
  gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
on_msg_factory_bind(GtkSignalListItemFactory *factory,
                    GtkListItem              *list_item,
                    gpointer                  user_data)
{
  GnGroupChatView *self = GN_GROUP_CHAT_VIEW(user_data);
  GnGroupMessageRow *row = GN_GROUP_MESSAGE_ROW(gtk_list_item_get_child(list_item));
  MarmotGobjectMessage *msg = gtk_list_item_get_item(list_item);

  const gchar *user_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  gn_group_message_row_bind(row, msg, user_pk);
}

static void
on_msg_factory_unbind(GtkSignalListItemFactory *factory,
                      GtkListItem              *list_item,
                      gpointer                  user_data)
{
  GnGroupMessageRow *row = GN_GROUP_MESSAGE_ROW(gtk_list_item_get_child(list_item));
  gn_group_message_row_unbind(row);
}

/* ── Send message handler ────────────────────────────────────────── */

static void
on_message_sent(GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  GnGroupChatView *self = GN_GROUP_CHAT_VIEW(user_data);
  g_autoptr(GError) error = NULL;

  gboolean ok = gn_mls_event_router_send_message_finish(
    GN_MLS_EVENT_ROUTER(source), result, &error);

  gn_group_composer_set_send_sensitive(self->composer, TRUE);

  if (!ok)
    {
      g_warning("GroupChatView: failed to send message: %s",
                error ? error->message : "unknown");
      /* TODO: Show inline error toast */
    }

  g_object_unref(self);   /* Release the ref we took for the async op */
}

static void
on_send_requested(GnGroupComposer *composer,
                  const gchar     *text,
                  gpointer         user_data)
{
  GnGroupChatView *self = GN_GROUP_CHAT_VIEW(user_data);

  if (text == NULL || *text == '\0')
    return;

  const gchar *group_id =
    marmot_gobject_group_get_mls_group_id_hex(self->group);

  gn_group_composer_set_send_sensitive(self->composer, FALSE);
  gn_group_composer_clear(self->composer);

  gn_mls_event_router_send_message_async(
    self->router,
    group_id,
    text,
    0,   /* kind=0 → default (kind:9 chat message) */
    NULL,
    on_message_sent,
    g_object_ref(self));   /* strong ref for async safety */
}

/* ── Auto-scroll on new messages ─────────────────────────────────── */

static void
on_messages_changed(GListModel *model,
                    guint       position,
                    guint       removed,
                    guint       added,
                    gpointer    user_data)
{
  GnGroupChatView *self = GN_GROUP_CHAT_VIEW(user_data);

  /* Auto-scroll to bottom when new messages arrive */
  if (added > 0 && position > 0)
    {
      GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(self->scroll);
      gdouble upper = gtk_adjustment_get_upper(adj);
      gtk_adjustment_set_value(adj, upper);
    }
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_group_chat_view_dispose(GObject *object)
{
  GnGroupChatView *self = GN_GROUP_CHAT_VIEW(object);

  g_clear_object(&self->service);
  g_clear_object(&self->router);
  g_clear_object(&self->group);
  g_clear_object(&self->msg_model);

  G_OBJECT_CLASS(gn_group_chat_view_parent_class)->dispose(object);
}

static void
gn_group_chat_view_class_init(GnGroupChatViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_group_chat_view_dispose;
}

static void
gn_group_chat_view_init(GnGroupChatView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupChatView *
gn_group_chat_view_new(GnMarmotService      *service,
                        GnMlsEventRouter    *router,
                        MarmotGobjectGroup  *group)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(router), NULL);
  g_return_val_if_fail(MARMOT_GOBJECT_IS_GROUP(group), NULL);

  GnGroupChatView *self = g_object_new(GN_TYPE_GROUP_CHAT_VIEW, NULL);
  self->service = g_object_ref(service);
  self->router  = g_object_ref(router);
  self->group   = g_object_ref(group);

  const gchar *mls_id = marmot_gobject_group_get_mls_group_id_hex(group);

  /* Create message model */
  self->msg_model = gn_group_message_model_new(service, mls_id);

  /* Message factory */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_msg_factory_setup), NULL);
  g_signal_connect(factory, "bind",  G_CALLBACK(on_msg_factory_bind), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_msg_factory_unbind), NULL);

  /* Selection model (no selection needed for messages) */
  GtkNoSelection *no_sel =
    gtk_no_selection_new(G_LIST_MODEL(g_object_ref(self->msg_model)));

  /* Message list view */
  self->message_list = GTK_LIST_VIEW(
    gtk_list_view_new(GTK_SELECTION_MODEL(no_sel), factory));
  gtk_widget_add_css_class(GTK_WIDGET(self->message_list), "navigation-sidebar");

  /* Scrolled container */
  self->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_scrolled_window_set_policy(self->scroll,
                                  GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(self->scroll, GTK_WIDGET(self->message_list));
  gtk_widget_set_vexpand(GTK_WIDGET(self->scroll), TRUE);

  /* Separator */
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

  /* Composer */
  self->composer = gn_group_composer_new();
  g_signal_connect(self->composer, "send-requested",
                   G_CALLBACK(on_send_requested), self);

  /* Assemble */
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->scroll));
  gtk_box_append(GTK_BOX(self), sep);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->composer));

  /* Listen for new messages to auto-scroll */
  g_signal_connect(self->msg_model, "items-changed",
                   G_CALLBACK(on_messages_changed), self);

  return self;
}
