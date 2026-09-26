/* nd-file-store.h - NIP-94 file store for WebDAV (SQLite-backed)
 *
 * SPDX-License-Identifier: MIT
 *
 * Same ownership and error contract as nd-calendar-store.h: getters
 * return owned objects; NULL without @error means "not found".
 */
#ifndef ND_FILE_STORE_H
#define ND_FILE_STORE_H

#include <glib.h>
#include "nd-file-entry.h"
#include "nd-store-db.h"

G_BEGIN_DECLS

typedef struct _NdFileStore NdFileStore;

NdFileStore *nd_file_store_new(NdStoreDb *db);
void nd_file_store_free(NdFileStore *store);

gboolean nd_file_store_put(NdFileStore       *store,
                           const NdFileEntry *entry,
                           gboolean          *out_created,
                           GError           **error);
NdFileEntry *nd_file_store_get(NdFileStore *store,
                               const gchar *path,
                               GError     **error);
gboolean nd_file_store_remove(NdFileStore *store,
                              const gchar *path,
                              gboolean    *out_removed,
                              GError     **error);
GPtrArray *nd_file_store_list_all(NdFileStore *store,
                                  GError     **error);
gboolean nd_file_store_count(NdFileStore *store,
                             guint       *out_count,
                             GError     **error);
gchar *nd_file_store_get_ctag(NdFileStore *store,
                              GError     **error);

G_END_DECLS
#endif /* ND_FILE_STORE_H */
