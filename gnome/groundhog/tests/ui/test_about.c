/* The About dialog and the bundled app icon.
 *
 * Default mode (no display): the resource bundle carries the app icon at the
 * hicolor layout the icon theme reads, and it decodes to the expected size.
 * --gui: the dialog shows the app icon, the build's version, the MIT licence
 * and the project links; exits 77 without a display. */
#include <adwaita.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdlib.h>
#include <string.h>

#include "gh-about-dialog.h"
#include "nostrc-test-gdk-frame.h"

void groundhog_register_resource(void);

static void
test_icon_resource(void)
{
  const char *sizes[] = { "512x512", "256x256" };
  for (guint i = 0; i < G_N_ELEMENTS(sizes); i++) {
    g_autofree gchar *path = g_strdup_printf(GH_ICON_RESOURCE_PATH "/%s/apps/org.nostr.Groundhog.png",
                                             sizes[i]);
    g_autoptr(GError) error = NULL;
    g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_resource(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(pixbuf);
    int want = atoi(sizes[i]);
    g_assert_cmpint(gdk_pixbuf_get_width(pixbuf), ==, want);
    g_assert_cmpint(gdk_pixbuf_get_height(pixbuf), ==, want);
    g_assert_true(gdk_pixbuf_get_has_alpha(pixbuf));
  }
  /* The placeholder is gone: nothing else claims the app id. */
  g_assert_false(g_resources_get_info(GH_ICON_RESOURCE_PATH "/org.nostr.Groundhog.svg",
                                      G_RESOURCE_LOOKUP_FLAGS_NONE, NULL, NULL, NULL));
}

#define OWNER_NPUB "npub1ehhfg09mr8z34wz85ek46a6rww4f7c7jsujxhdvmpqnl5hnrwsqq2szjqv"

static gboolean
has_identity_label(GtkWidget *root)
{
  if (GTK_IS_LABEL(root) && strstr(gtk_label_get_text(GTK_LABEL(root)), OWNER_NPUB))
    return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    if (has_identity_label(child))
      return TRUE;
  }
  return FALSE;
}

static void
test_gui_dialog(void)
{
  gh_about_dialog_register_icons();
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
  g_assert_true(gtk_icon_theme_has_icon(theme, "org.nostr.Groundhog"));

  AdwDialog *dialog = gh_about_dialog_new();
  AdwAboutDialog *about = ADW_ABOUT_DIALOG(dialog);
  g_assert_cmpstr(adw_about_dialog_get_application_name(about), ==, "Groundhog");
  g_assert_cmpstr(adw_about_dialog_get_application_icon(about), ==, "org.nostr.Groundhog");
  g_assert_cmpstr(adw_about_dialog_get_version(about), ==, GROUNDHOG_VERSION);
  g_assert_cmpint(adw_about_dialog_get_license_type(about), ==, GTK_LICENSE_MIT_X11);
  g_assert_cmpstr(adw_about_dialog_get_developer_name(about), ==, "Biz");
  const char *const *developers = adw_about_dialog_get_developers(about);
  g_assert_nonnull(developers);
  g_assert_cmpstr(developers[0], ==, "Biz");
  g_assert_null(developers[1]);
  g_assert_true(g_str_has_prefix(adw_about_dialog_get_website(about), "https://"));
  g_assert_true(g_str_has_prefix(adw_about_dialog_get_issue_url(about), "https://"));
  g_assert_nonnull(strstr(adw_about_dialog_get_comments(about), "Nostr"));

  GtkWidget *window = gtk_window_new();
  gtk_window_present(GTK_WINDOW(window));
  adw_dialog_present(dialog, window);
  for (int i = 0; i < 50; i++)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(dialog)));
  g_assert_true(has_identity_label(GTK_WIDGET(dialog)));
  adw_dialog_force_close(dialog);
  gtk_window_destroy(GTK_WINDOW(window));
  for (int i = 0; i < 50; i++)
    g_main_context_iteration(NULL, FALSE);
}

int
main(int argc, char **argv)
{
  gboolean gui = argc > 1 && g_strcmp0(argv[1], "--gui") == 0;
  if (gui) {
    argc--; argv++;
  }
  groundhog_register_resource();
  if (gui) {
    if (!gtk_init_check()) {
      g_print("SKIP: no display\n");
      return 77;
    }
    adw_init();
  }
  g_test_init(&argc, &argv, NULL);
  if (gui)
    nostrc_test_tolerate_gdk_frame_warning();
  if (gui)
    g_test_add_func("/groundhog/about/gui-dialog", test_gui_dialog);
  else
    g_test_add_func("/groundhog/about/icon-resource", test_icon_resource);
  return g_test_run();
}
