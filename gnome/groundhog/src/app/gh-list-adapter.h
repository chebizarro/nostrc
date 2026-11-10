#ifndef GH_LIST_ADAPTER_H
#define GH_LIST_ADAPTER_H
#include "gh-store-social.h"
G_BEGIN_DECLS
/* Storage-only adapter: no relay, crypto, signer or publication callbacks.
 * Revoke before the borrowed store closes, even if a caller retains service. */
typedef struct _GhListAdapter GhListAdapter;
GhListAdapter *gh_list_adapter_new(GhStore *store, guint64 generation);
const GNostrListAdapter *gh_list_adapter_get_callbacks(void);
void gh_list_adapter_revoke(GhListAdapter *self);
void gh_list_adapter_free(GhListAdapter *self);
G_END_DECLS
#endif
