/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-chat-view.c - Chat view for a NIP-29 group
 */

#include "gn-nip29-group-chat-view.h"
#include "gn-nip29-message-row.h"
#include "gn-nip29-composer.h"
#include "../model/gn-nip29-message-list-model.h"
#include "../model/gn-nip29-message-item.h"
#include <gnostr-plugin-api.h>
#include <nip29.h>

struct _GnNip29GroupChatView
{
  GtkBox parent_instance;

  /* Dependencies */
  GnNip29GroupService     *service;
  GnNip29GroupItem        *group_item;
  GnostrPluginContext     *plugin_context;

  /* Header widgets */
  GtkLabel                *name_label;
  GtkLabel                *meta_label;
  GtkButton               *join_button;
  GtkButton               *leave_button;
  GtkLabel                *status_label;

  /* Message area */
  GtkListView             *message_list;
  GtkScrolledWindow       *scroll;
  GtkWidget               *empty_box;
  GtkStack                *msg_stack;

  /* Composer */
  GnNip29Composer         *composer;
  GCancellable            *action_cancellable;
  gboolean                 action_busy;

  /* Model */
  GnNip29MessageListModel *msg_model;
  gulong                   sig_items_changed;
};

G_DEFINE_TYPE(GnNip29GroupChatView, gn_nip29_group_chat_view, GTK_TYPE_BOX)

/* ── Factory callbacks ───────────────────────────────────────────── */

static void
on_msg_factory_setup(GtkSignalListItemFactory *factory,
                     GtkListItem              *list_item,
                     gpointer                  user_data)
{
  GnNip29MessageRow *row = gn_nip29_message_row_new();
  gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
on_msg_factory_bind(GtkSignalListItemFactory *factory,
                    GtkListItem              *list_item,
                    gpointer                  user_data)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  GnNip29MessageRow *row = GN_NIP29_MESSAGE_ROW(gtk_list_item_get_child(list_item));
  GnNip29MessageItem *item = gtk_list_item_get_item(list_item);

  const char *user_pk = gn_nip29_group_service_get_current_pubkey(self->service);
  gn_nip29_message_row_bind(row, item, user_pk, self->plugin_context);
}

static void
on_msg_factory_unbind(GtkSignalListItemFactory *factory,
                      GtkListItem              *list_item,
                      gpointer                  user_data)
{
  GnNip29MessageRow *row = GN_NIP29_MESSAGE_ROW(gtk_list_item_get_child(list_item));
  gn_nip29_message_row_unbind(row);
}

/* ── Auto-scroll on new messages ─────────────────────────────────── */

static void
on_messages_changed(GListModel *model,
                    guint       position,
                    guint       removed,
                    guint       added,
                    gpointer    user_data)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  guint n = g_list_model_get_n_items(model);

  /* Toggle empty/list */
  if (n == 0)
    gtk_stack_set_visible_child(self->msg_stack, self->empty_box);
  else
    gtk_stack_set_visible_child(self->msg_stack, GTK_WIDGET(self->scroll));

  /* Auto-scroll to bottom when new messages arrive */
  if (added > 0 && n > 0)
    {
      GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(self->scroll);
      gdouble upper = gtk_adjustment_get_upper(adj);
      gtk_adjustment_set_value(adj, upper);
    }
}

/* ── User-authored action hooks ─────────────────────────────────── */

static void
set_action_status(GnNip29GroupChatView *self,
                  const char           *message,
                  gboolean              is_error)
{
  gtk_label_set_text(self->status_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->status_label),
                         message != NULL && message[0] != '\0');
  if (is_error)
    gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self->status_label), "error");
}

static void
set_action_busy(GnNip29GroupChatView *self,
                gboolean              busy)
{
  self->action_busy = busy;
  gtk_widget_set_sensitive(GTK_WIDGET(self->join_button), !busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->leave_button), !busy);
  gn_nip29_composer_set_send_sensitive(self->composer, !busy);
}

static const char *
current_group_key(GnNip29GroupChatView *self)
{
  return self->group_item ? gn_nip29_group_item_get_key(self->group_item) : NULL;
}

static void
on_send_done(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  (void)source;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_send_message_finish(self->service,
                                                           result,
                                                           &error);
  g_clear_object(&self->action_cancellable);
  set_action_busy(self, FALSE);

  if (ok)
    {
      gn_nip29_composer_clear(self->composer);
      set_action_status(self, "Message published. Refreshing relay state…", FALSE);
    }
  else
    {
      set_action_status(self, error ? error->message : "Failed to send message", TRUE);
    }

  g_object_unref(self);
}

static void
on_join_done(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  (void)source;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_join_group_finish(self->service,
                                                         result,
                                                         &error);
  g_clear_object(&self->action_cancellable);
  set_action_busy(self, FALSE);
  set_action_status(self,
                    ok ? "Join request published. Pending relay state refresh…"
                       : (error ? error->message : "Failed to join group"),
                    !ok);
  g_object_unref(self);
}

static void
on_leave_done(GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  (void)source;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_leave_group_finish(self->service,
                                                          result,
                                                          &error);
  g_clear_object(&self->action_cancellable);
  set_action_busy(self, FALSE);
  set_action_status(self,
                    ok ? "Leave request published. Pending relay state refresh…"
                       : (error ? error->message : "Failed to leave group"),
                    !ok);
  g_object_unref(self);
}

static void
start_action_cancellable(GnNip29GroupChatView *self,
                         const char           *status)
{
  g_clear_object(&self->action_cancellable);
  self->action_cancellable = g_cancellable_new();
  set_action_busy(self, TRUE);
  set_action_status(self, status, FALSE);
}

static void
on_send_requested(GnNip29Composer *composer,
                  const char      *text,
                  gpointer         user_data)
{
  (void)composer;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (self->action_busy)
    return;

  start_action_cancellable(self, "Signing and publishing message…");
  gn_nip29_group_service_send_message_async(self->service,
                                            current_group_key(self),
                                            text,
                                            self->action_cancellable,
                                            on_send_done,
                                            g_object_ref(self));
}

static void
on_join_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (self->action_busy)
    return;

  start_action_cancellable(self, "Signing and publishing join request…");
  gn_nip29_group_service_join_group_async(self->service,
                                          current_group_key(self),
                                          NULL,
                                          NULL,
                                          self->action_cancellable,
                                          on_join_done,
                                          g_object_ref(self));
}

static void
on_leave_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (self->action_busy)
    return;

  start_action_cancellable(self, "Signing and publishing leave request…");
  gn_nip29_group_service_leave_group_async(self->service,
                                           current_group_key(self),
                                           NULL,
                                           self->action_cancellable,
                                           on_leave_done,
                                           g_object_ref(self));
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_nip29_group_chat_view_dispose(GObject *object)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(object);

  if (self->msg_model != NULL && self->sig_items_changed > 0)
    {
      g_signal_handler_disconnect(self->msg_model, self->sig_items_changed);
      self->sig_items_changed = 0;
    }

  if (self->action_cancellable != NULL)
    g_cancellable_cancel(self->action_cancellable);
  g_clear_object(&self->action_cancellable);
  g_clear_object(&self->service);
  g_clear_object(&self->group_item);
  g_clear_object(&self->msg_model);
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_nip29_group_chat_view_parent_class)->dispose(object);
}

static void
gn_nip29_group_chat_view_class_init(GnNip29GroupChatViewClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_nip29_group_chat_view_dispose;
}

static void
gn_nip29_group_chat_view_init(GnNip29GroupChatView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
}

/* ── Metadata header helper ──────────────────────────────────────── */

static gchar *
build_meta_text(GnNip29GroupItem *item)
{
  GString *s = g_string_new(NULL);

  const char *relay = gn_nip29_group_item_get_relay_url(item);
  if (relay != NULL)
    g_string_append_printf(s, "%s", relay);

  if (gn_nip29_group_item_get_is_private(item))
    g_string_append(s, " · private");
  if (gn_nip29_group_item_get_is_closed(item))
    g_string_append(s, " · closed");
  if (gn_nip29_group_item_get_is_hidden(item))
    g_string_append(s, " · hidden");
  if (gn_nip29_group_item_get_is_restricted(item))
    g_string_append(s, " · restricted");

  if (gn_nip29_group_item_get_members_loaded(item))
    {
      guint mc = gn_nip29_group_item_get_member_count(item);
      gboolean partial = gn_nip29_group_item_get_members_may_be_partial(item);
      g_string_append_printf(s, " · %u member%s%s",
                             mc, mc == 1 ? "" : "s",
                             partial ? "+" : "");
    }
  else
    {
      g_string_append(s, " · members unknown");
    }

  if (!gn_nip29_group_item_get_admins_loaded(item))
    g_string_append(s, " · admin state unknown");

  return g_string_free(s, FALSE);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnNip29GroupChatView *
gn_nip29_group_chat_view_new(GnNip29GroupService *service,
                              GnNip29GroupItem    *group_item,
                              GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_NIP29_GROUP_ITEM(group_item), NULL);

  GnNip29GroupChatView *self = g_object_new(GN_TYPE_NIP29_GROUP_CHAT_VIEW, NULL);
  self->service = g_object_ref(service);
  self->group_item = g_object_ref(group_item);
  self->plugin_context = plugin_context;

  const char *group_key = gn_nip29_group_item_get_key(group_item);
  const char *display = gn_nip29_group_item_get_display_name(group_item);

  /* ── Header bar ────────────────────────────────────────────────── */
  GtkWidget *chat_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(chat_header, 12);
  gtk_widget_set_margin_end(chat_header, 12);
  gtk_widget_set_margin_top(chat_header, 8);
  gtk_widget_set_margin_bottom(chat_header, 8);

  self->name_label = GTK_LABEL(gtk_label_new(
    (display && *display) ? display : "Group Chat"));
  gtk_widget_add_css_class(GTK_WIDGET(self->name_label), "heading");
  gtk_label_set_ellipsize(self->name_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(self->name_label, 0);
  gtk_box_append(GTK_BOX(chat_header), GTK_WIDGET(self->name_label));

  g_autofree gchar *meta = build_meta_text(group_item);
  self->meta_label = GTK_LABEL(gtk_label_new(meta));
  gtk_widget_add_css_class(GTK_WIDGET(self->meta_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->meta_label), "caption");
  gtk_label_set_ellipsize(self->meta_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(self->meta_label, 0);
  gtk_box_append(GTK_BOX(chat_header), GTK_WIDGET(self->meta_label));

  GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(action_row, 6);

  self->join_button = GTK_BUTTON(gtk_button_new_with_label("Join"));
  gtk_widget_add_css_class(GTK_WIDGET(self->join_button), "pill");
  g_signal_connect(self->join_button, "clicked",
                   G_CALLBACK(on_join_clicked), self);
  gtk_box_append(GTK_BOX(action_row), GTK_WIDGET(self->join_button));

  self->leave_button = GTK_BUTTON(gtk_button_new_with_label("Leave"));
  gtk_widget_add_css_class(GTK_WIDGET(self->leave_button), "pill");
  g_signal_connect(self->leave_button, "clicked",
                   G_CALLBACK(on_leave_clicked), self);
  gtk_box_append(GTK_BOX(action_row), GTK_WIDGET(self->leave_button));

  self->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->status_label, 0);
  gtk_label_set_wrap(self->status_label, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "dim-label");
  gtk_widget_set_hexpand(GTK_WIDGET(self->status_label), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), FALSE);
  gtk_box_append(GTK_BOX(action_row), GTK_WIDGET(self->status_label));

  gtk_box_append(GTK_BOX(chat_header), action_row);

  gtk_box_append(GTK_BOX(self), chat_header);
  gtk_box_append(GTK_BOX(self), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* ── Message model ─────────────────────────────────────────────── */
  self->msg_model = gn_nip29_message_list_model_new(service, group_key);

  /* ── Message list view ─────────────────────────────────────────── */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_msg_factory_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_msg_factory_bind), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_msg_factory_unbind), NULL);

  GtkNoSelection *no_sel =
    gtk_no_selection_new(G_LIST_MODEL(g_object_ref(self->msg_model)));
  self->message_list = GTK_LIST_VIEW(
    gtk_list_view_new(GTK_SELECTION_MODEL(no_sel), factory));
  gtk_widget_add_css_class(GTK_WIDGET(self->message_list), "navigation-sidebar");

  self->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_scrolled_window_set_policy(self->scroll,
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(self->scroll, GTK_WIDGET(self->message_list));
  gtk_widget_set_vexpand(GTK_WIDGET(self->scroll), TRUE);

  /* Empty state */
  self->empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign(self->empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(self->empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(self->empty_box, TRUE);

  GtkWidget *empty_icon = gtk_image_new_from_icon_name("chat-bubble-text-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 48);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_box), empty_icon);

  GtkWidget *empty_lbl = gtk_label_new("No messages yet");
  gtk_widget_add_css_class(empty_lbl, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_box), empty_lbl);

  /* Stack for empty vs message list */
  self->msg_stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(self->msg_stack), TRUE);
  gtk_stack_add_named(self->msg_stack, self->empty_box, "empty");
  gtk_stack_add_named(self->msg_stack, GTK_WIDGET(self->scroll), "messages");

  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->msg_model));
  if (n == 0)
    gtk_stack_set_visible_child(self->msg_stack, self->empty_box);
  else
    gtk_stack_set_visible_child(self->msg_stack, GTK_WIDGET(self->scroll));

  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->msg_stack));

  /* ── Separator + Composer ──────────────────────────────────────── */
  gtk_box_append(GTK_BOX(self), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  self->composer = gn_nip29_composer_new();
  g_signal_connect(self->composer, "send-requested",
                   G_CALLBACK(on_send_requested), self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->composer));

  /* Listen for model changes */
  self->sig_items_changed = g_signal_connect(self->msg_model, "items-changed",
                                             G_CALLBACK(on_messages_changed), self);

  return self;
}
