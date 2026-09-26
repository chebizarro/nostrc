/**
 * test_keystore — GNostr holds no keys (nostrc-e5nz).
 *
 * Headless coverage for the identity metadata shim (src/util/keystore*.c)
 * and signer availability (src/ipc/gnostr-signer-availability.c):
 *
 *   /signer-availability/copy       pure UX decisions: which text, when a
 *                                   banner shows, when "Start" is offered
 *   /signer-availability/presence   private session bus (GTestDBus) with an
 *                                   activatable fake org.nostr.Signer:
 *                                   ACTIVATABLE → RUNNING once owned, and
 *                                   StartServiceByName failure is reported
 *   /keystore/identity-list         Linux + gnome-keyring-daemon: the
 *                                   client's identity list (shim → app
 *                                   bridge → gnostr_identity_list_stored)
 *                                   equals the keyring's
 *                                   org.gnostr.Signer/identity metadata;
 *                                   legacy org.gnostr.NostrKey items are
 *                                   reported only for org.gnostr.Client
 *
 * D-Bus and keyring cases skip cleanly when dbus-daemon /
 * gnome-keyring-daemon are missing.
 */
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/util/keystore.h"
#include "../src/ipc/gnostr-signer-availability.h"
#include <nostr-gobject-1.0/gnostr-app-bridge.h>
#include <nostr-gobject-1.0/gnostr-identity.h>

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>
#include "seahorse/secret_store.h"
#endif

static GTestDBus *s_bus;
static gboolean s_have_bus;

/* ---- copy ---- */

static void test_copy(void) {
  GnostrSignerStatus st = { .presence = GNOSTR_SIGNER_PRESENCE_RUNNING };
  g_assert_null(gnostr_signer_status_login_text(&st));
  g_assert_null(gnostr_signer_status_banner_text(&st, GNOSTR_SIGNER_NEED_ACTIVE));
  g_assert_null(gnostr_signer_status_legacy_text(&st));
  g_assert_false(gnostr_signer_status_can_start(&st));

  /* Installed but stopped: start is offered; a NIP-55L session is read-only. */
  st.presence = GNOSTR_SIGNER_PRESENCE_ACTIVATABLE;
  g_assert_true(gnostr_signer_status_can_start(&st));
  g_autofree char *login = gnostr_signer_status_login_text(&st);
  g_assert_nonnull(strstr(login, "not running"));
  g_autofree char *banner = gnostr_signer_status_banner_text(&st, GNOSTR_SIGNER_NEED_ACTIVE);
  g_assert_nonnull(strstr(banner, "read-only"));
  /* A saved local-signer account that is not signed in is told so, not
   * promised that starting the signer alone resumes signing. */
  g_autofree char *signed_out = gnostr_signer_status_banner_text(&st, GNOSTR_SIGNER_NEED_SIGNED_OUT);
  g_assert_nonnull(strstr(signed_out, "signed out"));
  g_assert_nonnull(strstr(signed_out, "sign in again"));
  g_assert_null(strstr(signed_out, "read-only"));
  /* A NIP-46 session does not depend on org.nostr.Signer. */
  g_assert_null(gnostr_signer_status_banner_text(&st, GNOSTR_SIGNER_NEED_NONE));

  /* Not installed: no start action, still read-only and says why. */
  st.presence = GNOSTR_SIGNER_PRESENCE_NOT_INSTALLED;
  g_assert_false(gnostr_signer_status_can_start(&st));
  g_autofree char *login2 = gnostr_signer_status_login_text(&st);
  g_assert_nonnull(strstr(login2, "not installed"));
  g_autofree char *banner2 = gnostr_signer_status_banner_text(&st, GNOSTR_SIGNER_NEED_ACTIVE);
  g_assert_nonnull(strstr(banner2, "read-only"));

  st.presence = GNOSTR_SIGNER_PRESENCE_NO_BUS;
  g_autofree char *login3 = gnostr_signer_status_login_text(&st);
  g_assert_nonnull(login3);

  /* Legacy client keys: flagged even for a NIP-46 session while the signer
   * that would import them is not running; the text matches the platform. */
  st.presence = GNOSTR_SIGNER_PRESENCE_ACTIVATABLE;
  st.legacy_keys = 2;
  st.legacy_auto_migrates = TRUE;
  g_autofree char *legacy = gnostr_signer_status_legacy_text(&st);
  g_assert_nonnull(strstr(legacy, "2 private keys"));
  g_assert_nonnull(strstr(legacy, "Start GNostr Signer"));
  g_autofree char *banner3 = gnostr_signer_status_banner_text(&st, GNOSTR_SIGNER_NEED_NONE);
  g_assert_nonnull(strstr(banner3, "older GNostr"));
  st.legacy_keys = 1;
  st.legacy_auto_migrates = FALSE;   /* macOS: user imports by hand */
  g_autofree char *legacy2 = gnostr_signer_status_legacy_text(&st);
  g_assert_nonnull(strstr(legacy2, "1 private key "));
  g_assert_nonnull(strstr(legacy2, "Import them in GNostr Signer"));
  g_assert_null(gnostr_signer_status_banner_text(&st, GNOSTR_SIGNER_NEED_NONE));

  /* No copy anywhere claims GNostr stores the key. */
  const char *all[] = { login, banner, signed_out, login2, banner2, login3, legacy, banner3, legacy2 };
  for (guint i = 0; i < G_N_ELEMENTS(all); i++) {
    g_assert_null(strstr(all[i], "stored in GNostr"));
    g_assert_null(strstr(all[i], "locally stored"));
  }
}

/* ---- presence on a private bus ---- */

typedef struct {
  GMainLoop *loop;
  GnostrSignerStatus st;
  gboolean ok;
  GError *error;
} Wait;

static void on_status(GObject *src, GAsyncResult *res, gpointer ud) {
  (void)src;
  Wait *w = ud;
  w->ok = gnostr_signer_status_query_finish(res, &w->st, &w->error);
  g_main_loop_quit(w->loop);
}

static void on_started(GObject *src, GAsyncResult *res, gpointer ud) {
  (void)src;
  Wait *w = ud;
  w->ok = gnostr_signer_start_finish(res, &w->error);
  g_main_loop_quit(w->loop);
}

static GnostrSignerStatus wait_status(void) {
  Wait w = { .loop = g_main_loop_new(NULL, FALSE) };
  gnostr_signer_status_query_async(NULL, on_status, &w);
  g_main_loop_run(w.loop);
  g_main_loop_unref(w.loop);
  g_assert_no_error(w.error);
  g_assert_true(w.ok);
  return w.st;
}

static void test_presence(void) {
  if (!s_have_bus) {
    g_test_skip("dbus-daemon not available");
    return;
  }
  /* The fake service file is in the bus's service dir: activatable. */
  GnostrSignerStatus st = wait_status();
  g_assert_cmpint(st.presence, ==, GNOSTR_SIGNER_PRESENCE_ACTIVATABLE);

  /* Its Exec fails: the start request reports an error, not success. */
  Wait w = { .loop = g_main_loop_new(NULL, FALSE) };
  gnostr_signer_start_async(NULL, on_started, &w);
  g_main_loop_run(w.loop);
  g_main_loop_unref(w.loop);
  g_assert_false(w.ok);
  g_assert_nonnull(w.error);
  g_clear_error(&w.error);

  /* Once something owns org.nostr.Signer it is running. */
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_nonnull(bus);
  GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                            "org.freedesktop.DBus", "RequestName",
                                            g_variant_new("(su)", GNOSTR_SIGNER_BUS_NAME, 4u),
                                            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE,
                                            5000, NULL, NULL);
  g_assert_nonnull(r);
  guint32 reply = 0;
  g_variant_get(r, "(u)", &reply);
  g_variant_unref(r);
  g_assert_cmpuint(reply, ==, 1); /* PRIMARY_OWNER */
  st = wait_status();
  g_assert_cmpint(st.presence, ==, GNOSTR_SIGNER_PRESENCE_RUNNING);
  g_assert_false(gnostr_signer_status_can_start(&st));
  g_object_unref(bus);
}

/* ---- identity list vs keyring (Linux) ---- */

#ifdef HAVE_LIBSECRET
static GSubprocess *s_keyring;

static gboolean start_keyring(void) {
  gchar *gk = g_find_program_in_path("gnome-keyring-daemon");
  if (!gk || !s_have_bus) { g_free(gk); return FALSE; }
  GError *err = NULL;
  s_keyring = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                               G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                               &err, gk, "--foreground", "--unlock",
                               "--components=secrets", NULL);
  g_free(gk);
  if (!s_keyring) { g_clear_error(&err); return FALSE; }
  GOutputStream *in = g_subprocess_get_stdin_pipe(s_keyring);
  g_assert_true(g_output_stream_write_all(in, "keystore-test", 13, NULL, NULL, NULL));
  g_assert_true(g_output_stream_close(in, NULL, NULL));
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  gboolean up = FALSE;
  for (int i = 0; i < 200 && !up; i++) {
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus", "NameHasOwner",
                                              g_variant_new("(s)", "org.freedesktop.secrets"),
                                              G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
                                              1000, NULL, NULL);
    if (r) { g_variant_get(r, "(b)", &up); g_variant_unref(r); }
    if (!up) g_usleep(50 * 1000);
  }
  g_object_unref(bus);
  return up;
}

static void seed_legacy(const char *npub, const char *application) {
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"npub", (gpointer)npub);
  if (application)
    g_hash_table_insert(attrs, (gpointer)"application", (gpointer)application);
  GError *err = NULL;
  /* Any non-empty secret: the shim must never read it. */
  g_assert_true(secret_password_storev_sync(&gnostr_secret_legacy_client_schema, attrs,
                                            SECRET_COLLECTION_DEFAULT, "GNostr: legacy",
                                            "nsec1notreadbythetest", NULL, &err));
  g_assert_no_error(err);
  g_hash_table_unref(attrs);
}

static GHashTable *keyring_npub_labels(void) {
  /* Ground truth straight from the keyring: npub → label ("" if unset). */
  GHashTable *truth = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GHashTable *all = gnostr_secret_store_find_all(NULL);
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init(&it, all);
  while (g_hash_table_iter_next(&it, NULL, &v)) {
    const char *npub = g_hash_table_lookup(v, "npub");
    const char *label = g_hash_table_lookup(v, "label");
    if (!g_hash_table_contains(truth, npub) || (label && *label))
      g_hash_table_replace(truth, g_strdup(npub), g_strdup(label ? label : ""));
  }
  g_hash_table_unref(all);
  return truth;
}

static void test_identity_list(void) {
  if (!s_keyring && !start_keyring()) {
    g_test_skip("gnome-keyring-daemon not available");
    return;
  }
  static const char *A = "npub1sg6plzptd64u62a878hep2kev88swjh3tw00gjsfl8f237lmu63q0uf63m";
  static const char *B = "npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6";
  static const char *C = "npub1xtscya34g58tk0z605fvr788k263gsu6cy9x0mhnm87echrgufzsevkk5s";
  const GnostrSecretIdentity ia = { .npub = A, .label = "Alice" };
  const GnostrSecretIdentity ib = { .npub = B };
  /* A second selector for the same identity must not duplicate it. */
  const GnostrSecretIdentity ia2 = { .key_id = "work", .npub = A, .label = "Alice" };
  GError *err = NULL;
  g_assert_true(gnostr_secret_store_save(&ia, "11", &err));
  g_assert_true(gnostr_secret_store_save(&ib, "22", &err));
  g_assert_true(gnostr_secret_store_save(&ia2, "11", &err));
  g_assert_no_error(err);
  seed_legacy(C, "org.gnostr.Client");
  seed_legacy(B, "org.example.Other");   /* another program's item: not ours */
  seed_legacy(A, NULL);                  /* no application: not provably ours */

  g_assert_true(gnostr_keystore_available());

  /* Shim vs keyring metadata. */
  GHashTable *truth = keyring_npub_labels();
  g_assert_cmpuint(g_hash_table_size(truth), ==, 2);
  GList *keys = gnostr_keystore_list_keys(&err);
  g_assert_no_error(err);
  g_assert_cmpuint(g_list_length(keys), ==, g_hash_table_size(truth));
  for (GList *l = keys; l; l = l->next) {
    GnostrKeyInfo *k = l->data;
    g_assert_true(g_hash_table_contains(truth, k->npub));
    g_assert_cmpstr(k->label ? k->label : "", ==, g_hash_table_lookup(truth, k->npub));
  }
  g_list_free_full(keys, (GDestroyNotify)gnostr_key_info_free);

  /* The same list as the app sees it: shim → app bridge → identity API. */
  static const GnostrAppBridge vtable = {
    .keystore_available   = gnostr_keystore_available,
    .keystore_has_key     = gnostr_keystore_has_key,
    .keystore_list_keys   = gnostr_keystore_list_keys,
    .key_info_free        = gnostr_key_info_free,
    .keystore_error_quark = gnostr_keystore_error_quark,
  };
  gnostr_app_bridge_install(&vtable);
  GList *ids = gnostr_identity_list_stored(&err);
  g_assert_no_error(err);
  g_assert_cmpuint(g_list_length(ids), ==, g_hash_table_size(truth));
  for (GList *l = ids; l; l = l->next) {
    GNostrIdentity *id = l->data;
    g_assert_cmpstr(id->label ? id->label : "", ==, g_hash_table_lookup(truth, id->npub));
    g_assert_true(id->signer_holds_key);
    g_assert_cmpstr(id->signer_type, ==, "nip55l");
  }
  g_list_free_full(ids, (GDestroyNotify)gnostr_identity_free);
  g_assert_true(gnostr_identity_signer_holds_key(A));
  g_assert_false(gnostr_identity_signer_holds_key(C)); /* only a legacy copy */
  gnostr_app_bridge_install(NULL);
  g_hash_table_unref(truth);

  /* Legacy client keys: org.gnostr.Client items only. */
  GList *legacy = gnostr_keystore_list_legacy_keys(&err);
  g_assert_no_error(err);
  g_assert_cmpuint(g_list_length(legacy), ==, 1);
  g_assert_cmpstr(((GnostrKeyInfo *)legacy->data)->npub, ==, C);
  g_list_free_full(legacy, (GDestroyNotify)gnostr_key_info_free);
  g_assert_true(gnostr_keystore_legacy_migrates_automatically());

  GnostrSignerStatus st = wait_status();
  g_assert_cmpuint(st.legacy_keys, ==, 1);
  g_assert_true(st.legacy_auto_migrates);
}
#endif

static void test_shim_basics(void) {
  g_assert_false(gnostr_keystore_has_key(NULL));
  g_assert_false(gnostr_keystore_has_key("not-an-npub"));
  GnostrKeyInfo k = { .npub = (char *)"npub1x", .label = (char *)"L", .created_at = 7 };
  GnostrKeyInfo *c = gnostr_key_info_copy(&k);
  g_assert_cmpstr(c->npub, ==, "npub1x");
  g_assert_cmpstr(c->label, ==, "L");
  g_assert_cmpint(c->created_at, ==, 7);
  gnostr_key_info_free(c);
}

/* Everything the bus and gnome-keyring-daemon touch lives under one tmp
 * dir: $HOME and $XDG_* point there, so the real user's keyrings are never
 * opened (an --unlock with the test password against the user's
 * login.keyring would fail and could prompt). */
static gchar *isolate_environment(void) {
  gchar *root = g_dir_make_tmp("gnostr-keystore-test-XXXXXX", NULL);
  g_assert_nonnull(root);
  static const char *const dirs[][2] = {
    { "XDG_DATA_HOME", "data" }, { "XDG_CONFIG_HOME", "config" },
    { "XDG_CACHE_HOME", "cache" }, { "XDG_RUNTIME_DIR", "runtime" },
    { NULL, "services" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
    gchar *d = g_build_filename(root, dirs[i][1], NULL);
    g_assert_cmpint(g_mkdir_with_parents(d, 0700), ==, 0);
    if (dirs[i][0]) g_setenv(dirs[i][0], d, TRUE);
    g_free(d);
  }
  g_setenv("HOME", root, TRUE);
  gchar *path = g_build_filename(root, "services", "org.nostr.Signer.service", NULL);
  g_assert_true(g_file_set_contents(path,
      "[D-BUS Service]\nName=org.nostr.Signer\nExec=/bin/false\n", -1, NULL));
  g_free(path);
  return root;
}

static void rm_rf(const char *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
      gchar *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        rm_rf(child);
      else
        g_unlink(child);
      g_free(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  gchar *root = isolate_environment();
  gchar *dbus_daemon = g_find_program_in_path("dbus-daemon");
  if (dbus_daemon) {
    gchar *service_dir = g_build_filename(root, "services", NULL);
    s_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_add_service_dir(s_bus, service_dir);
    g_free(service_dir);
    g_test_dbus_up(s_bus);
    s_have_bus = TRUE;
  }
  g_free(dbus_daemon);

  g_test_add_func("/keystore/shim-basics", test_shim_basics);
  g_test_add_func("/signer-availability/copy", test_copy);
  g_test_add_func("/signer-availability/presence", test_presence);
#ifdef HAVE_LIBSECRET
  g_test_add_func("/keystore/identity-list", test_identity_list);
#endif

  int rc = g_test_run();

  gboolean used_keyring = FALSE;
#ifdef HAVE_LIBSECRET
  /* libsecret keeps a process-wide SecretService holding the bus. */
  secret_service_disconnect();
  if (s_keyring) {
    used_keyring = TRUE;
    g_subprocess_force_exit(s_keyring);
    (void)g_subprocess_wait(s_keyring, NULL, NULL);
    g_clear_object(&s_keyring);
  }
#endif
  if (s_bus) {
    if (used_keyring) {
      /* libsecret's sync API parks proxies (and their session-bus refs) on
       * private main contexts that are never iterated again, so the bus
       * singleton is never finalized and g_test_dbus_down — also run by
       * GTestDBus's dispose — stalls on its weak-notify timeout. Same fix
       * as nips/nip55l/tests/test_signer_dbus_contract.c: stop the bus
       * without that check and let process exit reclaim the GTestDBus. */
      GDBusConnection *singleton = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
      if (singleton) {
        g_dbus_connection_set_exit_on_close(singleton, FALSE);
        g_object_unref(singleton);
      }
      g_test_dbus_stop(s_bus);
    } else {
      g_test_dbus_down(s_bus);
      g_object_unref(s_bus);
    }
  }
  rm_rf(root);
  g_free(root);
  return rc;
}
