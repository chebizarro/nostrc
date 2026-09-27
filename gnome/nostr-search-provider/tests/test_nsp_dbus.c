/* End-to-end over a private session bus (GTestDBus): the real
 * nostr-search-provider daemon against a mock session relay, and a fake
 * org.nostr.Dispatcher1 recording what ActivateResult forwards. Skipped
 * when dbus-daemon is unavailable. */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>

#include "nd-uri.h"
#include "nsp-testutil.h"

#define DEADLINE_MS 400

static GDBusConnection *bus;
static GPtrArray *opened; /* URIs received by the fake dispatcher */

static void dispatcher_call(GDBusConnection *c, const char *sender, const char *path,
                            const char *iface, const char *method, GVariant *params,
                            GDBusMethodInvocation *inv, gpointer user_data) {
  if (strcmp(method, "Open") == 0) {
    const char *uri = NULL;
    g_variant_get(params, "(&s@a{sv})", &uri, NULL);
    g_ptr_array_add(opened, g_strdup(uri));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "org.example.Test.desktop"));
    return;
  }
  g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no");
}

static const GDBusInterfaceVTable dispatcher_vtable = {dispatcher_call, NULL, NULL, {0}};

typedef struct {
  gboolean done;
  GVariant *reply;
  GError *error;
} CallRun;

static void on_call(GObject *src, GAsyncResult *res, gpointer user_data) {
  CallRun *c = user_data;
  c->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &c->error);
  c->done = TRUE;
}

/* Async + main-context iteration: the mock relay and the fake dispatcher
 * live in this process's main context and must keep running. */
static GVariant *call(const char *method, GVariant *args, const char *reply_type, gint64 *ms) {
  CallRun c = {0};
  gint64 t0 = g_get_monotonic_time();
  g_dbus_connection_call(bus, "org.nostr.SearchProvider", "/org/nostr/SearchProvider",
                         "org.gnome.Shell.SearchProvider2", method, args,
                         reply_type ? G_VARIANT_TYPE(reply_type) : NULL, G_DBUS_CALL_FLAGS_NONE,
                         10000, NULL, on_call, &c);
  g_assert_true(nsp_test_wait(&c.done, 15000));
  if (ms) *ms = (g_get_monotonic_time() - t0) / 1000;
  g_assert_no_error(c.error);
  return c.reply;
}

static char **initial(const char *term, gint64 *ms) {
  const char *terms[] = {term, NULL};
  g_autoptr(GVariant) r = call("GetInitialResultSet", g_variant_new("(^as)", terms), "(as)", ms);
  char **ids = NULL;
  g_variant_get(r, "(^as)", &ids);
  return ids;
}

static gboolean name_up;
static void on_appeared(GDBusConnection *c, const char *n, const char *o, gpointer u) { name_up = TRUE; }

static GPid spawn_daemon(const char *sock, const char *home) {
  g_auto(GStrv) env = g_get_environ();
  env = g_environ_setenv(env, "NOSTR_SEARCH_PROVIDER_SOCKET", sock, TRUE);
  env = g_environ_setenv(env, "NOSTR_SEARCH_PROVIDER_NO_NETWORK", "1", TRUE);
  g_autofree char *dl = g_strdup_printf("%d", DEADLINE_MS);
  env = g_environ_setenv(env, "NOSTR_SEARCH_PROVIDER_DEADLINE_MS", dl, TRUE);
  env = g_environ_setenv(env, "XDG_CACHE_HOME", home, TRUE);
  env = g_environ_setenv(env, "XDG_CONFIG_HOME", home, TRUE);
  /* No installed applications: LaunchSearch (nostrc-prqu.15) must find
   * nothing to launch instead of starting a real Nostr client. */
  env = g_environ_setenv(env, "XDG_DATA_HOME", home, TRUE);
  env = g_environ_setenv(env, "XDG_DATA_DIRS", home, TRUE);
  char *argv[] = {NSP_BINARY, "daemon", NULL};
  GPid pid = 0;
  g_autoptr(GError) err = NULL;
  g_assert_true(g_spawn_async(NULL, argv, env, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &err));
  name_up = FALSE;
  guint w = g_bus_watch_name_on_connection(bus, "org.nostr.SearchProvider", 0, on_appeared, NULL,
                                           NULL, NULL);
  g_assert_true(nsp_test_wait(&name_up, 10000));
  g_bus_unwatch_name(w);
  return pid;
}

static gboolean wait_opened(guint n) {
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (opened->len < n && g_get_monotonic_time() < end) g_main_context_iteration(NULL, TRUE);
  return opened->len >= n;
}

static void test_e2e(void) {
  g_autoptr(GTestDBus) tdb = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_autofree char *daemon_bin = g_find_program_in_path("dbus-daemon");
  if (!daemon_bin) {
    g_test_skip("dbus-daemon not available");
    return;
  }
  g_test_dbus_up(tdb);
  g_autoptr(GError) err = NULL;
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  g_assert_no_error(err);

  /* fake nostr-dispatcher */
  opened = g_ptr_array_new_with_free_func(g_free);
  g_autofree char *xml = NULL;
  g_assert_true(g_file_get_contents(ND_DISPATCHER_XML, &xml, NULL, NULL));
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, &err);
  g_assert_no_error(err);
  guint reg = g_dbus_connection_register_object(
      bus, "/org/nostr/Dispatcher1", g_dbus_node_info_lookup_interface(node, "org.nostr.Dispatcher1"),
      &dispatcher_vtable, NULL, NULL, &err);
  g_assert_no_error(err);
  guint own = g_bus_own_name_on_connection(bus, "org.nostr.Dispatcher1", 0, NULL, NULL, NULL, NULL);

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  NspMockRelay *m = nsp_mock_relay_new(NSP_MOCK_ANSWER, TRUE);
  g_autofree char *profile =
      nsp_test_event(NSP_TEST_SK_ALICE, 0, now - 100, "{\"display_name\":\"Alice <&>\"}", NULL);
  g_autofree char *note = nsp_test_event(NSP_TEST_SK_ALICE, 1, now - 7200, "hello from the overview", NULL);
  nsp_mock_relay_add(m, profile);
  nsp_mock_relay_add(m, note);
  g_autofree char *home = g_dir_make_tmp("nsp-home-XXXXXX", NULL);
  GPid pid = spawn_daemon(nsp_mock_relay_socket(m), home);

  /* npub → profile */
  g_autofree char *pk = nsp_test_pubkey(NSP_TEST_SK_ALICE);
  NdTarget t = {.entity = ND_ENTITY_PROFILE, .pubkey_hex = pk, .kind = 0};
  g_autofree char *np = nd_target_to_uri(&t);
  gint64 ms = 0;
  g_auto(GStrv) ids = initial(np + strlen("nostr:"), &ms); /* bare npub as typed */
  g_assert_cmpuint(g_strv_length(ids), ==, 1);
  g_assert_cmpstr(ids[0], ==, np);
  g_assert_cmpint(ms, <, 1000);

  g_autoptr(GVariant) metas = call("GetResultMetas", g_variant_new("(^as)", ids), "(aa{sv})", NULL);
  g_autoptr(GVariant) arr = g_variant_get_child_value(metas, 0);
  g_assert_cmpuint(g_variant_n_children(arr), ==, 1);
  g_autoptr(GVariant) meta = g_variant_get_child_value(arr, 0);
  const char *name = NULL;
  g_assert_true(g_variant_lookup(meta, "name", "&s", &name));
  g_assert_cmpstr(name, ==, "Alice <&>"); /* raw: the Shell escapes */
  g_autoptr(GVariant) icon = g_variant_lookup_value(meta, "icon", NULL);
  g_autoptr(GIcon) gi = g_icon_deserialize(icon);
  g_assert_true(G_IS_ICON(gi));

  /* free text + subsearch narrowing */
  g_auto(GStrv) text = initial("hello", NULL);
  g_assert_cmpuint(g_strv_length(text), ==, 1);
  const char *refined[] = {"hello", "overview", NULL};
  g_autoptr(GVariant) sub = call("GetSubsearchResultSet",
                                 g_variant_new("(^as^as)", text, refined), "(as)", NULL);
  g_auto(GStrv) subids = NULL;
  g_variant_get(sub, "(^as)", &subids);
  g_assert_cmpuint(g_strv_length(subids), ==, 1);

  /* activation goes through org.nostr.Dispatcher1.Open; an nsec id is
   * refused (the following valid call acts as an ordering barrier) */
  const char *no_terms[] = {NULL};
  g_autoptr(GVariant) a1 = call("ActivateResult",
      g_variant_new("(s^asu)", "nostr:nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5",
                    no_terms, 0), NULL, NULL);
  g_autoptr(GVariant) a2 = call("ActivateResult", g_variant_new("(s^asu)", text[0], no_terms, 0),
                                NULL, NULL);
  g_assert_true(wait_opened(1));
  g_assert_cmpuint(opened->len, ==, 1);
  g_assert_cmpstr(opened->pdata[0], ==, text[0]);

  g_autoptr(GVariant) ls = call("LaunchSearch", g_variant_new("(^asu)", refined, 0), NULL, NULL);
  g_autoptr(GVariant) xc = call("XUbuntuCancel", NULL, NULL, NULL);

  kill(pid, SIGTERM);
  g_spawn_close_pid(pid);
  nsp_mock_relay_free(m);

  /* a relay that never answers: the call still returns by the deadline */
  NspMockRelay *silent = nsp_mock_relay_new(NSP_MOCK_SILENT, TRUE);
  pid = spawn_daemon(nsp_mock_relay_socket(silent), home);
  g_auto(GStrv) s1 = initial(np + strlen("nostr:"), &ms);
  g_assert_cmpint(ms, <, 1000);
  g_assert_cmpuint(g_strv_length(s1), ==, 1); /* bare "Open Nostr profile" */
  g_auto(GStrv) s2 = initial("hello", &ms);
  g_assert_cmpint(ms, <, 200); /* breaker open */
  g_assert_cmpuint(g_strv_length(s2), ==, 0);
  kill(pid, SIGTERM);
  g_spawn_close_pid(pid);
  nsp_mock_relay_free(silent);

  g_bus_unown_name(own);
  g_dbus_connection_unregister_object(bus, reg);
  g_ptr_array_unref(opened);
  g_clear_object(&bus);
  g_rmdir(home);
  g_test_dbus_down(tdb);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nsp/dbus/e2e", test_e2e);
  return g_test_run();
}
