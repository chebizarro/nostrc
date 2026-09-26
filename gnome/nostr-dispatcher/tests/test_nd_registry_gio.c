/*
 * Registry over real GIO enumeration: .desktop files under a private
 * XDG_DATA_HOME, handlers.list under a private XDG_CONFIG_HOME, then a
 * dry-run dispatch (no network: private XDG_RUNTIME_DIR has no relay.sock
 * and the links carry no relay hints).
 */
#include "nd-dispatch.h"
#include "nd-error.h"
#include "nd-registry.h"

#include <stdlib.h>
#include <string.h>

#include "nostr/nip19/nip19.h"

static char *root;

static void write_file(const char *rel, const char *contents) {
  g_autofree char *p = g_build_filename(root, rel, NULL);
  g_autofree char *dir = g_path_get_dirname(p);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents(p, contents, -1, NULL));
}

static void desktop(const char *id, const char *extra) {
  g_autofree char *rel = g_strconcat("data/applications/", id, NULL);
  g_autofree char *c = g_strdup_printf(
      "[Desktop Entry]\nType=Application\nName=%s\nExec=true %%u\n%s\n", id, extra);
  write_file(rel, c);
}

typedef struct { GMainLoop *loop; NdOpenResult *r; GError *err; } Wait;

static void done(GObject *s, GAsyncResult *res, gpointer ud) {
  Wait *w = ud;
  w->r = nd_dispatch_open_finish(res, &w->err);
  g_main_loop_quit(w->loop);
}

static NdOpenResult *resolve(const char *uri, GError **error) {
  Wait w = {g_main_loop_new(NULL, FALSE), NULL, NULL};
  NdOpenOptions o = {.dry_run = TRUE};
  nd_dispatch_open_uri_async(nd_registry_new_default(), uri, &o, NULL, done, &w);
  g_main_loop_run(w.loop);
  g_main_loop_unref(w.loop);
  if (w.err) g_propagate_error(error, w.err);
  return w.r;
}

static void test_enumeration(void) {
  g_autoptr(NdRegistry) r = nd_registry_new_default();
  NdSource s;
  g_autofree char *a = nd_registry_choose(r, 30023, &s);
  g_assert_cmpstr(a, ==, "org.example.Reader.desktop");
  g_assert_cmpint(s, ==, ND_SOURCE_DECLARED);
  g_autofree char *b = nd_registry_choose(r, 14, &s);
  g_assert_cmpstr(b, ==, "org.example.Chat.desktop"); /* user handlers.list */
  g_assert_cmpint(s, ==, ND_SOURCE_HANDLERS_LIST);
  g_autofree char *c = nd_registry_choose(r, 1, &s);
  g_assert_cmpstr(c, ==, "org.example.Client.desktop");
  /* Hidden=true entry declaring 9 is ignored; falls back to `*`. */
  g_autofree char *d = nd_registry_choose(r, 9, &s);
  g_assert_cmpstr(d, ==, "org.example.Client.desktop");
  g_assert_cmpint(s, ==, ND_SOURCE_FALLBACK_DECLARED);
}

static void test_dispatch_dry_run(void) {
  /* naddr: kind from TLV, no fetch. */
  NostrEntityPointer ap = {
      .public_key = (char *)"3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d",
      .kind = 30023, .identifier = (char *)"x"};
  char *bech = NULL;
  g_assert_cmpint(nostr_nip19_encode_naddr(&ap, &bech), ==, 0);
  g_autofree char *uri = g_strconcat("nostr:", bech, NULL);
  free(bech);
  g_autoptr(GError) err = NULL;
  g_autoptr(NdOpenResult) r = resolve(uri, &err);
  g_assert_no_error(err);
  g_assert_cmpint(r->kind, ==, 30023);
  g_assert_cmpstr(r->desktop_id, ==, "org.example.Reader.desktop");
  g_assert_false(r->handed_off);

  /* note1: kind unknown, nothing local, no hints -> fallback handler. */
  guint8 id[32] = {0xab};
  g_assert_cmpint(nostr_nip19_encode_note(id, &bech), ==, 0);
  g_autofree char *note = g_strconcat("nostr:", bech, NULL);
  free(bech);
  g_autoptr(NdOpenResult) r2 = resolve(note, &err);
  g_assert_no_error(err);
  g_assert_cmpint(r2->kind, ==, -1);
  g_assert_cmpstr(r2->desktop_id, ==, "org.example.Client.desktop");

  /* nsec never gets past parsing. */
  guint8 sk[32] = {7};
  g_assert_cmpint(nostr_nip19_encode_nsec(sk, &bech), ==, 0);
  g_autofree char *nsec = g_strconcat("nostr:", bech, NULL);
  free(bech);
  g_assert_null(resolve(nsec, &err));
  g_assert_error(err, ND_ERROR, ND_ERROR_FORBIDDEN);
}

int main(int argc, char **argv) {
  root = g_dir_make_tmp("nd-gio-XXXXXX", NULL);
  g_autofree char *data = g_build_filename(root, "data", NULL);
  g_autofree char *config = g_build_filename(root, "config", NULL);
  g_autofree char *runtime = g_build_filename(root, "runtime", NULL);
  g_mkdir_with_parents(runtime, 0700);
  /* Must be set before GLib caches any XDG directory. */
  g_setenv("XDG_DATA_HOME", data, TRUE);
  g_setenv("XDG_DATA_DIRS", data, TRUE);
  g_setenv("XDG_CONFIG_HOME", config, TRUE);
  g_setenv("XDG_CONFIG_DIRS", config, TRUE);
  g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent", TRUE);

  desktop("org.example.Reader.desktop", "X-Nostr-Kinds=30023;30000-39999;");
  desktop("org.example.Client.desktop", "X-Nostr-Kinds=0;1;*;");
  desktop("org.example.Chat.desktop", "NoDisplay=true");
  desktop("org.example.Ghost.desktop", "Hidden=true\nX-Nostr-Kinds=9;");
  desktop(ND_SELF_DESKTOP_ID, "X-Nostr-Kinds=*;");
  write_file("config/nostr/handlers.list",
             "[Default Handlers]\n14=org.example.Chat.desktop;\n");

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nd/registry-gio/enumeration", test_enumeration);
  g_test_add_func("/nd/registry-gio/dispatch-dry-run", test_dispatch_dry_run);
  int rc = g_test_run();
  g_autofree char *cmd = g_strdup_printf("rm -rf '%s'", root);
  (void)!system(cmd);
  return rc;
}
