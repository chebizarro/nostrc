#ifndef GH_STORE_BLOSSOM_H
#define GH_STORE_BLOSSOM_H

#include "gh-store.h"

G_BEGIN_DECLS

/* The account's per-server consent to upload attachments as the account
 * (privacy charter §6 step 4, D6, AT-6; nostrc-dnsc), kept inside the
 * account's encrypted store: which attachment servers an account uses, and
 * that the account is known to them, is private, so it is never in GSettings
 * (PD-11, PT-11). One row of the store's `meta` table per server, under the
 * key GH_STORE_BLOSSOM_CONSENT_PREFIX + the normalized server URL
 * (gh_blossom_client_normalize_server()), valued with when the user
 * consented; no row means no consent, so a revocation deletes the row. It
 * belongs to this account only (another account's store has its own) and
 * goes with the store (forget crypto-shreds it). Store thread only. */

#define GH_STORE_BLOSSOM_CONSENT_PREFIX "blossom-consent:"
#define GH_STORE_BLOSSOM_MAX_SERVER 2048

/* Records (consent) or deletes (!consent) the consent for server, a
 * normalized http(s) URL of at most GH_STORE_BLOSSOM_MAX_SERVER bytes without
 * control characters (INVALID otherwise), at unix time now. CORRUPT on a
 * read-only store. */
gboolean gh_store_blossom_set_consent(GhStore *store, const gchar *server, gboolean consent,
                                      gint64 now, GError **error);
/* Every server the account consented to, sorted (an empty vector for none). */
GStrv gh_store_blossom_dup_consents(GhStore *store, GError **error);

G_END_DECLS
#endif
