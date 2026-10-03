#include "gh-about-dialog.h"

#include <glib/gi18n.h>

#ifndef GROUNDHOG_VERSION
#define GROUNDHOG_VERSION "unknown"
#endif
#ifndef GROUNDHOG_APP_ID
#define GROUNDHOG_APP_ID "org.nostr.Groundhog"
#endif

void
gh_about_dialog_register_icons(void)
{
  GdkDisplay *display = gdk_display_get_default();
  if (!display)
    return;
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
  g_auto(GStrv) paths = gtk_icon_theme_get_resource_path(theme);
  if (!paths || !g_strv_contains((const gchar *const *)paths, GH_ICON_RESOURCE_PATH))
    gtk_icon_theme_add_resource_path(theme, GH_ICON_RESOURCE_PATH);
}

AdwDialog *
gh_about_dialog_new(void)
{
  const char *developers[] = { "The nostrc contributors", NULL };
  AdwDialog *dialog = adw_about_dialog_new();
  AdwAboutDialog *about = ADW_ABOUT_DIALOG(dialog);
  adw_about_dialog_set_application_name(about, "Groundhog");
  adw_about_dialog_set_application_icon(about, GROUNDHOG_APP_ID);
  adw_about_dialog_set_version(about, GROUNDHOG_VERSION);
  adw_about_dialog_set_developer_name(about, _("The nostrc project"));
  adw_about_dialog_set_developers(about, developers);
  adw_about_dialog_set_license_type(about, GTK_LICENSE_MIT_X11);
  adw_about_dialog_set_website(about, "https://github.com/chebizarro/nostrc");
  adw_about_dialog_set_issue_url(about, "https://github.com/chebizarro/nostrc/issues");
  /* The metainfo summary, then what the app promises (charter §2.2). */
  adw_about_dialog_set_comments(about,
      _("A calm home for Nostr conversations.\n\n"
        "Private messages and encrypted groups that work with other Nostr apps. "
        "Your private key stays in Nostr Signer, and nothing loads from the web "
        "unless you ask."));
  /* Translators: put your name here, one per line, if you translated Groundhog. */
  adw_about_dialog_set_translator_credits(about, _("translator-credits"));
  return dialog;
}

void
gh_about_dialog_present(GtkWidget *parent)
{
  gh_about_dialog_register_icons();
  adw_dialog_present(gh_about_dialog_new(), parent);
}
