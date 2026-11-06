/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"
#include <glib/gi18n-lib.h>
#include <string.h>

typedef struct {
  char *name, *executable;
  GIcon *icon;
} DesktopLabel;
typedef struct {
  GMutex mutex;
  GHashTable *ids, *executables;
  GAppInfoMonitor *monitor;
} DesktopCatalog;

static void desktop_label_free(gpointer data) {
  DesktopLabel *label = data;
  g_free(label->name); g_free(label->executable); g_clear_object(&label->icon); g_free(label);
}
static gint compare_apps(gconstpointer a, gconstpointer b) {
  return g_strcmp0(g_app_info_get_id((GAppInfo *)a), g_app_info_get_id((GAppInfo *)b));
}
static void catalog_changed(GAppInfoMonitor *monitor, gpointer data) {
  (void)monitor; DesktopCatalog *catalog = data;
  GList *apps = g_list_sort(g_app_info_get_all(), compare_apps);
  g_mutex_lock(&catalog->mutex);
  g_hash_table_remove_all(catalog->executables);
  g_hash_table_remove_all(catalog->ids);
  for (GList *l = apps; l; l = l->next) {
    GAppInfo *app = l->data;
    const char *id = g_app_info_get_id(app), *command = g_app_info_get_executable(app);
    if (!id || g_hash_table_contains(catalog->ids, id)) continue;
    DesktopLabel *label = g_new0(DesktopLabel, 1);
    label->name = g_strdup(g_app_info_get_display_name(app));
    /* Bare Exec commands resolve through PATH, not the caller's basename. */
    label->executable = command ? (g_path_is_absolute(command) ? g_strdup(command) : g_find_program_in_path(command)) : NULL;
    GIcon *icon = g_app_info_get_icon(app);
    if (icon) label->icon = g_object_ref(icon);
    g_hash_table_insert(catalog->ids, g_strdup(id), label);
    if (label->executable && !g_hash_table_contains(catalog->executables, label->executable))
      g_hash_table_insert(catalog->executables, g_strdup(label->executable), label);
  }
  g_mutex_unlock(&catalog->mutex);
  g_list_free_full(apps, g_object_unref);
}
static gpointer catalog_new(gpointer unused) {
  (void)unused;
  /* Process-local desktop metadata only: no principals, grants or secrets are
   * retained. The monitor and indexed catalog share the process lifetime. */
  DesktopCatalog *catalog = g_new0(DesktopCatalog, 1);
  g_mutex_init(&catalog->mutex);
  catalog->ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, desktop_label_free);
  catalog->executables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  catalog->monitor = g_app_info_monitor_get();
  g_signal_connect(catalog->monitor, "changed", G_CALLBACK(catalog_changed), catalog);
  catalog_changed(catalog->monitor, catalog);
  return catalog;
}

static DesktopCatalog *catalog_get(void) {
  static GOnce once = G_ONCE_INIT;
  return g_once(&once, catalog_new, NULL);
}
void grotto_panels_app_info_prepare(void) { (void)catalog_get(); }

/* Names and icons are cosmetic desktop metadata, not proof of identity. */
char *grotto_panels_app_display_info(const char *principal, const char *label, GIcon **icon) {
  const char *p = principal ? principal : "";
  g_autofree char *id = NULL, *exe = NULL;
  if (g_str_has_prefix(p, "app:") || g_str_has_prefix(p, "flatpak:") || g_str_has_prefix(p, "snap:")) {
    const char *start = strchr(p, ':') + 1;
    id = g_strndup(start, strcspn(start, ";"));
  }
  const char *start = g_str_has_prefix(p, "exe:") ? p + 4 : strstr(p, ";exe:");
  if (start) {
    if (*start == ';') start += 5;
    exe = g_strndup(start, strcspn(start, ";"));
  }
  g_autofree char *base = exe && *exe ? g_path_get_basename(exe) : NULL;
  g_autofree char *desktop_id = id && !g_str_has_suffix(id, ".desktop") ? g_strconcat(id, ".desktop", NULL) : g_strdup(id);
  gboolean custom = label && *label && strcmp(label, p);
  DesktopCatalog *catalog = NULL;
  DesktopLabel *match = NULL;
  if ((id || exe) && (!custom || icon)) {
    catalog = catalog_get();
    g_mutex_lock(&catalog->mutex);
    match = desktop_id ? g_hash_table_lookup(catalog->ids, desktop_id) : NULL;
    /* A process-chosen app ID cannot override a contradictory executable. */
    if (match && exe && g_strcmp0(match->executable, exe)) match = NULL;
    if (!match && exe) match = g_hash_table_lookup(catalog->executables, exe);
  }
  const char *name = custom ? label : match ? match->name : base ? base : id ? id : *p ? p : _("App");
  char *result = g_strdup(name);
  if (icon) *icon = match && match->icon ? g_object_ref(match->icon) : g_themed_icon_new("application-x-executable-symbolic");
  if (catalog) g_mutex_unlock(&catalog->mutex);
  return result;
}
