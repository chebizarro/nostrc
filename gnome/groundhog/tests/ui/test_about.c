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

#define OWNER_URI "nostr:" OWNER_NPUB

typedef struct {
  guint identity_labels;
  guint identity_links;
} IdentityAudit;

static void
assert_no_email(const char *text)
{
  if (!text)
    return;
  g_assert_null(strchr(text, '@'));
  g_assert_false(g_str_has_prefix(text, "mailto:"));
}

static void
audit_dialog(GtkWidget *root, IdentityAudit *audit)
{
  assert_no_email(gtk_widget_get_tooltip_text(root));
  if (GTK_IS_LABEL(root)) {
    const char *text = gtk_label_get_text(GTK_LABEL(root));
    assert_no_email(text);
    assert_no_email(gtk_label_get_label(GTK_LABEL(root)));
    if (strstr(text, OWNER_NPUB))
      audit->identity_labels++;
  }
  if (GTK_IS_BUTTON(root))
    assert_no_email(gtk_button_get_label(GTK_BUTTON(root)));
  if (GTK_IS_EDITABLE(root))
    assert_no_email(gtk_editable_get_text(GTK_EDITABLE(root)));
  if (ADW_IS_PREFERENCES_ROW(root))
    assert_no_email(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(root)));
  if (ADW_IS_ACTION_ROW(root)) {
    assert_no_email(adw_action_row_get_subtitle(ADW_ACTION_ROW(root)));
    gboolean identity = g_strcmp0(
      adw_preferences_row_get_title(ADW_PREFERENCES_ROW(root)), OWNER_NPUB) == 0;
    gboolean has_destination = FALSE;

    /* libadwaita 1.5 stores add_link() destinations as GtkActionable
     * targets; newer versions use AdwLinkRow's URI property. Read the
     * rendered row's actual target rather than the source string. */
    if (GTK_IS_ACTIONABLE(root)) {
      GVariant *target = gtk_actionable_get_action_target_value(GTK_ACTIONABLE(root));
      if (target && g_variant_is_of_type(target, G_VARIANT_TYPE_STRING)) {
        const char *uri = g_variant_get_string(target, NULL);
        assert_no_email(uri);
        if (identity) {
          g_assert_cmpstr(uri, ==, OWNER_URI);
          has_destination = TRUE;
        }
      }
    }
    GParamSpec *uri_property = g_object_class_find_property(G_OBJECT_GET_CLASS(root), "uri");
    if (uri_property && G_PARAM_SPEC_VALUE_TYPE(uri_property) == G_TYPE_STRING) {
      g_autofree char *uri = NULL;
      g_object_get(root, "uri", &uri, NULL);
      assert_no_email(uri);
      if (identity) {
        g_assert_cmpstr(uri, ==, OWNER_URI);
        has_destination = TRUE;
      }
    }
    if (identity) {
      g_assert_true(has_destination);
      audit->identity_links++;
    }
  }
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child))
    audit_dialog(child, audit);
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
  assert_no_email(adw_about_dialog_get_developer_name(about));
  const char *const *developers = adw_about_dialog_get_developers(about);
  g_assert_nonnull(developers);
  g_assert_cmpstr(developers[0], ==, "Biz");
  assert_no_email(developers[0]);
  g_assert_null(developers[1]);
  g_assert_true(g_str_has_prefix(adw_about_dialog_get_website(about), "https://"));
  g_assert_true(g_str_has_prefix(adw_about_dialog_get_issue_url(about), "https://"));
  g_assert_nonnull(strstr(adw_about_dialog_get_comments(about), "Nostr"));
  assert_no_email(adw_about_dialog_get_comments(about));
  assert_no_email(adw_about_dialog_get_website(about));
  assert_no_email(adw_about_dialog_get_issue_url(about));

  GtkWidget *window = gtk_window_new();
  gtk_window_present(GTK_WINDOW(window));
  adw_dialog_present(dialog, window);
  for (int i = 0; i < 50; i++)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(dialog)));
  IdentityAudit audit = { 0 };
  audit_dialog(GTK_WIDGET(dialog), &audit);
  g_assert_cmpuint(audit.identity_labels, >, 0);
  g_assert_cmpuint(audit.identity_links, ==, 1);
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
