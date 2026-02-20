/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-welcome-list-view.c - Group Invitations View
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-welcome-list-view.h"
#include <gnostr-plugin-api.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

struct _GnWelcomeListView
{
  GtkBox parent_instance;

  GnMarmotService     *service;         /* strong ref */
  GnMlsEventRouter   *router;          /* strong ref */
  GnostrPluginContext *plugin_context;  /* borrowed */

  /* Widgets */
  GtkStack     *stack;          /* "empty" | "list" */
  GtkListBox   *list_box;
  GtkSpinner   *spinner;
  GtkLabel     *status_label;

  /* Signal IDs */
  gulong sig_welcome_received;
};

G_DEFINE_TYPE(GnWelcomeListView, gn_welcome_list_view, GTK_TYPE_BOX)

/* ── Forward declarations ────────────────────────────────────────── */

static void rebuild_list(GnWelcomeListView *self);

/* ── Accept / Decline flow ───────────────────────────────────────── */

typedef struct
{
  GnWelcomeListView    *view;     /* weak — view owns the flow */
  MarmotGobjectWelcome *welcome;  /* strong ref */
  GtkWidget            *row;      /* the list row to remove on success */
} WelcomeActionData;

static void
welcome_action_data_free(WelcomeActionData *data)
{
  g_clear_object(&data->welcome);
  g_free(data);
}

static void
on_accept_done(GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  WelcomeActionData *data = user_data;
  GnWelcomeListView *self = data->view;
  g_autoptr(GError) error = NULL;

  gboolean ok = marmot_gobject_client_accept_welcome_finish(
    MARMOT_GOBJECT_CLIENT(source), result, &error);

  if (!ok)
    {
      g_warning("WelcomeListView: failed to accept welcome: %s",
                error ? error->message : "unknown");
      /* Re-enable the row buttons */
      if (data->row != NULL)
        gtk_widget_set_sensitive(data->row, TRUE);
    }
  else
    {
      g_info("WelcomeListView: welcome accepted — group joined");

      /* Remove the row and refresh */
      if (data->row != NULL && self != NULL)
        {
          gtk_list_box_remove(self->list_box, data->row);
          rebuild_list(self);
        }
    }

  welcome_action_data_free(data);
}

static void
on_accept_clicked(GtkButton *button, gpointer user_data)
{
  WelcomeActionData *data = user_data;
  GnWelcomeListView *self = data->view;

  if (self == NULL || self->service == NULL)
    return;

  /* Disable row while async op runs */
  if (data->row != NULL)
    gtk_widget_set_sensitive(data->row, FALSE);

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  marmot_gobject_client_accept_welcome_async(
    client,
    data->welcome,
    NULL,
    on_accept_done,
    data);
}

static void
on_decline_clicked(GtkButton *button, gpointer user_data)
{
  WelcomeActionData *data = user_data;
  GnWelcomeListView *self = data->view;

  if (self == NULL) return;

  /*
   * Declining a welcome: mark it locally as declined.
   * libmarmot does not require a network action for decline —
   * we simply remove it from the pending list.
   */
  g_info("WelcomeListView: welcome declined by user");

  if (data->row != NULL)
    {
      gtk_list_box_remove(self->list_box, data->row);
      rebuild_list(self);
    }

  welcome_action_data_free(data);
}

/* ── Row builder ─────────────────────────────────────────────────── */

static GtkWidget *
build_welcome_row(GnWelcomeListView    *self,
                  MarmotGobjectWelcome *welcome)
{
  const gchar *group_name = marmot_gobject_welcome_get_group_name(welcome);
  const gchar *group_desc = marmot_gobject_welcome_get_group_description(welcome);
  const gchar *welcomer   = marmot_gobject_welcome_get_welcomer(welcome);
  guint member_count      = marmot_gobject_welcome_get_member_count(welcome);

  /* Outer row box */
  GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(row_box, 12);
  gtk_widget_set_margin_end(row_box, 12);
  gtk_widget_set_margin_top(row_box, 10);
  gtk_widget_set_margin_bottom(row_box, 10);

  /* Group name */
  GtkWidget *name_lbl = gtk_label_new(
    (group_name && *group_name) ? group_name : "(Unnamed Group)");
  gtk_widget_add_css_class(name_lbl, "heading");
  gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(row_box), name_lbl);

  /* Description (if any) */
  if (group_desc != NULL && *group_desc != '\0')
    {
      GtkWidget *desc_lbl = gtk_label_new(group_desc);
      gtk_widget_add_css_class(desc_lbl, "dim-label");
      gtk_widget_add_css_class(desc_lbl, "caption");
      gtk_label_set_ellipsize(GTK_LABEL(desc_lbl), PANGO_ELLIPSIZE_END);
      gtk_widget_set_halign(desc_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(row_box), desc_lbl);
    }

  /* Meta: invited by + member count */
  g_autofree gchar *meta = NULL;
  if (welcomer != NULL && strlen(welcomer) >= 16)
    {
      meta = g_strdup_printf("Invited by %.8s… · %u member%s",
                             welcomer, member_count,
                             member_count == 1 ? "" : "s");
    }
  else
    {
      meta = g_strdup_printf("%u member%s",
                             member_count,
                             member_count == 1 ? "" : "s");
    }

  GtkWidget *meta_lbl = gtk_label_new(meta);
  gtk_widget_add_css_class(meta_lbl, "dim-label");
  gtk_widget_add_css_class(meta_lbl, "caption");
  gtk_widget_set_halign(meta_lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(row_box), meta_lbl);

  /* Action buttons */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 4);
  gtk_box_append(GTK_BOX(row_box), btn_box);

  GtkWidget *decline_btn = gtk_button_new_with_label("Decline");
  gtk_widget_add_css_class(decline_btn, "flat");
  gtk_box_append(GTK_BOX(btn_box), decline_btn);

  GtkWidget *accept_btn = gtk_button_new_with_label("Accept");
  gtk_widget_add_css_class(accept_btn, "suggested-action");
  gtk_widget_add_css_class(accept_btn, "pill");
  gtk_box_append(GTK_BOX(btn_box), accept_btn);

  /* Wire up callbacks — data is owned by the accept button's closure */
  WelcomeActionData *data = g_new0(WelcomeActionData, 1);
  data->view    = self;
  data->welcome = g_object_ref(welcome);
  data->row     = row_box;   /* set after list_box_append */

  g_signal_connect_data(accept_btn, "clicked",
                        G_CALLBACK(on_accept_clicked),
                        data,
                        (GClosureNotify)NULL,
                        0);

  /* Decline shares the same data — only one will fire */
  g_signal_connect(decline_btn, "clicked",
                   G_CALLBACK(on_decline_clicked), data);

  return row_box;
}

/* ── List rebuild ────────────────────────────────────────────────── */

static void
rebuild_list(GnWelcomeListView *self)
{
  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != NULL)
    gtk_list_box_remove(self->list_box, child);

  if (self->service == NULL)
    {
      gtk_stack_set_visible_child_name(self->stack, "empty");
      return;
    }

  MarmotGobjectClient *client = gn_marmot_service_get_client(self->service);
  if (client == NULL)
    {
      gtk_stack_set_visible_child_name(self->stack, "empty");
      return;
    }

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) welcomes =
    marmot_gobject_client_get_pending_welcomes(client, &error);

  if (welcomes == NULL || welcomes->len == 0)
    {
      gtk_stack_set_visible_child_name(self->stack, "empty");
      return;
    }

  gtk_stack_set_visible_child_name(self->stack, "list");

  for (guint i = 0; i < welcomes->len; i++)
    {
      MarmotGobjectWelcome *welcome = g_ptr_array_index(welcomes, i);

      /* Only show pending welcomes */
      if (marmot_gobject_welcome_get_state(welcome) !=
          MARMOT_GOBJECT_WELCOME_STATE_PENDING)
        continue;

      GtkWidget *row = build_welcome_row(self, welcome);
      gtk_list_box_append(self->list_box, row);
    }

  /* If all were non-pending, show empty */
  if (gtk_widget_get_first_child(GTK_WIDGET(self->list_box)) == NULL)
    gtk_stack_set_visible_child_name(self->stack, "empty");
}

/* ── Signal handlers ─────────────────────────────────────────────── */

static void
on_welcome_received(GnMarmotService      *service,
                    MarmotGobjectWelcome *welcome,
                    gpointer              user_data)
{
  GnWelcomeListView *self = GN_WELCOME_LIST_VIEW(user_data);
  rebuild_list(self);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_welcome_list_view_dispose(GObject *object)
{
  GnWelcomeListView *self = GN_WELCOME_LIST_VIEW(object);

  if (self->sig_welcome_received > 0 && self->service != NULL)
    {
      g_signal_handler_disconnect(self->service, self->sig_welcome_received);
      self->sig_welcome_received = 0;
    }

  g_clear_object(&self->service);
  g_clear_object(&self->router);
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_welcome_list_view_parent_class)->dispose(object);
}

static void
gn_welcome_list_view_class_init(GnWelcomeListViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_welcome_list_view_dispose;
}

static void
gn_welcome_list_view_init(GnWelcomeListView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);

  /* Header */
  GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(header_box, 16);
  gtk_widget_set_margin_end(header_box, 16);
  gtk_widget_set_margin_top(header_box, 16);
  gtk_widget_set_margin_bottom(header_box, 8);
  gtk_box_append(GTK_BOX(self), header_box);

  GtkWidget *title = gtk_label_new("Group Invitations");
  gtk_widget_add_css_class(title, "title-4");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header_box), title);

  /* Refresh button */
  GtkWidget *refresh_btn =
    gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_add_css_class(refresh_btn, "flat");
  gtk_widget_add_css_class(refresh_btn, "circular");
  gtk_widget_set_tooltip_text(refresh_btn, "Refresh invitations");
  g_signal_connect_swapped(refresh_btn, "clicked",
                            G_CALLBACK(gn_welcome_list_view_refresh), self);
  gtk_box_append(GTK_BOX(header_box), refresh_btn);

  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(self), sep);

  /* Stack: "empty" page vs "list" page */
  self->stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(self->stack), TRUE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->stack));

  /* Empty page */
  GtkWidget *empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);

  GtkWidget *empty_icon =
    gtk_image_new_from_icon_name("mail-unread-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 48);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(empty_box), empty_icon);

  GtkWidget *empty_lbl = gtk_label_new("No pending invitations");
  gtk_widget_add_css_class(empty_lbl, "dim-label");
  gtk_widget_add_css_class(empty_lbl, "title-4");
  gtk_box_append(GTK_BOX(empty_box), empty_lbl);

  GtkWidget *empty_sub = gtk_label_new(
    "Group invitations will appear here when\nsomeone adds you to an MLS group.");
  gtk_widget_add_css_class(empty_sub, "dim-label");
  gtk_label_set_justify(GTK_LABEL(empty_sub), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(empty_box), empty_sub);

  gtk_stack_add_named(self->stack, empty_box, "empty");

  /* List page */
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *list_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(list_content, 16);
  gtk_widget_set_margin_end(list_content, 16);
  gtk_widget_set_margin_top(list_content, 12);
  gtk_widget_set_margin_bottom(list_content, 16);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_content);

  self->list_box = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->list_box, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->list_box), "boxed-list");
  gtk_box_append(GTK_BOX(list_content), GTK_WIDGET(self->list_box));

  gtk_stack_add_named(self->stack, scroll, "list");
  gtk_stack_set_visible_child_name(self->stack, "empty");
}

/* ── Public API ──────────────────────────────────────────────────── */

GnWelcomeListView *
gn_welcome_list_view_new(GnMarmotService     *service,
                          GnMlsEventRouter   *router,
                          GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(router), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnWelcomeListView *self = g_object_new(GN_TYPE_WELCOME_LIST_VIEW, NULL);
  self->service        = g_object_ref(service);
  self->router         = g_object_ref(router);
  self->plugin_context = plugin_context;

  /* Listen for new welcomes */
  self->sig_welcome_received = g_signal_connect(
    service, "welcome-received",
    G_CALLBACK(on_welcome_received), self);

  /* Initial load */
  rebuild_list(self);

  return self;
}

void
gn_welcome_list_view_refresh(GnWelcomeListView *self)
{
  g_return_if_fail(GN_IS_WELCOME_LIST_VIEW(self));
  rebuild_list(self);
}
