/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-manager-panel.c - Plugin manager settings panel
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-plugin-manager-panel.h"
#include "gnostr-plugin-row.h"
#include "../util/gnostr-plugin-manager.h"
#include <adwaita.h>

#ifdef HAVE_LIBPEAS
#include <libpeas.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-plugin-manager-panel.ui"

struct _GnostrPluginManagerPanel
{
  GtkWidget parent_instance;

  /* Template widgets */
  GtkSearchEntry *search_entry;
  GtkStack       *content_stack;
  GtkListBox     *plugin_list;
  GtkSpinner     *spinner;
  GtkButton      *btn_refresh;
  GtkButton      *btn_install_local;
  GtkButton      *btn_install_first;
  GtkButton      *btn_open_folder;
  GtkLabel       *lbl_stats;

  /* State */
  gchar *search_text;
  guint  search_timeout_id;
  guint  plugin_count;
  guint  enabled_count;
};

G_DEFINE_TYPE(GnostrPluginManagerPanel, gnostr_plugin_manager_panel, GTK_TYPE_WIDGET)

enum
{
  SIGNAL_PLUGIN_SETTINGS,
  SIGNAL_PLUGIN_INFO,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_search_changed(GtkSearchEntry *entry, GnostrPluginManagerPanel *self);
static void on_refresh_clicked(GtkButton *button, GnostrPluginManagerPanel *self);
static void on_install_clicked(GtkButton *button, GnostrPluginManagerPanel *self);
static void on_open_folder_clicked(GtkButton *button, GnostrPluginManagerPanel *self);
static void populate_plugin_list(GnostrPluginManagerPanel *self);
static void on_plugin_toggled(GnostrPluginRow *row, gboolean enabled, GnostrPluginManagerPanel *self);
static void on_plugin_settings(GnostrPluginRow *row, GnostrPluginManagerPanel *self);
static void on_plugin_info(GnostrPluginRow *row, GnostrPluginManagerPanel *self);
static void update_stats_label(GnostrPluginManagerPanel *self);
static gboolean filter_plugin_row(GtkListBoxRow *row, gpointer user_data);

static void
gnostr_plugin_manager_panel_dispose(GObject *object)
{
  GnostrPluginManagerPanel *self = GNOSTR_PLUGIN_MANAGER_PANEL(object);

  if (self->search_timeout_id > 0) {
    g_source_remove(self->search_timeout_id);
    self->search_timeout_id = 0;
  }

  g_clear_pointer(&self->search_text, g_free);

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_PLUGIN_MANAGER_PANEL);

  G_OBJECT_CLASS(gnostr_plugin_manager_panel_parent_class)->dispose(object);
}

static void
gnostr_plugin_manager_panel_class_init(GnostrPluginManagerPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_plugin_manager_panel_dispose;

  /**
   * GnostrPluginManagerPanel::plugin-settings:
   * @self: The panel
   * @plugin_id: The plugin module name
   *
   * Emitted when plugin settings should be shown.
   */
  signals[SIGNAL_PLUGIN_SETTINGS] =
    g_signal_new("plugin-settings",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  /**
   * GnostrPluginManagerPanel::plugin-info:
   * @self: The panel
   * @plugin_id: The plugin module name
   *
   * Emitted when plugin info should be shown.
   */
  signals[SIGNAL_PLUGIN_INFO] =
    g_signal_new("plugin-info",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);

  /* Load UI template */
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, search_entry);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, content_stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, plugin_list);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, spinner);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, btn_refresh);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, btn_install_local);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, btn_install_first);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, btn_open_folder);
  gtk_widget_class_bind_template_child(widget_class, GnostrPluginManagerPanel, lbl_stats);

  /* Set CSS name */
  gtk_widget_class_set_css_name(widget_class, "plugin-manager-panel");

  /* Layout manager */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
}

static void
gnostr_plugin_manager_panel_init(GnostrPluginManagerPanel *self)
{
  self->search_text = NULL;
  self->search_timeout_id = 0;
  self->plugin_count = 0;
  self->enabled_count = 0;

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signals */
  g_signal_connect(self->search_entry, "search-changed",
                   G_CALLBACK(on_search_changed), self);
  g_signal_connect(self->btn_refresh, "clicked",
                   G_CALLBACK(on_refresh_clicked), self);
  if (self->btn_install_local) {
    g_signal_connect(self->btn_install_local, "clicked",
                     G_CALLBACK(on_install_clicked), self);
  }
  if (self->btn_install_first) {
    g_signal_connect(self->btn_install_first, "clicked",
                     G_CALLBACK(on_install_clicked), self);
  }
  if (self->btn_open_folder) {
    g_signal_connect(self->btn_open_folder, "clicked",
                     G_CALLBACK(on_open_folder_clicked), self);
  }

  /* Set up filter function */
  gtk_list_box_set_filter_func(self->plugin_list, filter_plugin_row, self, NULL);

  /* Initial load */
  populate_plugin_list(self);
}

static gboolean
filter_plugin_row(GtkListBoxRow *row, gpointer user_data)
{
  GnostrPluginManagerPanel *self = GNOSTR_PLUGIN_MANAGER_PANEL(user_data);

  if (!self->search_text || !*self->search_text)
    return TRUE;

  /* Get the plugin row */
  GtkWidget *child = gtk_list_box_row_get_child(row);
  if (!GNOSTR_IS_PLUGIN_ROW(child))
    return TRUE;

  GnostrPluginRow *plugin_row = GNOSTR_PLUGIN_ROW(child);

#ifdef HAVE_LIBPEAS
  PeasPluginInfo *info = gnostr_plugin_row_get_plugin_info(plugin_row);
  if (!info)
    return FALSE;

  const char *name = peas_plugin_info_get_name(info);
  const char *desc = peas_plugin_info_get_description(info);

  gchar *search_lower = g_utf8_strdown(self->search_text, -1);
  gboolean match = FALSE;

  if (name) {
    gchar *name_lower = g_utf8_strdown(name, -1);
    match = strstr(name_lower, search_lower) != NULL;
    g_free(name_lower);
  }

  if (!match && desc) {
    gchar *desc_lower = g_utf8_strdown(desc, -1);
    match = strstr(desc_lower, search_lower) != NULL;
    g_free(desc_lower);
  }

  g_free(search_lower);
  return match;
#else
  return TRUE;
#endif
}

static gboolean
search_timeout_cb(gpointer user_data)
{
  GnostrPluginManagerPanel *self = GNOSTR_PLUGIN_MANAGER_PANEL(user_data);

  self->search_timeout_id = 0;

  /* Update filter */
  gtk_list_box_invalidate_filter(self->plugin_list);

  /* Check if we have any visible results */
  gboolean has_visible = FALSE;
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->plugin_list));
  while (child) {
    if (GTK_IS_LIST_BOX_ROW(child) && gtk_widget_get_visible(child)) {
      has_visible = TRUE;
      break;
    }
    child = gtk_widget_get_next_sibling(child);
  }

  /* Update stack visibility */
  if (self->plugin_count == 0) {
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
  } else if (!has_visible && self->search_text && *self->search_text) {
    gtk_stack_set_visible_child_name(self->content_stack, "no-results");
  } else {
    gtk_stack_set_visible_child_name(self->content_stack, "list");
  }

  return G_SOURCE_REMOVE;
}

static void
on_search_changed(GtkSearchEntry *entry, GnostrPluginManagerPanel *self)
{
  g_free(self->search_text);
  self->search_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));

  /* Debounce search */
  if (self->search_timeout_id > 0) {
    g_source_remove(self->search_timeout_id);
  }
  self->search_timeout_id = g_timeout_add(150, search_timeout_cb, self);
}

static void
on_refresh_clicked(GtkButton *button, GnostrPluginManagerPanel *self)
{
  (void)button;
  gnostr_plugin_manager_panel_refresh(self);
}

static void
on_install_clicked(GtkButton *button, GnostrPluginManagerPanel *self)
{
  (void)button;

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  if (!GTK_IS_WINDOW(root))
    return;
  GtkWindow *toplevel = GTK_WINDOW(root);

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Install Plugin");
  gtk_file_dialog_set_modal(dialog, TRUE);

  /* Filter for plugin files */
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Plugin Files");
  gtk_file_filter_add_pattern(filter, "*.plugin");
  gtk_file_filter_add_pattern(filter, "*.so");
  gtk_file_filter_add_pattern(filter, "*.dylib");

  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));

  gtk_file_dialog_open(dialog, GTK_WINDOW(toplevel), NULL, NULL, NULL);

  g_object_unref(filter);
  g_object_unref(filters);
  g_object_unref(dialog);
}

static void
on_open_folder_clicked(GtkButton *button, GnostrPluginManagerPanel *self)
{
  (void)button;
  (void)self;

  /* Open the user plugins directory */
  g_autofree char *plugins_dir = g_build_filename(g_get_user_data_dir(),
                                                   "gnostr", "plugins", NULL);

  /* Create directory if it doesn't exist */
  g_mkdir_with_parents(plugins_dir, 0755);

  /* Open with system file manager */
  g_autofree char *uri = g_filename_to_uri(plugins_dir, NULL, NULL);
  if (uri) {
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
  }
}

static void
on_plugin_toggled(GnostrPluginRow *row, gboolean enabled, GnostrPluginManagerPanel *self)
{
#ifdef HAVE_LIBPEAS
  PeasPluginInfo *info = gnostr_plugin_row_get_plugin_info(row);
  if (!info)
    return;

  const char *module_name = peas_plugin_info_get_module_name(info);
  GnostrPluginManager *manager = gnostr_plugin_manager_get_default();

  GError *error = NULL;
  if (enabled) {
    if (!gnostr_plugin_manager_enable_plugin(manager, module_name, &error)) {
      g_warning("Failed to enable plugin %s: %s",
                module_name, error ? error->message : "Unknown error");
      g_clear_error(&error);
      /* Revert checkbox */
      gnostr_plugin_row_set_enabled(row, FALSE);
      gnostr_plugin_row_set_state(row, GNOSTR_PLUGIN_STATE_ERROR);
      return;
    }
    self->enabled_count++;
    gnostr_plugin_row_set_state(row, GNOSTR_PLUGIN_STATE_ACTIVE);
  } else {
    gnostr_plugin_manager_disable_plugin(manager, module_name);
    if (self->enabled_count > 0)
      self->enabled_count--;
    gnostr_plugin_row_set_state(row, GNOSTR_PLUGIN_STATE_UNLOADED);
  }

  update_stats_label(self);
#else
  (void)row;
  (void)enabled;
  (void)self;
#endif
}

static void
on_plugin_settings(GnostrPluginRow *row, GnostrPluginManagerPanel *self)
{
#ifdef HAVE_LIBPEAS
  PeasPluginInfo *info = gnostr_plugin_row_get_plugin_info(row);
  if (!info)
    return;

  const char *module_name = peas_plugin_info_get_module_name(info);
  g_signal_emit(self, signals[SIGNAL_PLUGIN_SETTINGS], 0, module_name);
#else
  (void)row;
  (void)self;
#endif
}

static void
on_plugin_info(GnostrPluginRow *row, GnostrPluginManagerPanel *self)
{
#ifdef HAVE_LIBPEAS
  PeasPluginInfo *info = gnostr_plugin_row_get_plugin_info(row);
  if (!info)
    return;

  const char *module_name = peas_plugin_info_get_module_name(info);
  g_signal_emit(self, signals[SIGNAL_PLUGIN_INFO], 0, module_name);
#else
  (void)row;
  (void)self;
#endif
}

static void
update_stats_label(GnostrPluginManagerPanel *self)
{
  g_autofree char *stats = NULL;

  if (self->plugin_count == 0) {
    stats = g_strdup("No plugins installed");
  } else if (self->plugin_count == 1) {
    stats = g_strdup_printf("1 plugin (%u enabled)", self->enabled_count);
  } else {
    stats = g_strdup_printf("%u plugins (%u enabled)",
                            self->plugin_count, self->enabled_count);
  }

  gtk_label_set_text(self->lbl_stats, stats);
}

static void
populate_plugin_list(GnostrPluginManagerPanel *self)
{
  /* Show loading state */
  gtk_stack_set_visible_child_name(self->content_stack, "loading");

  /* Clear existing rows */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->plugin_list));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(self->plugin_list, child);
    child = next;
  }

  self->plugin_count = 0;
  self->enabled_count = 0;

#ifdef HAVE_LIBPEAS
  GnostrPluginManager *manager = gnostr_plugin_manager_get_default();
  PeasEngine *engine = peas_engine_get_default();
  (void)manager; /* May be used for settings checks later */

  /* PeasEngine is a GListModel in libpeas 2 */
  guint n_plugins = g_list_model_get_n_items(G_LIST_MODEL(engine));

  for (guint i = 0; i < n_plugins; i++) {
    PeasPluginInfo *info = g_list_model_get_item(G_LIST_MODEL(engine), i);
    if (!info)
      continue;

    /* Skip hidden plugins */
    if (peas_plugin_info_is_hidden(info)) {
      g_object_unref(info);
      continue;
    }

    /* Create row */
    GtkWidget *row = gnostr_plugin_row_new(info);
    GnostrPluginRow *plugin_row = GNOSTR_PLUGIN_ROW(row);

    /* Set initial state */
    gboolean loaded = peas_plugin_info_is_loaded(info);
    gnostr_plugin_row_set_enabled(plugin_row, loaded);

    if (loaded) {
      gnostr_plugin_row_set_state(plugin_row, GNOSTR_PLUGIN_STATE_ACTIVE);
      self->enabled_count++;
    } else if (!peas_plugin_info_is_available(info, NULL)) {
      gnostr_plugin_row_set_state(plugin_row, GNOSTR_PLUGIN_STATE_ERROR);
    }

    /* Check if plugin has settings */
    /* TODO: Check for GnostrConfigurable interface */
    gnostr_plugin_row_set_has_settings(plugin_row, FALSE);

    /* Connect signals */
    g_signal_connect(plugin_row, "toggled",
                     G_CALLBACK(on_plugin_toggled), self);
    g_signal_connect(plugin_row, "settings-clicked",
                     G_CALLBACK(on_plugin_settings), self);
    g_signal_connect(plugin_row, "info-clicked",
                     G_CALLBACK(on_plugin_info), self);

    /* Add to list */
    gtk_list_box_append(self->plugin_list, row);
    self->plugin_count++;

    g_object_unref(info);
  }
#endif

  /* Update display */
  update_stats_label(self);

  if (self->plugin_count == 0) {
    gtk_stack_set_visible_child_name(self->content_stack, "empty");
  } else {
    gtk_stack_set_visible_child_name(self->content_stack, "list");
  }
}

/* Public API */

GtkWidget *
gnostr_plugin_manager_panel_new(void)
{
  return g_object_new(GNOSTR_TYPE_PLUGIN_MANAGER_PANEL, NULL);
}

void
gnostr_plugin_manager_panel_refresh(GnostrPluginManagerPanel *self)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER_PANEL(self));

#ifdef HAVE_LIBPEAS
  /* Rescan for plugins */
  GnostrPluginManager *manager = gnostr_plugin_manager_get_default();
  gnostr_plugin_manager_discover_plugins(manager);
#endif

  /* Repopulate list */
  populate_plugin_list(self);
}

void
gnostr_plugin_manager_panel_filter(GnostrPluginManagerPanel *self,
                                    const char               *search_text)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER_PANEL(self));

  gtk_editable_set_text(GTK_EDITABLE(self->search_entry),
                        search_text ? search_text : "");
}

void
gnostr_plugin_manager_panel_show_plugin_settings(GnostrPluginManagerPanel *self,
                                                  const char               *plugin_id)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER_PANEL(self));
  g_return_if_fail(plugin_id != NULL);

  /* TODO: Show plugin settings dialog */
  g_debug("Show settings for plugin: %s", plugin_id);
}

void
gnostr_plugin_manager_panel_show_plugin_info(GnostrPluginManagerPanel *self,
                                              const char               *plugin_id)
{
  g_return_if_fail(GNOSTR_IS_PLUGIN_MANAGER_PANEL(self));
  g_return_if_fail(plugin_id != NULL);

#ifdef HAVE_LIBPEAS
  PeasEngine *engine = peas_engine_get_default();
  PeasPluginInfo *info = peas_engine_get_plugin_info(engine, plugin_id);

  if (!info) {
    g_warning("Plugin not found: %s", plugin_id);
    return;
  }

  /* Get plugin details */
  const char *name = peas_plugin_info_get_name(info);
  const char *desc = peas_plugin_info_get_description(info);
  const char *version = peas_plugin_info_get_version(info);
  const char *website = peas_plugin_info_get_website(info);
  const char *copyright = peas_plugin_info_get_copyright(info);
  const char *const *authors = peas_plugin_info_get_authors(info);

  /* Build authors string */
  g_autofree char *authors_str = NULL;
  if (authors && authors[0]) {
    authors_str = g_strjoinv("\n", (char **)authors);
  }

  /* Create and show about dialog */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  AdwAboutDialog *dialog = ADW_ABOUT_DIALOG(adw_about_dialog_new());

  adw_about_dialog_set_application_name(dialog, name ? name : plugin_id);
  adw_about_dialog_set_version(dialog, version ? version : "");
  adw_about_dialog_set_comments(dialog, desc ? desc : "");
  adw_about_dialog_set_website(dialog, website ? website : "");
  adw_about_dialog_set_copyright(dialog, copyright ? copyright : "");
  if (authors && authors[0]) {
    adw_about_dialog_set_developers(dialog, (const char **)authors);
  }

  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(root));
#else
  (void)self;
  (void)plugin_id;
#endif
}
