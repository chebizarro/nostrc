/* LaunchSearch argv / desktop-action construction (nsp-launch.h). */
#include <gio/gio.h>
#include "nsp-launch.h"

static void assert_argv(char **got, const char *const *want) {
  g_assert_nonnull(got);
  g_assert_cmpuint(g_strv_length(got), ==, g_strv_length((char **)want));
  for (guint i = 0; want[i]; i++) g_assert_cmpstr(got[i], ==, want[i]);
}

static GKeyFile *kf_from(const char *data) {
  GKeyFile *kf = g_key_file_new();
  g_assert_true(g_key_file_load_from_data(kf, data, -1, G_KEY_FILE_NONE, NULL));
  return kf;
}

/* gnostr's shipped entry (apps/gnostr/data/org.gnostr.gnostr.desktop). */
#define GNOSTR_DESKTOP                                                     \
  "[Desktop Entry]\nType=Application\nName=GNostr\nExec=gnostr %U\n"      \
  "X-Nostr-Kinds=0;1;*;\nX-Nostr-Search-Arg=--search\nActions=search;\n"  \
  "\n[Desktop Action search]\nName=Search Nostr\nExec=gnostr --search \"\"\n"

static void test_entry_gnostr(void) {
  g_autoptr(GKeyFile) kf = kf_from(GNOSTR_DESKTOP);
  g_autofree char *arg = NULL, *exec = NULL;
  gboolean action = FALSE;
  g_assert_true(nsp_search_entry_from_keyfile(kf, &arg, &exec, &action));
  g_assert_cmpstr(arg, ==, "--search");
  g_assert_cmpstr(exec, ==, "gnostr --search \"\"");
  g_assert_true(action);

  const char *terms[] = {"hello", "world", NULL};
  g_auto(GStrv) argv = nsp_search_argv(exec, arg, terms, NULL);
  assert_argv(argv, (const char *[]){"gnostr", "--search=hello world", NULL});

  /* Empty terms keep the action's "open search" meaning. */
  const char *none[] = {NULL};
  g_auto(GStrv) empty = nsp_search_argv(exec, arg, none, NULL);
  assert_argv(empty, (const char *[]){"gnostr", "--search=", NULL});
}

static void test_entry_without_action_uses_main_exec(void) {
  g_autoptr(GKeyFile) kf = kf_from("[Desktop Entry]\nType=Application\nName=X\n"
                                   "Exec=/opt/x/bin/x --new-window %u\n"
                                   "X-Nostr-Search-Arg=--find\n");
  g_autofree char *arg = NULL, *exec = NULL;
  gboolean action = TRUE;
  g_assert_true(nsp_search_entry_from_keyfile(kf, &arg, &exec, &action));
  g_assert_false(action);
  const char *terms[] = {"zaps", NULL};
  g_auto(GStrv) argv = nsp_search_argv(exec, arg, terms, NULL);
  assert_argv(argv, (const char *[]){"/opt/x/bin/x", "--new-window", "--find=zaps", NULL});
}

static void test_entry_without_search_arg(void) {
  g_autoptr(GKeyFile) kf = kf_from("[Desktop Entry]\nType=Application\nName=Y\n"
                                   "Exec=y %U\nActions=search;\n"
                                   "[Desktop Action search]\nExec=y --search \"\"\n");
  g_autofree char *arg = NULL, *exec = NULL;
  g_assert_false(nsp_search_entry_from_keyfile(kf, &arg, &exec, NULL));
  g_assert_null(arg);
  g_assert_null(exec);

  /* A value that is not a plain option is not a search entry point. */
  g_autoptr(GKeyFile) bad = kf_from("[Desktop Entry]\nType=Application\nName=Z\n"
                                    "Exec=z\nX-Nostr-Search-Arg=; rm -rf ~\n");
  g_assert_false(nsp_search_entry_from_keyfile(bad, &arg, &exec, NULL));
}

static void test_flatpak_export(void) {
  const char *exec = "/usr/bin/flatpak run --branch=stable --arch=aarch64 "
                     "--command=gnostr --file-forwarding org.gnostr.gnostr @@u %U @@";
  const char *terms[] = {"#nostr", NULL};
  g_auto(GStrv) argv = nsp_search_argv(exec, "--search", terms, NULL);
  assert_argv(argv, (const char *[]){"/usr/bin/flatpak", "run", "--branch=stable",
                                     "--arch=aarch64", "--command=gnostr",
                                     "--file-forwarding", "org.gnostr.gnostr",
                                     "--search=#nostr", NULL});
  /* Flatpak's exported action line (existing option replaced in place). */
  const char *action = "/usr/bin/flatpak run --command=gnostr org.gnostr.gnostr "
                       "--search \"\"";
  g_auto(GStrv) a = nsp_search_argv(action, "--search", terms, NULL);
  assert_argv(a, (const char *[]){"/usr/bin/flatpak", "run", "--command=gnostr",
                                  "org.gnostr.gnostr", "--search=#nostr", NULL});
}

static void test_terms_are_one_argument(void) {
  /* Shell metacharacters and a leading '-' stay inert data. */
  const char *terms[] = {"-rf", "$(id)", "a;b", "", "\"q\"", NULL};
  g_auto(GStrv) argv = nsp_search_argv("app --search=old", "--search", terms, NULL);
  assert_argv(argv, (const char *[]){"app", "--search=-rf $(id) a;b \"q\"", NULL});

  /* Short options take the terms as the next argument. */
  const char *t2[] = {"x", NULL};
  g_auto(GStrv) s = nsp_search_argv("app -s %U", "-s", t2, NULL);
  assert_argv(s, (const char *[]){"app", "-s", "x", NULL});

  /* %% is a literal percent. */
  g_auto(GStrv) pct = nsp_search_argv("app --rate=50%%", "--search", t2, NULL);
  assert_argv(pct, (const char *[]){"app", "--rate=50%", "--search=x", NULL});
}

static void test_refusals(void) {
  const char *ok[] = {"x", NULL};
  g_autoptr(GError) err = NULL;
  g_assert_null(nsp_search_argv("app --x=%u", "--search", ok, &err)); /* embedded code */
  g_clear_error(&err);
  g_assert_null(nsp_search_argv("app 'unterminated", "--search", ok, &err));
  g_clear_error(&err);
  g_assert_null(nsp_search_argv("%U", "--search", ok, &err)); /* nothing to run */
  g_clear_error(&err);
  g_assert_null(nsp_search_argv("app", "search", ok, &err)); /* not an option */
  g_clear_error(&err);
  const char *ctl[] = {"a\nb", NULL};
  g_assert_null(nsp_search_argv("app", "--search", ctl, &err));
  g_clear_error(&err);
  g_autofree char *big = g_strnfill(NSP_SEARCH_TERMS_MAX + 1, 'a');
  const char *huge[] = {big, NULL};
  g_assert_null(nsp_search_argv("app", "--search", huge, &err));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nsp/launch/gnostr", test_entry_gnostr);
  g_test_add_func("/nsp/launch/main-exec", test_entry_without_action_uses_main_exec);
  g_test_add_func("/nsp/launch/no-search-arg", test_entry_without_search_arg);
  g_test_add_func("/nsp/launch/flatpak", test_flatpak_export);
  g_test_add_func("/nsp/launch/terms-one-arg", test_terms_are_one_argument);
  g_test_add_func("/nsp/launch/refusals", test_refusals);
  return g_test_run();
}
