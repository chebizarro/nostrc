#include <gtk/gtk.h>

static void
test_bundled_resource_icons_resolve(void)
{
  static const char *const icon_names[] = {
    "gnostr-bug-symbolic",
    "gnostr-compose-symbolic",
    "gnostr-notifications-symbolic",
    "gnostr-messages-symbolic",
    "gnostr-calendar-symbolic",
    "gnostr-network-offline-symbolic",
    "gnostr-network-wired-symbolic",
  };

  g_autoptr(GtkIconTheme) theme = gtk_icon_theme_new();
  gtk_icon_theme_add_resource_path(theme, "/org/gnostr/icons");

  for (gsize i = 0; i < G_N_ELEMENTS(icon_names); i++)
    g_assert_true(gtk_icon_theme_has_icon(theme, icon_names[i]));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gnostr/icons/bundled-resources-resolve",
                  test_bundled_resource_icons_resolve);
  return g_test_run();
}
