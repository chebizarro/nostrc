#include "gh-nip46-credentials.h"
#include "nostrc-test-bus.h"
#include <libsecret/secret.h>
#include <glib/gstdio.h>
#include <string.h>

/* nostrc-eixk6: libsecret 0.21.4 secret_collection_signal() breaks out
 * of its Items scan on an existing path without unrefing the full reference
 * from g_variant_iter_next_value(). GIO allocated that path before the
 * libsecret callback, so its allocation stack has no libsecret frame.
 * Its static signal handler is stripped from Ubuntu's libsecret binary,
 * so the precise lsan.supp rule cannot match in CI without debug symbols.
 * Keep the necessary module fallback and this GIO allocation-site rule
 * local to this executable, never suite-wide in lsan.supp. */
const char *
__lsan_default_suppressions(void)
{
  return "leak:libsecret-1.so.0\nleak:g_variant_new_object_path\n";
}

static const SecretSchema schema = {
  .name = "org.nostr.Groundhog.Nip46Credential", .flags = SECRET_SCHEMA_NONE,
  .attributes = { { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
                  { "version", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } }
};
static NostrcTestBus *bus;
static gchar *daemon_program;
static GMainLoop *loop;
static gboolean keyring_started;
static void ensure_keyring(void);
static const gchar *account_a = "1111111111111111111111111111111111111111111111111111111111111111";
static const gchar *account_b = "2222222222222222222222222222222222222222222222222222222222222222";
static const gchar *signer_a = "3333333333333333333333333333333333333333333333333333333333333333";
static const gchar *signer_b = "4444444444444444444444444444444444444444444444444444444444444444";
static const gchar *client_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const gchar *client_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static GhNip46Credential *
credential(const gchar *account, const gchar *signer, const gchar *secret)
{
  const gchar *relays[] = { "wss://nos.lol", "wss://relay.nostr.band", NULL };
  GError *error = NULL;
  GhNip46Credential *c = gh_nip46_credential_new(account, signer, secret, relays, &error);
  g_assert_no_error(error);
  return c;
}

typedef struct { GError *error; GhNip46Credential *credential; GPtrArray *list; gboolean ok; } Result;
static void
on_lookup(GObject *source, GAsyncResult *r, gpointer data)
{
  Result *out = data;
  out->credential = gh_nip46_credential_store_lookup_finish(GH_NIP46_CREDENTIAL_STORE(source), r, &out->error);
  g_main_loop_quit(loop);
}
static void
on_store(GObject *source, GAsyncResult *r, gpointer data)
{
  Result *out = data;
  out->ok = gh_nip46_credential_store_store_finish(GH_NIP46_CREDENTIAL_STORE(source), r, &out->error);
  g_main_loop_quit(loop);
}
static void
on_delete(GObject *source, GAsyncResult *r, gpointer data)
{
  Result *out = data;
  out->ok = gh_nip46_credential_store_delete_finish(GH_NIP46_CREDENTIAL_STORE(source), r, &out->error);
  g_main_loop_quit(loop);
}
static void
on_list(GObject *source, GAsyncResult *r, gpointer data)
{
  Result *out = data;
  out->list = gh_nip46_credential_store_list_finish(GH_NIP46_CREDENTIAL_STORE(source), r, &out->error);
  g_main_loop_quit(loop);
}
static Result
lookup(GhNip46CredentialStore *store, const gchar *account, GCancellable *cancel)
{
  Result out = {0};
  gh_nip46_credential_store_lookup_async(store, account, cancel, on_lookup, &out);
  g_main_loop_run(loop);
  return out;
}
static Result
save(GhNip46CredentialStore *store, GhNip46Credential *c, GCancellable *cancel)
{
  Result out = {0};
  gh_nip46_credential_store_store_async(store, c, FALSE, cancel, on_store, &out);
  g_main_loop_run(loop);
  return out;
}
static Result
remove_item(GhNip46CredentialStore *store, const gchar *account, GCancellable *cancel)
{
  Result out = {0};
  gh_nip46_credential_store_delete_async(store, account, FALSE, cancel, on_delete, &out);
  g_main_loop_run(loop);
  return out;
}
static Result
list(GhNip46CredentialStore *store)
{
  Result out = {0};
  gh_nip46_credential_store_list_async(store, NULL, on_list, &out);
  g_main_loop_run(loop);
  return out;
}
static void
clear_result(Result *r)
{
  g_clear_error(&r->error);
  gh_nip46_credential_free(r->credential);
  if (r->list) g_ptr_array_unref(r->list);
}
static GList *
raw_items(const gchar *account)
{
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  if (account) g_hash_table_insert(attrs, "account", (gpointer)account);
  GError *error = NULL;
  GList *items = secret_service_search_sync(NULL, &schema, attrs,
      SECRET_SEARCH_ALL | SECRET_SEARCH_LOAD_SECRETS, NULL, &error);
  g_assert_no_error(error);
  g_hash_table_unref(attrs);
  return items;
}
static void
raw_store(const gchar *account, const gchar *version, const gchar *json)
{
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, "account", (gpointer)account);
  g_hash_table_insert(attrs, "version", (gpointer)version);
  SecretValue *value = secret_value_new(json, -1, "application/json");
  GError *error = NULL;
  g_assert_true(secret_service_store_sync(NULL, &schema, attrs, SECRET_COLLECTION_DEFAULT,
                                          GH_NIP46_CREDENTIAL_LABEL, value, NULL, &error));
  g_assert_no_error(error);
  secret_value_unref(value);
  g_hash_table_unref(attrs);
}
static void
raw_clear(const gchar *account)
{
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, "account", (gpointer)account);
  GError *error = NULL;
  g_assert_true(secret_service_clear_sync(NULL, &schema, attrs, NULL, &error));
  g_assert_no_error(error);
  g_hash_table_unref(attrs);
}
static void
test_round_trip(void)
{
  ensure_keyring();
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_secret_service();
  Result missing = lookup(store, account_a, NULL);
  g_assert_error(missing.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND);
  clear_result(&missing);
  g_autoptr(GhNip46Credential) first = credential(account_a, signer_a, client_a);
  Result saved = save(store, first, NULL);
  g_assert_no_error(saved.error); g_assert_true(saved.ok); clear_result(&saved);
  Result found = lookup(store, account_a, NULL);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_client_secret_hex(found.credential), ==, client_a);
  g_assert_cmpstr(gh_nip46_credential_get_remote_signer_pubkey_hex(found.credential), ==, signer_a);
  clear_result(&found);
  GList *items = raw_items(account_a);
  g_assert_cmpuint(g_list_length(items), ==, 1);
  SecretItem *raw = items->data;
  gchar *label = secret_item_get_label(raw);
  g_assert_cmpstr(label, ==, GH_NIP46_CREDENTIAL_LABEL);
  g_assert_null(strstr(label, account_a)); g_assert_null(strstr(label, client_a));
  g_free(label);
  GHashTable *attrs = secret_item_get_attributes(raw);
  g_assert_cmpstr(g_hash_table_lookup(attrs, "account"), ==, account_a);
  g_assert_cmpstr(g_hash_table_lookup(attrs, "version"), ==, "1");
  GHashTableIter iter; gpointer key, value;
  g_hash_table_iter_init(&iter, attrs);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_assert_null(strstr((const gchar *)value, client_a));
    g_assert_true(g_str_equal(key, "account") || g_str_equal(key, "version") ||
                  g_str_equal(key, "xdg:schema"));
  }
  g_hash_table_unref(attrs);
  g_list_free_full(items, g_object_unref);
  Result listed = list(store);
  g_assert_no_error(listed.error); g_assert_cmpuint(listed.list->len, ==, 1);
  GhIdentityInfo *identity = g_ptr_array_index(listed.list, 0);
  g_assert_cmpint(identity->backend, ==, GH_SIGNER_BACKEND_NIP46);
  g_assert_true(g_str_has_prefix(identity->npub, "npub1"));
  clear_result(&listed);
  g_autoptr(GhNip46Credential) second = credential(account_a, signer_b, client_b);
  saved = save(store, second, NULL);
  g_assert_no_error(saved.error); g_assert_true(saved.ok); clear_result(&saved);
  items = raw_items(account_a); g_assert_cmpuint(g_list_length(items), ==, 1);
  g_list_free_full(items, g_object_unref);
  found = lookup(store, account_a, NULL);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_client_secret_hex(found.credential), ==, client_b);
  clear_result(&found);
  Result deleted = remove_item(store, account_a, NULL);
  g_assert_no_error(deleted.error); g_assert_true(deleted.ok); clear_result(&deleted);
  items = raw_items(account_a); g_assert_null(items);
  deleted = remove_item(store, account_a, NULL);
  g_assert_no_error(deleted.error); g_assert_true(deleted.ok); clear_result(&deleted);
  g_object_unref(store);
}
static void
test_newer_conflict(void)
{
  ensure_keyring();
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_secret_service();
  raw_store(account_a, "2", "{\"version\":2}");
  Result found = lookup(store, account_a, NULL);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
  clear_result(&found);
  g_autoptr(GhNip46Credential) c = credential(account_a, signer_a, client_a);
  Result saved = save(store, c, NULL);
  g_assert_error(saved.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
  clear_result(&saved);
  Result deleted = remove_item(store, account_a, NULL);
  g_assert_error(deleted.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
  clear_result(&deleted);
  GList *items = raw_items(account_a); g_assert_cmpuint(g_list_length(items), ==, 1);
  g_list_free_full(items, g_object_unref);
  raw_store(account_a, "0", "{\"version\":0}");
  found = lookup(store, account_a, NULL);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
  clear_result(&found);
  raw_clear(account_a);
  raw_store(account_a, "1", "{\"version\":1}");
  found = lookup(store, account_a, NULL);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_INVALID);
  clear_result(&found);
  saved = save(store, c, NULL);
  g_assert_error(saved.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_INVALID);
  clear_result(&saved);
  Result repaired = remove_item(store, account_a, NULL);
  g_assert_no_error(repaired.error); g_assert_true(repaired.ok); clear_result(&repaired);
  g_assert_null(raw_items(account_a));
  g_object_unref(store);
}
static void
test_interrupted(void)
{
  ensure_keyring();
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_secret_service();
  g_autoptr(GhNip46Credential) c = credential(account_b, signer_a, client_a);
  GCancellable *cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  Result saved = save(store, c, cancel);
  g_assert_error(saved.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); clear_result(&saved);
  Result missing = lookup(store, account_b, NULL);
  g_assert_error(missing.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND);
  clear_result(&missing);
  g_cancellable_reset(cancel);
  saved = save(store, c, NULL); g_assert_no_error(saved.error); clear_result(&saved);
  g_cancellable_cancel(cancel);
  Result deleted = remove_item(store, account_b, cancel);
  g_assert_error(deleted.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); clear_result(&deleted);
  Result found = lookup(store, account_b, NULL); g_assert_no_error(found.error); clear_result(&found);
  g_cancellable_reset(cancel);
  g_autoptr(GhNip46Credential) replacement = credential(account_b, signer_b, client_b);
  Result racing_store = {0};
  gh_nip46_credential_store_store_async(store, replacement, FALSE, cancel, on_store, &racing_store);
  g_cancellable_cancel(cancel); /* cancellation while the real backend is in flight */
  g_main_loop_run(loop);
  g_assert_true(racing_store.ok || g_error_matches(racing_store.error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
  found = lookup(store, account_b, NULL);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_client_secret_hex(found.credential), ==,
                  racing_store.ok ? client_b : client_a);
  clear_result(&found); clear_result(&racing_store);
  g_cancellable_reset(cancel);
  Result racing_delete = {0};
  gh_nip46_credential_store_delete_async(store, account_b, FALSE, cancel, on_delete, &racing_delete);
  g_cancellable_cancel(cancel);
  g_main_loop_run(loop);
  g_assert_true(racing_delete.ok || g_error_matches(racing_delete.error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
  found = lookup(store, account_b, NULL);
  if (racing_delete.ok) {
    g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND);
  } else {
    g_assert_no_error(found.error);
  }
  clear_result(&found); clear_result(&racing_delete);
  deleted = remove_item(store, account_b, NULL); g_assert_no_error(deleted.error); clear_result(&deleted);
  g_object_unref(cancel); g_object_unref(store);
}
static void
test_zz_locked(void)
{
  ensure_keyring();
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_secret_service();
  g_autoptr(GhNip46Credential) c = credential(account_a, signer_a, client_a);
  Result saved = save(store, c, NULL); g_assert_no_error(saved.error); clear_result(&saved);
  GError *error = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &error);
  g_assert_no_error(error);
  SecretCollection *collection = secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT,
    SECRET_COLLECTION_NONE, NULL, &error);
  g_assert_no_error(error);
  GList *objects = g_list_append(NULL, collection), *locked = NULL;
  secret_service_lock_sync(service, objects, NULL, &locked, &error);
  g_assert_no_error(error);
  g_list_free_full(locked, g_object_unref); g_list_free(objects);
  g_object_unref(collection); g_object_unref(service);
  Result listed = list(store);
  g_assert_no_error(listed.error); g_assert_cmpuint(listed.list->len, ==, 1);
  clear_result(&listed);
  Result found = lookup(store, account_a, NULL);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_LOCKED);
  clear_result(&found);
  g_object_unref(store);
}
static void
test_unavailable(void)
{
  g_assert_false(keyring_started);
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_secret_service();
  Result found = lookup(store, account_a, NULL);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR,
                 GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE);
  clear_result(&found);
  Result listed = list(store);
  g_assert_error(listed.error, GH_NIP46_CREDENTIAL_ERROR,
                 GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE);
  clear_result(&listed);
  g_object_unref(store);
}
static void
start_private_bus(void)
{
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  const gchar *root = nostrc_test_bus_get_dir(bus);
  static const gchar *vars[] = { "HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    gchar *dir = g_build_filename(root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
    g_free(dir);
  }
}
static void
ensure_keyring(void)
{
  if (keyring_started) return;
  keyring_started = TRUE;
  const gchar *root = nostrc_test_bus_get_dir(bus);
  gchar *password = g_build_filename(root, "password", NULL);
  GError *error = NULL;
  g_assert_true(g_file_set_contents_full(password, "nip46-test", 10, G_FILE_SET_CONTENTS_NONE, 0600, &error));
  g_assert_no_error(error);
  const gchar *argv[] = { daemon_program, "--foreground", "--unlock", "--components=secrets", NULL };
  nostrc_test_bus_spawn_supervised(bus, "gnome-keyring.log", password, argv);
  g_free(password);
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error(error);
  gboolean appeared = FALSE;
  for (guint i = 0; i < 300 && !appeared; i++) {
    GVariant *owner = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", "org.freedesktop.secrets"),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
    g_assert_no_error(error);
    g_variant_get(owner, "(b)", &appeared);
    g_variant_unref(owner);
    if (!appeared) g_usleep(100000);
  }
  if (!appeared) { nostrc_test_bus_dump_log(bus, "gnome-keyring.log"); g_error("Secret Service not ready"); }
  g_object_unref(connection);
}
int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (!nostrc_test_bus_available() || !(daemon_program = g_find_program_in_path("gnome-keyring-daemon"))) {
    g_print("SKIP: dbus-daemon or gnome-keyring-daemon unavailable\n");
    return 77;
  }
  start_private_bus();
  loop = g_main_loop_new(NULL, FALSE);
  g_test_add_func("/nip46-credentials/00-unavailable", test_unavailable);
  g_test_add_func("/nip46-credentials/round-trip", test_round_trip);
  g_test_add_func("/nip46-credentials/newer-conflict", test_newer_conflict);
  g_test_add_func("/nip46-credentials/interrupted", test_interrupted);
  g_test_add_func("/nip46-credentials/zz-locked", test_zz_locked);
  int status = g_test_run();
  g_main_loop_unref(loop);
  secret_service_disconnect();
  nostrc_test_bus_down(bus);
  g_free(daemon_program);
  return status;
}
