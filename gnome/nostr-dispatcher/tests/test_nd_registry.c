/* handlers.list precedence + X-Nostr-Kinds resolution (pure registry). */
#include "nd-registry.h"

#include <glib/gstdio.h>
#include <string.h>

static char *tmpdir;

static char *write_list(const char *name, const char *contents) {
  char *p = g_build_filename(tmpdir, name, NULL);
  g_assert_true(g_file_set_contents(p, contents, -1, NULL));
  return p;
}

/* Installed apps: id + X-Nostr-Kinds (NULL = key absent). */
static GPtrArray *apps(const char *const *pairs) {
  GPtrArray *a = g_ptr_array_new_with_free_func(nd_app_decl_free);
  for (int i = 0; pairs[i]; i += 2) g_ptr_array_add(a, nd_app_decl_new(pairs[i], pairs[i + 1]));
  return a;
}

static char *choose(NdRegistry *r, gint kind, NdSource *src) {
  return nd_registry_choose(r, kind, src);
}

#define ASSERT_CHOICE(reg, kind, want_id, want_src) do {           \
    NdSource _s = ND_SOURCE_NONE;                                  \
    g_autofree char *_id = choose(reg, kind, &_s);                 \
    g_assert_cmpstr(_id, ==, want_id);                             \
    g_assert_cmpint(_s, ==, want_src);                             \
  } while (0)

static void test_declarations(void) {
  const char *pairs[] = {
      "org.gnostr.gnostr.desktop", "0;1;6;7;30023;*;",
      "org.example.Longform.desktop", "30023;",
      "org.example.Addressable.desktop", "30000-39999;",
      "org.example.Wide.desktop", "0-65535;",
      "a.example.Tie.desktop", "31000-31009;",
      "b.example.Tie.desktop", "31000-31009;",
      "org.example.NoKinds.desktop", NULL,
      ND_SELF_DESKTOP_ID, "*;",
      NULL};
  g_autoptr(NdRegistry) r = nd_registry_new(NULL, apps(pairs));

  /* Exact declarations tie on width 1: desktop-id order decides. */
  ASSERT_CHOICE(r, 30023, "org.example.Longform.desktop", ND_SOURCE_DECLARED);
  ASSERT_CHOICE(r, 1, "org.gnostr.gnostr.desktop", ND_SOURCE_DECLARED);
  /* Narrowest range wins. */
  ASSERT_CHOICE(r, 30311, "org.example.Addressable.desktop", ND_SOURCE_DECLARED);
  ASSERT_CHOICE(r, 31005, "a.example.Tie.desktop", ND_SOURCE_DECLARED);
  ASSERT_CHOICE(r, 9, "org.example.Wide.desktop", ND_SOURCE_DECLARED);
  /* Unknown kind: only `*` declarers; the dispatcher itself never. */
  ASSERT_CHOICE(r, -1, "org.gnostr.gnostr.desktop", ND_SOURCE_FALLBACK_DECLARED);
}

static void test_handlers_list_precedence(void) {
  g_autofree char *user = write_list("user.list",
      "# user overrides\n"
      "[Default Handlers]\n"
      "1-10=org.example.UserRange.desktop;\n"
      "14=org.example.NotInstalled.desktop;org.example.Groundhog.desktop;\n"
      "20=../../bin/evil;org.example.Evil desktop;\n"
      "garbage key=org.example.Groundhog.desktop\n"
      "[Dispatcher]\n"
      "fetch-relay-hints=false\n");
  g_autofree char *etc = write_list("etc.list",
      "[Default Handlers]\n"
      "1=org.example.SystemExact.desktop\n"
      "40=org.example.RangeB.desktop\n"
      "30-50=org.example.RangeA.desktop\n"
      "35-55=org.example.RangeC.desktop\n"
      "*=org.example.NotInstalled.desktop;org.example.SysFallback.desktop\n"
      "[Dispatcher]\n"
      "fetch-relay-hints=true\n");
  g_autofree char *usr = write_list("usr.list",
      "[Default Handlers]\n"
      "65=org.example.Deep.desktop\n"
      "66=" ND_SELF_DESKTOP_ID "\n");
  const char *files[] = {user, etc, usr, "/nonexistent/handlers.list", NULL};
  const char *pairs[] = {
      "org.example.UserRange.desktop", NULL, "org.example.SystemExact.desktop", NULL,
      "org.example.Groundhog.desktop", NULL, "org.example.RangeA.desktop", NULL,
      "org.example.RangeB.desktop", NULL, "org.example.RangeC.desktop", NULL,
      "org.example.SysFallback.desktop", NULL, "org.example.Deep.desktop", NULL,
      "org.example.Declared.desktop", "20;45;66;",
      ND_SELF_DESKTOP_ID, "*", NULL};
  g_autoptr(NdRegistry) r = nd_registry_new(files, apps(pairs));

  /* A user range beats a system exact key: the first file with a hit wins. */
  ASSERT_CHOICE(r, 1, "org.example.UserRange.desktop", ND_SOURCE_HANDLERS_LIST);
  /* First *installed* id of the value list. */
  ASSERT_CHOICE(r, 14, "org.example.Groundhog.desktop", ND_SOURCE_HANDLERS_LIST);
  /* Invalid ids (paths, spaces) are never used; falls to declarations. */
  ASSERT_CHOICE(r, 20, "org.example.Declared.desktop", ND_SOURCE_DECLARED);
  /* Within a file: exact beats ranges. */
  ASSERT_CHOICE(r, 40, "org.example.RangeB.desktop", ND_SOURCE_HANDLERS_LIST);
  /* Narrowest range; equal width -> first key in file order. */
  ASSERT_CHOICE(r, 45, "org.example.RangeA.desktop", ND_SOURCE_HANDLERS_LIST);
  ASSERT_CHOICE(r, 52, "org.example.RangeC.desktop", ND_SOURCE_HANDLERS_LIST);
  ASSERT_CHOICE(r, 65, "org.example.Deep.desktop", ND_SOURCE_HANDLERS_LIST);
  /* The dispatcher's own id is refused even when configured. */
  ASSERT_CHOICE(r, 66, "org.example.Declared.desktop", ND_SOURCE_DECLARED);
  /* Nothing declares 9999: `*` from handlers.list (first installed). */
  ASSERT_CHOICE(r, 9999, "org.example.SysFallback.desktop", ND_SOURCE_FALLBACK_LIST);
  ASSERT_CHOICE(r, -1, "org.example.SysFallback.desktop", ND_SOURCE_FALLBACK_LIST);

  /* Highest-precedence file decides the privacy switch. */
  g_assert_false(nd_registry_fetch_relay_hints(r));
}

static void test_removed_handlers(void) {
  g_autofree char *user = write_list("removed.list",
      "[Removed Handlers]\n"
      "1=org.example.Noisy.desktop;\n"
      "*=org.example.Greedy.desktop;\n");
  const char *files[] = {user, NULL};
  const char *pairs[] = {
      "org.example.Noisy.desktop", "1;2;",
      "org.example.Quiet.desktop", "0-100;",
      "org.example.Greedy.desktop", "1;*;",
      "org.gnostr.gnostr.desktop", "*;",
      NULL};
  g_autoptr(NdRegistry) r = nd_registry_new(files, apps(pairs));
  ASSERT_CHOICE(r, 1, "org.example.Quiet.desktop", ND_SOURCE_DECLARED);
  ASSERT_CHOICE(r, 2, "org.example.Noisy.desktop", ND_SOURCE_DECLARED);
  ASSERT_CHOICE(r, 500, "org.gnostr.gnostr.desktop", ND_SOURCE_FALLBACK_DECLARED);
  ASSERT_CHOICE(r, -1, "org.gnostr.gnostr.desktop", ND_SOURCE_FALLBACK_DECLARED);
  g_assert_true(nd_registry_fetch_relay_hints(r)); /* default on */
}

static void test_nothing(void) {
  const char *pairs[] = {"org.example.NoKinds.desktop", NULL, NULL};
  g_autoptr(NdRegistry) r = nd_registry_new(NULL, apps(pairs));
  ASSERT_CHOICE(r, 1, NULL, ND_SOURCE_NONE);
  ASSERT_CHOICE(r, -1, NULL, ND_SOURCE_NONE);
}

static void test_set_default_roundtrip(void) {
  g_autofree char *path = g_build_filename(tmpdir, "sub", "dir", "handlers.list", NULL);
  g_autoptr(GError) err = NULL;
  g_assert_true(nd_registry_set_default(path, "14", "org.example.Groundhog.desktop", &err));
  g_assert_no_error(err);

  /* Hand-edit: add a comment and another section, then set again. */
  g_autofree char *c = NULL;
  g_assert_true(g_file_get_contents(path, &c, NULL, NULL));
  g_autofree char *edited = g_strconcat("# my notes\n", c,
                                        "\n[Removed Handlers]\n1=org.example.Noisy.desktop;\n", NULL);
  g_assert_true(g_file_set_contents(path, edited, -1, NULL));
  g_assert_true(nd_registry_set_default(path, "30000-39999", "org.example.Reader.desktop", &err));
  g_assert_true(nd_registry_set_default(path, "*", "org.gnostr.gnostr.desktop", &err));
  g_assert_true(nd_registry_set_default(path, "14", "org.example.Other.desktop", &err));

  g_autofree char *after = NULL;
  g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
  g_assert_nonnull(strstr(after, "# my notes"));
  g_assert_nonnull(strstr(after, "[Removed Handlers]"));
  g_assert_nonnull(strstr(after, "14=org.example.Other.desktop;"));
  g_assert_nonnull(strstr(after, "30000-39999=org.example.Reader.desktop;"));
  g_assert_null(strstr(after, "Groundhog"));

  /* Validation. */
  g_assert_false(nd_registry_set_default(path, "abc", "org.example.X.desktop", &err));
  g_clear_error(&err);
  g_assert_false(nd_registry_set_default(path, "1", "/usr/bin/sh", &err));
  g_clear_error(&err);
  g_assert_false(nd_registry_set_default(path, "1", ND_SELF_DESKTOP_ID, &err));
  g_clear_error(&err);

  const char *files[] = {path, NULL};
  const char *pairs[] = {"org.example.Other.desktop", NULL, "org.example.Reader.desktop", NULL,
                         "org.gnostr.gnostr.desktop", NULL, NULL};
  g_autoptr(NdRegistry) r = nd_registry_new(files, apps(pairs));
  ASSERT_CHOICE(r, 14, "org.example.Other.desktop", ND_SOURCE_HANDLERS_LIST);
  ASSERT_CHOICE(r, 30023, "org.example.Reader.desktop", ND_SOURCE_HANDLERS_LIST);
  ASSERT_CHOICE(r, 1, "org.gnostr.gnostr.desktop", ND_SOURCE_FALLBACK_LIST);
}

static void test_desktop_id_validation(void) {
  g_assert_true(nd_desktop_id_valid("org.gnostr.gnostr.desktop"));
  g_assert_true(nd_desktop_id_valid("my-app_2.desktop"));
  g_assert_false(nd_desktop_id_valid(".desktop"));
  g_assert_false(nd_desktop_id_valid("org.example.App"));
  g_assert_false(nd_desktop_id_valid("../evil.desktop"));
  g_assert_false(nd_desktop_id_valid("a b.desktop"));
  g_assert_false(nd_desktop_id_valid("a;b.desktop"));
  g_assert_false(nd_desktop_id_valid(ND_SELF_DESKTOP_ID));
  g_assert_false(nd_desktop_id_valid(NULL));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  tmpdir = g_dir_make_tmp("nd-registry-XXXXXX", NULL);
  g_test_add_func("/nd/registry/declarations", test_declarations);
  g_test_add_func("/nd/registry/handlers-list-precedence", test_handlers_list_precedence);
  g_test_add_func("/nd/registry/removed", test_removed_handlers);
  g_test_add_func("/nd/registry/nothing", test_nothing);
  g_test_add_func("/nd/registry/set-default", test_set_default_roundtrip);
  g_test_add_func("/nd/registry/desktop-id", test_desktop_id_validation);
  int rc = g_test_run();
  g_autofree char *cmd = g_strdup_printf("rm -rf '%s'", tmpdir);
  (void)!system(cmd);
  g_free(tmpdir);
  return rc;
}
