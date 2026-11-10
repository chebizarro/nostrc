#include "gh-social-lists.h"
#include <sqlite3.h>
#include <string.h>
#include <glib/gstdio.h>
#include <nostr-kinds.h>

#define ACCOUNT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static gint64 scalar(GhStore *store, const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  gint64 value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}
static GhStore *open_memory(void)
{
  GError *error = NULL;
  GhStore *store = gh_store_open_ephemeral(ACCOUNT, NULL, &error);
  g_assert_no_error(error);
  return store;
}
static GhSocialLists *bind(GhStore *store)
{
  GhSocialLists *owner = gh_social_lists_new(NULL);
  GError *error = NULL;
  g_assert_true(gh_social_lists_bind_store(owner, store, 1, &error));
  g_assert_no_error(error);
  return owner;
}
static GNostrListDocument *append(GhSocialLists *owner, gboolean priv, const gchar *name, const gchar *value, GError **error)
{
  g_autoptr(GNostrListDocument) doc = gh_social_lists_ensure(owner, NOSTR_KIND_MUTE_LIST, "", error);
  if (!doc) return NULL;
  const gchar *fields[] = { name, value };
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, priv, fields, 2, 0, 0));
  return gh_social_lists_apply(owner, NOSTR_KIND_MUTE_LIST, "", gnostr_list_document_get_local_revision(doc), ops, error);
}

static void test_indexes_rollback(void)
{
  GhStore *store = open_memory();
  g_autoptr(GhSocialLists) owner = bind(store);
  GError *error = NULL;
  g_autoptr(GNostrListDocument) first = append(owner, FALSE, "word", "CAFÉ", &error);
  g_assert_no_error(error); g_assert_nonnull(first);
  g_autoptr(GNostrListDocument) second = append(owner, TRUE, "word", "cafe\314\201", &error);
  g_assert_no_error(error); g_assert_nonnull(second);
  guint64 revision = gnostr_list_document_get_local_revision(second);
  g_assert_cmpuint(revision, ==, gnostr_list_document_get_local_revision(first) + 1);
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_mutes"), ==, 1);
  g_assert_cmpint(scalar(store, "SELECT is_private FROM social_mutes"), ==, 1);
  g_assert_cmpint(scalar(store, "SELECT length(entry_key) FROM social_mutes"), ==, 10);
  g_assert_cmpint(scalar(store, "SELECT local_revision FROM social_documents"), ==, revision);
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_edits"), ==, 2);
  for (guint rollback = 0; rollback < 2; rollback++) {
    const gchar *sql = rollback ?
      "CREATE TRIGGER reject_social BEFORE INSERT ON social_mutes BEGIN SELECT RAISE(ROLLBACK,'injected'); END" :
      "CREATE TRIGGER reject_social BEFORE INSERT ON social_mutes BEGIN SELECT RAISE(ABORT,'injected'); END";
    g_assert_true(gh_store_exec(store, sql, &error));
    GNostrListDocument *failed = append(owner, TRUE, "word", "another", &error);
    g_assert_null(failed); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(scalar(store, "SELECT local_revision FROM social_documents"), ==, revision);
    g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_edits"), ==, 2);
    g_assert_cmpint(scalar(store, "SELECT is_private FROM social_mutes"), ==, 1);
    g_autoptr(GNostrListDocument) unchanged = gnostr_list_service_lookup(gh_social_lists_get_service(owner), NOSTR_KIND_MUTE_LIST, "");
    g_assert_cmpuint(gnostr_list_document_get_local_revision(unchanged), ==, revision);
    g_assert_true(gh_store_exec(store, "DROP TRIGGER reject_social", &error));
    g_assert_no_error(error);
  }
  gh_social_lists_close(owner);
  gh_store_close(store);
}

static void test_bounds(void)
{
  GhStore *store = open_memory();
  g_autoptr(GhSocialLists) owner = bind(store);
  GError *error = NULL;
  g_autofree gchar *long_value = g_strnfill(NOSTR_LIST_MAX_FIELD_BYTES + 1, 'x');
  g_assert_null(append(owner, TRUE, "word", long_value, &error));
  g_assert_nonnull(error); g_clear_error(&error);
  /* Ensuring the hide list saves Stage 4's empty baseline before the rejected edit. */
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_documents"), ==, 1);
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_edits"), ==, 0);
  /* Fill another identity through SQL to test the disk-wide limit, not merely
   * the engine's loaded-document accounting. Never evict dirty rows. */
  g_assert_true(gh_store_exec(store,
    "INSERT INTO social_documents(kind,identifier,decoded_json,dirty) VALUES"
    "(30015,'other',zeroblob(16777000),1),(30015,'another',zeroblob(16777000),1)", &error));
  g_assert_null(append(owner, TRUE, "word", "small", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FULL); g_clear_error(&error);
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_documents"), ==, 3);
  g_assert_cmpint(scalar(store, "SELECT MIN(dirty) FROM social_documents WHERE kind=30015"), ==, 1);
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_mutes"), ==, 0);
  gh_social_lists_close(owner);
  gh_store_close(store);
}

static void test_migration_reopen(void)
{
  GError *error = NULL;
  g_autofree gchar *dir = g_dir_make_tmp("social-migrate-XXXXXX", &error);
  g_assert_no_error(error);
  guint8 raw[GH_STORE_KEY_SIZE]; memset(raw, 42, sizeof(raw));
  g_autoptr(GBytes) key = g_bytes_new(raw, sizeof(raw));
  GhStoreConfig config = { .account_pubkey = ACCOUNT, .data_dir = dir };
  const gchar *uuid = "00000000-0000-4000-8000-000000000003";
  GhStore *store = gh_store_open_with_key(&config, key, uuid, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error); g_assert_nonnull(store);
  GhStoreSocialPreferences prefs;
  g_assert_true(gh_store_social_get_preferences(store, &prefs, &error));
  g_assert_false(prefs.sync_enabled); g_assert_true(prefs.blossom_imported);
  g_assert_true(gh_store_exec(store,
    "DROP TABLE social_preferences; DROP TABLE social_documents; DROP TABLE social_edits;"
    "DROP TABLE social_removals; DROP TABLE social_publications; DROP TABLE social_follows;"
    "DROP TABLE social_mutes; DROP TABLE social_servers;"
    "DELETE FROM schema_migrations WHERE version=11; PRAGMA user_version=10;", &error));
  gh_store_close(store);
  store = gh_store_open_with_key(&config, key, uuid, GH_STORE_OPEN_NONE, &error);
  g_assert_no_error(error); g_assert_nonnull(store);
  g_assert_cmpint(scalar(store, "PRAGMA user_version"), ==, 11);
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM schema_migrations"), ==, 11);
  g_assert_true(gh_store_social_get_preferences(store, &prefs, &error));
  g_assert_false(prefs.sync_enabled); g_assert_false(prefs.blossom_imported);
  g_autoptr(GhSocialLists) owner = bind(store);
  g_autoptr(GNostrListDocument) saved = append(owner, TRUE, "word", "kept", &error);
  g_assert_no_error(error); g_assert_nonnull(saved);
  gh_social_lists_close(owner); gh_store_close(store);
  store = gh_store_open_with_key(&config, key, uuid, GH_STORE_OPEN_NONE, &error);
  g_assert_no_error(error);
  g_assert_true(gh_social_lists_bind_store(owner, store, 2, &error));
  g_autoptr(GNostrListDocument) restored = gnostr_list_service_lookup(gh_social_lists_get_service(owner), NOSTR_KIND_MUTE_LIST, "");
  g_assert_nonnull(restored);
  g_assert_cmpuint(gnostr_list_document_get_local_revision(restored), ==, gnostr_list_document_get_local_revision(saved));
  g_assert_cmpint(scalar(store, "SELECT count(*) FROM social_mutes"), ==, 1);
  gh_social_lists_close(owner); gh_store_close(store);
  store = gh_store_open_with_key(&config, key, uuid, GH_STORE_OPEN_ALLOW_CORRUPT, &error);
  g_assert_no_error(error); g_assert_true(gh_store_is_read_only(store));
  g_assert_true(gh_social_lists_bind_store(owner, store, 3, &error));
  g_assert_null(append(owner, TRUE, "word", "read-only", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT); g_clear_error(&error);
  g_assert_cmpuint(scalar(store, "SELECT local_revision FROM social_documents WHERE kind=10000"), ==,
    gnostr_list_document_get_local_revision(saved));
  gh_social_lists_close(owner); gh_store_close(store);
  g_assert_true(gh_store_delete_files(dir, ACCOUNT, &error));
  g_assert_no_error(error);
  g_autofree gchar *accounts = g_build_filename(dir, "groundhog", "accounts", NULL);
  g_autofree gchar *groundhog = g_build_filename(dir, "groundhog", NULL);
  g_rmdir(accounts); g_rmdir(groundhog); g_rmdir(dir);
}

static void test_legacy_servers(void)
{
  GError *error = NULL;
  GhStore *a = open_memory();
  GhStore *b = gh_store_open_ephemeral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", NULL, &error);
  g_assert_no_error(error);
  const gchar *legacy[] = { "https://second.example", "https://first.example/media", NULL };
  g_assert_true(gh_store_social_import_blossom_legacy(a, legacy, &error));
  g_auto(GStrv) fresh = gh_store_social_dup_servers(a, &error);
  g_assert_no_error(error); g_assert_null(fresh[0]); /* fresh v11 never imports */
  g_assert_true(gh_store_exec(a, "UPDATE social_preferences SET blossom_imported=0; CREATE TRIGGER fail_import BEFORE UPDATE ON social_preferences BEGIN SELECT RAISE(ABORT,'injected'); END", &error));
  g_assert_false(gh_store_social_import_blossom_legacy(a, legacy, &error));
  g_assert_nonnull(error); g_clear_error(&error);
  g_assert_cmpint(scalar(a, "SELECT count(*) FROM social_documents"), ==, 0);
  g_assert_cmpint(scalar(a, "SELECT count(*) FROM social_servers"), ==, 0);
  g_assert_cmpint(scalar(a, "SELECT blossom_imported FROM social_preferences"), ==, 0);
  g_assert_true(gh_store_exec(a, "DROP TRIGGER fail_import", &error));
  g_assert_true(gh_store_social_import_blossom_legacy(a, legacy, &error));
  g_auto(GStrv) imported = gh_store_social_dup_servers(a, &error);
  g_assert_no_error(error); g_assert_cmpstr(imported[0], ==, legacy[0]);
  g_assert_cmpstr(imported[1], ==, legacy[1]); g_assert_null(imported[2]);
  g_assert_cmpint(scalar(a, "SELECT blossom_imported FROM social_preferences"), ==, 1);
  g_assert_cmpint(scalar(a, "SELECT count(*) FROM social_publications"), ==, 0);
  const gchar *later[] = { "https://later.example", NULL };
  g_assert_true(gh_store_social_import_blossom_legacy(a, later, &error));
  g_auto(GStrv) unchanged = gh_store_social_dup_servers(a, &error);
  g_assert_cmpstr(unchanged[0], ==, legacy[0]);
  g_assert_true(gh_store_exec(b, "UPDATE social_preferences SET blossom_imported=0", &error));
  g_assert_true(gh_store_social_import_blossom_legacy(b, later, &error));
  g_auto(GStrv) other = gh_store_social_dup_servers(b, &error);
  g_assert_no_error(error); g_assert_cmpstr(other[0], ==, later[0]); g_assert_null(other[1]);
  g_autoptr(GhSocialLists) owner = bind(a);
  const gchar *empty[] = { NULL };
  g_assert_true(gh_social_lists_set_servers(owner, empty, &error));
  g_assert_true(gh_store_exec(a, "UPDATE social_preferences SET blossom_imported=0", &error));
  g_assert_true(gh_store_social_import_blossom_legacy(a, later, &error));
  g_auto(GStrv) cleared = gh_store_social_dup_servers(a, &error);
  g_assert_no_error(error); g_assert_null(cleared[0]); /* an explicit empty list wins */
  gh_social_lists_close(owner); gh_store_close(a); gh_store_close(b);
  while (g_main_context_iteration(NULL, FALSE)) { } /* no import callback may borrow a closed store */
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/store-social/indexes-rollback", test_indexes_rollback);
  g_test_add_func("/groundhog/store-social/bounds", test_bounds);
  g_test_add_func("/groundhog/store-social/v10-migration-reopen", test_migration_reopen);
  g_test_add_func("/groundhog/store-social/legacy-servers-per-account-atomic", test_legacy_servers);
  return g_test_run();
}
