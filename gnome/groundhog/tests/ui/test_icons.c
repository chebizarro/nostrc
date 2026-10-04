/* Every literal icon referenced by the current Blueprint/C sources must be
 * bundled or explicitly supplied by GTK itself. Runs without a display. */
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#ifndef GROUNDHOG_SOURCE_DIR
#error "GROUNDHOG_SOURCE_DIR must point to the Groundhog source tree"
#endif

void groundhog_register_resource(void);

/* GTK 4's internal symbolic icons; unlike Adwaita theme assets these are
 * present even when no desktop icon theme is installed. */
static const char *const GTK_BUILTINS[] = {
  "edit-copy-symbolic", "edit-delete-symbolic", "face-smile-symbolic",
  "list-add-symbolic", "list-remove-symbolic", "object-select-symbolic",
  "user-trash-symbolic", "window-close-symbolic", NULL
};

static void
collect_file(const gchar *path, GRegex *pattern, GHashTable *names)
{
  g_autofree gchar *source = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_file_get_contents(path, &source, NULL, &error));
  g_assert_no_error(error);
  GMatchInfo *matches = NULL;
  g_regex_match(pattern, source, 0, &matches);
  while (g_match_info_matches(matches)) {
    g_autofree gchar *name = g_match_info_fetch(matches, 1);
    g_hash_table_add(names, g_steal_pointer(&name));
    g_match_info_next(matches, &error);
    g_assert_no_error(error);
  }
  g_match_info_free(matches);
}

static void
collect_dir(const gchar *path, const gchar *suffix, GRegex *pattern,
            GHashTable *names)
{
  g_autoptr(GError) error = NULL;
  GDir *dir = g_dir_open(path, 0, &error);
  g_assert_no_error(error);
  g_assert_nonnull(dir);
  const gchar *entry;
  while ((entry = g_dir_read_name(dir))) {
    g_autofree gchar *child = g_build_filename(path, entry, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR))
      collect_dir(child, suffix, pattern, names);
    else if (g_str_has_suffix(entry, suffix))
      collect_file(child, pattern, names);
  }
  g_dir_close(dir);
}

static GHashTable *
referenced_icons(void)
{
  GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GError) error = NULL;
  /* Blueprint property values, including non-symbolic hicolor app icons. */
  g_autoptr(GRegex) blueprint = g_regex_new("icon-name\\s*:\\s*\"([^\"]+)\"", 0, 0, &error);
  g_assert_no_error(error);
  g_autofree gchar *ui = g_build_filename(GROUNDHOG_SOURCE_DIR, "data", "ui", NULL);
  collect_dir(ui, ".blp", blueprint, names);
  /* C API icon names are string literals; catch all symbolic literals,
   * including values passed through local variables before GTK calls. */
  g_autoptr(GRegex) c_names = g_regex_new("\"([A-Za-z0-9-]+-symbolic(?:-rtl)?)\"",
                                         0, 0, &error);
  g_assert_no_error(error);
  g_autofree gchar *src = g_build_filename(GROUNDHOG_SOURCE_DIR, "src", "ui", NULL);
  collect_dir(src, ".c", c_names, names);
  return names;
}

static gchar *
resource_path(const gchar *name)
{
  if (g_str_equal(name, "org.nostr.Groundhog"))
    return g_strdup("/org/nostr/Groundhog/icons/512x512/apps/org.nostr.Groundhog.png");
  return g_strdup_printf("/org/nostr/Groundhog/icons/scalable/actions/%s.svg", name);
}

static void
test_icon_resources(void)
{
  g_autoptr(GHashTable) names = referenced_icons();
  g_assert_cmpuint(g_hash_table_size(names), >, 35);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, names);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const gchar *name = key;
    if (g_strv_contains(GTK_BUILTINS, name))
      continue;
    g_autofree gchar *path = resource_path(name);
    g_autoptr(GError) error = NULL;
    gboolean found = g_resources_get_info(path, G_RESOURCE_LOOKUP_FLAGS_NONE,
                                          NULL, NULL, &error);
    if (!found)
      g_test_message("unresolved icon %s (missing resource %s)", name, path);
    g_assert_true(found);
  }
}

static void
test_icon_svg_valid(void)
{
  g_autoptr(GHashTable) names = referenced_icons();
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, names);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const gchar *name = key;
    if (g_strv_contains(GTK_BUILTINS, name) || g_str_equal(name, "org.nostr.Groundhog"))
      continue;
    g_autofree gchar *path = resource_path(name);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes =
      g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    g_assert_no_error(error);
    g_assert_nonnull(bytes);
    gsize len = 0;
    const char *data = g_bytes_get_data(bytes, &len);
    g_assert_cmpuint(len, >, 20);
    gboolean ok = g_str_has_prefix(data, "<?xml") ||
                  g_str_has_prefix(data, "<svg") ||
                  g_str_has_prefix(data, "\xef\xbb\xbf<?xml");
    if (!ok)
      g_test_message("bad SVG header for %s: %.40s", name, data);
    g_assert_true(ok);
  }
}

int
main(int argc, char **argv)
{
  groundhog_register_resource();
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/icons/resources", test_icon_resources);
  g_test_add_func("/groundhog/icons/svg-valid", test_icon_svg_valid);
  return g_test_run();
}
