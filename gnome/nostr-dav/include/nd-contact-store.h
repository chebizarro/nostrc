/* nd-contact-store.h - Contact store for CardDAV (SQLite-backed)
 *
 * SPDX-License-Identifier: MIT
 *
 * Same ownership and error contract as nd-calendar-store.h: getters
 * return owned objects; NULL without @error means "not found".
 */
#ifndef ND_CONTACT_STORE_H
#define ND_CONTACT_STORE_H

#include <glib.h>
#include "nd-vcard.h"
#include "nd-store-db.h"

G_BEGIN_DECLS

typedef struct _NdContactStore NdContactStore;

NdContactStore *nd_contact_store_new(NdStoreDb *db);
void nd_contact_store_free(NdContactStore *store);

gboolean nd_contact_store_put(NdContactStore  *store,
                              const NdContact *contact,
                              gboolean        *out_created,
                              GError         **error);
NdContact *nd_contact_store_get(NdContactStore *store,
                                const gchar    *uid,
                                GError        **error);
gboolean nd_contact_store_remove(NdContactStore *store,
                                 const gchar    *uid,
                                 gboolean       *out_removed,
                                 GError        **error);
GPtrArray *nd_contact_store_list_all(NdContactStore *store,
                                     GError        **error);
gboolean nd_contact_store_count(NdContactStore *store,
                                guint          *out_count,
                                GError        **error);
gchar *nd_contact_store_get_ctag(NdContactStore *store,
                                 GError        **error);

G_END_DECLS
#endif /* ND_CONTACT_STORE_H */
