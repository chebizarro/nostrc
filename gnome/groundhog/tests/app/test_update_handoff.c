#include "gh-update-handoff.h"
#include <glib.h>

static void test_channels(void)
{
  g_assert_cmpstr(gh_update_handoff_package_channel(), ==, "source");
  const gchar *channels[] = { "flatpak", "deb", "rpm", "arch", "nix", "source", "unknown" };
  for (guint i = 0; i < G_N_ELEMENTS(channels); i++) {
    const gchar *text = gh_update_handoff_instructions(channels[i]);
    g_assert_nonnull(text);
    g_assert_cmpuint(strlen(text), >, 20);
    g_assert_null(strstr(text, "http"));
    g_assert_null(strstr(text, "up to date"));
  }
  g_assert_cmpstr(gh_update_handoff_instructions("unknown"), ==,
                  gh_update_handoff_instructions("source"));
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/update-handoff/channels", test_channels);
  return g_test_run();
}
