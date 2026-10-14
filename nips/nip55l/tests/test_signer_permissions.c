#include "signer_permissions.h"
#include "signer_permission_policy.h"
#include <nostrc-profile-guard.h>
#include <glib/gstdio.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>

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
  g_assert_true(signer_permissions_reload(p, NULL));
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
  g_assert_cmpint(evaluate(p, "event", G_MAXUINT16 + 1), ==, SIGNER_PERMISSION_DENY);
  g_assert_cmpint(evaluate(p, "event", -1), ==, SIGNER_PERMISSION_DENY);
  remember(p, IDENTITY, "future-operation", -1, FALSE, 0);
  g_assert_cmpint(evaluate(p, "future-operation", -1), ==, SIGNER_PERMISSION_DENY);
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
  /* After initial import even review inventory is v2-authoritative. */
  g_assert_false(signer_permissions_needs_review(p, PRINCIPAL, NULL));
  g_assert_true(g_key_file_load_from_file(legacy, legacy_path, 0, NULL));
  g_assert_false(g_key_file_has_key(legacy, "event", key, NULL));
  g_assert_true(signer_permissions_clear(p, PRINCIPAL, NULL, NULL, &removed, NULL)); g_assert_true(removed);
  g_assert_true(g_key_file_has_key(legacy, "get_public_key", key, NULL));
  g_autoptr(SignerPermissions) after_clear = signer_permissions_new(NULL);
  g_assert_cmpint(evaluate(after_clear, "get_public_key", -1), ==, SIGNER_PERMISSION_ASK);
}
static void test_review_pruning_and_load_failure(void) {
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
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, FALSE, &error), ==, SIGNER_PERMISSION_DENY);
  g_assert_no_error(error);
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ASK);
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
static guint64 listed_expiry(SignerPermissions *p, const gchar *operation, const gchar *identity) {
  g_autoptr(GVariant) rows = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_nonnull(rows);
  for (gsize i = 0; i < g_variant_n_children(rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
    const gchar *op, *id, *decision; guint64 expires;
    g_assert_true(g_variant_lookup(row, "operation", "&s", &op));
    g_assert_true(g_variant_lookup(row, "identity", "&s", &id));
    if (strcmp(op, operation) || strcmp(id, identity)) continue;
    g_assert_true(g_variant_lookup(row, "decision", "&s", &decision));
    g_assert_cmpstr(decision, ==, "allow");
    g_assert_true(g_variant_lookup(row, "expires-at", "t", &expires)); return expires;
  }
  g_assert_not_reached(); return 0;
}
static void test_one_time_non_event_import(void) {
  g_autoptr(SignerPermissions) p = start(); guint64 expiry = g_get_real_time() / G_USEC_PER_SEC + 3600;
  g_autofree gchar *path = store_path("signer-grants.ini");
  g_autofree gchar *legacy = g_strdup_printf(
      "[get_public_key]\n%s|*=allow:%" G_GUINT64_FORMAT "\n"
      "[get_relays]\n%s|*=allow\n[nip44_conversation_key]\n%s|%s=allow\n"
      "[nip44_encrypt]\n%s|%s=allow:%" G_GUINT64_FORMAT "\n",
      PRINCIPAL, expiry, PRINCIPAL, PRINCIPAL, IDENTITY, PRINCIPAL, IDENTITY, expiry);
  g_assert_true(g_file_set_contents(path, legacy, -1, NULL));
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "get_relays", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "nip44_conversation_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpuint(listed_expiry(p, "get_public_key", "*"), ==, expiry);
  g_assert_cmpuint(listed_expiry(p, "get_relays", "*"), ==, 0);
  g_assert_cmpuint(listed_expiry(p, "nip44_conversation_key", IDENTITY), ==, 0);
  guint64 revision = signer_permissions_revision(p);
  g_autofree gchar *late = g_strdup_printf(
      "[get_public_key]\n%s|*=deny\n[get_relays]\n%s|*=deny\n"
      "[nip44_encrypt]\n%s|%s=allow\n[nip44_decrypt]\n%s|%s=allow\n"
      "[event]\n%s|*=allow\n", PRINCIPAL, PRINCIPAL, PRINCIPAL, IDENTITY, PRINCIPAL, IDENTITY, PRINCIPAL);
  g_assert_true(g_file_set_contents(path, late, -1, NULL));
  /* No later legacy row, even a deny or a forever allow, changes authority. */
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "get_relays", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "nip44_decrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_TEXT_NOTE), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpuint(listed_expiry(p, "nip44_encrypt", IDENTITY), ==, expiry);
  g_assert_cmpuint(signer_permissions_revision(p), ==, revision);
  /* Existing pre-marker v2 and a partially completed migration both remain
   * authoritative; absence/false markers must never reopen legacy import. */
  g_autofree gchar *v2 = store_path("signer-permissions-v2.ini");
  g_autoptr(GKeyFile) kf = g_key_file_new(); g_assert_true(g_key_file_load_from_file(kf, v2, 0, NULL));
  g_key_file_remove_key(kf, "permissions", "legacy-imported", NULL);
  g_key_file_remove_key(kf, "permissions", "legacy-complete", NULL);
  g_autofree gchar *pre_marker = g_key_file_to_data(kf, NULL, NULL);
  g_assert_true(g_file_set_contents(v2, pre_marker, -1, NULL));
  g_autoptr(SignerPermissions) restarted = signer_permissions_new(NULL);
  g_assert_cmpuint(listed_expiry(restarted, "nip44_encrypt", IDENTITY), ==, expiry);
  g_assert_cmpuint(listed_expiry(restarted, "nip44_conversation_key", IDENTITY), ==, 0);
  g_assert_cmpint(evaluate(restarted, "nip44_decrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_key_file_set_boolean(kf, "permissions", "legacy-imported", TRUE);
  g_key_file_set_boolean(kf, "permissions", "legacy-complete", FALSE);
  g_autofree gchar *partial = g_key_file_to_data(kf, NULL, NULL);
  g_assert_true(g_file_set_contents(v2, partial, -1, NULL));
  g_autoptr(SignerPermissions) interrupted = signer_permissions_new(NULL);
  g_assert_cmpint(evaluate(interrupted, "nip44_decrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpuint(listed_expiry(interrupted, "nip44_encrypt", IDENTITY), ==, expiry);
  gboolean removed = FALSE;
  g_assert_true(signer_permissions_clear(p, PRINCIPAL, NULL, NULL, &removed, NULL)); g_assert_true(removed);
  g_autoptr(SignerPermissions) cleared = signer_permissions_new(NULL);
  g_assert_cmpint(evaluate(cleared, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(evaluate(cleared, "nip44_decrypt", -1), ==, SIGNER_PERMISSION_ASK);
}
static void test_bad_record_and_corrupt_legacy_isolation(void) {
  g_autoptr(SignerPermissions) p = start();
  g_autofree gchar *old = store_path("signer-grants.ini");
  g_assert_true(g_file_set_contents(old, "not a key file: PRIVATE-DIAGNOSTIC-SENTINEL\n", -1, NULL));
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "nostr-signer: ignored unreadable legacy permission file");
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ASK);
  g_test_assert_expected_messages();
  remember(p, IDENTITY, "get_public_key", -1, TRUE, 0);
  remember(p, IDENTITY, "nip44_encrypt", -1, TRUE, 0);
  g_assert_true(g_file_set_contents(old, "not a key file: PRIVATE-DIAGNOSTIC-SENTINEL\n", -1, NULL));
  g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "nostr-signer: ignored unreadable legacy permission file");
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_test_assert_expected_messages();
  g_autofree gchar *path = store_path("signer-permissions-v2.ini");
  g_autoptr(GKeyFile) kf = g_key_file_new(); g_assert_true(g_key_file_load_from_file(kf, path, 0, NULL));
  g_auto(GStrv) groups = g_key_file_get_groups(kf, NULL);
  for (guint i = 0; groups[i]; i++) {
    g_autofree gchar *op = g_key_file_get_string(kf, groups[i], "operation", NULL);
    if (!g_strcmp0(op, "nip44_encrypt")) g_key_file_set_string(kf, groups[i], "expires-at", "PRIVATE-DIAGNOSTIC-SENTINEL");
  }
  g_autofree gchar *bad = g_key_file_to_data(kf, NULL, NULL); g_assert_true(g_file_set_contents(path, bad, -1, NULL));
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_DENY);
  g_assert_cmpint(evaluate(p, "nip44_decrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_autoptr(GVariant) rows = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 1);
  gboolean removed = FALSE;
  g_assert_true(signer_permissions_clear(p, PRINCIPAL, IDENTITY, "nip44_encrypt", &removed, NULL)); g_assert_true(removed);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ASK);
}
static void test_actual_write_failure_and_shared_directory(void) {
  g_autoptr(SignerPermissions) p = start();
  g_autofree gchar *path = store_path("signer-permissions-v2.ini");
  g_autofree gchar *dir = g_path_get_dirname(path); g_assert_cmpint(g_chmod(dir, 0755), ==, 0);
  remember(p, IDENTITY, "get_public_key", -1, TRUE, 0);
  GStatBuf st; g_assert_cmpint(g_stat(dir, &st), ==, 0); g_assert_cmpuint(st.st_mode & 0777, ==, 0755);
  g_assert_cmpint(g_stat(path, &st), ==, 0); g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
  g_autofree gchar *before = NULL; g_assert_true(g_file_get_contents(path, &before, NULL, NULL));
  guint64 revision = signer_permissions_revision(p);
  struct rlimit original, limited;
  g_assert_cmpint(getrlimit(RLIMIT_FSIZE, &original), ==, 0); limited = original; limited.rlim_cur = 1;
  void (*handler)(int) = signal(SIGXFSZ, SIG_IGN);
  g_assert_cmpint(setrlimit(RLIMIT_FSIZE, &limited), ==, 0);
  GError *error = NULL;
  gboolean saved = signer_permissions_remember(p, PRINCIPAL, IDENTITY, "nip44_encrypt", -1, TRUE, 600, &error);
  g_assert_cmpint(setrlimit(RLIMIT_FSIZE, &original), ==, 0); signal(SIGXFSZ, handler);
  g_assert_false(saved); g_assert_nonnull(error); g_clear_error(&error);
  g_assert_cmpuint(signer_permissions_revision(p), ==, revision);
  g_autofree gchar *after = NULL; g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
  g_assert_cmpstr(before, ==, after);
  g_assert_cmpint(evaluate(p, "get_public_key", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(g_stat(dir, &st), ==, 0); g_assert_cmpuint(st.st_mode & 0777, ==, 0755);
  g_assert_cmpint(g_chmod(dir, 0700), ==, 0); /* test fixture restores its own directory */
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
  /* Independent specification, not the evaluator's own policy table. */
  const guint32 public_expected[] = {NOSTR_KIND_TEXT_NOTE, NOSTR_KIND_REPOST,
    NOSTR_KIND_GENERIC_REPOST, NOSTR_KIND_REACTION, NOSTR_KIND_ARTICLE, NOSTR_KIND_COMMENT};
  for (guint i = 0; i < G_N_ELEMENTS(public_expected); i++)
    g_assert_cmpint(evaluate(p, "event", public_expected[i]), ==, SIGNER_PERMISSION_ALLOW);
  const guint32 sensitive_expected[] = {NOSTR_KIND_FOLLOW_LIST, NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE,
    NOSTR_KIND_DELETION, NOSTR_KIND_RELAY_LIST_METADATA, NOSTR_KIND_CLIENT_AUTHENTICATION,
    NOSTR_KIND_PROFILE_METADATA, NOSTR_KIND_GIFT_WRAP, NOSTR_KIND_NUTZAP};
  for (guint i = 0; i < G_N_ELEMENTS(sensitive_expected); i++)
    g_assert_cmpint(evaluate(p, "event", sensitive_expected[i]), ==, SIGNER_PERMISSION_ASK);
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
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "connect", -1, TRUE, NULL), ==, SIGNER_PERMISSION_ASK);
  remember(p, "", "connect", -1, TRUE, 0);
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, "", "connect", -1, TRUE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "nip44_conversation_key", -1, TRUE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate(p, NULL, IDENTITY, "get_public_key", -1, TRUE, NULL), ==, SIGNER_PERMISSION_ASK);
  remember(p, IDENTITY, "nip44_conversation_key", -1, TRUE, 0);
  g_assert_cmpint(evaluate(p, "nip44_conversation_key", -1), ==, SIGNER_PERMISSION_ALLOW);
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
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "event", NOSTR_KIND_TEXT_NOTE, TRUE, NULL), ==, SIGNER_PERMISSION_ASK);
  /* Explicit denies also beat attested built-in headless defaults. */
  remember(p, IDENTITY, "get_public_key", -1, FALSE, 0);
  g_assert_cmpint(signer_permissions_evaluate(p, PRINCIPAL, IDENTITY, "get_public_key", -1, TRUE, NULL), ==, SIGNER_PERMISSION_DENY);
}
static void test_management_self_test_has_no_scope(void) {
  g_autoptr(SignerPermissions) p = start(); g_autoptr(GError) error = NULL;
  g_assert_false(signer_permissions_remember(p, PRINCIPAL, IDENTITY, "self_test", -1, TRUE, 600, &error));
  g_assert_nonnull(error);
  /* Unknown-operation denials remain a fail-closed evaluator feature; the
   * MANAGEMENT gate itself ignores remember for both approval and denial. */
  g_autoptr(GVariant) rows = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_nonnull(rows); g_assert_cmpuint(g_variant_n_children(rows), ==, 0);
}
static SignerPermissionResult flow_eval(SignerPermissions *p, const gchar *id) {
  return signer_permissions_evaluate_flow(p, PRINCIPAL, IDENTITY, 0, "event", NOSTR_KIND_SEAL,
      "room-flow", 2, 0, id, FALSE, NULL);
}
static void test_flow_scope_budget_and_restart(void) {
  g_autoptr(SignerPermissions) p = start();
  remember(p, IDENTITY, "event", NOSTR_KIND_SEAL, TRUE, 600);
  g_assert_cmpint(flow_eval(p, "first-seen"), ==, SIGNER_PERMISSION_ASK);
  g_assert_true(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 600, NULL));
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_SEAL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate_flow(p, PRINCIPAL, IDENTITY, 0, "event", NOSTR_KIND_SEAL,
      "other-flow", 2, 0, "1", FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate_flow(p, "other-client", IDENTITY, 0, "event", NOSTR_KIND_SEAL,
      "room-flow", 2, 0, "1", FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate_flow(p, PRINCIPAL, "other-identity", 0, "event", NOSTR_KIND_SEAL,
      "room-flow", 2, 0, "1", FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate_flow(p, PRINCIPAL, IDENTITY, 2, "event", NOSTR_KIND_SEAL,
      "room-flow", 2, 0, "1", FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate_flow(p, PRINCIPAL, IDENTITY, 0, "event", NOSTR_KIND_GIFT_WRAP,
      "room-flow", 2, 0, "1", FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate_flow(p, PRINCIPAL, IDENTITY, 0, "nip44_encrypt", -1,
      "room-flow", 2, 0, "1", FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(signer_permissions_evaluate_flow(p, PRINCIPAL, IDENTITY, 0, "event", NOSTR_KIND_SEAL,
      "room-flow", 3, 0, "1", FALSE, NULL), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ALLOW); /* recheck is not a new slot */
  g_assert_cmpint(flow_eval(p, "2"), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(flow_eval(p, "3"), ==, SIGNER_PERMISSION_ASK);
  g_assert_false(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 600, NULL)); /* completed ID is a tombstone */
  g_assert_false(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 99, FALSE, TRUE, 600, NULL)); /* cannot silently reset/enlarge */
  g_assert_false(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "long-flow", 2, FALSE, TRUE, 601, NULL));
  g_autoptr(GVariant) rows = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 1); /* ordinary grant only */
  g_autofree gchar *path = store_path("signer-permissions-v2.ini");
  g_autofree gchar *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_assert_null(strstr(contents, "room-flow"));
  g_autoptr(SignerPermissions) restarted = signer_permissions_new(NULL);
  g_assert_cmpint(flow_eval(restarted, "1"), ==, SIGNER_PERMISSION_ASK);
  remember(p, IDENTITY, "event", NOSTR_KIND_SEAL, FALSE, 600);
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_DENY); /* deny wins over reserved work */
  gboolean removed = FALSE;
  g_assert_true(signer_permissions_clear(p, PRINCIPAL, IDENTITY, NULL, &removed, NULL));
  g_assert_true(removed);
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ASK);
}
static void test_room_consent_atomic_failure_and_nonrequired(void) {
  g_autoptr(SignerPermissions) p = start();
  g_assert_true(signer_permissions_reload(p, NULL));
  g_assert_cmpint(flow_eval(p, "first-seen"), ==, SIGNER_PERMISSION_ASK);
  g_autofree gchar *path = store_path("signer-permissions-v2.ini");
  g_unlink(path); g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_autoptr(GError) error = NULL;
  g_assert_false(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, TRUE, TRUE, 600, &error));
  g_assert_nonnull(error);
  g_assert_cmpint(g_rmdir(path), ==, 0);
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ASK);
  g_assert_true(signer_permissions_set_required_confirmation(p, NULL, 0, NULL));
  g_assert_true(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, TRUE, TRUE, 600, NULL));
  g_assert_cmpint(evaluate(p, "nip44_encrypt", -1), ==, SIGNER_PERMISSION_ALLOW);
  g_assert_cmpint(evaluate(p, "event", NOSTR_KIND_SEAL), ==, SIGNER_PERMISSION_ALLOW);
  g_autoptr(GVariant) rows = signer_permissions_list(p, PRINCIPAL, NULL);
  g_assert_cmpuint(g_variant_n_children(rows), ==, 2);
  guint32 required = NOSTR_KIND_SEAL;
  g_assert_true(signer_permissions_set_required_confirmation(p, &required, 1, NULL));
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ASK); /* ordinary grant is not flow consent */
}
#ifdef NIP55L_TEST_TRUST_ENV
static gint64 fixture_flow_clock(gpointer data) { return *(gint64 *)data; }
static void test_flow_expiry(void) {
  g_autoptr(SignerPermissions) p = start();
  gint64 now = G_USEC_PER_SEC;
  signer_permissions_set_flow_clock_for_test(p, fixture_flow_clock, &now);
  g_assert_cmpint(flow_eval(p, "first-seen"), ==, SIGNER_PERMISSION_ASK);
  g_assert_true(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 1, NULL));
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ALLOW);
  guint64 revision = signer_permissions_revision(p);
  g_assert_true(signer_permissions_revision_is_current(p, revision));
  now += G_USEC_PER_SEC; /* the exact expiry boundary; no sleep/readiness timer */
  g_assert_false(signer_permissions_revision_is_current(p, revision));
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ASK);
  g_assert_cmpint(flow_eval(p, "2"), ==, SIGNER_PERMISSION_ASK);
  g_assert_false(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 600, NULL)); /* expired ID cannot be re-minted */
}
static void test_flow_first_seen_and_tombstone(void) {
  g_autoptr(SignerPermissions) p = start();
  gint64 now = G_USEC_PER_SEC;
  signer_permissions_set_flow_clock_for_test(p, fixture_flow_clock, &now);
  g_assert_cmpint(flow_eval(p, "first"), ==, SIGNER_PERMISSION_ASK);
  now += 9 * 60 * G_USEC_PER_SEC;
  g_assert_true(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 600, NULL));
  g_assert_cmpint(flow_eval(p, "1"), ==, SIGNER_PERMISSION_ALLOW);
  now += 60 * G_USEC_PER_SEC; /* ten minutes from arrival, not consent */
  g_assert_cmpint(flow_eval(p, "2"), ==, SIGNER_PERMISSION_ASK);
  g_assert_false(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 600, NULL));
  now += 599 * G_USEC_PER_SEC;
  g_assert_cmpint(flow_eval(p, "later"), ==, SIGNER_PERMISSION_ASK);
  g_assert_false(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 600, NULL));
  now += G_USEC_PER_SEC; /* bounded tombstone retires; new explicit consent still required */
  g_assert_cmpint(flow_eval(p, "fresh"), ==, SIGNER_PERMISSION_ASK);
  g_assert_true(signer_permissions_consent_room(p, PRINCIPAL, IDENTITY, 0,
      "room-flow", 2, FALSE, TRUE, 600, NULL));
  g_assert_cmpint(flow_eval(p, "fresh"), ==, SIGNER_PERMISSION_ALLOW);
}
#endif
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_setenv("NOSTRC_TEST_ISOLATION", "1", TRUE);
  g_autofree gchar *config = g_dir_make_tmp("signer-permissions-XXXXXX", NULL);
  g_assert_nonnull(config); g_setenv("XDG_CONFIG_HOME", config, TRUE);
  g_autofree gchar *dir = g_build_filename(config, "gnostr", NULL); g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
  g_test_add_func("/permissions/flow-scope-budget-restart", test_flow_scope_budget_and_restart);
#ifdef NIP55L_TEST_TRUST_ENV
  g_test_add_func("/permissions/flow-expiry", test_flow_expiry);
  g_test_add_func("/permissions/flow-first-seen-tombstone", test_flow_first_seen_and_tombstone);
#endif
  g_test_add_func("/permissions/room-consent-atomic-nonrequired", test_room_consent_atomic_failure_and_nonrequired);
  g_test_add_func("/permissions/management-self-test-no-scope", test_management_self_test_has_no_scope);
  g_test_add_func("/permissions/exact-scope-restart", test_exact_scope_and_restart);
  g_test_add_func("/permissions/expiry-deny-conflict", test_expiry_deny_and_conflict);
  g_test_add_func("/permissions/migration-legacy-revoke", test_migration_and_legacy_revoke);
  g_test_add_func("/permissions/review-pruning-load-failure", test_review_pruning_and_load_failure);
  g_test_add_func("/permissions/schema-profile-guard", test_schema_and_profile_guard);
  g_test_add_func("/permissions/one-time-non-event-import", test_one_time_non_event_import);
  g_test_add_func("/permissions/corrupt-record-isolation", test_bad_record_and_corrupt_legacy_isolation);
  g_test_add_func("/permissions/actual-write-failure-shared-directory", test_actual_write_failure_and_shared_directory);
  g_test_add_func("/trust-tiers/live-policy", test_tiers);
  gint result = g_test_run(); remove_files(); g_rmdir(dir); g_rmdir(config); return result;
}
