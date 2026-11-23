/* i18n.c - Internationalization support implementation
 *
 * SPDX-License-Identifier: MIT
 */
#include "i18n.h"
#include "settings_manager.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <string.h>

/* Supported languages and text directions. */
typedef struct {
  const gchar *code;
  gboolean rtl;
} LanguageInfo;

static const LanguageInfo supported_languages[] = {
  { "en", FALSE },
  { "ja", FALSE },
  { "es", FALSE },
  { "pt_BR", FALSE },
  { "id", FALSE },
  { "fa", TRUE  },
  { NULL, FALSE }
};

static gchar *current_language = NULL;

/* nostrc-z5d2: g_setenv/setlocale are not thread-safe against concurrent
 * getenv/gettext in GTask or D-Bus worker threads. They are only allowed
 * while gn_i18n_init() runs (single-threaded); after that, a language
 * change saves the preference and takes effect after a restart. */
static gboolean env_mutation_safe = TRUE;

void
gn_i18n_init(void)
{
  const gchar *localedir;

  /* Set locale from environment */
  setlocale(LC_ALL, "");

  /* Determine locale directory */
#ifdef LOCALEDIR
  localedir = LOCALEDIR;
#else
  /* Development fallback - look relative to binary */
  localedir = g_build_filename(g_get_user_data_dir(), "locale", NULL);
  if (!g_file_test(localedir, G_FILE_TEST_IS_DIR)) {
    /* Try system location */
    localedir = "/usr/share/locale";
  }
#endif

  /* Initialize gettext */
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  /* Follow the system locale; the retired in-app language key is ignored. */

  /* Apply RTL text direction if needed */
  gn_i18n_apply_text_direction();

  /* From here on, worker threads may exist: freeze the environment */
  env_mutation_safe = FALSE;

  g_debug("i18n: initialized with locale directory: %s", localedir);
}

const gchar *
gn_i18n_get_language(void)
{
  if (current_language)
    return current_language;

  /* Return system language */
  const gchar *lang = g_getenv("LANG");
  if (lang) {
    /* Extract language code from LANG (e.g., "ja_JP.UTF-8" -> "ja") */
    static gchar lang_code[16];
    g_strlcpy(lang_code, lang, sizeof(lang_code));
    gchar *dot = strchr(lang_code, '.');
    if (dot) *dot = '\0';
    return lang_code;
  }

  return "en";
}

gboolean
gn_i18n_is_rtl(const gchar *code)
{
  if (!code)
    return FALSE;

  for (guint i = 0; supported_languages[i].code; i++) {
    if (g_str_equal(supported_languages[i].code, code)) {
      return supported_languages[i].rtl;
    }
  }

  return FALSE;
}

gboolean
gn_i18n_is_current_rtl(void)
{
  return gn_i18n_is_rtl(gn_i18n_get_language());
}

void
gn_i18n_apply_text_direction(void)
{
  gboolean rtl = gn_i18n_is_current_rtl();
  GtkTextDirection direction = rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR;

  /* Set default text direction for all widgets */
  gtk_widget_set_default_direction(direction);

  g_debug("i18n: text direction set to %s", rtl ? "RTL" : "LTR");
}
