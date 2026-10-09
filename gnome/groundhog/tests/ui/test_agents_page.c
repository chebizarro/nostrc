#include "gh-agents-page.h"

static void
test_prompts(void)
{
  static const gchar *names[] = {
    "Hermes", "OpenClaw", "OpenCode", "Codex", "Claude Code", "Pi"
  };
  const gchar *npub = "npub1testaccountpublicidentifier";
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    g_autofree gchar *prompt = gh_agents_page_dup_prompt(names[i], npub);
    g_assert_nonnull(prompt);
    g_assert_nonnull(g_strstr_len(prompt, -1, names[i]));
    g_assert_nonnull(g_strstr_len(prompt, -1, npub));
    g_assert_null(g_strstr_len(prompt, -1, "{npub}"));
    g_assert_nonnull(g_strstr_len(prompt, -1, "approval"));
  }
  g_assert_null(gh_agents_page_dup_prompt("unknown", npub));
  g_assert_null(gh_agents_page_dup_prompt("Hermes", NULL));
}

extern void groundhog_register_resource(void);

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  groundhog_register_resource();
  g_test_add_func("/groundhog/agents/prompts", test_prompts);
  return g_test_run();
}
