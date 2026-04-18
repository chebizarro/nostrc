/* nd-contact-store.h - In-memory contact cache for CardDAV
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ND_CONTACT_STORE_H
#define ND_CONTACT_STORE_H

#include <glib.h>
#include "nd-vcard.h"

G_BEGIN_DECLS

typedef struct _NdContactStore NdContactStore;

NdContactStore *nd_contact_store_new(void);
void nd_contact_store_free(NdContactStore *store);

void nd_contact_store_put(NdContactStore *store, NdContact *contact);
const NdContact *nd_contact_store_get(NdContactStore *store, const gchar *uid);
gboolean nd_contact_store_remove(NdContactStore *store, const gchar *uid);
GPtrArray *nd_contact_store_list_all(NdContactStore *store);
guint nd_contact_store_count(NdContactStore *store);
gchar *nd_contact_store_get_ctag(NdContactStore *store);

G_END_DECLS
#endif /* ND_CONTACT_STORE_H */
