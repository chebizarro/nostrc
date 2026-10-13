#include "signer_permissions.h"
#include "signer_permission_policy.h"
#include <nostrc-profile-guard.h>
#include <glib/gstdio.h>
#include <string.h>

static const gchar *PRINCIPAL = "web:https://client.example/path|with-punctuation";
static const gchar *IDENTITY = "npub-test-identity";
static gchar *store_path(const gchar *name) { return g_build_filename(g_get_user_config_dir(), "gnostr", name, NULL); }
static void remove_files(void) {
  g_autofree gchar *v2 = store_path("signer-permissions-v2.ini");
  g_autofree gchar *v1 = store_path("signer-grants.ini");
  g_unlink(v2); g_unlink(v1); g_rmdir(v2);
}
static SignerPermissions *start(void) { remove_files(); return signer_permissions_new(NULL); }
static SignerPermissionResult evaluate(SignerPermissions *p, const gchar *op, gint kind) {
  GError *error = NULL;
  SignerPermissionResult result = signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, op, kind, FALSE, &error);
  g_assert_no_error(error); return result;
}
static void remember(SignerPermissions *p, const gchar *identity, const gchar *op, gint kind, gboolean allow, guint64 ttl) {
  GError *error = NULL;
  g_assert_true(signer_permissions_remember(p, PRINCIPAL, identity, op, kind, allow, ttl, &error)); g_assert_no_error(error);
}
static GVariant *replacement(const gchar *op, gint kind, const gchar *decision, guint64 expires) {
  GVariantBuilder rows, row;
  g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&row, "{sv}", "principal", g_variant_new_string(PRINCIPAL));
  g_variant_builder_add(&row, "{sv}", "identity", g_variant_new_string(IDENTITY));
  g_variant_builder_add(&row, "{sv}", "operation", g_variant_new_string(op));
  g_variant_builder_add(&row, "{sv}", "event-kind", g_variant_new_int32(kind));
  g_variant_builder_add(&row, "{sv}", "decision", g_variant_new_string(decision));
  g_variant_builder_add(&row, "{sv}", "expires-at", g_variant_new_uint64(expires));
  g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
  return g_variant_ref_sink(g_variant_builder_end(&rows));
}
static void test_exact_scope_and_restart(void) {
  g_autoptr(SignerPermissions) p = start();
  remember(p, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, 600);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_REACTION), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate(p, "other", IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, "other", "event", NOSTR_KIND_TEXT_NOTE, FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_autoptr(SignerPermissions) restarted = signer_permissions_new(NULL);
  g_assert_cmpint(evaluate(restarted, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ALLOW);
  g_autoptr(GVariant) rows = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 1);
  g_autoptr(GVariant) row = g_variant_get_child_value(rows, 0); guint64 expiry;
  g_assert_true(g_variant_lookup(row, "expires-at", "t", &expiry));
  g_assert_cmpuint(expiry, >=, g_get_real_time() / G_USEC_PER_SEC + 590);
  g_autoptr(GVariant) again = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_true(g_variant_equal(rows, again)); /* activity never extends expiry */
  g_autoptr(GVariant) legacy = signer_permissions_list_legacy(p, NULL);
  g_assert_cmpuint(g_variant_n_children(legacy), ==, 0); /* scoped rows not exposed */
  g_autofree gchar *path = store_path("signer-permissions-v2.ini");
  g_autofree gchar *dir = g_path_get_dirname(path);
  GStatBuf st; g_assert_cmpint(g_stat(path, &st), ==, 0); g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
  g_assert_cmpint(g_stat(dir, &st), ==, 0); g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
  g_autoptr(GKeyFile) kf = g_key_file_new(); g_assert_true(g_key_file_load_from_file(kf, path, 0, NULL));
  g_auto(GStrv) groups = g_key_file_get_groups(kf, NULL);
  for (guint i = 0; groups[i]; i++) g_assert_null(strstr(groups[i], PRINCIPAL));
  const gchar *encoded = "exe:/client=[section]|UTF8-ü";
  g_assert_true(signer_permissions_remember(p, encoded, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, 0, NULL));
  g_assert_cmpint(signer_permissions_evaluate(p, encoded, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, FALSE, NULL), ==, SIGNER_PERMISSION_ALLOW);
}
static void test_expiry_deny_and_conflict(void) {
  g_autoptr(SignerPermissions) p = start();
  g_autoptr(GVariant) expired = replacement("event", NOSTR_KIND_TEXT_NOTE, "allow", g_get_real_time() / G_USEC_PER_SEC);
  g_assert_true(signer_permissions_replace(p, PRINCIPAL, IDENTITY, expired, signer_permissions_revision(p), NULL));
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  g_autoptr(GVariant) expired_deny = replacement("event", NOSTR_KIND_TEXT_NOTE, "deny", g_get_real_time() / G_USEC_PER_SEC);
  g_assert_true(signer_permissions_replace(p, PRINCIPAL, IDENTITY, expired_deny, signer_permissions_revision(p), NULL));
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  remember(p, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, 0);
  remember(p, "*", "event", -1, FALSE, 0);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_DENY);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_PROFILE_METADATA), ==, SIGNER_PERMISSION_DENY);
  guint64 stale = signer_permissions_revision(p);
  remember(p, IDENTITY, "nip44_encrypt", -1, TRUE, 0);
  GError *error = NULL;
  g_assert_false(signer_permissions_replace(p, PRINCIPAL, IDENTITY, expired, stale, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_false(signer_permissions_remember(p, PRINCIPAL, "*", "event", -1, TRUE, 0, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
  g_assert_false(signer_permissions_remember(p, PRINCIPAL, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, G_MAXUINT64, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
  g_assert_cmpint(evaluate(p, "unknown", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "event", G_MAXUINT16 + 1), ==, SIGNER_PERMISSION_ASK);
}
static void test_migration_and_legacy_revoke(void) {
  g_autoptr(SignerPermissions) p = start();
  g_autofree gchar *legacy_path = store_path("signer-grants.ini");
  g_autofree gchar *data = g_strdup_printf("[event]\n%s|%s=allow\nother|%s=deny\n[get_public_key]\n%s|%s=allow\n[nip44_encrypt]\n%s|%s=allow:invalid\n", PRINCIPAL, IDENTITY, IDENTITY, PRINCIPAL, IDENTITY, PRINCIPAL, IDENTITY);
  g_assert_true(g_file_set_contents(legacy_path, data, -1, NULL));
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_true(signer_permissions_needs_review(p, PRINCIPAL, NULL));
  g_assert_cmpint(signer_permissions_evaluate(p, "other", IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, FALSE, NULL), ==, SIGNER_PERMISSION_DENY);
  g_autoptr(GVariant) rows = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 2);
  g_autoptr(GKeyFile) legacy = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(legacy, legacy_path, 0, NULL));
  g_autofree gchar *key = g_strdup_printf("%s|%s", PRINCIPAL, IDENTITY);
  g_assert_false(g_key_file_has_key(legacy, "event", key, NULL));
  guint64 revision = signer_permissions_revision(p);
  g_assert_true(signer_permissions_reload(p, NULL)); g_assert_cmpuint(signer_permissions_revision(p), ==, revision);
  g_autoptr(SignerPermissions) restarted = signer_permissions_new(NULL);
  g_assert_true(signer_permissions_reload(restarted, NULL));
  g_autoptr(GVariant) repeat = signer_permissions_list(restarted, PRINCIPAL, NULL);
  g_assert_true(g_variant_equal(rows, repeat));
  remember(p, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, 0);
  remember(p, IDENTITY, "event", NOSTR_KIND_REACTION, FALSE, 600);
  gboolean removed = FALSE;
  g_assert_true(signer_permissions_clear(p, PRINCIPAL, IDENTITY, "event", &removed, NULL)); g_assert_true(removed);
  g_assert_false(signer_permissions_needs_review(p, PRINCIPAL, NULL));
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_REACTION), ==, SIGNER_PERMISSION_ASK);
  /* Old GUI reintroduces a broad allow after migration: re-sanitise, never execute. */
  g_key_file_set_string(legacy, "event", key, "allow");
  g_autofree gchar *rewritten = g_key_file_to_data(legacy, NULL, NULL);
  g_assert_true(g_file_set_contents(legacy_path, rewritten, -1, NULL));
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  /* A fresh legacy write is still visibly needs-review, never authority. */
  g_assert_true(signer_permissions_needs_review(p, PRINCIPAL, NULL));
  g_assert_true(g_key_file_load_from_file(legacy, legacy_path, 0, NULL));
  g_assert_false(g_key_file_has_key(legacy, "event", key, NULL));
  g_assert_true(signer_permissions_clear(p, PRINCIPAL, NULL, NULL, &removed, NULL)); g_assert_true(removed);
  g_autoptr(SignerPermissions) after_clear = signer_permissions_new(NULL);
  g_assert_cmpint(evaluate(after_clear, "get_public_key", -1), ==, SIGNER_PERMISSION_ASK);
}
static void test_review_pruning_and_failure(void) {
  g_autoptr(SignerPermissions) p = start();
  g_autofree gchar *old_path = store_path("signer-grants.ini");
  g_autofree gchar *legacy = g_strdup_printf("[event]\n%s|%s=allow\n", PRINCIPAL, IDENTITY);
  g_assert_true(g_file_set_contents(old_path, legacy, -1, NULL));
  g_autofree gchar *path = store_path("signer-permissions-v2.ini");
  g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  GError *error = NULL;
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, FALSE, &error), ==, SIGNER_PERMISSION_ERROR);
  g_assert_nonnull(error); g_clear_error(&error);
  g_autofree gchar *unchanged = NULL; g_assert_true(g_file_get_contents(old_path, &unchanged, NULL, NULL));
  g_assert_cmpstr(legacy, ==, unchanged); /* migration failure has not silently discarded consent metadata */
  g_assert_cmpint(g_rmdir(path), ==, 0);
  g_assert_true(signer_permissions_reload(p, NULL));
  g_autoptr(GKeyFile) kf = g_key_file_new(); g_assert_true(g_key_file_load_from_file(kf, path, 0, NULL));
  g_auto(GStrv) groups = g_key_file_get_groups(kf, NULL);
  for (guint i = 0; groups[i]; i++) if (g_str_has_prefix(groups[i], "record-"))
    g_key_file_set_uint64(kf, groups[i], "created-at", g_get_real_time() / G_USEC_PER_SEC - 91 * 24 * 60 * 60);
  g_autofree gchar *aged = g_key_file_to_data(kf, NULL, NULL); g_assert_true(g_file_set_contents(path, aged, -1, NULL));
  g_assert_false(signer_permissions_needs_review(p, PRINCIPAL, NULL));
  guint64 revision = signer_permissions_revision(p);
  g_assert_cmpint(g_unlink(path), ==, 0); g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_assert_false(signer_permissions_remember(p, PRINCIPAL, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, 600, &error));
  g_assert_nonnull(error); g_clear_error(&error); g_assert_cmpuint(signer_permissions_revision(p), ==, revision);
}
static void test_schema_and_profile_guard(void) {
  g_autoptr(SignerPermissions) p = start();
  g_autofree gchar *path = store_path("signer-permissions-v2.ini");
  g_assert_true(g_file_set_contents(path, "[permissions]\nversion=3\n", -1, NULL));
  GError *error = NULL; g_assert_false(signer_permissions_reload(p, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED); g_clear_error(&error);
  g_assert_cmpint(g_unlink(path), ==, 0);
  remember(p, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, 0);
  g_autoptr(GKeyFile) kf = g_key_file_new(); g_assert_true(g_key_file_load_from_file(kf, path, 0, NULL));
  g_auto(GStrv) groups = g_key_file_get_groups(kf, NULL);
  for (guint i = 0; groups[i]; i++) if (g_str_has_prefix(groups[i], "record-"))
    g_key_file_set_integer(kf, groups[i], "event-kind", G_MAXUINT16 + 1);
  g_autofree gchar *bad = g_key_file_to_data(kf, NULL, NULL); g_assert_true(g_file_set_contents(path, bad, -1, NULL));
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, FALSE, &error), ==, SIGNER_PERMISSION_ERROR);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
  const gchar *home = nostrc_profile_guard_real_home();
  if (!home) { g_test_skip("no passwd home"); return; }
  g_autofree gchar *link = g_build_filename(g_get_user_config_dir(), "gnostr", NULL);
  remove_files(); g_assert_cmpint(g_rmdir(link), ==, 0);
  g_autofree gchar *real = g_build_filename(home, ".config", NULL);
  g_assert_cmpint(symlink(real, link), ==, 0);
  g_assert_false(signer_permissions_reload(p, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&error);
  g_assert_cmpint(g_unlink(link), ==, 0); g_assert_cmpint(g_mkdir(link, 0700), ==, 0);
}
typedef struct { SignerTrustTier tier; gboolean consented; } Client;
static SignerTrustTier trust(const gchar *principal, gboolean *consented, gpointer data) {
  Client *client = data;
  if (strcmp(principal, PRINCIPAL)) { *consented = FALSE; return SIGNER_TRUST_LOW; }
  *consented = client->consented; return client->tier;
}
static void test_tiers(void) {
  g_autoptr(SignerPermissions) p = start(); Client client = {SIGNER_TRUST_LOW, FALSE};
  signer_permissions_set_trust_lookup(p, trust, &client);
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ASK);
  client.consented = TRUE; signer_permissions_policy_changed(p);
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  client.tier = SIGNER_TRUST_MEDIUM; signer_permissions_policy_changed(p);
  for (guint i = 0; i < G_N_ELEMENTS(signer_public_publishing_kinds); i++)
    g_assert_cmpint(evaluate(p, "event", signer_public_publishing_kinds[i]), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "get_relays", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "ping", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "event", G_MAXUINT16), ==, SIGNER_PERMISSION_ASK);
  client.tier = SIGNER_TRUST_FULL; signer_permissions_policy_changed(p);
  g_assert_cmpint(evaluate(p, "nip44_decrypt", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "event", G_MAXUINT16), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "nip44_conversation_key", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "raw_key_export", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "connect", -1), ==, SIGNER_PERMISSION_ASK);
  for (guint i = 0; i < G_N_ELEMENTS(signer_default_required_confirmation_kinds); i++) {
    gint kind = signer_default_required_confirmation_kinds[i];
    remember(p, IDENTITY, "event", kind, TRUE, 0);
    g_assert_cmpint(evaluate(p, "event", kind), ==, SIGNER_PERMISSION_ASK);
    remember(p, IDENTITY, "event", kind, FALSE, 0);
    g_assert_cmpint(evaluate(p, "event", kind), ==, SIGNER_PERMISSION_DENY);
  }
  remember(p, IDENTITY, "nip44_encrypt", -1, TRUE, 600);
  client.tier = SIGNER_TRUST_LOW; signer_permissions_policy_changed(p);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate(p, "unknown", IDENTITY, "get_public_key", -1, FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  guint32 required[] = {NOSTR_KIND_TEXT_NOTE};
  g_assert_true(signer_permissions_set_required_confirmation(p, required, G_N_ELEMENTS(required), NULL));
  remember(p, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, 0);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  /* Explicit denies also beat attested built-in headless defaults. */
  remember(p, IDENTITY, "get_public_key", -1, FALSE, 0);
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "get_public_key", -1, TRUE, NULL), ==, SIGNER_PERMISSION_DENY);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_setenv("NOSTRC_TEST_ISOLATION", "1", TRUE);
  g_autofree gchar *config = g_dir_make_tmp("signer-permissions-XXXXXX", NULL);
  g_assert_nonnull(config); g_setenv("XDG_CONFIG_HOME", config, TRUE);
  g_autofree gchar *dir = g_build_filename(config, "gnostr", NULL); g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_test_add_func("/permissions/exact-scope-restart", test_exact_scope_and_restart);
  g_test_add_func("/permissions/expiry-deny-conflict", test_expiry_deny_and_conflict);
  g_test_add_func("/permissions/migration-legacy-revoke", test_migration_and_legacy_revoke);
  g_test_add_func("/permissions/review-pruning-failure", test_review_pruning_and_failure);
  g_test_add_func("/permissions/schema-profile-guard", test_schema_and_profile_guard);
  g_test_add_func("/trust-tiers/live-policy", test_tiers);
  gint result = g_test_run(); remove_files(); g_rmdir(dir); g_rmdir(config); return result;
}
