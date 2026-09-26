/* test-nn-provider.c - the real module entry points + NautilusMenuProvider
 * against fake NautilusFileInfo objects (no Nautilus process).
 *
 * SPDX-License-Identifier: MIT
 *
 * Tools are fake executables in a private PATH; org.nostr.Share.desktop is
 * the real data file from gnome/nostr-share, found through XDG_DATA_DIRS.
 * Activation is captured by nn_launch_set_hook().
 */
#include <nautilus-extension.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn-launch.h"

void nautilus_module_initialize(GTypeModule *module);
void nautilus_module_list_types(const GType **types, int *num_types);

/* --- Fake NautilusFileInfo ------------------------------------------------ */

#define FAKE_TYPE_FILE (fake_file_get_type())
G_DECLARE_FINAL_TYPE(FakeFile, fake_file, FAKE, FILE, GObject)
struct _FakeFile {
  GObject   parent_instance;
  GFile    *location;
  gchar    *mime;
  GFileType type;
};

static void fake_file_iface_init(NautilusFileInfoInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeFile, fake_file, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(NAUTILUS_TYPE_FILE_INFO, fake_file_iface_init))

static char *ff_get_name(NautilusFileInfo *fi) { return g_file_get_basename(FAKE_FILE(fi)->location); }
static char *ff_get_uri(NautilusFileInfo *fi) { return g_file_get_uri(FAKE_FILE(fi)->location); }
static char *ff_get_mime_type(NautilusFileInfo *fi) { return g_strdup(FAKE_FILE(fi)->mime); }
static GFileType ff_get_file_type(NautilusFileInfo *fi) { return FAKE_FILE(fi)->type; }
static GFile *ff_get_location(NautilusFileInfo *fi) { return g_object_ref(FAKE_FILE(fi)->location); }
static gboolean ff_is_directory(NautilusFileInfo *fi) { return FAKE_FILE(fi)->type == G_FILE_TYPE_DIRECTORY; }
static gboolean ff_is_gone(NautilusFileInfo *fi) { (void)fi; return FALSE; }

static void
fake_file_iface_init(NautilusFileInfoInterface *iface)
{
  iface->get_name = ff_get_name;
  iface->get_uri = ff_get_uri;
  iface->get_mime_type = ff_get_mime_type;
  iface->get_file_type = ff_get_file_type;
  iface->get_location = ff_get_location;
  iface->is_directory = ff_is_directory;
  iface->is_gone = ff_is_gone;
}

static void
fake_file_finalize(GObject *o)
{
  FakeFile *f = FAKE_FILE(o);
  g_clear_object(&f->location);
  g_free(f->mime);
  G_OBJECT_CLASS(fake_file_parent_class)->finalize(o);
}
static void fake_file_class_init(FakeFileClass *k) { G_OBJECT_CLASS(k)->finalize = fake_file_finalize; }
static void fake_file_init(FakeFile *f) { (void)f; }

static NautilusFileInfo *
fake(const gchar *uri_or_path, const gchar *mime, GFileType type)
{
  FakeFile *f = g_object_new(FAKE_TYPE_FILE, NULL);
  f->location = g_file_new_for_commandline_arg(uri_or_path);
  f->mime = g_strdup(mime);
  f->type = type;
  return NAUTILUS_FILE_INFO(f);
}

/* --- A GTypeModule to hand to nautilus_module_initialize() ---------------- */

#define TEST_TYPE_MODULE (test_module_get_type())
G_DECLARE_FINAL_TYPE(TestModule, test_module, TEST, MODULE, GTypeModule)
struct _TestModule { GTypeModule parent_instance; };
G_DEFINE_TYPE(TestModule, test_module, G_TYPE_TYPE_MODULE)
static gboolean tm_load(GTypeModule *m) { (void)m; return TRUE; }
static void tm_unload(GTypeModule *m) { (void)m; }
static void test_module_class_init(TestModuleClass *k)
{
  G_TYPE_MODULE_CLASS(k)->load = tm_load;
  G_TYPE_MODULE_CLASS(k)->unload = tm_unload;
}
static void test_module_init(TestModule *m) { (void)m; }

/* --- Fixture -------------------------------------------------------------- */

static gchar *root, *bin, *home;
static NautilusMenuProvider *provider;
static GPtrArray *launched;   /* gchar* joined argv */

static gboolean
capture(const gchar *const *argv, gpointer data)
{
  (void)data;
  g_ptr_array_add(launched, g_strjoinv(" ", (gchar **)argv));
  return TRUE;
}

static void
fake_tool(const gchar *name)
{
  g_autofree gchar *p = g_build_filename(bin, name, NULL);
  g_assert_true(g_file_set_contents(p, "#!/bin/sh\nexit 0\n", -1, NULL));
  g_assert_cmpint(g_chmod(p, 0755), ==, 0);
}

static gchar *
items_of(GList *items)
{
  GString *s = g_string_new(NULL);
  for (GList *l = items; l; l = l->next) {
    g_autofree gchar *name = NULL;
    g_object_get(l->data, "name", &name, NULL);
    g_string_append_printf(s, "%s%s", s->len ? "," : "", name + strlen("NostrNautilus::"));
  }
  return g_string_free(s, FALSE);
}

static gchar *
menu_for(GList *files)
{
  GList *items = nautilus_menu_provider_get_file_items(provider, files);
  gchar *r = items_of(items);
  g_list_free_full(items, g_object_unref);
  return r;
}

/* Activate item @name for @files; returns what was launched. */
static gchar *
activate(GList *files, const gchar *name)
{
  g_ptr_array_set_size(launched, 0);
  GList *items = nautilus_menu_provider_get_file_items(provider, files);
  for (GList *l = items; l; l = l->next) {
    g_autofree gchar *n = NULL;
    g_object_get(l->data, "name", &n, NULL);
    if (g_str_equal(n, name))
      nautilus_menu_item_activate(NAUTILUS_MENU_ITEM(l->data));
  }
  g_list_free_full(items, g_object_unref);
  g_ptr_array_add(launched, NULL);
  gchar *r = g_strjoinv(" | ", (gchar **)launched->pdata);
  g_ptr_array_remove_index(launched, launched->len - 1);
  return r;
}

static gchar *
home_path(const gchar *name)
{
  return g_build_filename(home, name, NULL);
}

/* --- Tests ---------------------------------------------------------------- */

static void
test_module_types(void)
{
  const GType *types = NULL;
  int n = -1;
  nautilus_module_list_types(&types, &n);
  g_assert_cmpint(n, ==, 1);
  g_assert_true(g_type_is_a(types[0], NAUTILUS_TYPE_MENU_PROVIDER));

  g_setenv("NOSTR_NAUTILUS_DISABLE", "1", TRUE);
  nautilus_module_list_types(&types, &n);
  g_assert_cmpint(n, ==, 0);
  g_unsetenv("NOSTR_NAUTILUS_DISABLE");
}

static void
test_file_items(void)
{
  g_autofree gchar *jpg = home_path("a b.jpg");
  g_autofree gchar *txt = home_path("notes.txt");
  g_autofree gchar *zip = home_path("a.zip");
  g_autofree gchar *sealed = home_path("a.pdf.nsealed");
  g_autofree gchar *repo = home_path("repo");
  g_autofree gchar *plain = home_path("plain");

  struct { const gchar *path, *mime; GFileType type; const gchar *expect; } cases[] = {
    { jpg,    "image/jpeg",                   G_FILE_TYPE_REGULAR,   "share,upload,encrypt" },
    { txt,    "text/plain",                   G_FILE_TYPE_REGULAR,   "share,encrypt" },
    { zip,    "application/zip",              G_FILE_TYPE_REGULAR,   "upload,encrypt" },
    { sealed, "application/vnd.nostr.sealed", G_FILE_TYPE_REGULAR,   "upload,decrypt" },
    { repo,   "inode/directory",              G_FILE_TYPE_DIRECTORY, "share" },
    { plain,  "inode/directory",              G_FILE_TYPE_DIRECTORY, "" },
    { "sftp://host/p.png", "image/png",       G_FILE_TYPE_REGULAR,   "share,upload" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    GList *files = g_list_append(NULL, fake(cases[i].path, cases[i].mime, cases[i].type));
    g_autofree gchar *got = menu_for(files);
    if (!g_str_equal(got, cases[i].expect))
      g_error("%s: expected '%s', got '%s'", cases[i].path, cases[i].expect, got);
    g_list_free_full(files, g_object_unref);
  }

  /* Multi-selection: every file must qualify. */
  GList *mixed = NULL;
  mixed = g_list_append(mixed, fake(jpg, "image/jpeg", G_FILE_TYPE_REGULAR));
  mixed = g_list_append(mixed, fake(sealed, "application/vnd.nostr.sealed", G_FILE_TYPE_REGULAR));
  g_autofree gchar *m = menu_for(mixed);
  g_assert_cmpstr(m, ==, "upload");
  g_list_free_full(mixed, g_object_unref);

  g_assert_null(nautilus_menu_provider_get_file_items(provider, NULL));
}

static void
test_activation(void)
{
  g_autofree gchar *jpg = home_path("a b.jpg");
  g_autofree gchar *sealed = home_path("a.pdf.nsealed");
  g_autofree gchar *share = g_build_filename(bin, "nostr-share", NULL);
  g_autofree gchar *seal_gtk = g_build_filename(bin, "nostr-seal-gtk", NULL);
  g_autofree gchar *helper = g_build_filename(bin, "nostr-nautilus-seal", NULL);

  GList *files = g_list_append(NULL, fake(jpg, "image/jpeg", G_FILE_TYPE_REGULAR));
  g_autofree gchar *a = activate(files, "NostrNautilus::share");
  g_autofree gchar *ea = g_strdup_printf("%s %s", share, jpg);
  g_assert_cmpstr(a, ==, ea);
  g_autofree gchar *b = activate(files, "NostrNautilus::upload");
  g_autofree gchar *eb = g_strdup_printf("%s --kind 1063 %s", share, jpg);
  g_assert_cmpstr(b, ==, eb);
  g_autofree gchar *c = activate(files, "NostrNautilus::encrypt");
  g_autofree gchar *ec = g_strdup_printf("%s %s", helper, jpg);
  g_assert_cmpstr(c, ==, ec);
  g_list_free_full(files, g_object_unref);

  files = g_list_append(NULL, fake(sealed, "application/vnd.nostr.sealed", G_FILE_TYPE_REGULAR));
  g_autofree gchar *d = activate(files, "NostrNautilus::decrypt");
  g_autofree gchar *ed = g_strdup_printf("%s %s", seal_gtk, sealed);
  g_assert_cmpstr(d, ==, ed);
  g_list_free_full(files, g_object_unref);
}

static void
test_background(void)
{
  g_autofree gchar *repo = home_path("repo");
  g_autofree gchar *plain = home_path("plain");
  g_autofree gchar *share = g_build_filename(bin, "nostr-share", NULL);

  g_autoptr(NautilusFileInfo) r = fake(repo, "inode/directory", G_FILE_TYPE_DIRECTORY);
  GList *items = nautilus_menu_provider_get_background_items(provider, r);
  g_autofree gchar *names = items_of(items);
  g_assert_cmpstr(names, ==, "share-repo");
  g_ptr_array_set_size(launched, 0);
  nautilus_menu_item_activate(NAUTILUS_MENU_ITEM(items->data));
  g_assert_cmpuint(launched->len, ==, 1);
  g_autofree gchar *expect = g_strdup_printf("%s %s", share, repo);
  g_assert_cmpstr(g_ptr_array_index(launched, 0), ==, expect);
  g_list_free_full(items, g_object_unref);

  g_autoptr(NautilusFileInfo) p = fake(plain, "inode/directory", G_FILE_TYPE_DIRECTORY);
  g_assert_null(nautilus_menu_provider_get_background_items(provider, p));
}

static void
test_tools_missing(void)
{
  /* Without the tools on PATH (and no helper) nothing is offered. */
  g_autofree gchar *jpg = home_path("a b.jpg");
  const gchar *old_path = g_getenv("PATH");
  g_autofree gchar *saved = g_strdup(old_path);
  g_setenv("PATH", "/nonexistent", TRUE);
  g_setenv("NOSTR_NAUTILUS_SEAL_HELPER", "/nonexistent/helper", TRUE);
  GList *files = g_list_append(NULL, fake(jpg, "image/jpeg", G_FILE_TYPE_REGULAR));
  g_autofree gchar *got = menu_for(files);
  g_assert_cmpstr(got, ==, "");
  g_list_free_full(files, g_object_unref);
  g_setenv("PATH", saved, TRUE);
  g_autofree gchar *helper = g_build_filename(bin, "nostr-nautilus-seal", NULL);
  g_setenv("NOSTR_NAUTILUS_SEAL_HELPER", helper, TRUE);
}

int
main(int argc, char **argv)
{
  root = g_dir_make_tmp("nn-provider-XXXXXX", NULL);
  bin = g_build_filename(root, "bin", NULL);
  home = g_build_filename(root, "home", NULL);
  g_autofree gchar *apps = g_build_filename(root, "share", "applications", NULL);
  g_autofree gchar *datadir = g_build_filename(root, "share", NULL);
  g_mkdir_with_parents(bin, 0700);
  g_mkdir_with_parents(apps, 0700);
  g_autofree gchar *repo_git = g_build_filename(home, "repo", ".git", NULL);
  g_autofree gchar *plain = g_build_filename(home, "plain", NULL);
  g_mkdir_with_parents(repo_git, 0700);
  g_mkdir_with_parents(plain, 0700);

  /* The real desktop file (MimeType=, TryExec=nostr-share). */
  g_autofree gchar *desktop = NULL;
  gsize len = 0;
  g_assert_true(g_file_get_contents(NN_SHARE_DESKTOP_FILE, &desktop, &len, NULL));
  g_autofree gchar *dst = g_build_filename(apps, "org.nostr.Share.desktop", NULL);
  g_assert_true(g_file_set_contents(dst, desktop, (gssize)len, NULL));

  g_setenv("GIO_USE_VFS", "local", TRUE);   /* no gvfs daemon / session bus */
  g_setenv("XDG_DATA_DIRS", datadir, TRUE);
  g_setenv("XDG_DATA_HOME", datadir, TRUE);
  g_setenv("PATH", bin, TRUE);
  g_unsetenv("NOSTR_NAUTILUS_DISABLE");

  g_test_init(&argc, &argv, NULL);
  fake_tool("nostr-share");
  fake_tool("nostr-seal");
  fake_tool("nostr-seal-gtk");
  fake_tool("nostr-nautilus-seal");
  g_autofree gchar *helper = g_build_filename(bin, "nostr-nautilus-seal", NULL);
  g_setenv("NOSTR_NAUTILUS_SEAL_HELPER", helper, TRUE);

  GTypeModule *module = g_object_new(TEST_TYPE_MODULE, NULL);
  g_type_module_use(module);
  nautilus_module_initialize(module);
  const GType *types = NULL;
  int n = 0;
  nautilus_module_list_types(&types, &n);
  g_assert_cmpint(n, ==, 1);
  provider = NAUTILUS_MENU_PROVIDER(g_object_new(types[0], NULL));

  launched = g_ptr_array_new_with_free_func(g_free);
  nn_launch_set_hook(capture, NULL);

  g_test_add_func("/nostr-nautilus/provider/module-types", test_module_types);
  g_test_add_func("/nostr-nautilus/provider/file-items", test_file_items);
  g_test_add_func("/nostr-nautilus/provider/activation", test_activation);
  g_test_add_func("/nostr-nautilus/provider/background", test_background);
  g_test_add_func("/nostr-nautilus/provider/tools-missing", test_tools_missing);
  int rc = g_test_run();

  g_object_unref(provider);
  g_ptr_array_unref(launched);
  g_autofree gchar *cmd = g_strdup_printf("/bin/rm -rf '%s'", root);
  if (system(cmd) != 0)
    g_printerr("could not remove %s\n", root);
  return rc;
}
