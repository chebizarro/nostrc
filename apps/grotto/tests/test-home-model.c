/* Identity portion of the Home model: real AccountsStore + daemon inventory I/O. */
#include "accounts_store.h"
#include "backup-recovery.h"
#include "settings_manager.h"
#include "tests/common/nostrc-test-bus.h"
#include <glib/gstdio.h>
#include <string.h>
static gchar *identity_a, *identity_b, *path;
static const gchar *inventory;
static GDBusConnection *server;
static gboolean bound(gpointer data) { (void)data; g_error("Identity model did not complete"); return G_SOURCE_REMOVE; }
static void wait_count(guint *count, guint target) {
  guint id = g_timeout_add_seconds(10, bound, NULL);
  while (*count < target) g_main_context_iteration(NULL, TRUE);
  g_source_remove(id);
}
static void inventory_call(GDBusConnection *bus, const gchar *sender, const gchar *object, const gchar *interface,
    const gchar *method, GVariant *args, GDBusMethodInvocation *invocation, gpointer data) {
  (void)bus; (void)sender; (void)object; (void)interface; (void)method; (void)args; (void)data;
  const gchar *ids[] = {inventory, NULL};
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", ids));
}
static const GDBusInterfaceVTable vtable = {inventory_call, NULL, NULL, {0}};
static void sync_done(AccountsStore *as, gpointer data) { (void)as; (*(guint *)data)++; }
static void sync_inventory(AccountsStore *as) {
  guint done = 0; accounts_store_sync_with_secrets_async(as, sync_done, &done); wait_count(&done, 1);
}
static void notification(AccountsStore *as, AccountsChangeType change, const gchar *id, gpointer data) {
  (void)as; (void)change; guint *count = data; (*count)++;
  if (id) {
    g_autoptr(GKeyFile) disk = g_key_file_new(); g_assert_true(g_key_file_load_from_file(disk, path, G_KEY_FILE_NONE, NULL));
    g_assert_true(g_key_file_has_key(disk, "accounts", id, NULL));
  }
}
static void persist_before_notify(void) {
  g_unlink(path); AccountsStore *as = accounts_store_new(); guint count = 0;
  accounts_store_connect_changed(as, notification, &count);
  g_assert_true(accounts_store_add(as, identity_a, "A", NULL)); g_assert_cmpuint(count, ==, 1);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
  g_autoptr(GVariant) before = g_settings_get_value(settings, "account-order");
  g_unlink(path); g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_autoptr(GError) error = NULL;
  g_assert_false(accounts_store_add(as, identity_b, "B", &error)); g_assert_nonnull(error);
  g_assert_false(accounts_store_exists(as, identity_b)); g_assert_cmpuint(count, ==, 1);
  g_autoptr(GVariant) after = g_settings_get_value(settings, "account-order"); g_assert_true(g_variant_equal(before, after));
  accounts_store_free(as); g_rmdir(path);
}
static void transactional_reload(void) {
  g_unlink(path); AccountsStore *as = accounts_store_new();
  g_assert_true(accounts_store_add(as, identity_a, "Old", NULL));
  g_assert_true(accounts_store_import_pubkey(as, identity_b, "Watch", NULL, NULL));
  g_autofree gchar *replacement = g_strdup_printf("[accounts]\n%s=New\n[state]\nactive=%s\n", identity_a, identity_b);
  g_assert_true(g_file_set_contents(path, replacement, -1, NULL));
  g_assert_true(accounts_store_load(as, NULL)); g_assert_cmpuint(accounts_store_count(as), ==, 1);
  g_assert_false(accounts_store_exists(as, identity_b)); g_assert_false(accounts_store_is_watch_only(as, identity_a));
  gchar *active = NULL; g_assert_false(accounts_store_get_active(as, &active, NULL));
  g_assert_true(g_file_set_contents(path, "[broken", -1, NULL));
  g_autoptr(GError) error = NULL; g_assert_false(accounts_store_load(as, &error)); g_assert_nonnull(error);
  g_assert_cmpuint(accounts_store_count(as), ==, 1);
  AccountEntry *entry = accounts_store_find(as, identity_a); g_assert_cmpstr(entry->label, ==, "New"); accounts_store_entry_free(entry);
  g_unlink(path); g_assert_true(accounts_store_load(as, NULL)); g_assert_cmpuint(accounts_store_count(as), ==, 0);
  accounts_store_free(as);
}
static void selection_not_default(void) {
  g_unlink(path); AccountsStore *as = accounts_store_new();
  g_assert_true(accounts_store_add(as, identity_a, "A", NULL)); g_assert_true(accounts_store_add(as, identity_b, "B", NULL));
  g_assert_true(accounts_store_set_active(as, identity_a, NULL));
  gchar *before = NULL, *after = NULL; g_file_get_contents(path, &before, NULL, NULL);
  g_assert_true(accounts_store_set_selected(as, identity_b)); g_file_get_contents(path, &after, NULL, NULL);
  g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
  g_autofree gchar *default_id = g_settings_get_string(settings, "default-identity"); g_assert_cmpstr(default_id, ==, identity_a);
  g_assert_true(accounts_store_remove(as, identity_a, NULL));
  g_free(default_id); default_id = g_settings_get_string(settings, "default-identity"); g_assert_cmpstr(default_id, ==, "");
  g_assert_cmpstr(accounts_store_get_selected(as), ==, identity_b);
  gchar *active = NULL; g_assert_false(accounts_store_get_active(as, &active, NULL));
  accounts_store_free(as);
}
static void authoritative_inventory(void) {
  g_unlink(path); AccountsStore *as = accounts_store_new();
  g_assert_true(accounts_store_import_pubkey(as, identity_b, "Watch", NULL, NULL));
  inventory = identity_a; sync_inventory(as);
  g_assert_true(accounts_store_set_selected(as, identity_a));
  g_assert_true(accounts_store_set_setup_state(as, identity_a, "backup-pending", NULL));
  g_autoptr(GPtrArray) entries = accounts_store_list(as); g_assert_cmpuint(entries->len, ==, 2);
  for (guint i = 0; i < entries->len; i++) {
    AccountEntry *e = entries->pdata[i];
    if (!strcmp(e->id, identity_a)) { g_assert_true(e->has_secret); g_assert_false(e->watch_only); g_assert_cmpstr(e->setup_state, ==, "backup-pending"); }
    else { g_assert_false(e->has_secret); g_assert_true(e->watch_only); }
  }
  inventory = identity_b; sync_inventory(as);
  g_assert_cmpstr(accounts_store_get_selected(as), ==, identity_a);
  AccountEntry *missing = accounts_store_find(as, identity_a); g_assert_false(missing->has_secret); accounts_store_entry_free(missing);
  AccountEntry *signable = accounts_store_find(as, identity_b); g_assert_true(signable->has_secret); g_assert_false(signable->watch_only); accounts_store_entry_free(signable);
  accounts_store_free(as);
}
static void stored_key_survives_metadata_failure(void) {
  g_unlink(path); AccountsStore *as = accounts_store_new(); inventory = identity_a;
  g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*Could not persist daemon identity metadata*");
  sync_inventory(as); g_test_assert_expected_messages();
  g_assert_false(accounts_store_exists(as, identity_a));
  g_autoptr(GPtrArray) entries = accounts_store_list(as); g_assert_cmpuint(entries->len, ==, 1);
  AccountEntry *entry = entries->pdata[0]; g_assert_cmpstr(entry->id, ==, identity_a); g_assert_true(entry->has_secret);
  g_assert_true(accounts_store_set_selected(as, identity_a)); accounts_store_free(as); g_rmdir(path);
}
static void monitor_notice(AccountsStore *as, AccountsChangeType change, const gchar *id, gpointer data) {
  (void)change; (void)id;
  if (!accounts_store_exists(as, identity_a) && accounts_store_exists(as, identity_b)) (*(guint *)data)++;
}
static void monitored_replace(void) {
  g_unlink(path); inventory = NULL;
  AccountsStore *as = accounts_store_get_default(); sync_inventory(as);
  g_assert_true(accounts_store_add(as, identity_a, "A", NULL)); guint notices = 0;
  guint handler = accounts_store_connect_changed(as, monitor_notice, &notices);
  g_autofree gchar *content = g_strdup_printf("[accounts]\n%s=B\n", identity_b);
  g_assert_true(g_file_set_contents_full(path, content, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL));
  wait_count(&notices, 1); g_assert_cmpuint(accounts_store_count(as), ==, 1);
  accounts_store_disconnect_changed(as, handler);
}
static void inventory_notice(AccountsStore *as, AccountsChangeType change, const gchar *id, gpointer data) {
  (void)change; (void)id;
  AccountEntry *entry = accounts_store_find(as, identity_b);
  if (entry && entry->has_secret) (*(guint *)data)++;
  accounts_store_entry_free(entry);
}
static void inventory_signal_refresh(void) {
  inventory = NULL; AccountsStore *as = accounts_store_get_default(); sync_inventory(as);
  guint notices = 0; guint handler = accounts_store_connect_changed(as, inventory_notice, &notices);
  inventory = identity_b;
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer", "org.nostr.Signer", "IdentitiesChanged", g_variant_new("(t)", (guint64)1), NULL));
  wait_count(&notices, 1);
  accounts_store_disconnect_changed(as, handler);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL); nostrc_test_bus_tolerate_ebadf();
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus);
  server = nostrc_test_bus_connect(bus);
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml("<node><interface name='org.nostr.Signer'><method name='ListIdentities'><arg type='as' direction='out'/></method></interface></node>", NULL);
  guint registration = g_dbus_connection_register_object(server, "/org/nostr/signer", info->interfaces[0], &vtable, NULL, NULL, NULL);
  g_autoptr(GVariant) name = g_dbus_connection_call_sync(server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.nostr.Signer", 0), NULL, 0, 5000, NULL, NULL);
  gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000001", &identity_a, NULL);
  gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &identity_b, NULL);
  g_assert_nonnull(identity_a); g_assert_nonnull(identity_b);
  g_autofree gchar *dir = g_build_filename(g_get_user_config_dir(), "grotto", NULL); g_mkdir_with_parents(dir, 0700);
  path = g_build_filename(dir, "accounts.ini", NULL);
  g_test_add_func("/home/identities/persist-before-notify", persist_before_notify);
  g_test_add_func("/home/identities/transactional-reload", transactional_reload);
  g_test_add_func("/home/identities/selection-not-default", selection_not_default);
  g_test_add_func("/home/identities/authoritative-inventory", authoritative_inventory);
  g_test_add_func("/home/identities/stored-key-metadata-failure", stored_key_survives_metadata_failure);
  g_test_add_func("/home/identities/monitored-replace", monitored_replace);
  g_test_add_func("/home/identities/daemon-change-signal", inventory_signal_refresh);
  int result = g_test_run(); accounts_store_free(accounts_store_get_default());
  g_dbus_connection_unregister_object(server, registration); g_dbus_node_info_unref(info); nostrc_test_bus_down(bus);
  g_free(identity_a); g_free(identity_b); g_free(path); return result;
}
