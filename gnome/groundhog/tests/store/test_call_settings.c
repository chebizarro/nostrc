/* Same async contract against an explicitly referenced native Keychain on
 * macOS, and the private-bus Secret Service elsewhere. No real credentials. */
#include "gh-call-settings.h"
#include "nostrc-test-bus.h"
#include "turn-test-keychain.h"
#include <libsecret/secret.h>

static const gchar *url = "turns://turn.example.net:5349?transport=tcp";
static GAsyncResult *pending;
static GMainLoop *loop;

static void
done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source; (void)data;
  pending = g_object_ref(result);
  g_main_loop_quit(loop);
}

static gboolean
expired(gpointer data)
{
  (void)data;
  g_error("TURN async operation timed out");
  return G_SOURCE_REMOVE;
}

static GAsyncResult *
wait_result(void)
{
  guint timeout = g_timeout_add_seconds(15, expired, NULL);
  g_main_loop_run(loop);
  g_source_remove(timeout);
  return g_steal_pointer(&pending);
}

static void
store(const gchar *server, const gchar *username, const gchar *password)
{
  gh_call_turn_password_store_async(server, username, password, NULL, done, NULL);
  g_autoptr(GAsyncResult) result = wait_result();
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_call_turn_password_store_finish(result, &error));
  g_assert_no_error(error);
}

static void
lookup(const gchar *server, const gchar *username, const gchar *expected)
{
  gh_call_turn_password_lookup_async(server, username, NULL, done, NULL);
  g_autoptr(GAsyncResult) result = wait_result();
  g_autoptr(GError) error = NULL;
  gchar *password = gh_call_turn_password_lookup_finish(result, &error);
  if (expected) g_assert_no_error(error);
  else g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpstr(password, ==, expected);
  gh_call_turn_password_free(password);
}

static void
delete(const gchar *server, const gchar *username)
{
  gh_call_turn_password_delete_async(server, username, NULL, done, NULL);
  g_autoptr(GAsyncResult) result = wait_result();
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_call_turn_password_delete_finish(result, &error));
  g_assert_no_error(error);
}

static void
test_roundtrip(void)
{
  const gchar *other = "turn://turn.example.net:3478";
  lookup(url, "alice", NULL);
  delete(url, "alice"); /* missing delete is successful */
  store(url, "alice", "variable-length TURN password 🔐");
  store(url, "bob", "other-user");
  store(other, "alice", "other-server");
  lookup(url, "alice", "variable-length TURN password 🔐");
  lookup(url, "bob", "other-user");
  lookup(other, "alice", "other-server");
  store(url, "alice", "replacement");
  lookup(url, "alice", "replacement");
  delete(url, "alice");
  lookup(url, "alice", NULL);
  lookup(url, "bob", "other-user");
  lookup(other, "alice", "other-server");
  delete(url, "alice");
  delete(url, "bob");
  delete(other, "alice");
}

static void
test_invalid_and_cancelled(void)
{
  g_autoptr(GError) error = NULL;
  gh_call_turn_password_store_async(url, "alice", "", NULL, done, NULL);
  g_autoptr(GAsyncResult) result = wait_result();
  g_assert_false(gh_call_turn_password_store_finish(result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_clear_object(&result);
  gh_call_turn_password_lookup_async("turn://user:secret@turn.example.net", "alice", NULL, done, NULL);
  result = wait_result();
  g_assert_null(gh_call_turn_password_lookup_finish(result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null(strstr(error->message, "user:secret"));
  g_clear_error(&error);
  g_clear_object(&result);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  gh_call_turn_password_store_async(url, "cancelled", "not-stored", cancel, done, NULL);
  result = wait_result();
  g_assert_false(gh_call_turn_password_store_finish(result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  lookup(url, "cancelled", NULL);
}

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static void
test_locked(void)
{
  store(url, "locked", "retained");
  g_assert_cmpint(SecKeychainLock(turn_test_keychain), ==, errSecSuccess);
  for (guint op = 0; op < 3; op++) {
    if (op == 0) gh_call_turn_password_lookup_async(url, "locked", NULL, done, NULL);
    if (op == 1) gh_call_turn_password_store_async(url, "locked", "new", NULL, done, NULL);
    if (op == 2) gh_call_turn_password_delete_async(url, "locked", NULL, done, NULL);
    g_autoptr(GAsyncResult) result = wait_result();
    g_autoptr(GError) error = NULL;
    if (op == 0) g_assert_null(gh_call_turn_password_lookup_finish(result, &error));
    if (op == 1) g_assert_false(gh_call_turn_password_store_finish(result, &error));
    if (op == 2) g_assert_false(gh_call_turn_password_delete_finish(result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  }
  g_assert_cmpint(SecKeychainUnlock(turn_test_keychain, 4, "test", TRUE), ==, errSecSuccess);
  lookup(url, "locked", "retained");
  delete(url, "locked");
}
#pragma clang diagnostic pop

#ifndef GH_CALL_TURN_DEFAULT_SECRET_SERVICE
static void
test_default_refused(void)
{
  gh_call_turn_password_set_test_keychain(NULL);
  for (guint op = 0; op < 3; op++) {
    if (op == 0) gh_call_turn_password_lookup_async(url, "alice", NULL, done, NULL);
    if (op == 1) gh_call_turn_password_store_async(url, "alice", "not-stored", NULL, done, NULL);
    if (op == 2) gh_call_turn_password_delete_async(url, "alice", NULL, done, NULL);
    g_autoptr(GAsyncResult) result = wait_result();
    g_autoptr(GError) error = NULL;
    if (op == 0) g_assert_null(gh_call_turn_password_lookup_finish(result, &error));
    if (op == 1) g_assert_false(gh_call_turn_password_store_finish(result, &error));
    if (op == 2) g_assert_false(gh_call_turn_password_delete_finish(result, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_nonnull(strstr(error->message, "Refusing"));
  }
  gh_call_turn_password_set_test_keychain(turn_test_keychain);
}
#endif
#else
static void
service_appeared(GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer data)
{
  (void)connection; (void)name; (void)owner; (void)data;
  g_main_loop_quit(loop);
}

static void
start_secret_service(NostrcTestBus *bus)
{
  g_autofree gchar *path = g_build_filename(nostrc_test_bus_get_dir(bus), "secrets.json", NULL);
  const gchar *argv[] = { g_getenv("SECRET_SERVICE_PYTHON"), g_getenv("SECRET_SERVICE_SCRIPT"), path, NULL };
  g_assert_nonnull(argv[0]);
  g_assert_nonnull(argv[1]);
  nostrc_test_bus_spawn_supervised(bus, "secret-service.log", NULL, argv);
  guint watch = g_bus_watch_name(G_BUS_TYPE_SESSION, "org.freedesktop.secrets", 0,
                                 service_appeared, NULL, NULL, NULL);
  guint timeout = g_timeout_add_seconds(15, expired, NULL);
  g_main_loop_run(loop);
  g_source_remove(timeout);
  g_bus_unwatch_name(watch);
}
#endif

int
main(int argc, char **argv)
{
  g_setenv("NOSTRC_TEST_ISOLATION", "1", TRUE);
#ifndef __APPLE__
  /* Match the Preferences test's process boundary: libsecret caches loaded
   * collections for the client's lifetime. The parent owns and checks the
   * bus; only the child loads libsecret collections. */
  if (!g_getenv("GH_CALL_SETTINGS_CHILD")) {
    loop = g_main_loop_new(NULL, FALSE);
    g_assert_true(nostrc_test_bus_available());
    NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
    nostrc_test_bus_up(bus);
    start_secret_service(bus);
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_setenv(launcher, "GH_CALL_SETTINGS_CHILD", "1", TRUE);
    g_autoptr(GSubprocess) child =
      g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv, &error);
    g_assert_no_error(error);
    g_subprocess_wait(child, NULL, &error);
    g_assert_no_error(error);
    int result = g_subprocess_get_if_exited(child) ? g_subprocess_get_exit_status(child) : 1;
    nostrc_test_bus_down(bus);
    g_main_loop_unref(loop);
    return result;
  }
#endif
  g_test_init(&argc, &argv, NULL);
  loop = g_main_loop_new(NULL, FALSE);
#ifdef __APPLE__
  turn_test_keychain_setup();
  g_test_add_func("/calls/turn/keychain-locked", test_locked);
#ifndef GH_CALL_TURN_DEFAULT_SECRET_SERVICE
  g_test_add_func("/calls/turn/default-keychain-refused", test_default_refused);
#endif
#endif
  g_test_add_func("/calls/turn/roundtrip", test_roundtrip);
  g_test_add_func("/calls/turn/invalid-and-cancelled", test_invalid_and_cancelled);
  int result = g_test_run();
  secret_service_disconnect();
  g_main_loop_unref(loop);
  return result;
}
