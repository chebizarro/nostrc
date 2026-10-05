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

/* Supported languages with native names */
typedef struct {
  const gchar *code;
  const gchar *name;
  gboolean rtl;
} LanguageInfo;

static const LanguageInfo supported_languages[] = {
  { "en",    "English",      FALSE },
  { "ja",    "日本語",        FALSE },
  { "es",    "Español",      FALSE },
  { "pt_BR", "Português (Brasil)", FALSE },
  { "id",    "Bahasa Indonesia", FALSE },
  { "fa",    "فارسی",         TRUE  },
  { NULL,    NULL,           FALSE }
};

static gchar *current_language = NULL;

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

  /* Check for saved language preference */
  SettingsManager *sm = settings_manager_get_default();
  if (sm) {
    gchar *saved_lang = settings_manager_get_language(sm);
    if (saved_lang && *saved_lang) {
      gn_i18n_set_language(saved_lang);
      g_free(saved_lang);
    }
  }

  /* Apply RTL text direction if needed */
  gn_i18n_apply_text_direction();

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

void
gn_i18n_set_language(const gchar *lang)
{
  g_free(current_language);
  current_language = lang ? g_strdup(lang) : NULL;

  if (lang) {
    /* Set environment variables for gettext */
    g_setenv("LANGUAGE", lang, TRUE);
    g_setenv("LC_ALL", lang, TRUE);
    g_setenv("LC_MESSAGES", lang, TRUE);
    g_setenv("LANG", lang, TRUE);

    /* Re-bind text domain to pick up new language */
    setlocale(LC_ALL, "");

    /* Save preference */
    SettingsManager *sm = settings_manager_get_default();
    if (sm) {
      settings_manager_set_language(sm, lang);
    }

    /* Apply RTL text direction if needed */
    gn_i18n_apply_text_direction();

    g_debug("i18n: language set to %s", lang);
  } else {
    /* Reset to system default */
    g_unsetenv("LANGUAGE");
    setlocale(LC_ALL, "");

    SettingsManager *sm = settings_manager_get_default();
    if (sm) {
      settings_manager_set_language(sm, "");
    }

    /* Apply text direction based on system setting */
    gn_i18n_apply_text_direction();

    g_debug("i18n: language reset to system default");
  }
}

gchar **
gn_i18n_get_available_languages(void)
{
  guint count = G_N_ELEMENTS(supported_languages) - 1; /* -1 for NULL terminator */
  gchar **result = g_new0(gchar *, count + 1);

  for (guint i = 0; i < count; i++) {
    result[i] = g_strdup(supported_languages[i].code);
  }
  result[count] = NULL;

  return result;
}

const gchar *
gn_i18n_get_language_name(const gchar *code)
{
  if (!code)
    return "System Default";

  for (guint i = 0; supported_languages[i].code; i++) {
    if (g_str_equal(supported_languages[i].code, code)) {
      return supported_languages[i].name;
    }
  }

  return code; /* Return code if name not found */
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
