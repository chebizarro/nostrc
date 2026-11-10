#include "gh-social-lists.h"
#include "gh-list-adapter.h"
#include <nostr-kinds.h>
#include <sqlite3.h>
#include <string.h>
#define ACCOUNT_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ACCOUNT_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void changed(GhSocialLists *self, GNostrListDocument *doc, guint *count)
{
  (void)self; (void)doc; (*count)++;
}
static void test_local_account_isolation(void)
{
  GError *error = NULL;
  GhStore *a = gh_store_open_ephemeral(ACCOUNT_A, NULL, &error);
  g_assert_no_error(error);
  GhStore *b = gh_store_open_ephemeral(ACCOUNT_B, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhSocialLists) owner = gh_social_lists_new(NULL);
  g_assert_true(gh_social_lists_bind_store(owner, a, 1, &error));
  g_assert_no_error(error);
  g_assert_false(gh_social_lists_get_sync_enabled(owner));
  g_assert_false(gh_social_lists_set_sync_enabled(owner, TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED); g_clear_error(&error);
  g_autoptr(GNostrListDocument) empty = gh_social_lists_ensure(owner, NOSTR_KIND_INTEREST_SETS, "local", &error);
  g_assert_no_error(error); g_assert_nonnull(empty);
  guint count = 0;
  g_signal_connect(owner, "document-changed", G_CALLBACK(changed), &count);
  const gchar *fields[] = { "t", "groundhog", "", "trailing" };
  const gchar *extension[] = { "x-extension", "opaque", "", "more" };
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, TRUE, fields, 4, 0, 0));
  g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, TRUE, extension, 4, 0, 0));
  g_autoptr(GNostrListDocument) first = gh_social_lists_apply(owner, NOSTR_KIND_INTEREST_SETS, "local", 0, ops, &error);
  g_assert_no_error(error); g_assert_nonnull(first); g_assert_cmpuint(count, ==, 1);
  g_assert_true(gnostr_list_document_get_dirty(first));
  g_assert_null(gnostr_list_document_get_source_json(first));
  g_assert_cmpuint(nostr_nip51_document_tag_count(gnostr_list_document_get_codec(first), TRUE), ==, 2);
  g_assert_null(gh_social_lists_apply(owner, NOSTR_KIND_INTEREST_SETS, "local", 0, ops, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG); g_clear_error(&error);
  g_assert_cmpuint(count, ==, 1);
  g_autofree gchar *export = gh_social_lists_export(first);
  g_assert_nonnull(strstr(export, "trailing"));
  g_autoptr(GNostrListDocument) empty_set = gh_social_lists_ensure(owner, NOSTR_KIND_INTEREST_SETS, "empty", &error);
  g_assert_no_error(error);
  g_autofree gchar *empty_export = gh_social_lists_export(empty_set);
  g_autoptr(GNostrListService) stale = g_object_ref(gh_social_lists_get_service(owner));
  g_assert_true(gh_social_lists_bind_store(owner, b, 2, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(gh_social_lists_get_service(owner))), ==, 0);
  g_assert_null(gnostr_list_service_apply(stale, NOSTR_KIND_INTEREST_SETS, "local", 1, ops, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED); g_clear_error(&error);
  g_autoptr(GNostrListDocument) imported = gh_social_lists_import(owner, export, &error);
  g_assert_no_error(error); g_assert_nonnull(imported);
  g_assert_null(gh_social_lists_import(owner, export, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS); g_clear_error(&error);
  g_autoptr(GNostrListDocument) empty_import = gh_social_lists_import(owner, empty_export, &error);
  g_assert_no_error(error); g_assert_nonnull(empty_import);
  g_assert_cmpuint(gnostr_list_document_get_local_revision(empty_import), ==, 1);
  g_autofree gchar *roundtrip = gh_social_lists_export(empty_import);
  g_assert_cmpstr(roundtrip, ==, empty_export);
  g_assert_true(gh_social_lists_bind_store(owner, b, 22, &error));
  g_assert_no_error(error);
  g_autoptr(GNostrListDocument) empty_restored = gnostr_list_service_lookup(gh_social_lists_get_service(owner), NOSTR_KIND_INTEREST_SETS, "empty");
  g_assert_nonnull(empty_restored);
  g_assert_cmpuint(nostr_nip51_document_tag_count(gnostr_list_document_get_codec(empty_restored), FALSE), ==, 1);
  g_assert_cmpuint(nostr_nip51_document_tag_count(gnostr_list_document_get_codec(empty_restored), TRUE), ==, 0);
  gh_social_lists_close(owner);
  gh_store_close(b);
  g_assert_true(gh_social_lists_bind_store(owner, a, 3, &error));
  g_assert_no_error(error);
  g_autoptr(GNostrListDocument) restored = gnostr_list_service_lookup(gh_social_lists_get_service(owner), NOSTR_KIND_INTEREST_SETS, "local");
  g_assert_cmpuint(gnostr_list_document_get_local_revision(restored), ==, 1);
  g_autofree gchar *after = gh_social_lists_export(restored);
  g_assert_cmpstr(export, ==, after);
  gh_social_lists_close(owner);
  gh_store_close(a);
  while (g_main_context_iteration(NULL, FALSE));
}

static void test_malformed_and_limits(void)
{
  GError *error = NULL;
  GhStore *store = gh_store_open_ephemeral(ACCOUNT_A, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhSocialLists) owner = gh_social_lists_new(NULL);
  g_assert_true(gh_social_lists_bind_store(owner, store, 1, &error));
  const gchar *bad[] = { "{}", "{\"kind\":30015,\"identifier\":\"x\",\"public\":[],\"private\":[[\"t\",4]]}",
    "{\"kind\":30015,\"identifier\":\"x\",\"public\":[[\"d\",\"different\"]],\"private\":[]}",
    "{\"kind\":3,\"identifier\":\"\",\"public\":[[\"p\",\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]],\"private\":[]}" };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_assert_null(gh_social_lists_import(owner, bad[i], &error));
    g_assert_nonnull(error); g_clear_error(&error);
  }
  g_autofree gchar *id = g_strnfill(257, 'x');
  g_assert_null(gh_social_lists_ensure(owner, NOSTR_KIND_INTEREST_SETS, id, &error));
  g_assert_nonnull(error); g_clear_error(&error);
  g_autoptr(GNostrListDocument) doc = gh_social_lists_ensure(owner, NOSTR_KIND_INTEREST_SETS, "bounds", &error);
  g_assert_nonnull(doc); g_assert_no_error(error);
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_autofree gchar *large = g_strnfill(16000, 'x');
  const gchar *tag[] = { "x-extension", large };
  for (guint i = 0; i < 4; i++) g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, TRUE, tag, 2, 0, 0));
  g_assert_null(gh_social_lists_apply(owner, NOSTR_KIND_INTEREST_SETS, "bounds", 0, ops, &error));
  g_assert_nonnull(error); g_clear_error(&error);
  g_assert_true(gh_social_lists_set_sync_enabled(owner, FALSE, &error));
  g_assert_no_error(error);
  gh_social_lists_close(owner); gh_store_close(store);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/privacy/social/default-no-sync", test_local_account_isolation);
  g_test_add_func("/groundhog/social-lists/malformed-and-limits", test_malformed_and_limits);
  return g_test_run();
}
