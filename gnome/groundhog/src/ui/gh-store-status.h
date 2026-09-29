#ifndef GH_STORE_STATUS_H
#define GH_STORE_STATUS_H

#include "gh-account-store.h"
#include "gh-status.h"

G_BEGIN_DECLS

/* The status input for the active account's message store (charter §7.15
 * #14-#16): each GhAccountStore state maps onto one GhStatusStore. */
GhStatusStore gh_store_status_map(GhAccountStoreState state);

/* Keeps status's store input (and its explanation) in step with store until
 * either is finalized. */
void gh_store_status_attach(GhStatus *status, GhAccountStore *store);

G_END_DECLS
#endif
