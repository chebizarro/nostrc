/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-mls-dm-list-view.c - MLS Direct Messages List View
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-mls-dm-list-view.h"
#include "gn-group-chat-view.h"
#include <gnostr-plugin-api.h>
#include <marmot-gobject-1.0/marmot-gobject.h>

struct _GnMlsDmListView
{
  GtkBox parent_instance;

  GnMarmotService     *service;         /* strong ref */
  GnMlsEventRouter   *router;          /* strong ref */
  GnMlsDmManager     *dm_manager;      /* strong ref */
  GnostrPluginContext *plugin_context;  /* borrowed */

  /* Widgets */
  GtkStack    *stack;         /* "empty" | "list" */
  GtkListBox  *dm_list;
  GtkSpinner  *spinner;
  GtkLabel    *status_label;

  /* New DM dialog widgets */
  AdwDialog   *new_dm_dialog;
  AdwEntryRow *peer_entry;
  GtkButton   *start_dm_button;
  GtkLabel    *new_dm_status;
  GtkSpinner  *new_dm_spinner;

  /* Signal IDs */
  gulong sig_group_joined;
};

G_DEFINE_TYPE(GnMlsDmListView, gn_mls_dm_list_view, GTK_TYPE_BOX)

/* ── Forward declarations ────────────────────────────────────────── */

static void rebuild_dm_list(GnMlsDmListView *self);

/* ── Open DM flow ────────────────────────────────────────────────── */

static void
on_open_dm_done(GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  GnMlsDmListView *self = GN_MLS_DM_LIST_VIEW(user_data);
  g_autoptr(GError) error = NULL;

  g_autoptr(MarmotGobjectGroup) group =
    gn_mls_dm_manager_open_dm_finish(GN_MLS_DM_MANAGER(source), result, &error);

  gtk_spinner_stop(self->new_dm_spinner);
  gtk_widget_set_visible(GTK_WIDGET(self->new_dm_spinner), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->start_dm_button), TRUE);

  if (group == NULL)
    {
      const gchar *msg = error ? error->message : "Failed to open DM";
      gtk_label_set_text(self->new_dm_status, msg);
      gtk_widget_set_visible(GTK_WIDGET(self->new_dm_status), TRUE);
      g_object_unref(self);
      return;
    }

  g_info("MlsDmListView: DM group ready — %s",
         marmot_gobject_group_get_name(group));

  /* Close the dialog */
  if (self->new_dm_dialog != NULL)
    adw_dialog_close(self->new_dm_dialog);

  /* Refresh list and open the chat */
  rebuild_dm_list(self);

  /* Navigate to the chat view */
  GtkWidget *chat = GTK_WIDGET(gn_group_chat_view_new(
    self->service, self->router, group, self->plugin_context));

  GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
  AdwDialog *chat_dialog = adw_dialog_new();

  const gchar *name = marmot_gobject_group_get_name(group);
  g_autofree gchar *peer_short = NULL;
  if (name != NULL && g_str_has_prefix(name, "dm:"))
    {
      /* Extract peer pubkey from "dm:<pk1>+<pk2>" */
      const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
      const gchar *rest = name + 3;   /* skip "dm:" */
      g_auto(GStrv) parts = g_strsplit(rest, "+", 2);
      const gchar *peer_pk = NULL;
      if (parts != NULL && parts[0] != NULL && parts[1] != NULL)
        peer_pk = (g_strcmp0(parts[0], my_pk) == 0) ? parts[1] : parts[0];
      if (peer_pk != NULL && strlen(peer_pk) >= 16)
        peer_short = g_strdup_printf("DM: %.8s…", peer_pk);
    }

  adw_dialog_set_title(chat_dialog,
                        peer_short ? peer_short : "Encrypted DM");
  adw_dialog_set_content_width(chat_dialog, 600);
  adw_dialog_set_content_height(chat_dialog, 500);

  GtkWidget *toolbar_view = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), chat);
  adw_dialog_set_child(chat_dialog, toolbar_view);
  adw_dialog_present(chat_dialog, toplevel);

  g_object_unref(self);
}

static void
on_start_dm_clicked(GtkButton *button, gpointer user_data)
{
  GnMlsDmListView *self = GN_MLS_DM_LIST_VIEW(user_data);

  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->peer_entry));
  if (text == NULL || *text == '\0')
    return;

  g_autofree gchar *pk = g_strstrip(g_strdup(text));

  /* Validate hex pubkey */
  if (strlen(pk) != 64)
    {
      gtk_label_set_text(self->new_dm_status,
                         "Invalid pubkey — enter 64-character hex");
      gtk_widget_set_visible(GTK_WIDGET(self->new_dm_status), TRUE);
      return;
    }

  for (gsize i = 0; i < 64; i++)
    {
      if (!g_ascii_isxdigit(pk[i]))
        {
          gtk_label_set_text(self->new_dm_status,
                             "Invalid pubkey — enter 64-character hex");
          gtk_widget_set_visible(GTK_WIDGET(self->new_dm_status), TRUE);
          return;
        }
    }

  gtk_widget_set_visible(GTK_WIDGET(self->new_dm_status), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->start_dm_button), FALSE);
  gtk_spinner_start(self->new_dm_spinner);
  gtk_widget_set_visible(GTK_WIDGET(self->new_dm_spinner), TRUE);

  gn_mls_dm_manager_open_dm_async(
    self->dm_manager,
    pk,
    NULL,
    on_open_dm_done,
    g_object_ref(self));
}

/* ── New DM dialog ───────────────────────────────────────────────── */

static void
show_new_dm_dialog(GnMlsDmListView *self)
{
  GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));

  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, "New Encrypted DM");
  adw_dialog_set_content_width(dialog, 380);
  adw_dialog_set_content_height(dialog, 260);
  self->new_dm_dialog = dialog;

  GtkWidget *toolbar_view = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), content);

  AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp, "Recipient");
  adw_preferences_group_set_description(grp,
    "Enter the Nostr public key of the person you want to message. "
    "They must have published a key package (kind:443).");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(grp));

  self->peer_entry = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->peer_entry),
                                "Pubkey (hex)");
  g_signal_connect(self->peer_entry, "entry-activated",
                   G_CALLBACK(on_start_dm_clicked), self);
  adw_preferences_group_add(grp, GTK_WIDGET(self->peer_entry));

  /* Status row */
  GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(status_box, 8);
  gtk_box_append(GTK_BOX(content), status_box);

  self->new_dm_spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_widget_set_visible(GTK_WIDGET(self->new_dm_spinner), FALSE);
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(self->new_dm_spinner));

  self->new_dm_status = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->new_dm_status), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->new_dm_status), "caption");
  gtk_widget_set_visible(GTK_WIDGET(self->new_dm_status), FALSE);
  gtk_label_set_wrap(self->new_dm_status, TRUE);
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(self->new_dm_status));

  /* Start button */
  self->start_dm_button = GTK_BUTTON(
    gtk_button_new_with_label("Start Encrypted DM"));
  gtk_widget_add_css_class(GTK_WIDGET(self->start_dm_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->start_dm_button), "pill");
  gtk_widget_set_halign(GTK_WIDGET(self->start_dm_button), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(GTK_WIDGET(self->start_dm_button), 12);
  g_signal_connect(self->start_dm_button, "clicked",
                   G_CALLBACK(on_start_dm_clicked), self);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->start_dm_button));

  adw_dialog_set_child(dialog, toolbar_view);
  adw_dialog_present(dialog, toplevel);
}

/* ── DM list row builder ─────────────────────────────────────────── */

static void
on_dm_row_activated(GtkListBox    *list_box,
                    GtkListBoxRow *row,
                    gpointer       user_data)
{
  GnMlsDmListView *self = GN_MLS_DM_LIST_VIEW(user_data);

  MarmotGobjectGroup *group = g_object_get_data(G_OBJECT(row), "mls-group");
  if (group == NULL) return;

  GtkWidget *chat = GTK_WIDGET(gn_group_chat_view_new(
    self->service, self->router, group, self->plugin_context));

  const gchar *name = marmot_gobject_group_get_name(group);
  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);
  g_autofree gchar *peer_short = NULL;

  if (name != NULL && g_str_has_prefix(name, "dm:"))
    {
      const gchar *rest = name + 3;
      g_auto(GStrv) parts = g_strsplit(rest, "+", 2);
      const gchar *peer_pk = NULL;
      if (parts != NULL && parts[0] != NULL && parts[1] != NULL)
        peer_pk = (g_strcmp0(parts[0], my_pk) == 0) ? parts[1] : parts[0];
      if (peer_pk != NULL && strlen(peer_pk) >= 16)
        peer_short = g_strdup_printf("DM: %.8s…", peer_pk);
    }

  AdwDialog *chat_dialog = adw_dialog_new();
  adw_dialog_set_title(chat_dialog, peer_short ? peer_short : "Encrypted DM");
  adw_dialog_set_content_width(chat_dialog, 600);
  adw_dialog_set_content_height(chat_dialog, 500);

  GtkWidget *toolbar_view = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), chat);
  adw_dialog_set_child(chat_dialog, toolbar_view);

  GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
  adw_dialog_present(chat_dialog, toplevel);
}

static void
rebuild_dm_list(GnMlsDmListView *self)
{
  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->dm_list))) != NULL)
    gtk_list_box_remove(self->dm_list, child);

  if (self->dm_manager == NULL)
    {
      gtk_stack_set_visible_child_name(self->stack, "empty");
      return;
    }

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) dm_groups =
    gn_mls_dm_manager_get_dm_groups(self->dm_manager, &error);

  if (dm_groups == NULL || dm_groups->len == 0)
    {
      gtk_stack_set_visible_child_name(self->stack, "empty");
      return;
    }

  gtk_stack_set_visible_child_name(self->stack, "list");

  const gchar *my_pk = gn_marmot_service_get_user_pubkey_hex(self->service);

  for (guint i = 0; i < dm_groups->len; i++)
    {
      MarmotGobjectGroup *group = g_ptr_array_index(dm_groups, i);
      const gchar *name = marmot_gobject_group_get_name(group);

      /* Derive display name from canonical DM name */
      g_autofree gchar *display_name = NULL;
      if (name != NULL && g_str_has_prefix(name, "dm:"))
        {
          const gchar *rest = name + 3;
          g_auto(GStrv) parts = g_strsplit(rest, "+", 2);
          const gchar *peer_pk = NULL;
          if (parts != NULL && parts[0] != NULL && parts[1] != NULL)
            peer_pk = (g_strcmp0(parts[0], my_pk) == 0) ? parts[1] : parts[0];
          if (peer_pk != NULL && strlen(peer_pk) >= 16)
            display_name = g_strdup_printf("%.8s…%.8s",
                                           peer_pk,
                                           peer_pk + strlen(peer_pk) - 8);
        }

      /* Row content */
      GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
      gtk_widget_set_margin_start(row_box, 12);
      gtk_widget_set_margin_end(row_box, 12);
      gtk_widget_set_margin_top(row_box, 8);
      gtk_widget_set_margin_bottom(row_box, 8);

      GtkWidget *icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
      gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
      gtk_widget_add_css_class(icon, "dim-label");
      gtk_box_append(GTK_BOX(row_box), icon);

      GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_hexpand(text_box, TRUE);
      gtk_box_append(GTK_BOX(row_box), text_box);

      GtkWidget *name_lbl = gtk_label_new(
        display_name ? display_name : "(Unknown peer)");
      gtk_widget_add_css_class(name_lbl, "heading");
      gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
      gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(text_box), name_lbl);

      GtkWidget *sub_lbl = gtk_label_new("MLS encrypted · Forward secrecy");
      gtk_widget_add_css_class(sub_lbl, "dim-label");
      gtk_widget_add_css_class(sub_lbl, "caption");
      gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(text_box), sub_lbl);

      GtkWidget *chevron =
        gtk_image_new_from_icon_name("go-next-symbolic");
      gtk_widget_add_css_class(chevron, "dim-label");
      gtk_box_append(GTK_BOX(row_box), chevron);

      GtkListBoxRow *list_row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
      gtk_list_box_row_set_child(list_row, row_box);
      g_object_set_data_full(G_OBJECT(list_row), "mls-group",
                              g_object_ref(group), g_object_unref);
      gtk_list_box_append(self->dm_list, GTK_WIDGET(list_row));
    }
}

/* ── Signal handlers ─────────────────────────────────────────────── */

static void
on_group_joined(GnMarmotService    *service,
                MarmotGobjectGroup *group,
                gpointer            user_data)
{
  GnMlsDmListView *self = GN_MLS_DM_LIST_VIEW(user_data);

  /* If the joined group is a DM, refresh our list */
  const gchar *name = marmot_gobject_group_get_name(group);
  if (name != NULL && g_str_has_prefix(name, "dm:"))
    rebuild_dm_list(self);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_mls_dm_list_view_dispose(GObject *object)
{
  GnMlsDmListView *self = GN_MLS_DM_LIST_VIEW(object);

  if (self->sig_group_joined > 0 && self->service != NULL)
    {
      g_signal_handler_disconnect(self->service, self->sig_group_joined);
      self->sig_group_joined = 0;
    }

  g_clear_object(&self->service);
  g_clear_object(&self->router);
  g_clear_object(&self->dm_manager);
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_mls_dm_list_view_parent_class)->dispose(object);
}

static void
gn_mls_dm_list_view_class_init(GnMlsDmListViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_mls_dm_list_view_dispose;
}

static void
gn_mls_dm_list_view_init(GnMlsDmListView *self)
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

  GtkWidget *title = gtk_label_new("Encrypted DMs");
  gtk_widget_add_css_class(title, "title-4");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header_box), title);

  /* New DM button */
  GtkWidget *new_btn =
    gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_add_css_class(new_btn, "flat");
  gtk_widget_add_css_class(new_btn, "circular");
  gtk_widget_set_tooltip_text(new_btn, "New encrypted DM");
  g_signal_connect_swapped(new_btn, "clicked",
                            G_CALLBACK(show_new_dm_dialog), self);
  gtk_box_append(GTK_BOX(header_box), new_btn);

  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(self), sep);

  /* Stack */
  self->stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(self->stack), TRUE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->stack));

  /* Empty page */
  GtkWidget *empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);

  GtkWidget *empty_icon =
    gtk_image_new_from_icon_name("avatar-default-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 48);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(empty_box), empty_icon);

  GtkWidget *empty_lbl = gtk_label_new("No encrypted DMs yet");
  gtk_widget_add_css_class(empty_lbl, "dim-label");
  gtk_widget_add_css_class(empty_lbl, "title-4");
  gtk_box_append(GTK_BOX(empty_box), empty_lbl);

  GtkWidget *empty_sub = gtk_label_new(
    "Start an MLS-encrypted DM for forward secrecy.\n"
    "Tap + to message someone by their Nostr pubkey.");
  gtk_widget_add_css_class(empty_sub, "dim-label");
  gtk_label_set_justify(GTK_LABEL(empty_sub), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(empty_box), empty_sub);

  GtkWidget *new_dm_btn = gtk_button_new_with_label("New Encrypted DM");
  gtk_widget_add_css_class(new_dm_btn, "suggested-action");
  gtk_widget_add_css_class(new_dm_btn, "pill");
  gtk_widget_set_halign(new_dm_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(new_dm_btn, 8);
  g_signal_connect_swapped(new_dm_btn, "clicked",
                            G_CALLBACK(show_new_dm_dialog), self);
  gtk_box_append(GTK_BOX(empty_box), new_dm_btn);

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

  self->dm_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->dm_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->dm_list), "boxed-list");
  g_signal_connect(self->dm_list, "row-activated",
                   G_CALLBACK(on_dm_row_activated), self);
  gtk_box_append(GTK_BOX(list_content), GTK_WIDGET(self->dm_list));

  gtk_stack_add_named(self->stack, scroll, "list");
  gtk_stack_set_visible_child_name(self->stack, "empty");
}

/* ── Public API ──────────────────────────────────────────────────── */

GnMlsDmListView *
gn_mls_dm_list_view_new(GnMarmotService     *service,
                          GnMlsEventRouter   *router,
                          GnMlsDmManager     *dm_manager,
                          GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_MARMOT_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_MLS_EVENT_ROUTER(router), NULL);
  g_return_val_if_fail(GN_IS_MLS_DM_MANAGER(dm_manager), NULL);
  g_return_val_if_fail(plugin_context != NULL, NULL);

  GnMlsDmListView *self = g_object_new(GN_TYPE_MLS_DM_LIST_VIEW, NULL);
  self->service        = g_object_ref(service);
  self->router         = g_object_ref(router);
  self->dm_manager     = g_object_ref(dm_manager);
  self->plugin_context = plugin_context;

  /* Listen for newly joined DM groups */
  self->sig_group_joined = g_signal_connect(
    service, "group-joined",
    G_CALLBACK(on_group_joined), self);

  /* Initial load */
  rebuild_dm_list(self);

  return self;
}
