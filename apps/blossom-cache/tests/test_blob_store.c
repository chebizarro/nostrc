/*
 * test_blob_store.c - Unit tests for BcBlobStore
 *
 * SPDX-License-Identifier: MIT
 */

#include "bc-blob-store.h"
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

/* Compute SHA-256 of a string for test data */
static gchar *
compute_sha256(const gchar *data, gsize len)
{
  g_autoptr(GChecksum) ck = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(ck, (const guchar *)data, len);
  return g_strdup(g_checksum_get_string(ck));
}

/* ---- Fixtures ---- */

typedef struct {
  gchar       *tmp_dir;
  BcBlobStore *store;
} StoreFixture;

static void
fixture_setup(StoreFixture *f, gconstpointer data)
{
  (void)data;
  GError *error = NULL;

  f->tmp_dir = g_dir_make_tmp("blossom-cache-test-XXXXXX", &error);
  g_assert_no_error(error);

  f->store = bc_blob_store_new_sqlite(f->tmp_dir, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->store);
}

static void
rm_rf(const gchar *path)
{
  g_autoptr(GFile) dir = g_file_new_for_path(path);
  g_autoptr(GFileEnumerator) enumerator =
    g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                              NULL, NULL);

  if (enumerator != NULL) {
    GFileInfo *child_info = NULL;
    while (g_file_enumerator_iterate(enumerator, &child_info, NULL, NULL, NULL) &&
           child_info != NULL) {
      const gchar *name = g_file_info_get_name(child_info);
      g_autofree gchar *child_path = g_build_filename(path, name, NULL);

      if (g_file_test(child_path, G_FILE_TEST_IS_DIR)) {
        rm_rf(child_path);
      } else {
        g_unlink(child_path);
      }
      child_info = NULL;
    }
  }

  g_rmdir(path);
}

static void
fixture_teardown(StoreFixture *f, gconstpointer data)
{
  (void)data;
  g_clear_object(&f->store);
  rm_rf(f->tmp_dir);
  g_free(f->tmp_dir);
}

/* ---- Tests ---- */

static void
test_store_empty(StoreFixture *f, gconstpointer data)
{
  (void)data;

  g_assert_cmpuint(bc_blob_store_get_blob_count(f->store), ==, 0);
  g_assert_cmpint(bc_blob_store_get_total_size(f->store), ==, 0);
  g_assert_false(bc_blob_store_contains(f->store,
    "deadbeef00000000000000000000000000000000000000000000000000000000"));
}

static void
test_store_put_and_get(StoreFixture *f, gconstpointer data)
{
  (void)data;

  const gchar *content = "Hello, Blossom!";
  gsize len = strlen(content);
  g_autofree gchar *sha = compute_sha256(content, len);
  g_autoptr(GBytes) bytes = g_bytes_new(content, len);

  GError *error = NULL;

  /* Put with verification */
  gboolean ok = bc_blob_store_put(f->store, sha, bytes, "text/plain", TRUE, &error);
  g_assert_no_error(error);
  g_assert_true(ok);

  /* Contains? */
  g_assert_true(bc_blob_store_contains(f->store, sha));

  /* Get info */
  g_autoptr(BcBlobInfo) info = bc_blob_store_get_info(f->store, sha, &error);
  g_assert_no_error(error);
  g_assert_nonnull(info);
  g_assert_cmpstr(info->sha256, ==, sha);
  g_assert_cmpint(info->size, ==, (gint64)len);
  g_assert_cmpstr(info->mime_type, ==, "text/plain");
  g_assert_cmpuint(info->access_count, ==, 1);

  /* Content file should exist */
  g_autofree gchar *path = bc_blob_store_get_content_path(f->store, sha);
  g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

  /* Read content and verify */
  gchar *read_content = NULL;
  gsize read_len = 0;
  g_assert_true(g_file_get_contents(path, &read_content, &read_len, NULL));
  g_assert_cmpmem(content, len, read_content, read_len);
  g_free(read_content);

  /* Counts */
  g_assert_cmpuint(bc_blob_store_get_blob_count(f->store), ==, 1);
  g_assert_cmpint(bc_blob_store_get_total_size(f->store), ==, (gint64)len);
}

static void
test_store_put_duplicate(StoreFixture *f, gconstpointer data)
{
  (void)data;

  const gchar *content = "duplicate test";
  gsize len = strlen(content);
  g_autofree gchar *sha = compute_sha256(content, len);
  g_autoptr(GBytes) bytes = g_bytes_new(content, len);

  GError *error = NULL;

  g_assert_true(bc_blob_store_put(f->store, sha, bytes, NULL, TRUE, &error));
  g_assert_no_error(error);

  /* Second put should be a no-op success */
  g_assert_true(bc_blob_store_put(f->store, sha, bytes, NULL, TRUE, &error));
  g_assert_no_error(error);

  g_assert_cmpuint(bc_blob_store_get_blob_count(f->store), ==, 1);
}

static void
test_store_hash_mismatch(StoreFixture *f, gconstpointer data)
{
  (void)data;

  const gchar *content = "some data";
  gsize len = strlen(content);
  g_autoptr(GBytes) bytes = g_bytes_new(content, len);

  GError *error = NULL;
  gboolean ok = bc_blob_store_put(f->store,
    "0000000000000000000000000000000000000000000000000000000000000000",
    bytes, NULL, TRUE, &error);

  g_assert_false(ok);
  g_assert_error(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_HASH_MISMATCH);
  g_clear_error(&error);

  g_assert_cmpuint(bc_blob_store_get_blob_count(f->store), ==, 0);
}

static void
test_store_delete(StoreFixture *f, gconstpointer data)
{
  (void)data;

  const gchar *content = "delete me";
  gsize len = strlen(content);
  g_autofree gchar *sha = compute_sha256(content, len);
  g_autoptr(GBytes) bytes = g_bytes_new(content, len);

  GError *error = NULL;
  bc_blob_store_put(f->store, sha, bytes, "text/plain", TRUE, &error);
  g_assert_no_error(error);
  g_assert_true(bc_blob_store_contains(f->store, sha));

  g_assert_true(bc_blob_store_delete(f->store, sha, &error));
  g_assert_no_error(error);
  g_assert_false(bc_blob_store_contains(f->store, sha));
  g_assert_cmpuint(bc_blob_store_get_blob_count(f->store), ==, 0);
}

static void
test_store_evict_lru(StoreFixture *f, gconstpointer data)
{
  (void)data;

  GError *error = NULL;

  /* Insert 3 blobs with distinct access times */
  for (int i = 0; i < 3; i++) {
    g_autofree gchar *content = g_strdup_printf("blob number %d with padding", i);
    gsize len = strlen(content);
    g_autofree gchar *sha = compute_sha256(content, len);
    g_autoptr(GBytes) bytes = g_bytes_new(content, len);

    bc_blob_store_put(f->store, sha, bytes, "text/plain", TRUE, &error);
    g_assert_no_error(error);

    /* Small sleep to ensure different timestamps */
    g_usleep(10000); /* 10ms */
  }

  g_assert_cmpuint(bc_blob_store_get_blob_count(f->store), ==, 3);

  /* Evict enough to remove at least 1 blob */
  gint64 total = bc_blob_store_get_total_size(f->store);
  gint evicted = bc_blob_store_evict_lru(f->store, total / 2, &error);
  g_assert_no_error(error);
  g_assert_cmpint(evicted, >, 0);
  g_assert_cmpuint(bc_blob_store_get_blob_count(f->store), <, 3);
}

static void
test_store_not_found(StoreFixture *f, gconstpointer data)
{
  (void)data;

  GError *error = NULL;
  BcBlobInfo *info = bc_blob_store_get_info(f->store,
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    &error);

  g_assert_null(info);
  g_assert_error(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);
}

/* ---- Main ---- */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add("/blob-store/empty", StoreFixture, NULL,
             fixture_setup, test_store_empty, fixture_teardown);
  g_test_add("/blob-store/put-and-get", StoreFixture, NULL,
             fixture_setup, test_store_put_and_get, fixture_teardown);
  g_test_add("/blob-store/put-duplicate", StoreFixture, NULL,
             fixture_setup, test_store_put_duplicate, fixture_teardown);
  g_test_add("/blob-store/hash-mismatch", StoreFixture, NULL,
             fixture_setup, test_store_hash_mismatch, fixture_teardown);
  g_test_add("/blob-store/delete", StoreFixture, NULL,
             fixture_setup, test_store_delete, fixture_teardown);
  g_test_add("/blob-store/evict-lru", StoreFixture, NULL,
             fixture_setup, test_store_evict_lru, fixture_teardown);
  g_test_add("/blob-store/not-found", StoreFixture, NULL,
             fixture_setup, test_store_not_found, fixture_teardown);

  return g_test_run();
}
