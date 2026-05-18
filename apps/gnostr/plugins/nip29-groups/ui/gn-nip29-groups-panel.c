/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-groups-panel.c - Main NIP-29 groups list/detail panel
 */

#include "gn-nip29-groups-panel.h"
#include "gn-nip29-group-list-row.h"
#include "gn-nip29-group-chat-view.h"
#include "gn-nip29-add-group-dialog.h"
#include "../model/gn-nip29-group-list-model.h"
#include "../model/gn-nip29-group-item.h"
#include <gnostr-plugin-api.h>

struct _GnNip29GroupsPanel
{
  GtkBox parent_instance;

  /* Dependencies */
  GnNip29GroupService     *service;
  AdwNavigationView       *nav_view;       /* weak — host owns it */
  GnostrPluginContext     *plugin_context;  /* borrowed */

  /* Widgets */
  GtkListView             *list_view;
  GtkWidget               *empty_page;
  GtkStack                *stack;
  GtkWidget               *error_bar;
  GtkLabel                *error_label;

  /* Model */
  GnNip29GroupListModel   *model;

  /* Signal IDs for cleanup */
  gulong                   sig_error;
  gulong                   sig_items_changed;
};

G_DEFINE_TYPE(GnNip29GroupsPanel, gn_nip29_groups_panel, GTK_TYPE_BOX)

/* ── Forward declarations ────────────────────────────────────────── */
static void on_add_group_clicked(GtkButton *button, gpointer user_data);
static void on_create_group_clicked(GtkButton *button, gpointer user_data);
static void on_refresh_clicked(GtkButton *button, gpointer user_data);

/* ── Factory callbacks ───────────────────────────────────────────── */

static void
on_factory_setup(GtkSignalListItemFactory *factory,
                 GtkListItem              *list_item,
                 gpointer                  user_data)
{
  GnNip29GroupListRow *row = gn_nip29_group_list_row_new();
  gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
on_factory_bind(GtkSignalListItemFactory *factory,
                GtkListItem              *list_item,
                gpointer                  user_data)
{
  GnNip29GroupListRow *row = GN_NIP29_GROUP_LIST_ROW(gtk_list_item_get_child(list_item));
  GnNip29GroupItem *item = gtk_list_item_get_item(list_item);
  gn_nip29_group_list_row_bind(row, item);
}

static void
on_factory_unbind(GtkSignalListItemFactory *factory,
                  GtkListItem              *list_item,
                  gpointer                  user_data)
{
  GnNip29GroupListRow *row = GN_NIP29_GROUP_LIST_ROW(gtk_list_item_get_child(list_item));
  gn_nip29_group_list_row_unbind(row);
}

/* ── Group activation ────────────────────────────────────────────── */

static void
on_group_activated(GtkListView *list_view,
                   guint        position,
                   gpointer     user_data)
{
  GnNip29GroupsPanel *self = GN_NIP29_GROUPS_PANEL(user_data);

  g_autoptr(GnNip29GroupItem) item =
    g_list_model_get_item(G_LIST_MODEL(self->model), position);
  if (item == NULL)
    return;

  const char *display = gn_nip29_group_item_get_display_name(item);
  const char *key = gn_nip29_group_item_get_key(item);

  GnNip29GroupChatView *chat = gn_nip29_group_chat_view_new(
    self->service, item, self->plugin_context);

  if (self->nav_view != NULL)
    {
      AdwNavigationPage *page = adw_navigation_page_new(
        GTK_WIDGET(chat),
        (display && *display) ? display : "Group Chat");
      adw_navigation_page_set_tag(page, key);
      adw_navigation_view_push(self->nav_view, page);
    }
  else
    {
      /* Fallback: present in an AdwDialog */
      GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));

      AdwDialog *dialog = adw_dialog_new();
      adw_dialog_set_title(dialog, (display && *display) ? display : "Group Chat");
      adw_dialog_set_content_width(dialog, 600);
      adw_dialog_set_content_height(dialog, 500);

      GtkWidget *tv = adw_toolbar_view_new();
      adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), adw_header_bar_new());
      adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(chat));
      adw_dialog_set_child(dialog, tv);
      adw_dialog_present(dialog, toplevel);
    }
}

/* ── Model change → empty/list toggle ────────────────────────────── */

static void
on_items_changed(GListModel *model,
                 guint       position,
                 guint       removed,
                 guint       added,
                 gpointer    user_data)
{
  GnNip29GroupsPanel *self = GN_NIP29_GROUPS_PANEL(user_data);
  guint n = g_list_model_get_n_items(model);

  if (n == 0)
    gtk_stack_set_visible_child_name(self->stack, "empty");
  else
    gtk_stack_set_visible_child_name(self->stack, "list");
}

/* ── Error banner ────────────────────────────────────────────────── */

static void
on_error_reported(GnNip29GroupService *service,
                  const char          *message,
                  GnNip29GroupsPanel  *self)
{
  gtk_label_set_text(self->error_label, message);
  gtk_widget_set_visible(self->error_bar, TRUE);
}

static void
on_error_dismiss(GtkButton *button, gpointer user_data)
{
  GnNip29GroupsPanel *self = GN_NIP29_GROUPS_PANEL(user_data);
  gtk_widget_set_visible(self->error_bar, FALSE);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_nip29_groups_panel_dispose(GObject *object)
{
  GnNip29GroupsPanel *self = GN_NIP29_GROUPS_PANEL(object);

  if (self->model != NULL && self->sig_items_changed > 0)
    {
      g_signal_handler_disconnect(self->model, self->sig_items_changed);
      self->sig_items_changed = 0;
    }

  if (self->service != NULL && self->sig_error > 0)
    {
      g_signal_handler_disconnect(self->service, self->sig_error);
      self->sig_error = 0;
    }

  g_clear_object(&self->service);
  g_clear_object(&self->model);
  self->nav_view = NULL;
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_nip29_groups_panel_parent_class)->dispose(object);
}

static void
gn_nip29_groups_panel_class_init(GnNip29GroupsPanelClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_nip29_groups_panel_dispose;
}

static void
gn_nip29_groups_panel_init(GnNip29GroupsPanel *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
}

/* ── Action callbacks ────────────────────────────────────────────── */

static void
on_add_group_clicked(GtkButton *button, gpointer user_data)
{
  GnNip29GroupsPanel *self = GN_NIP29_GROUPS_PANEL(user_data);
  if (self->service == NULL)
    return;

  GnNip29AddGroupDialog *dialog = gn_nip29_add_group_dialog_new(self->service);
  GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
  adw_dialog_present(ADW_DIALOG(dialog), toplevel);
}

static void
on_create_group_clicked(GtkButton *button, gpointer user_data)
{
  GnNip29GroupsPanel *self = GN_NIP29_GROUPS_PANEL(user_data);
  if (self->service == NULL)
    return;

  GnNip29AddGroupDialog *dialog = gn_nip29_add_group_dialog_new_create(self->service);
  GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
  adw_dialog_present(ADW_DIALOG(dialog), toplevel);
}

static void
on_refresh_clicked(GtkButton *button, gpointer user_data)
{
  GnNip29GroupsPanel *self = GN_NIP29_GROUPS_PANEL(user_data);
  if (self->service != NULL)
    gn_nip29_group_service_refresh_all(self->service);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnNip29GroupsPanel *
gn_nip29_groups_panel_new(GnNip29GroupService *service,
                           AdwNavigationView   *nav_view,
                           GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(service), NULL);

  GnNip29GroupsPanel *self = g_object_new(GN_TYPE_NIP29_GROUPS_PANEL, NULL);
  self->service = g_object_ref(service);
  self->nav_view = nav_view;
  self->plugin_context = plugin_context;

  /* ── Error info bar ────────────────────────────────────────────── */
  self->error_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(self->error_bar, 12);
  gtk_widget_set_margin_end(self->error_bar, 12);
  gtk_widget_set_margin_top(self->error_bar, 4);
  gtk_widget_set_visible(self->error_bar, FALSE);

  GtkWidget *error_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_widget_add_css_class(error_icon, "error");
  gtk_box_append(GTK_BOX(self->error_bar), error_icon);

  self->error_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->error_label, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->error_label), "error");
  gtk_widget_set_hexpand(GTK_WIDGET(self->error_label), TRUE);
  gtk_label_set_xalign(self->error_label, 0);
  gtk_box_append(GTK_BOX(self->error_bar), GTK_WIDGET(self->error_label));

  GtkWidget *dismiss_btn = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(dismiss_btn, "flat");
  gtk_widget_add_css_class(dismiss_btn, "circular");
  g_signal_connect(dismiss_btn, "clicked", G_CALLBACK(on_error_dismiss), self);
  gtk_box_append(GTK_BOX(self->error_bar), dismiss_btn);

  gtk_box_append(GTK_BOX(self), self->error_bar);

  /* ── Header with title and action buttons ──────────────────────── */
  GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(header_box, 12);
  gtk_widget_set_margin_end(header_box, 8);
  gtk_widget_set_margin_top(header_box, 8);
  gtk_widget_set_margin_bottom(header_box, 4);

  GtkWidget *title = gtk_label_new("Groups");
  gtk_widget_add_css_class(title, "title-3");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header_box), title);

  GtkWidget *refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_add_css_class(refresh_btn, "flat");
  gtk_widget_add_css_class(refresh_btn, "circular");
  gtk_widget_set_tooltip_text(refresh_btn, "Refresh all groups");
  g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), self);
  gtk_box_append(GTK_BOX(header_box), refresh_btn);

  GtkWidget *create_btn = gtk_button_new_from_icon_name("folder-new-symbolic");
  gtk_widget_add_css_class(create_btn, "flat");
  gtk_widget_add_css_class(create_btn, "circular");
  gtk_widget_set_tooltip_text(create_btn, "Create a group");
  g_signal_connect(create_btn, "clicked", G_CALLBACK(on_create_group_clicked), self);
  gtk_box_append(GTK_BOX(header_box), create_btn);

  GtkWidget *add_btn = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_add_css_class(add_btn, "flat");
  gtk_widget_add_css_class(add_btn, "circular");
  gtk_widget_set_tooltip_text(add_btn, "Track a group");
  g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_group_clicked), self);
  gtk_box_append(GTK_BOX(header_box), add_btn);

  gtk_box_append(GTK_BOX(self), header_box);
  gtk_box_append(GTK_BOX(self), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* ── Model ─────────────────────────────────────────────────────── */
  self->model = gn_nip29_group_list_model_new(service);

  /* ── Stack: empty page / list view ─────────────────────────────── */
  self->stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(self->stack), TRUE);

  /* Empty page */
  self->empty_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(self->empty_page, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(self->empty_page, GTK_ALIGN_CENTER);

  GtkWidget *empty_icon = gtk_image_new_from_icon_name("system-users-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 64);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_page), empty_icon);

  GtkWidget *empty_title = gtk_label_new("No Groups Tracked");
  gtk_widget_add_css_class(empty_title, "title-2");
  gtk_box_append(GTK_BOX(self->empty_page), empty_title);

  GtkWidget *empty_desc = gtk_label_new(
    "Track a NIP-29 group by providing its relay URL and group ID.");
  gtk_widget_add_css_class(empty_desc, "dim-label");
  gtk_label_set_wrap(GTK_LABEL(empty_desc), TRUE);
  gtk_label_set_justify(GTK_LABEL(empty_desc), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(self->empty_page), empty_desc);

  GtkWidget *add_empty_btn = gtk_button_new_with_label("Track Group");
  gtk_widget_add_css_class(add_empty_btn, "suggested-action");
  gtk_widget_add_css_class(add_empty_btn, "pill");
  gtk_widget_set_halign(add_empty_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(add_empty_btn, 12);
  g_signal_connect(add_empty_btn, "clicked",
                   G_CALLBACK(on_add_group_clicked), self);
  gtk_box_append(GTK_BOX(self->empty_page), add_empty_btn);

  gtk_stack_add_named(self->stack, self->empty_page, "empty");

  /* List view */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), NULL);

  GtkSingleSelection *selection =
    gtk_single_selection_new(G_LIST_MODEL(g_object_ref(self->model)));
  gtk_single_selection_set_autoselect(selection, FALSE);
  gtk_single_selection_set_can_unselect(selection, TRUE);

  self->list_view = GTK_LIST_VIEW(
    gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory));
  gtk_list_view_set_single_click_activate(self->list_view, TRUE);
  g_signal_connect(self->list_view, "activate",
                   G_CALLBACK(on_group_activated), self);

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(self->list_view));
  gtk_widget_set_vexpand(scroll, TRUE);

  gtk_stack_add_named(self->stack, scroll, "list");

  /* Set initial visible child */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->model));
  if (n == 0)
    gtk_stack_set_visible_child(self->stack, self->empty_page);
  else
    gtk_stack_set_visible_child(self->stack, GTK_WIDGET(scroll));

  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->stack));

  /* ── Signal connections ────────────────────────────────────────── */
  self->sig_items_changed = g_signal_connect(self->model, "items-changed",
                                             G_CALLBACK(on_items_changed), self);
  self->sig_error = g_signal_connect(service, "error-reported",
                                     G_CALLBACK(on_error_reported), self);

  return self;
}
