/* KC-6 (charter §9.2): GhStoreKey's production backend against a real
 * gnome-keyring: store, lookup and delete round-trip, the item's schema,
 * attributes, label and secret as another Secret Service client sees them,
 * per-account isolation, and the locked keyring in background mode.
 *
 * By default the test is hermetic: it starts a private session bus
 * (GTestDBus) and its own `gnome-keyring-daemon --unlock` with a throwaway
 * HOME/XDG tree, so it never touches the developer's keyring. It exits 77
 * only when no Secret Service can be started (gnome-keyring-daemon or
 * dbus-daemon is not installed).
 *
 * With GH_STORE_KEY_TEST_SESSION_KEYRING=1 it instead uses the ambient
 * session bus, which must already have an unlocked Secret Service (CI runs
 * it under dbus-run-session after `gnome-keyring-daemon --unlock`); a missing
 * service is then a failure, not a skip. That mode is for throwaway sessions
 * only: it locks the default keyring at the end and leaves one locked test
 * item there (and a failing run may leave more). */
#include "fake-secret.h"

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

static GTestDBus *private_bus;
static GSubprocess *keyring;
static gchar *tmp_root;

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
  guint watch = g_bus_watch_name(G_BUS_TYPE_SESSION, "org.freedesktop.secrets",
                                 G_BUS_NAME_WATCHER_FLAGS_NONE, on_name_appeared, NULL,
                                 &appeared, NULL);
  guint deadline = g_timeout_add_seconds(seconds, on_deadline, &expired);
  while (!appeared && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_bus_unwatch_name(watch);
  if (!expired)
    g_source_remove(deadline);
  return appeared;
}

static void
start_private_keyring(const gchar *daemon)
{
  GError *error = NULL;
  tmp_root = g_dir_make_tmp("gh-store-key-keyring-XXXXXX", &error);
  g_assert_no_error(error);
  static const gchar *vars[] = { "HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                 "XDG_RUNTIME_DIR" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    gchar *dir = g_build_filename(tmp_root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
    g_free(dir);
  }
  private_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(private_bus); /* exports DBUS_SESSION_BUS_ADDRESS */

  /* The launcher inherits the private bus and the throwaway XDG tree. The
   * login keyring is created (or unlocked) with the password on stdin. */
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                                            G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
  keyring = g_subprocess_launcher_spawn(launcher, &error, daemon, "--foreground", "--unlock",
                                        "--components=secrets", NULL);
  g_object_unref(launcher);
  g_assert_no_error(error);
  GOutputStream *stdin_pipe = g_subprocess_get_stdin_pipe(keyring);
  g_assert_true(g_output_stream_write_all(stdin_pipe, "kc6-test", 8, NULL, NULL, &error));
  g_assert_true(g_output_stream_close(stdin_pipe, NULL, &error));
  g_assert_no_error(error);
}

static void
stop_private_keyring(void)
{
  /* libsecret's service singleton keeps the bus connection referenced, so
   * stop the bus without GTestDBus's connection-leak check. */
  secret_service_disconnect();
  GDBusConnection *singleton = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (singleton) {
    g_dbus_connection_set_exit_on_close(singleton, FALSE);
    g_object_unref(singleton);
  }
  g_subprocess_force_exit(keyring);
  (void)g_subprocess_wait(keyring, NULL, NULL);
  g_clear_object(&keyring);
  g_test_dbus_stop(private_bus);
  gh_test_remove_tree(tmp_root);
  g_free(tmp_root);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  gboolean session = g_strcmp0(g_getenv("GH_STORE_KEY_TEST_SESSION_KEYRING"), "1") == 0;
  if (!session) {
    gchar *daemon = g_find_program_in_path("gnome-keyring-daemon");
    gchar *dbus_daemon = g_find_program_in_path("dbus-daemon");
    if (!daemon || !dbus_daemon) {
      g_print("SKIP: no Secret Service available (%s is not installed)\n",
              daemon ? "dbus-daemon" : "gnome-keyring-daemon");
      g_free(daemon);
      g_free(dbus_daemon);
      return 77;
    }
    start_private_keyring(daemon);
    g_free(daemon);
    g_free(dbus_daemon);
  }
  if (!wait_for_secret_service(30)) {
    g_printerr("FAIL: org.freedesktop.secrets did not appear on the %s session bus\n",
               session ? "ambient" : "private");
    if (!session)
      stop_private_keyring();
    return 1;
  }

  g_test_add_func("/store-key/kc6/round-trip", test_round_trip);
  g_test_add_func("/store-key/kc6/locked-background", test_locked_background);
  int status = g_test_run();
  if (session)
    secret_service_disconnect();
  else
    stop_private_keyring();
  return status;
}
