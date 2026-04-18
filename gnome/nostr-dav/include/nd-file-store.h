/* nd-file-store.h - In-memory NIP-94 file cache for WebDAV
 *
 * SPDX-License-Identifier: MIT
 *
 * Stores NdFileEntry objects indexed by path and provides
 * WebDAV-oriented queries (list all, get by path, by SHA-256).
 */
#ifndef ND_FILE_STORE_H
#define ND_FILE_STORE_H

#include <glib.h>
#include "nd-file-entry.h"

G_BEGIN_DECLS

typedef struct _NdFileStore NdFileStore;

/**
 * nd_file_store_new:
 *
 * Creates a new empty file store.
 * Returns: (transfer full): a new store.
 */
NdFileStore *nd_file_store_new(void);

/**
 * nd_file_store_free:
 * @store: (transfer full): the store to free
 */
void nd_file_store_free(NdFileStore *store);

/**
 * nd_file_store_put:
 * @store: the store
 * @entry: (transfer full): file entry to store (takes ownership)
 *
 * Adds or replaces a file by path.
 */
void nd_file_store_put(NdFileStore *store,
                       NdFileEntry *entry);

/**
 * nd_file_store_get:
 * @store: the store
 * @path: the file path
 *
 * Returns: (transfer none) (nullable): the entry, or NULL if not found.
 */
const NdFileEntry *nd_file_store_get(NdFileStore *store,
                                     const gchar *path);

/**
 * nd_file_store_remove:
 * @store: the store
 * @path: the file path to remove
 *
 * Returns: TRUE if a file was removed.
 */
gboolean nd_file_store_remove(NdFileStore *store,
                              const gchar *path);

/**
 * nd_file_store_list_all:
 * @store: the store
 *
 * Returns: (transfer container): GPtrArray of const NdFileEntry*.
 *   Caller owns the array but not the elements.
 */
GPtrArray *nd_file_store_list_all(NdFileStore *store);

/**
 * nd_file_store_count:
 * @store: the store
 *
 * Returns: number of files.
 */
guint nd_file_store_count(NdFileStore *store);

/**
 * nd_file_store_get_ctag:
 * @store: the store
 *
 * Returns a string that changes whenever the store is modified.
 * Used as the WebDAV getctag property for sync.
 *
 * Returns: (transfer full): ctag string.
 */
gchar *nd_file_store_get_ctag(NdFileStore *store);

G_END_DECLS
#endif /* ND_FILE_STORE_H */
