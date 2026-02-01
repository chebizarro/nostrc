/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-repo-browser.c - NIP-34 Repository Browser View
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#define G_LOG_DOMAIN "gnostr-repo-browser"

#include "gnostr-repo-browser.h"
#include <adwaita.h>

/* Repository data stored in list model */
typedef struct {
  gchar *id;
  gchar *name;
  gchar *description;
  gchar *clone_url;
  gchar *web_url;
  gchar *maintainer_pubkey;
  gint64 updated_at;
} RepoData;

static void
repo_data_free(RepoData *data)
{
  if (!data) return;
  g_free(data->id);
  g_free(data->name);
  g_free(data->description);
  g_free(data->clone_url);
  g_free(data->web_url);
  g_free(data->maintainer_pubkey);
  g_free(data);
}

struct _GnostrRepoBrowser
{
  GtkWidget parent_instance;

  /* Main layout */
  GtkWidget *main_box;
  GtkWidget *header_box;
  GtkWidget *search_entry;
  GtkWidget *refresh_button;
  GtkWidget *stack;

  /* Views */
  GtkWidget *loading_view;
  GtkWidget *empty_view;
  GtkWidget *list_view;
  GtkWidget *scrolled_window;
  GtkListBox *repo_list;

  /* Data */
  GHashTable *repositories;  /* id -> RepoData */
  gchar *filter_text;
  gchar *selected_id;

  /* State */
  gboolean is_loading;
};

G_DEFINE_TYPE(GnostrRepoBrowser, gnostr_repo_browser, GTK_TYPE_WIDGET)

enum {
  SIGNAL_REPO_SELECTED,
  SIGNAL_CLONE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void rebuild_list(GnostrRepoBrowser *self);
static gboolean repo_matches_filter(GnostrRepoBrowser *self, RepoData *data);

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  g_free(self->filter_text);
  self->filter_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
  rebuild_list(self);
}

static void
on_row_activated(GtkListBox *list_box G_GNUC_UNUSED,
                 GtkListBoxRow *row,
                 gpointer user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  const char *id = g_object_get_data(G_OBJECT(row), "repo-id");

  if (id)
    {
      g_free(self->selected_id);
      self->selected_id = g_strdup(id);
      g_signal_emit(self, signals[SIGNAL_REPO_SELECTED], 0, id);
    }
}

static void
on_clone_clicked(GtkButton *button, gpointer user_data)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(user_data);
  const char *url = g_object_get_data(G_OBJECT(button), "clone-url");

  if (url)
    g_signal_emit(self, signals[SIGNAL_CLONE_REQUESTED], 0, url);
}

static GtkWidget *
create_repo_row(GnostrRepoBrowser *self, RepoData *data)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(row, 12);
  gtk_widget_set_margin_end(row, 12);
  gtk_widget_set_margin_top(row, 10);
  gtk_widget_set_margin_bottom(row, 10);

  /* Header: name + clone button */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *name_label = gtk_label_new(data->name ? data->name : data->id);
  gtk_widget_add_css_class(name_label, "heading");
  gtk_widget_set_halign(name_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(name_label, TRUE);
  gtk_box_append(GTK_BOX(header), name_label);

  if (data->clone_url)
    {
      GtkWidget *clone_btn = gtk_button_new_from_icon_name("folder-download-symbolic");
      gtk_widget_set_tooltip_text(clone_btn, "Clone repository");
      gtk_widget_add_css_class(clone_btn, "flat");
      g_object_set_data_full(G_OBJECT(clone_btn), "clone-url",
                             g_strdup(data->clone_url), g_free);
      g_signal_connect(clone_btn, "clicked", G_CALLBACK(on_clone_clicked), self);
      gtk_box_append(GTK_BOX(header), clone_btn);
    }

  if (data->web_url)
    {
      GtkWidget *web_btn = gtk_button_new_from_icon_name("web-browser-symbolic");
      gtk_widget_set_tooltip_text(web_btn, "Open in browser");
      gtk_widget_add_css_class(web_btn, "flat");
      gtk_box_append(GTK_BOX(header), web_btn);
    }

  gtk_box_append(GTK_BOX(row), header);

  /* Description */
  if (data->description && *data->description)
    {
      GtkWidget *desc = gtk_label_new(data->description);
      gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(desc), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_max_width_chars(GTK_LABEL(desc), 60);
      gtk_label_set_xalign(GTK_LABEL(desc), 0);
      gtk_widget_add_css_class(desc, "dim-label");
      gtk_box_append(GTK_BOX(row), desc);
    }

  /* Clone URL */
  if (data->clone_url)
    {
      GtkWidget *url_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

      GtkWidget *icon = gtk_image_new_from_icon_name("terminal-symbolic");
      gtk_widget_add_css_class(icon, "dim-label");
      gtk_box_append(GTK_BOX(url_box), icon);

      GtkWidget *url_label = gtk_label_new(data->clone_url);
      gtk_label_set_selectable(GTK_LABEL(url_label), TRUE);
      gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_add_css_class(url_label, "monospace");
      gtk_widget_add_css_class(url_label, "dim-label");
      gtk_widget_set_halign(url_label, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(url_box), url_label);

      gtk_box_append(GTK_BOX(row), url_box);
    }

  /* Store ID on the row widget for selection handling */
  GtkWidget *list_row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
  g_object_set_data_full(G_OBJECT(list_row), "repo-id",
                         g_strdup(data->id), g_free);

  return list_row;
}

static gboolean
repo_matches_filter(GnostrRepoBrowser *self, RepoData *data)
{
  if (!self->filter_text || *self->filter_text == '\0')
    return TRUE;

  gchar *filter_lower = g_utf8_strdown(self->filter_text, -1);
  gboolean matches = FALSE;

  if (data->name)
    {
      gchar *name_lower = g_utf8_strdown(data->name, -1);
      if (g_strstr_len(name_lower, -1, filter_lower))
        matches = TRUE;
      g_free(name_lower);
    }

  if (!matches && data->description)
    {
      gchar *desc_lower = g_utf8_strdown(data->description, -1);
      if (g_strstr_len(desc_lower, -1, filter_lower))
        matches = TRUE;
      g_free(desc_lower);
    }

  if (!matches && data->id)
    {
      gchar *id_lower = g_utf8_strdown(data->id, -1);
      if (g_strstr_len(id_lower, -1, filter_lower))
        matches = TRUE;
      g_free(id_lower);
    }

  g_free(filter_lower);
  return matches;
}

static void
rebuild_list(GnostrRepoBrowser *self)
{
  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->repo_list))) != NULL)
    gtk_list_box_remove(self->repo_list, child);

  /* Add matching repositories */
  GHashTableIter iter;
  gpointer key, value;
  guint visible_count = 0;

  g_hash_table_iter_init(&iter, self->repositories);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      RepoData *data = (RepoData *)value;
      if (repo_matches_filter(self, data))
        {
          GtkWidget *row = create_repo_row(self, data);
          gtk_list_box_append(self->repo_list, row);
          visible_count++;
        }
    }

  /* Update stack visibility */
  if (self->is_loading)
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->loading_view);
  else if (visible_count == 0)
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->empty_view);
  else
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->list_view);
}

static void
gnostr_repo_browser_dispose(GObject *object)
{
  GnostrRepoBrowser *self = GNOSTR_REPO_BROWSER(object);

  g_clear_pointer(&self->repositories, g_hash_table_unref);
  g_clear_pointer(&self->filter_text, g_free);
  g_clear_pointer(&self->selected_id, g_free);

  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(gnostr_repo_browser_parent_class)->dispose(object);
}

static void
gnostr_repo_browser_class_init(GnostrRepoBrowserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_repo_browser_dispose;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "repo-browser");

  signals[SIGNAL_REPO_SELECTED] =
    g_signal_new("repo-selected",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_CLONE_REQUESTED] =
    g_signal_new("clone-requested",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_repo_browser_init(GnostrRepoBrowser *self)
{
  self->repositories = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)repo_data_free);
  self->is_loading = FALSE;

  /* Main container */
  self->main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(self->main_box, GTK_WIDGET(self));

  /* Header with search and refresh */
  self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(self->header_box, 12);
  gtk_widget_set_margin_end(self->header_box, 12);
  gtk_widget_set_margin_top(self->header_box, 12);
  gtk_widget_set_margin_bottom(self->header_box, 8);

  self->search_entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->search_entry, TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->search_entry), "Search repositories...");
  g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
  gtk_box_append(GTK_BOX(self->header_box), self->search_entry);

  self->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(self->refresh_button, "Refresh");
  gtk_box_append(GTK_BOX(self->header_box), self->refresh_button);

  gtk_box_append(GTK_BOX(self->main_box), self->header_box);

  /* Stack for different states */
  self->stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_vexpand(self->stack, TRUE);
  gtk_box_append(GTK_BOX(self->main_box), self->stack);

  /* Loading view */
  self->loading_view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(self->loading_view, GTK_ALIGN_CENTER);
  GtkWidget *spinner = gtk_spinner_new();
  gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
  gtk_widget_set_size_request(spinner, 32, 32);
  gtk_box_append(GTK_BOX(self->loading_view), spinner);
  GtkWidget *loading_label = gtk_label_new("Loading repositories...");
  gtk_widget_add_css_class(loading_label, "dim-label");
  gtk_box_append(GTK_BOX(self->loading_view), loading_label);
  gtk_stack_add_named(GTK_STACK(self->stack), self->loading_view, "loading");

  /* Empty view */
  self->empty_view = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty_view), "folder-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(self->empty_view), "No Repositories");
  adw_status_page_set_description(ADW_STATUS_PAGE(self->empty_view),
    "No git repositories found. Repositories are published via kind 30617 events.");
  gtk_stack_add_named(GTK_STACK(self->stack), self->empty_view, "empty");

  /* List view */
  self->list_view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->scrolled_window = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(self->scrolled_window, TRUE);

  self->repo_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->repo_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(GTK_WIDGET(self->repo_list), "boxed-list");
  g_signal_connect(self->repo_list, "row-activated", G_CALLBACK(on_row_activated), self);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled_window),
                                 GTK_WIDGET(self->repo_list));
  gtk_box_append(GTK_BOX(self->list_view), self->scrolled_window);
  gtk_stack_add_named(GTK_STACK(self->stack), self->list_view, "list");

  /* Start with empty view */
  gtk_stack_set_visible_child(GTK_STACK(self->stack), self->empty_view);
}

GnostrRepoBrowser *
gnostr_repo_browser_new(void)
{
  return g_object_new(GNOSTR_TYPE_REPO_BROWSER, NULL);
}

void
gnostr_repo_browser_add_repository(GnostrRepoBrowser *self,
                                    const char        *id,
                                    const char        *name,
                                    const char        *description,
                                    const char        *clone_url,
                                    const char        *web_url,
                                    const char        *maintainer_pubkey,
                                    gint64             updated_at)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_return_if_fail(id != NULL);

  RepoData *data = g_new0(RepoData, 1);
  data->id = g_strdup(id);
  data->name = g_strdup(name);
  data->description = g_strdup(description);
  data->clone_url = g_strdup(clone_url);
  data->web_url = g_strdup(web_url);
  data->maintainer_pubkey = g_strdup(maintainer_pubkey);
  data->updated_at = updated_at;

  g_hash_table_replace(self->repositories, g_strdup(id), data);
  rebuild_list(self);
}

void
gnostr_repo_browser_clear(GnostrRepoBrowser *self)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_hash_table_remove_all(self->repositories);
  rebuild_list(self);
}

void
gnostr_repo_browser_set_loading(GnostrRepoBrowser *self, gboolean loading)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  self->is_loading = loading;
  rebuild_list(self);
}

void
gnostr_repo_browser_set_filter(GnostrRepoBrowser *self, const char *filter_text)
{
  g_return_if_fail(GNOSTR_IS_REPO_BROWSER(self));
  g_free(self->filter_text);
  self->filter_text = g_strdup(filter_text);
  rebuild_list(self);
}

const char *
gnostr_repo_browser_get_selected_id(GnostrRepoBrowser *self)
{
  g_return_val_if_fail(GNOSTR_IS_REPO_BROWSER(self), NULL);
  return self->selected_id;
}

guint
gnostr_repo_browser_get_count(GnostrRepoBrowser *self)
{
  g_return_val_if_fail(GNOSTR_IS_REPO_BROWSER(self), 0);
  return g_hash_table_size(self->repositories);
}
