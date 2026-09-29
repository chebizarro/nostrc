#ifndef GH_STORE_CONTACTS_H
#define GH_STORE_CONTACTS_H

#include "gh-store.h"

G_BEGIN_DECLS

/* The account's own marks on its contacts, kept in the encrypted store
 * (privacy charter §3.3 table `contacts`, schema v3; P8: local only, never
 * published or shared; G19). Store thread only.
 *
 * Verified. The account compared a contact's key with them out of band
 * (the full npub, or Groundhog's safety code, gh-privacy-summary.h) and
 * marked it verified. Groundhog checks nothing with anyone: the mark is
 * only the user's own record, which is exactly what the UI says. */

/* When pubkey (64 lowercase hex) was marked verified (unix seconds), or 0
 * when it is not marked. */
gboolean gh_store_contacts_get_verified(GhStore *store, const gchar *pubkey,
                                        gint64 *out_verified_at, GError **error);
/* Marks pubkey verified at verified_at (> 0), or clears the mark (0). INVALID
 * for a malformed key or a negative time; CORRUPT on a read-only store. */
gboolean gh_store_contacts_set_verified(GhStore *store, const gchar *pubkey,
                                        gint64 verified_at, GError **error);

G_END_DECLS
#endif
