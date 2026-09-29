/* KC-6 (charter §9.2): GhStoreKey's production backend against a real
 * gnome-keyring: store, lookup and delete round-trip, the item's schema,
 * attributes, label and secret as another Secret Service client sees them,
 * per-account isolation, and the locked keyring in background mode. Before
 * the keyring starts, it also checks KC-4 on a live bus that has no Secret
 * Service.
 *
 * By default the test is hermetic: it starts a private session bus and its
 * own `gnome-keyring-daemon --unlock` with a throwaway HOME/XDG tree, so it
 * never touches the developer's keyring. Both daemons are bound to this
 * process's lifetime and the tree is removed however the test ends (see
 * "Private daemons" below). Without dbus-daemon it
 * exits 77 at once; without gnome-keyring-daemon it runs the bus-only check
 * and then exits 77, since KC-6 itself could not run.
 *
 * With GH_STORE_KEY_TEST_SESSION_KEYRING=1 it instead uses the ambient
 * session bus, which must already have an unlocked Secret Service (CI runs
 * it under dbus-run-session after `gnome-keyring-daemon --unlock`); a missing
 * service is then a failure, not a skip. That mode is for throwaway sessions
 * only: it locks the default keyring at the end and leaves one locked test
 * item there (and a failing run may leave more). */
#include "fake-secret.h"
#include "nostrc-test-bus.h"

#include <glib/gstdio.h>
#include <libsecret/secret.h>
#include <string.h>

/* Written out independently of gh-store-key.c: a drift tripwire. */
static const SecretSchema expected_schema = {
  .name = "org.nostr.Groundhog.StoreKey",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "store-id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "version", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 },
  },
};

/* The session bus, referenced for the whole run (see main()). */
static GDBusConnection *session_bus;
/* Hermetic mode only. */
static NostrcTestBus *private_bus;
static const gchar *tmp_root; /* the bus's throwaway directory */
static gboolean keyring_started;
static gchar *keyring_program; /* NULL: gnome-keyring-daemon is not installed */

static void ensure_keyring(void);

static gchar *
random_account(void)
{
  GString *hex = g_string_sized_new(64);
  for (guint i = 0; i < 32; i++)
    g_string_append_printf(hex, "%02x", (guint)g_random_int_range(0, 256));
  return g_string_free(hex, FALSE);
}

/* The account's items as any Secret Service client sees them. */
static GList *
raw_items(const gchar *account, gboolean load_secrets)
{
  GHashTable *attributes = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attributes, "account", (gpointer)account);
  GError *error = NULL;
  GList *items = secret_service_search_sync(
    NULL, &expected_schema, attributes,
    SECRET_SEARCH_ALL | (load_secrets ? SECRET_SEARCH_LOAD_SECRETS : 0), NULL, &error);
  g_assert_no_error(error);
  g_hash_table_unref(attributes);
  return items;
}

static void
assert_item(const gchar *account, const GhTestKeyResult *expected)
{
  GList *items = raw_items(account, TRUE);
  g_assert_cmpuint(g_list_length(items), ==, 1);
  SecretItem *item = items->data;
  g_assert_false(secret_item_get_locked(item));
  gchar *schema_name = secret_item_get_schema_name(item);
  g_assert_cmpstr(schema_name, ==, "org.nostr.Groundhog.StoreKey");
  g_free(schema_name);

  GHashTable *attributes = secret_item_get_attributes(item);
  g_assert_cmpstr(g_hash_table_lookup(attributes, "account"), ==, account);
  g_assert_cmpstr(g_hash_table_lookup(attributes, "store-id"), ==, expected->store_id);
  g_assert_cmpstr(g_hash_table_lookup(attributes, "version"), ==, "1");
  GHashTableIter iter;
  gpointer name;
  g_hash_table_iter_init(&iter, attributes);
  while (g_hash_table_iter_next(&iter, &name, NULL))
    g_assert_true(g_str_equal(name, "account") || g_str_equal(name, "store-id") ||
                  g_str_equal(name, "version") || g_str_equal(name, "xdg:schema"));
  g_hash_table_unref(attributes);

  /* The label reveals nothing about the account. */
  gchar *label = secret_item_get_label(item);
  g_assert_cmpstr(label, ==, "Groundhog message storage key");
  g_free(label);

  /* The raw 32 bytes survive the service byte for byte (gnome-keyring does
   * not persist content types, so only the bytes are compared). */
  SecretValue *value = secret_item_get_secret(item);
  g_assert_nonnull(value);
  gsize len = 0;
  const gchar *data = secret_value_get(value, &len);
  g_assert_cmpmem(data, len, g_bytes_get_data(expected->key, NULL), GH_STORE_KEY_SIZE);
  secret_value_unref(value);
  g_list_free_full(items, g_object_unref);
}

static void
assert_no_items(const gchar *account)
{
  GList *items = raw_items(account, FALSE);
  g_assert_cmpuint(g_list_length(items), ==, 0);
  g_list_free_full(items, g_object_unref);
}

static void
assert_error(const GhTestKeyResult *result, gint code)
{
  g_assert_error(result->error, GH_STORE_KEY_ERROR, code);
  g_assert_null(result->key);
}

static void
test_round_trip(void)
{
  ensure_keyring();
  GhStoreKey *store_key = gh_store_key_new(NULL);
  gchar *a = random_account(), *b = random_account();

  GhTestKeyResult missing = gh_test_lookup(store_key, a, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_error(&missing, GH_STORE_KEY_ERROR_NOT_FOUND);
  assert_no_items(a);

  /* Background first open: created in the unlocked default keyring. */
  GhTestKeyResult created = gh_test_lookup_or_create(store_key, a, GH_STORE_KEY_FLAGS_NONE, NULL);
  g_assert_no_error(created.error);
  g_assert_true(created.created);
  g_assert_cmpuint(g_bytes_get_size(created.key), ==, GH_STORE_KEY_SIZE);
  g_assert_true(g_uuid_string_is_valid(created.store_id));
  assert_item(a, &created);

  GhTestKeyResult reopened = gh_test_lookup_or_create(store_key, a, GH_STORE_KEY_FLAGS_NONE, NULL);
  g_assert_no_error(reopened.error);
  g_assert_false(reopened.created);
  g_assert_true(g_bytes_equal(reopened.key, created.key));
  g_assert_cmpstr(reopened.store_id, ==, created.store_id);
  GhTestKeyResult looked_up = gh_test_lookup(store_key, a, GH_STORE_KEY_FLAGS_INTERACTIVE, NULL);
  g_assert_no_error(looked_up.error);
  g_assert_true(g_bytes_equal(looked_up.key, created.key));

  /* Interactive first open goes through the service's own store path. */
  GhTestKeyResult other = gh_test_lookup_or_create(store_key, b, GH_STORE_KEY_FLAGS_INTERACTIVE, NULL);
  g_assert_no_error(other.error);
  g_assert_true(other.created);
  g_assert_false(g_bytes_equal(other.key, created.key));
  g_assert_cmpstr(other.store_id, !=, created.store_id);
  assert_item(b, &other);

  /* Forget one account: its item is gone, the other's is untouched. */
  GError *error = NULL;
  g_assert_true(gh_test_destroy(store_key, a, GH_STORE_KEY_FLAGS_NONE, &error));
  g_assert_no_error(error);
  assert_no_items(a);
  GhTestKeyResult gone = gh_test_lookup(store_key, a, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_error(&gone, GH_STORE_KEY_ERROR_NOT_FOUND);
  g_assert_true(gh_test_destroy(store_key, a, GH_STORE_KEY_FLAGS_NONE, &error));
  g_assert_no_error(error);
  assert_item(b, &other);
  GhTestKeyResult b_again = gh_test_lookup(store_key, b, GH_STORE_KEY_FLAGS_NONE, NULL);
  g_assert_no_error(b_again.error);
  g_assert_true(g_bytes_equal(b_again.key, other.key));

  g_assert_true(gh_test_destroy(store_key, b, GH_STORE_KEY_FLAGS_INTERACTIVE, &error));
  g_assert_no_error(error);
  assert_no_items(b);

  /* An item another client wrote under the schema with a 16-byte secret is
   * refused, not truncated or padded into a key, and forget removes it. */
  gchar *e = random_account();
  gchar *store_id = g_uuid_string_random();
  GHashTable *attributes = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attributes, "account", e);
  g_hash_table_insert(attributes, "store-id", store_id);
  g_hash_table_insert(attributes, "version", "1");
  SecretValue *short_value = secret_value_new("0123456789abcdef", 16, "application/octet-stream");
  g_assert_true(secret_service_store_sync(NULL, &expected_schema, attributes,
                                          SECRET_COLLECTION_DEFAULT, "malformed", short_value,
                                          NULL, &error));
  g_assert_no_error(error);
  secret_value_unref(short_value);
  g_hash_table_unref(attributes);
  GhTestKeyResult malformed = gh_test_lookup_or_create(store_key, e, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_error(&malformed, GH_STORE_KEY_ERROR_INVALID);
  g_assert_true(gh_test_destroy(store_key, e, GH_STORE_KEY_FLAGS_NONE, &error));
  g_assert_no_error(error);
  assert_no_items(e);
  gh_test_key_result_clear(&malformed);
  g_free(store_id);
  g_free(e);

  GhTestKeyResult *results[] = { &missing, &created, &reopened, &looked_up, &other, &gone,
                                 &b_again };
  for (guint i = 0; i < G_N_ELEMENTS(results); i++)
    gh_test_key_result_clear(results[i]);
  g_free(a);
  g_free(b);
  g_object_unref(store_key);
}

/* KC-2 against the real service: once the default keyring is locked, the
 * background calls report LOCKED without prompting (there is no prompter
 * here, so a prompt would fail or hang), create nothing and delete nothing.
 * Runs last: nothing unlocks the keyring again. */
static void
test_locked_background(void)
{
  ensure_keyring();
  GhStoreKey *store_key = gh_store_key_new(NULL);
  gchar *c = random_account(), *d = random_account();
  GhTestKeyResult created = gh_test_lookup_or_create(store_key, c, GH_STORE_KEY_FLAGS_NONE, NULL);
  g_assert_no_error(created.error);

  GError *error = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &error);
  g_assert_no_error(error);
  SecretCollection *collection = secret_collection_for_alias_sync(
    service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(collection);
  GList *objects = g_list_append(NULL, collection);
  GList *locked = NULL;
  secret_service_lock_sync(service, objects, NULL, &locked, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(g_list_length(locked), ==, 1);
  g_list_free_full(locked, g_object_unref);
  g_list_free(objects);
  g_object_unref(collection);
  g_object_unref(service);

  GList *items = raw_items(c, FALSE);
  g_assert_cmpuint(g_list_length(items), ==, 1);
  g_assert_true(secret_item_get_locked(items->data));
  g_list_free_full(items, g_object_unref);

  GhTestKeyResult lookup = gh_test_lookup(store_key, c, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_error(&lookup, GH_STORE_KEY_ERROR_LOCKED);
  GhTestKeyResult reopen = gh_test_lookup_or_create(store_key, c, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_error(&reopen, GH_STORE_KEY_ERROR_LOCKED);
  GhTestKeyResult fresh = gh_test_lookup_or_create(store_key, d, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_error(&fresh, GH_STORE_KEY_ERROR_LOCKED);
  assert_no_items(d);
  g_assert_false(gh_test_destroy(store_key, c, GH_STORE_KEY_FLAGS_NONE, &error));
  g_assert_error(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_LOCKED);
  g_clear_error(&error);
  items = raw_items(c, FALSE);
  g_assert_cmpuint(g_list_length(items), ==, 1);
  g_list_free_full(items, g_object_unref);

  GhTestKeyResult *results[] = { &created, &lookup, &reopen, &fresh };
  for (guint i = 0; i < G_N_ELEMENTS(results); i++)
    gh_test_key_result_clear(results[i]);
  g_free(c);
  g_free(d);
  g_object_unref(store_key);
}

static void
on_name_appeared(GDBusConnection *connection, const gchar *name, const gchar *owner,
                 gpointer user_data)
{
  (void)connection;
  (void)name;
  (void)owner;
  *(gboolean *)user_data = TRUE;
}

static gboolean
on_deadline(gpointer user_data)
{
  *(gboolean *)user_data = TRUE;
  return G_SOURCE_REMOVE;
}

/* Wait (bounded) for org.freedesktop.secrets to be owned on the session bus. */
static gboolean
wait_for_secret_service(guint seconds)
{
  gboolean appeared = FALSE, expired = FALSE;
  guint watch = g_bus_watch_name_on_connection(session_bus, "org.freedesktop.secrets",
                                               G_BUS_NAME_WATCHER_FLAGS_NONE, on_name_appeared,
                                               NULL, &appeared, NULL);
  guint deadline = g_timeout_add_seconds(seconds, on_deadline, &expired);
  while (!appeared && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_bus_unwatch_name(watch);
  if (!expired)
    g_source_remove(deadline);
  return appeared;
}

/* ---- Private daemons -------------------------------------------------------
 *
 * The bus and gnome-keyring run under tests/common/nostrc-test-bus.h: each is
 * stopped however this process ends (teardown, abort, SIGKILL), logs to a
 * file in the bus's throwaway directory rather than to ctest's pipes, and
 * that directory (which also holds HOME and the XDG dirs) is removed once
 * both have stopped. GTestDBus is not used: on macOS its crash watcher never
 * notices the test dying, orphaning a dbus-daemon that keeps ctest's output
 * pipe open until the test times out. */
static void
start_private_bus(void)
{
  private_bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(private_bus);
  tmp_root = nostrc_test_bus_get_dir(private_bus);
  static const gchar *vars[] = { "HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                 "XDG_RUNTIME_DIR" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    gchar *dir = g_build_filename(tmp_root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
    g_free(dir);
  }
}

/* Hermetic mode: start gnome-keyring on the private bus the first time a
 * KC-6 case needs it. Its login keyring is created with the password on its
 * stdin. A no-op in session mode. */
static void
ensure_keyring(void)
{
  if (!private_bus || keyring_started)
    return;
  g_assert_nonnull(keyring_program);
  GError *error = NULL;
  gchar *password = g_build_filename(tmp_root, "keyring-password", NULL);
  g_assert_true(g_file_set_contents_full(password, "kc6-test", 8, G_FILE_SET_CONTENTS_NONE, 0600,
                                         &error));
  const gchar *argv[] = { keyring_program, "--foreground", "--unlock", "--components=secrets",
                          NULL };
  nostrc_test_bus_spawn_supervised(private_bus, "gnome-keyring.log", password, argv);
  keyring_started = TRUE;
  g_free(password);
  if (!wait_for_secret_service(30)) {
    nostrc_test_bus_dump_log(private_bus, "gnome-keyring.log");
    g_error("org.freedesktop.secrets did not appear on the private session bus");
  }
}

static void
stop_private_daemons(void)
{
  /* Quiesce D-Bus first: once libsecret's proxies are gone the daemons
   * disappearing only marks our connection closed (the bus flushes it). */
  secret_service_disconnect();
  gh_test_run_until_idle();
  nostrc_test_bus_down(private_bus);
  private_bus = NULL;
  tmp_root = NULL;
}

/* KC-4 on a live bus: nothing owns org.freedesktop.secrets (the keyring is
 * not started yet), so every operation is UNAVAILABLE, interactive or not. */
static void
test_bus_without_secret_service(void)
{
  g_assert_false(keyring_started);
  GhStoreKey *store_key = gh_store_key_new(NULL);
  gchar *account = random_account();
  for (guint interactive = 0; interactive < 2; interactive++) {
    GhStoreKeyFlags flags = interactive ? GH_STORE_KEY_FLAGS_INTERACTIVE : GH_STORE_KEY_FLAGS_NONE;
    GhTestKeyResult open = gh_test_lookup_or_create(store_key, account, flags, NULL);
    assert_error(&open, GH_STORE_KEY_ERROR_UNAVAILABLE);
    GhTestKeyResult lookup = gh_test_lookup(store_key, account, flags, NULL);
    assert_error(&lookup, GH_STORE_KEY_ERROR_UNAVAILABLE);
    GError *error = NULL;
    g_assert_false(gh_test_destroy(store_key, account, flags, &error));
    g_assert_error(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_UNAVAILABLE);
    g_clear_error(&error);
    gh_test_key_result_clear(&open);
    gh_test_key_result_clear(&lookup);
  }
  g_free(account);
  g_object_unref(store_key);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  gboolean session = g_strcmp0(g_getenv("GH_STORE_KEY_TEST_SESSION_KEYRING"), "1") == 0;
  if (!session) {
    if (!nostrc_test_bus_available()) {
      g_print("SKIP: no Secret Service available (dbus-daemon is not installed)\n");
      return 77;
    }
    keyring_program = g_find_program_in_path("gnome-keyring-daemon");
    start_private_bus();
  }
  /* One session-bus connection for the whole run, which libsecret shares
   * (it is the GLib singleton) and which is never closed or finalized before
   * exit. GDBus closing a connection can close its socket while the socket's
   * read source is still polled; on macOS that select() fails with EBADF and
   * the warning aborts the test. A peer that goes away only marks it closed.
   * In hermetic mode this is the connection nostrc-test-bus already holds; it
   * releases it once the bus is gone, so ours is dropped just before that. */
  GError *error = NULL;
  session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error(error);
  g_dbus_connection_set_exit_on_close(session_bus, FALSE);
  if (session && !wait_for_secret_service(30)) {
    g_printerr("FAIL: org.freedesktop.secrets did not appear on the ambient session bus\n");
    return 1;
  }

  gboolean kc6 = session || keyring_program != NULL;
  if (!session)
    g_test_add_func("/store-key/kc4/bus-without-secret-service", test_bus_without_secret_service);
  if (kc6) {
    g_test_add_func("/store-key/kc6/round-trip", test_round_trip);
    g_test_add_func("/store-key/kc6/locked-background", test_locked_background);
  }
  int status = g_test_run();
  if (session) {
    secret_service_disconnect();
    g_dbus_connection_flush_sync(session_bus, NULL, NULL);
  } else {
    g_clear_object(&session_bus);
    stop_private_daemons();
  }
  g_free(keyring_program);
  if (status == 0 && !kc6) {
    g_print("SKIP: KC-6 needs gnome-keyring-daemon (the bus-only check passed)\n");
    return 77;
  }
  return status;
}
