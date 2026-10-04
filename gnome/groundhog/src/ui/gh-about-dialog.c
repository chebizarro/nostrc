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
  const char *developers[] = { "Biz", NULL };
  const char *owner_npub = "npub1ehhfg09mr8z34wz85ek46a6rww4f7c7jsujxhdvmpqnl5hnrwsqq2szjqv";
  AdwDialog *dialog = adw_about_dialog_new();
  AdwAboutDialog *about = ADW_ABOUT_DIALOG(dialog);
  adw_about_dialog_set_application_name(about, "Groundhog");
  adw_about_dialog_set_application_icon(about, GROUNDHOG_APP_ID);
  adw_about_dialog_set_version(about, GROUNDHOG_VERSION);
  adw_about_dialog_set_developer_name(about, "Biz");
  adw_about_dialog_set_developers(about, developers);
  g_autofree gchar *nostr_uri = g_strconcat("nostr:", owner_npub, NULL);
  adw_about_dialog_add_link(about, owner_npub, nostr_uri);
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

  /* Adwaita icon theme attribution (nostrc-9juh). */
  adw_about_dialog_add_legal_section(about,
      "Adwaita Icon Theme",
      /* TRANSLATORS: short attribution shown in About > Legal. */
      _("Symbolic icons from the GNOME Project's Adwaita icon theme."),
      GTK_LICENSE_LGPL_3_0,
      NULL);

  return dialog;
}

void
gh_about_dialog_present(GtkWidget *parent)
{
  gh_about_dialog_register_icons();
  adw_dialog_present(gh_about_dialog_new(), parent);
}
