#include "test-control-common.h"
#include <nostr/nip55l/signer_ops.h>
#include <nostr/nip55l/error.h>
#include <nostr/nip19/nip19.h>
#include <nostr/crypto/bip39.h>
#include <nostr-event.h>
#include <nostr-json.h>
#include <secure_buf.h>
#include <keys.h>
extern char *nostr_nip06_private_key_from_seed(const unsigned char *seed);

static void begin_test(ControlFixture *f, const gchar *identity, BusReply *reply) {
  g_dbus_connection_call(f->client, g_dbus_connection_get_unique_name(f->server), GROTTO_CONTROL_PATH,
      GROTTO_CONTROL_INTERFACE, "SelfTestIdentity", g_variant_new("(s)", identity), G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE, 20000, NULL, call_done, reply);
}
static void one_review_no_grants(ControlFixture *f, gconstpointer data) {
  (void)data; char *identity = NULL; g_assert_cmpint(nostr_nip55l_get_public_key(&identity), ==, 0);
  BusReply result = {0}; begin_test(f, identity, &result);
  const gchar *id = pending_id(f, 0); GVariant *row = f->pending->pdata[0];
  const gchar *transport, *operation, *preview, *principal; gboolean rememberable = TRUE;
  g_assert_true(g_variant_lookup(row, "transport", "&s", &transport)); g_assert_cmpstr(transport, ==, "MANAGEMENT");
  g_assert_true(g_variant_lookup(row, "operation", "&s", &operation)); g_assert_cmpstr(operation, ==, "self_test");
  g_assert_true(g_variant_lookup(row, "preview", "&s", &preview)); g_assert_nonnull(strstr(preview, "unpublished")); g_assert_nonnull(strstr(preview, "encrypt and decrypt"));
  g_assert_true(g_variant_lookup(row, "rememberable", "b", &rememberable)); g_assert_false(rememberable);
  g_assert_true(g_variant_lookup(row, "principal", "&s", &principal));
  decision(f, id, TRUE, TRUE); /* An old/malicious UI's remember=true cannot create a grant. */
  wait_count(&result.done, 1); g_assert_no_error(result.error);
  const gchar *verdict; g_variant_get(result.value, "(&s)", &verdict); g_assert_cmpstr(verdict, ==, "passed");
  g_autoptr(GVariant) permissions = get_rows(f, "ListPermissions", g_variant_new("(s)", principal));
  g_assert_cmpuint(g_variant_n_children(permissions), ==, 0);
  g_autoptr(GVariant) clients = get_rows(f, "ListClients", NULL); g_assert_cmpuint(g_variant_n_children(clients), ==, 0);
  g_assert_cmpuint(f->pending->len, ==, 1); g_variant_unref(result.value); free(identity);
}
static void denied_no_grants(ControlFixture *f, gconstpointer data) {
  (void)data; char *identity = NULL; g_assert_cmpint(nostr_nip55l_get_public_key(&identity), ==, 0);
  BusReply result = {0}; begin_test(f, identity, &result);
  const gchar *id = pending_id(f, 0); decision(f, id, FALSE, TRUE);
  wait_count(&result.done, 1); g_assert_nonnull(result.error);
  g_autofree gchar *remote = g_dbus_error_get_remote_error(result.error);
  g_assert_cmpstr(remote, ==, "org.nostr.Signer.Error.ApprovalDenied");
  const gchar *principal; g_variant_lookup(f->pending->pdata[0], "principal", "&s", &principal);
  g_autoptr(GVariant) permissions = get_rows(f, "ListPermissions", g_variant_new("(s)", principal));
  g_assert_cmpuint(g_variant_n_children(permissions), ==, 0); g_error_free(result.error); free(identity);
}
static void cancel_only_self_test(ControlFixture *f, gconstpointer data) {
  (void)data; char *identity = NULL; nostr_nip55l_get_public_key(&identity);
  BusReply result = {0}; begin_test(f, identity, &result); pending_id(f, 0);
  g_autoptr(GVariant) cancelled = control_call(f, "CancelIdentitySelfTest", g_variant_new("(s)", identity));
  wait_count(&result.done, 1); g_assert_nonnull(result.error);
  g_autofree gchar *name = g_dbus_error_get_remote_error(result.error);
  g_assert_cmpstr(name, ==, "org.nostr.Signer.Error.Cancelled");
  g_assert_cmpuint(signer_approval_service_pending_count(f->approval), ==, 0);
  g_assert_cmpint(nostr_nip55l_identity_self_test(identity), ==, 0); /* key is preserved */
  g_error_free(result.error); free(identity);
}
static void invalid_creation(ControlFixture *f, gconstpointer data) {
  (void)data;
  /* Valid mnemonic, wrong key: rejected before any keyring or write. */
  expect_control_error(f, "StoreKeyWithMnemonic", g_variant_new("(sss)", TEST_KEY, "",
      "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"), GROTTO_CONTROL_ERROR "InvalidInput");
  char *npub = NULL;
  g_assert_cmpint(nostr_nip55l_store_key_with_mnemonic(TEST_KEY, "", "not seed words", &npub), ==, NOSTR_SIGNER_ERROR_INVALID_ARG);
  g_assert_null(npub);
  g_assert_cmpint(nostr_nip55l_import_identity("key", "not a key", "", "", &npub), ==, NOSTR_SIGNER_ERROR_INVALID_KEY);
  g_assert_cmpint(nostr_nip55l_import_identity("mnemonic", "not seed words", "", "", &npub), ==, NOSTR_SIGNER_ERROR_INVALID_KEY);
  /* Client or remote transports cannot submit the private management operation. */
  SignerCaller who = {.kind = SIGNER_CALLER_NIP46, .principal = "nip46:client", .sender = "nip46:client"};
  SignerGateContext ctx = {.transport = SIGNER_TRANSPORT_NIP46}; TestReply reply = {0};
  g_autoptr(SignerGateHandle) handle = signer_approval_service_submit(f->approval, &who, who.sender,
      SIGNER_GATE_SELF_TEST, NULL, NULL, "explicit", &ctx, gate_reply, &reply);
  g_assert_cmpuint(reply.count, ==, 1); g_assert_nonnull(reply.error); clear_reply(&reply);
}
#ifdef G09_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif
static void service_seen(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface,
    const gchar *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal;
  const gchar *name, *old, *owner; g_variant_get(args, "(&s&s&s)", &name, &old, &owner);
  if (*owner) (*(guint *)data)++;
}
static void daemon_seen(GDBusConnection *bus, const gchar *name, const gchar *owner, gpointer data) {
  (void)bus; (void)name; (void)owner; (*(guint *)data)++;
}
static void allow_consumer(ControlFixture *f, const gchar *binary, const gchar *identity) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) tuple = control_call_to(f, "org.nostr.Signer", "GetStatus", NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(tuple);
  g_autoptr(GVariant) status = g_variant_get_child_value(tuple, 0); guint64 revision;
  g_assert_true(g_variant_lookup(status, "permissions_revision", "t", &revision));
  g_autofree gchar *principal = g_strconcat("exe:", binary, NULL);
  GVariantBuilder rows, row; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string(principal));
  g_variant_builder_add(&row, "{sv}", "identity", g_variant_new_string(identity));
  g_variant_builder_add(&row, "{sv}", "operation", g_variant_new_string("event"));
  g_variant_builder_add(&row, "{sv}", "event-kind", g_variant_new_int32(1));
  g_variant_builder_add(&row, "{sv}", "decision", g_variant_new_string("allow"));
  g_variant_builder_add(&row, "{sv}", "expires-at", g_variant_new_uint64(0));
  g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
  g_autoptr(GVariant) allowed = control_call_to(f, "org.nostr.Signer", "SetPermissions",
      g_variant_new("(ss@aa{sv}t)", principal, identity, g_variant_builder_end(&rows), revision), &error);
  g_assert_no_error(error); g_assert_nonnull(allowed);
}
static void two_real_consumers(ControlFixture *f, const gchar *old, const gchar *successor) {
  guint ready = 0;
  guint watch = g_bus_watch_name_on_connection(f->client, "org.nostr.Signer", 0, daemon_seen, NULL, &ready, NULL);
  const gchar *command[] = {IDENTITY_DAEMON_BIN, "--no-approval-ui", NULL};
  GSubprocess *daemon = nostrc_test_bus_spawn_supervised(f->bus, "g09-daemon.log", NULL, command);
  g_assert_nonnull(daemon); wait_count(&ready, 1); g_bus_unwatch_name(watch);
  allow_consumer(f, CONSUMER_A_BIN, old); allow_consumer(f, CONSUMER_B_BIN, successor);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) defaulted = control_call_to(f, "org.nostr.Signer", "SetDefaultIdentityForLegacy", g_variant_new("(s)", successor), &error);
  g_assert_no_error(error); g_assert_nonnull(defaulted);
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_unsetenv(launcher, "NOSTR_SIGNER_SECKEY_HEX");
  g_subprocess_launcher_unsetenv(launcher, "NOSTR_SIGNER_NSEC");
  g_subprocess_launcher_unsetenv(launcher, "NOSTR_SIGNER_ALLOW_KEY_MUTATIONS");
  GSubprocess *a = g_subprocess_launcher_spawn(launcher, &error, CONSUMER_A_BIN, old, NULL);
  g_assert_no_error(error);
  GSubprocess *b = g_subprocess_launcher_spawn(launcher, &error, CONSUMER_B_BIN, successor, NULL);
  g_assert_no_error(error); g_object_unref(launcher);
  /* Separate executable principals, concurrent real D-Bus calls, explicit
   * selectors, and no private-key environment in either consumer. */
  g_assert_true(g_subprocess_wait_check(a, NULL, &error)); g_assert_no_error(error);
  g_assert_true(g_subprocess_wait_check(b, NULL, &error)); g_assert_no_error(error);
  g_object_unref(a); g_object_unref(b);
  /* transfer-none supervisor belongs to the bus; bus_down stops the daemon. */
}
static void live_custody(ControlFixture *f, gconstpointer data) {
  (void)data;
  g_autofree gchar *program = g_find_program_in_path("gnome-keyring-daemon");
  if (!program) { g_test_skip("requires a disposable Secret Service, never the developer keychain"); return; }
  guint ready = 0;
  guint watch = g_dbus_connection_signal_subscribe(f->client, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "NameOwnerChanged", "/org/freedesktop/DBus", "org.freedesktop.secrets", G_DBUS_SIGNAL_FLAGS_NONE, service_seen, &ready, NULL);
  g_autoptr(GError) error = NULL;
  GSubprocess *keyring = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
      G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error, program, "--foreground", "--unlock", "--components=secrets", NULL);
  g_assert_no_error(error); g_assert_nonnull(keyring);
  GOutputStream *input = g_subprocess_get_stdin_pipe(keyring);
  g_assert_true(g_output_stream_write_all(input, "g09-disposable", 14, NULL, NULL, &error));
  g_assert_true(g_output_stream_close(input, NULL, &error)); wait_count(&ready, 1);
  g_dbus_connection_signal_unsubscribe(f->client, watch);
  const gchar *phrase = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
  unsigned char seed[64]; g_assert_true(nostr_bip39_seed(phrase, "", seed));
  char *key = nostr_nip06_private_key_from_seed(seed); secure_wipe(seed, sizeof(seed));
  g_assert_nonnull(key);
  g_autoptr(GVariant) stored = control_call(f, "StoreKeyWithMnemonic", g_variant_new("(sss)", key, "Shared local label", phrase));
  gchar *old; g_variant_get(stored, "(s)", &old);
  g_autoptr(GVariant) exported = control_call(f, "ExportIdentity", g_variant_new("(sss)", old, "mnemonic", ""));
  const gchar *actual; g_variant_get(exported, "(&s)", &actual); g_assert_cmpstr(actual, ==, phrase);
  char **identities = NULL; int count = 0;
  g_assert_cmpint(nostr_nip55l_list_identities(&identities, &count), ==, 0);
  guint matched = 0; for (int i = 0; i < count; i++) { if (!strcmp(identities[i], old)) matched++; free(identities[i]); }
  free(identities); g_assert_cmpuint(matched, ==, 1); /* phrase role is not an identity */
  char *same_label = NULL;
  g_assert_cmpint(nostr_nip55l_import_identity("key", TEST_KEY, "", "Shared local label", &same_label), ==, 0);
  g_assert_cmpstr(same_label, !=, old); free(same_label);
  g_assert_cmpint(nostr_nip55l_identity_self_test(old), ==, 0);
  g_autoptr(GVariant) defaulted = control_call(f, "SetDefaultIdentityForLegacy", g_variant_new("(s)", old));
  char *legacy = NULL;
  g_assert_cmpint(nostr_nip55l_get_public_key(&legacy), ==, 0);
  g_assert_cmpstr(legacy, ==, old); free(legacy); legacy = NULL;
  SignerPermissions *permissions = signer_approval_service_permissions(f->approval);
  g_assert_true(signer_permissions_remember(permissions, "exe:/old-client", old, "event", 1, TRUE, 600, NULL));
  g_autoptr(GVariant) paused = control_call(f, "SetPaused", g_variant_new("(b)", TRUE));
  expect_control_error(f, "RotateIdentity", g_variant_new("(s)", old), GROTTO_CONTROL_ERROR "Paused");
  g_autoptr(GVariant) resumed = control_call(f, "SetPaused", g_variant_new("(b)", FALSE));
  g_autoptr(GVariant) rotated = control_call(f, "RotateIdentity", g_variant_new("(s)", old));
  gchar *successor; g_variant_get(rotated, "(s)", &successor); g_assert_cmpstr(successor, !=, old);
  g_assert_cmpint(nostr_nip55l_get_public_key(&legacy), ==, 0);
  g_assert_cmpstr(legacy, ==, old); free(legacy); legacy = NULL;
  g_assert_cmpint(nostr_nip55l_identity_self_test(old), ==, 0);
  g_assert_cmpint(nostr_nip55l_identity_self_test(successor), ==, 0);
  g_autoptr(GVariant) announcement = control_call(f, "GetIdentityMigration", g_variant_new("(s)", old));
  const gchar *json; g_variant_get(announcement, "(&s)", &json);
  NostrEvent *event = nostr_event_new(); g_assert_cmpint(nostr_event_deserialize(event, json), ==, 0);
  g_assert_true(nostr_event_check_signature(event)); g_assert_cmpint(event->kind, ==, 1776); nostr_event_free(event);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
  g_autofree gchar *default_id = g_settings_get_string(settings, "default-identity"); g_assert_cmpstr(default_id, ==, old);
  g_autoptr(GVariant) scopes = get_rows(f, "ListPermissions", g_variant_new("(s)", "exe:/old-client"));
  g_assert_cmpuint(g_variant_n_children(scopes), ==, 1);
  g_autoptr(GVariant) scope = g_variant_get_child_value(scopes, 0); const gchar *bound;
  g_assert_true(g_variant_lookup(scope, "identity", "&s", &bound)); g_assert_cmpstr(bound, ==, old);
  g_autoptr(GVariant) seed_export = control_call(f, "ExportIdentity", g_variant_new("(sss)", successor, "mnemonic", ""));
  /* NIP-49 and raw imports do not invent seed words. */
  char *nsec = NULL; g_assert_cmpint(nostr_nip55l_export_identity(successor, "nsec", "", &nsec), ==, 0);
  char *imported = NULL; g_assert_cmpint(nostr_nip55l_import_identity("key", TEST_KEY, "", "", &imported), ==, 0);
  expect_control_error(f, "ExportIdentity", g_variant_new("(sss)", imported, "mnemonic", ""), GROTTO_CONTROL_ERROR "NotFound");
  char *encrypted = NULL;
  g_assert_cmpint(nostr_nip55l_export_identity(successor, "ncryptsec", "test-only", &encrypted), ==, 0);
  char *roundtrip = NULL; g_assert_cmpint(nostr_nip55l_import_identity("ncryptsec", encrypted, "test-only", "", &roundtrip), ==, 0);
  g_assert_cmpstr(roundtrip, ==, successor);
  char *invalid = NULL; g_assert_cmpint(nostr_nip55l_import_identity("ncryptsec", encrypted, "wrong", "", &invalid), !=, 0); g_assert_null(invalid);
  secure_wipe(nsec, strlen(nsec)); free(nsec); secure_wipe(encrypted, strlen(encrypted)); free(encrypted);
  secure_wipe(key, strlen(key)); free(key); free(imported); free(roundtrip);
  two_real_consumers(f, old, successor);
  /* Only explicit removal drops the old recovery role; successor stays usable. */
  g_assert_cmpint(nostr_nip55l_clear_key(old), ==, 0);
  g_assert_cmpint(nostr_nip55l_identity_self_test(successor), ==, 0);
#ifdef G09_HAVE_LIBSECRET
  secret_service_disconnect();
#endif
  free(old); free(successor);
  g_subprocess_force_exit(keyring); g_subprocess_wait(keyring, NULL, NULL); g_object_unref(keyring);
}
static void rejected_seed_input_not_logged(void) {
  if (g_test_subprocess()) {
    char *identity = NULL;
    g_assert_cmpint(nostr_nip55l_import_identity("mnemonic", TEST_KEY, "", "", &identity), ==, NOSTR_SIGNER_ERROR_INVALID_KEY);
    g_assert_null(identity); return;
  }
  g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed();
  g_autofree gchar *secret = g_strdup_printf("*%s*", TEST_KEY);
  g_test_trap_assert_stdout_unmatched(secret); g_test_trap_assert_stderr_unmatched(secret);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL); test_environment(argv[0]);
#define ADD(name, fn) g_test_add("/identity/" name, ControlFixture, NULL, fixture_setup, fn, fixture_teardown)
  ADD("one-review-no-grants", one_review_no_grants); ADD("denied-no-grants", denied_no_grants);
  ADD("cancel-preserves-key", cancel_only_self_test); ADD("invalid-creation", invalid_creation); ADD("live-custody", live_custody);
#undef ADD
  g_test_add_func("/identity/rejected-seed-input-not-logged", rejected_seed_input_not_logged);
  return g_test_run();
}
