/*
 * test_session_relay_dbus — org.nostr.SessionRelay1 on the real
 * nostr-session-relayd (bead nostrc-janr).
 *
 * Runs headless: a private GTestDBus session bus, a temp HOME /
 * XDG_RUNTIME_DIR (the daemon fallback-binds relay.sock there), and a
 * session-relay.conf with a known supported_nips list. Checks:
 *   - the daemon claims org.nostr.SessionRelay1 and exports the object;
 *   - live introspection matches gnome/dbus/org.nostr.SessionRelay1.xml
 *     (every property with its type, every method with its signature);
 *   - GetStats carries every documented key with the documented type and
 *     values consistent with the config / storage state;
 *   - Get on individual properties agrees with GetStats;
 *   - StorageBytes reflects files in the storage dir (du semantics);
 *   - `--stats` prints the same data; with no daemon it exits 1;
 *   - SIGTERM → clean exit and the name disappears.
 */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#define NAME "org.nostr.SessionRelay1"
#define PATH "/org/nostr/SessionRelay1"
#define IFACE "org.nostr.SessionRelay1"

typedef struct {
  GTestDBus *bus;
  GDBusConnection *conn;
  gchar *root;
  gchar **envp;
  GPid pid;
  const gchar *relayd;
} Fixture;

static gchar **make_env(const Fixture *f) {
  gchar **env = g_get_environ();
  g_autofree gchar *home = g_build_filename(f->root, "home", NULL);
  g_autofree gchar *run = g_build_filename(f->root, "run", NULL);
  g_autofree gchar *data = g_build_filename(home, ".local", "share", NULL);
  g_autofree gchar *cfg = g_build_filename(home, ".config", NULL);
  env = g_environ_setenv(env, "HOME", home, TRUE);
  env = g_environ_setenv(env, "XDG_RUNTIME_DIR", run, TRUE);
  env = g_environ_setenv(env, "XDG_DATA_HOME", data, TRUE);
  env = g_environ_setenv(env, "XDG_CONFIG_HOME", cfg, TRUE);
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS",
                         g_test_dbus_get_bus_address(f->bus), TRUE);
  env = g_environ_unsetenv(env, "LISTEN_FDS");
  env = g_environ_unsetenv(env, "LISTEN_PID");
  env = g_environ_unsetenv(env, "NOTIFY_SOCKET");
  return env;
}

static gboolean name_has_owner(GDBusConnection *c) {
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "NameHasOwner", g_variant_new("(s)", NAME), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  gboolean b = FALSE;
  if (r) g_variant_get(r, "(b)", &b);
  return b;
}

static void wait_for_owner(Fixture *f, gboolean want) {
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (name_has_owner(f->conn) != want) {
    if (g_get_monotonic_time() > deadline)
      g_error("timed out waiting for %s owner=%d", NAME, want);
    g_usleep(50 * 1000);
  }
}

static void setup(Fixture *f, gconstpointer data) {
  (void)data;
  f->relayd = g_getenv("NOSTR_SESSION_RELAYD");
  if (!f->relayd || !g_file_test(f->relayd, G_FILE_TEST_IS_EXECUTABLE)) {
    g_test_skip("NOSTR_SESSION_RELAYD not set / not executable");
    return;
  }
  f->root = g_dir_make_tmp("nsr-dbus-XXXXXX", NULL);
  g_assert_nonnull(f->root);
  g_autofree gchar *cfgdir = g_build_filename(f->root, "home", ".config", "nostr", NULL);
  g_autofree gchar *run = g_build_filename(f->root, "run", NULL);
  g_assert_cmpint(g_mkdir_with_parents(cfgdir, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(run, 0700), ==, 0);
  g_autofree gchar *conf = g_build_filename(cfgdir, "session-relay.conf", NULL);
  /* Unknown retention_* keys must not break the loader (settings UI
   * writes them ahead of the implementation). */
  g_assert_true(g_file_set_contents(conf,
      "# test\nsupported_nips = [1, 11, 45]\n"
      "retention_enabled = true\nretention_cache_max_mb = 512\n", -1, NULL));

  f->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(f->bus);
  f->conn = g_dbus_connection_new_for_address_sync(
      g_test_dbus_get_bus_address(f->bus),
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, NULL);
  g_assert_nonnull(f->conn);
  f->envp = make_env(f);
}

static void start_daemon(Fixture *f) {
  const gchar *argv[] = {f->relayd, NULL};
  GError *err = NULL;
  g_assert_true(g_spawn_async(NULL, (gchar **)argv, f->envp,
                              G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &f->pid, &err));
  g_assert_no_error(err);
  wait_for_owner(f, TRUE);
}

static int stop_daemon(Fixture *f) {
  if (!f->pid) return -1;
  kill(f->pid, SIGTERM);
  int status = 0;
  gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
  while (waitpid(f->pid, &status, WNOHANG) == 0) {
    if (g_get_monotonic_time() > deadline) {
      kill(f->pid, SIGKILL);
      waitpid(f->pid, &status, 0);
      g_error("daemon did not exit on SIGTERM");
    }
    g_usleep(20 * 1000);
  }
  g_spawn_close_pid(f->pid);
  f->pid = 0;
  return status;
}

static void teardown(Fixture *f, gconstpointer data) {
  (void)data;
  if (!f->bus) return;
  if (f->pid) stop_daemon(f);
  g_clear_object(&f->conn);
  g_test_dbus_down(f->bus);
  g_clear_object(&f->bus);
  g_strfreev(f->envp);
  if (f->root) {
    g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", f->root);
    g_assert_cmpint(system(cmd), ==, 0);
  }
  g_free(f->root);
}

static GVariant *get_stats(Fixture *f) {
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_sync(
      f->conn, NAME, PATH, IFACE, "GetStats", NULL, G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, &err);
  g_assert_no_error(err);
  GVariant *d = g_variant_get_child_value(r, 0);
  g_variant_unref(r);
  return d;
}

static GVariant *get_prop(Fixture *f, const gchar *prop) {
  GError *err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      f->conn, NAME, PATH, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", IFACE, prop), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, &err);
  g_assert_no_error(err);
  GVariant *v = NULL;
  g_variant_get(r, "(v)", &v);
  return v;
}

static void test_interface_matches_xml(Fixture *f, gconstpointer data) {
  (void)data;
  if (!f->bus) return;
  start_daemon(f);

  g_autofree gchar *xml = NULL;
  g_assert_true(g_file_get_contents(NSR_INTERFACE_XML, &xml, NULL, NULL));
  g_autoptr(GDBusNodeInfo) doc = g_dbus_node_info_new_for_xml(xml, NULL);
  g_assert_nonnull(doc);
  GDBusInterfaceInfo *want = g_dbus_node_info_lookup_interface(doc, IFACE);
  g_assert_nonnull(want);

  GError *err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
      f->conn, NAME, PATH, "org.freedesktop.DBus.Introspectable", "Introspect",
      NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, &err);
  g_assert_no_error(err);
  const gchar *live_xml = NULL;
  g_variant_get(r, "(&s)", &live_xml);
  g_autoptr(GDBusNodeInfo) live = g_dbus_node_info_new_for_xml(live_xml, NULL);
  GDBusInterfaceInfo *got = g_dbus_node_info_lookup_interface(live, IFACE);
  g_assert_nonnull(got);

  guint n_want = 0, n_got = 0;
  for (GDBusPropertyInfo **p = want->properties; p && *p; p++, n_want++) {
    GDBusPropertyInfo *g = g_dbus_interface_info_lookup_property(got, (*p)->name);
    g_assert_nonnull(g);
    g_assert_cmpstr(g->signature, ==, (*p)->signature);
    g_assert_cmpint(g->flags, ==, (*p)->flags);
  }
  for (GDBusPropertyInfo **p = got->properties; p && *p; p++) n_got++;
  g_assert_cmpuint(n_got, ==, n_want);
  n_want = n_got = 0;
  for (GDBusMethodInfo **m = want->methods; m && *m; m++, n_want++) {
    GDBusMethodInfo *g = g_dbus_interface_info_lookup_method(got, (*m)->name);
    g_assert_nonnull(g);
    for (guint i = 0; (*m)->out_args && (*m)->out_args[i]; i++)
      g_assert_cmpstr(g->out_args[i]->signature, ==, (*m)->out_args[i]->signature);
  }
  for (GDBusMethodInfo **m = got->methods; m && *m; m++) n_got++;
  g_assert_cmpuint(n_got, ==, n_want);

  g_assert_cmpint(stop_daemon(f), ==, 0);
  wait_for_owner(f, FALSE);
}

static void test_get_stats(Fixture *f, gconstpointer data) {
  (void)data;
  if (!f->bus) return;
  /* A known file in the storage dir so StorageBytes is non-zero whatever
   * backend is compiled in. */
  g_autofree gchar *sdir = g_build_filename(f->root, "home", ".local", "share",
                                            "nostr", "session-relay", NULL);
  g_assert_cmpint(g_mkdir_with_parents(sdir, 0700), ==, 0);
  g_autofree gchar *blob = g_build_filename(sdir, "fixture.bin", NULL);
  gchar buf[16384];
  memset(buf, 0x5a, sizeof buf);
  g_assert_true(g_file_set_contents(blob, buf, sizeof buf, NULL));
  start_daemon(f);

  g_autoptr(GVariant) s = get_stats(f);
  const struct { const gchar *key; const gchar *type; } keys[] = {
      {"event_count", "x"}, {"storage_bytes", "t"}, {"connected_clients", "u"},
      {"uptime", "t"}, {"storage_backend", "s"}, {"requested_storage_backend", "s"},
      {"storage_path", "s"}, {"supported_nips", "au"}, {"retention_supported", "b"},
      {"connections_total", "t"}, {"subscriptions", "u"}, {"events_streamed", "t"},
      {"version", "s"},
  };
  for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
    g_autoptr(GVariant) v = g_variant_lookup_value(s, keys[i].key, NULL);
    if (!v) g_error("GetStats lacks %s", keys[i].key);
    g_assert_cmpstr(g_variant_get_type_string(v), ==, keys[i].type);
  }

  const gchar *backend = NULL, *requested = NULL, *path = NULL;
  gint64 events = 0;
  guint64 bytes = 0;
  guint32 clients = 99;
  gboolean retention = TRUE;
  g_assert_true(g_variant_lookup(s, "storage_backend", "&s", &backend));
  g_assert_true(g_variant_lookup(s, "requested_storage_backend", "&s", &requested));
  g_assert_true(g_variant_lookup(s, "storage_path", "&s", &path));
  g_assert_true(g_variant_lookup(s, "event_count", "x", &events));
  g_assert_true(g_variant_lookup(s, "storage_bytes", "t", &bytes));
  g_assert_true(g_variant_lookup(s, "connected_clients", "u", &clients));
  g_assert_true(g_variant_lookup(s, "retention_supported", "b", &retention));
  g_assert_cmpstr(requested, ==, "nostrdb");
  g_assert_true(g_str_equal(backend, "nostrdb") || g_str_equal(backend, "none"));
  if (g_str_equal(backend, "none")) g_assert_cmpint(events, ==, 0);
  else g_assert_cmpint(events, >=, -1);
  g_assert_cmpstr(path, ==, sdir);
  g_assert_cmpuint(bytes, >=, sizeof buf);
  g_assert_cmpuint(clients, ==, 0);
  g_assert_false(retention);

  g_autoptr(GVariant) nips = g_variant_lookup_value(s, "supported_nips", G_VARIANT_TYPE("au"));
  gsize n = 0;
  const guint32 *a = g_variant_get_fixed_array(nips, &n, sizeof(guint32));
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpuint(a[0], ==, 1);
  g_assert_cmpuint(a[1], ==, 11);
  g_assert_cmpuint(a[2], ==, 45);

  /* Properties agree with GetStats. */
  g_autoptr(GVariant) pb = get_prop(f, "StorageBackend");
  g_assert_cmpstr(g_variant_get_string(pb, NULL), ==, backend);
  g_autoptr(GVariant) pr = get_prop(f, "RetentionSupported");
  g_assert_false(g_variant_get_boolean(pr));
  g_autoptr(GVariant) pn = get_prop(f, "SupportedNips");
  g_assert_true(g_variant_equal(pn, nips));
  g_autoptr(GVariant) pu = get_prop(f, "Uptime");
  g_assert_true(g_variant_is_of_type(pu, G_VARIANT_TYPE_UINT64));

  /* --stats CLI against the running daemon. */
  const gchar *argv[] = {f->relayd, "--stats", NULL};
  g_autofree gchar *out = NULL;
  gint status = -1;
  GError *err = NULL;
  g_assert_true(g_spawn_sync(NULL, (gchar **)argv, f->envp, G_SPAWN_DEFAULT,
                             NULL, NULL, &out, NULL, &status, &err));
  g_assert_no_error(err);
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_autofree gchar *want_backend = g_strdup_printf("storage_backend: %s\n", backend);
  g_assert_nonnull(strstr(out, want_backend));
  g_assert_nonnull(strstr(out, "supported_nips: 1,11,45\n"));
  g_assert_nonnull(strstr(out, "retention_supported: false\n"));

  g_assert_cmpint(stop_daemon(f), ==, 0);
  wait_for_owner(f, FALSE);

  /* --stats with no daemon: exit 1, no activation attempt succeeds. */
  g_clear_pointer(&out, g_free);
  g_assert_true(g_spawn_sync(NULL, (gchar **)argv, f->envp,
                             G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                             &status, &err));
  g_assert_false(g_spawn_check_wait_status(status, NULL));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add("/session-relay/dbus/interface-matches-xml", Fixture, NULL, setup,
             test_interface_matches_xml, teardown);
  g_test_add("/session-relay/dbus/get-stats", Fixture, NULL, setup,
             test_get_stats, teardown);
  return g_test_run();
}
