/* test_i18n.c — the generated en@pseudo catalog loads at runtime through
 * gh_i18n_init(), and the merged desktop entry and metainfo carry it
 * (nostrc-gofet.10). Skips (77) when the C library offers no non-C locale:
 * GNU gettext ignores LANGUAGE under C, and glibc treats C.UTF-8 as C. */
#include "gh-i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean
select_locale(void)
{
  static const char *const candidates[] = { "en_US.UTF-8", "en_US.utf8", "en_GB.UTF-8" };
  for (gsize i = 0; i < G_N_ELEMENTS(candidates); i++) {
    g_setenv("LC_ALL", candidates[i], TRUE);
    if (gh_i18n_init(GH_TEST_LOCALEDIR))
      return TRUE;
  }
  return FALSE;
}

static void
test_runtime_translation(void)
{
  /* A string from main.c option descriptions (N_), resolved by _(). */
  g_assert_cmpstr(_("Check the Groundhog GUI and exit"), ==,
                  "⟦Çĥéçķ ţĥé Ĝŕöûñđĥöĝ ĜÛÎ àñđ éẋîţ⟧");
  /* Plural forms come from the catalog header, not English rules. */
  g_assert_cmpstr(g_dngettext(NULL, "%u file attached", "%u files attached", 1), ==,
                  "⟦%u ƒîļé àţţàçĥéđ⟧");
  g_assert_cmpstr(g_dngettext(NULL, "%u file attached", "%u files attached", 3), ==,
                  "⟦%u ƒîļéš àţţàçĥéđ⟧");
  /* An unknown string falls back unchanged. */
  g_assert_cmpstr(_("not-a-groundhog-msgid"), ==, "not-a-groundhog-msgid");
  g_assert_cmpstr(textdomain(NULL), ==, GH_GETTEXT_DOMAIN);
}

static void
test_merged_metadata(void)
{
  g_autofree gchar *desktop = NULL;
  g_autofree gchar *metainfo = NULL;
  g_assert_true(g_file_get_contents(GH_TEST_DESKTOP, &desktop, NULL, NULL));
  g_assert_nonnull(strstr(desktop, "Comment[en@pseudo]=⟦"));
  g_assert_nonnull(strstr(desktop, "Exec=groundhog %U"));
  g_assert_true(g_file_get_contents(GH_TEST_METAINFO, &metainfo, NULL, NULL));
  /* gettext >= 0.22 writes the BCP 47 tag (en-pseudo), older the POSIX name. */
  g_assert_true(g_regex_match_simple("xml:lang=\"en[-@]pseudo\">⟦", metainfo, 0, 0));
}

int
main(int argc, char **argv)
{
  g_setenv("LANGUAGE", "en@pseudo", TRUE);
  g_unsetenv("LC_MESSAGES");
  g_unsetenv("LANG");
  if (!select_locale()) {
    fprintf(stderr, "no en_US/en_GB UTF-8 locale available; skipping\n");
    return 77;
  }
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/i18n/runtime-translation", test_runtime_translation);
  g_test_add_func("/groundhog/i18n/merged-metadata", test_merged_metadata);
  return g_test_run();
}
