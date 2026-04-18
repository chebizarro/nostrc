/* nd-file-store.c - In-memory NIP-94 file cache for WebDAV
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-file-store.h"
#include <string.h>

struct _NdFileStore {
  GHashTable *files;     /* path → NdFileEntry* (owned) */
  guint64     version;   /* bumped on every mutation, used as ctag */
};

NdFileStore *
nd_file_store_new(void)
{
  NdFileStore *store = g_new0(NdFileStore, 1);
  store->files = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free,
    (GDestroyNotify)nd_file_entry_free);
  store->version = 0;
  return store;
}

void
nd_file_store_free(NdFileStore *store)
{
  if (store == NULL) return;
  g_hash_table_unref(store->files);
  g_free(store);
}

void
nd_file_store_put(NdFileStore *store,
                  NdFileEntry *entry)
{
  g_return_if_fail(store != NULL);
  g_return_if_fail(entry != NULL);
  g_return_if_fail(entry->path != NULL);

  g_hash_table_replace(store->files, g_strdup(entry->path), entry);
  store->version++;
}

const NdFileEntry *
nd_file_store_get(NdFileStore *store,
                  const gchar *path)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(path != NULL, NULL);

  return g_hash_table_lookup(store->files, path);
}

gboolean
nd_file_store_remove(NdFileStore *store,
                     const gchar *path)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(path != NULL, FALSE);

  gboolean removed = g_hash_table_remove(store->files, path);
  if (removed) store->version++;
  return removed;
}

GPtrArray *
nd_file_store_list_all(NdFileStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);

  GPtrArray *list = g_ptr_array_new();
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, store->files);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    g_ptr_array_add(list, value);
  }

  return list;
}

guint
nd_file_store_count(NdFileStore *store)
{
  g_return_val_if_fail(store != NULL, 0);
  return g_hash_table_size(store->files);
}

gchar *
nd_file_store_get_ctag(NdFileStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return g_strdup_printf("%" G_GUINT64_FORMAT, store->version);
}
