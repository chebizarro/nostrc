/* gh-i18n.h — process locale and gettext domain setup (nostrc-gofet.10). */
#pragma once

#include <glib.h>
#include <glib/gi18n.h>
#include <locale.h>

#define GH_GETTEXT_DOMAIN "groundhog"

/* Selects the user's locale and binds the "groundhog" domain to localedir
 * with UTF-8 output. main() calls this before any settings, tray, CLI option
 * or UI string is created, because _() and GtkBuilder resolve translations
 * through the default domain set here. Returns FALSE when the C library
 * rejected the locale from the environment (the "C" locale stays active and
 * strings stay untranslated). */
static inline gboolean
gh_i18n_init(const char *localedir)
{
  gboolean locale_ok = setlocale(LC_ALL, "") != NULL;
  bindtextdomain(GH_GETTEXT_DOMAIN, localedir);
  bind_textdomain_codeset(GH_GETTEXT_DOMAIN, "UTF-8");
  textdomain(GH_GETTEXT_DOMAIN);
  return locale_ok;
}
