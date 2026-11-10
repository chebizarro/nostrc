#ifndef GH_LIST_ADAPTER_H
#define GH_LIST_ADAPTER_H
#include "gh-store-social.h"
#include "gh-auth-policy.h"
G_BEGIN_DECLS
/* Revoke before the borrowed store closes, even if a caller retains service. */
typedef struct _GhListAdapter GhListAdapter;
typedef struct {
  GhAccountController *accounts;
  const GhRelayTransport *scope_transport;
  const GhRelayAuthTransport *scope_auth_transport;
  gpointer scope_data;
  const GhRelayPublishTransport *publish_transport;
  const GhRelayPublishAuthTransport *publish_auth_transport;
  gpointer publish_data;
  guint publish_deadline;
} GhListAdapterNetwork;
/* Owned copies keep transport seams valid across delayed account binds. */
void gh_list_adapter_network_copy(GhListAdapterNetwork *dest, const GhListAdapterNetwork *source);
void gh_list_adapter_network_clear(GhListAdapterNetwork *network);
typedef GPtrArray *(*GhListDeriveFunc)(gpointer data, GNostrListDocument *snapshot, GError **error);
GhListAdapter *gh_list_adapter_new(GhStore *store, guint64 generation);
/* Session-only compatibility service for existing group-list callers. */
GhListAdapter *gh_list_adapter_new_volatile(const gchar *account, guint64 generation);
void gh_list_adapter_bind_service(GhListAdapter *self, GNostrListService *service);
void gh_list_adapter_set_network(GhListAdapter *self, const GhListAdapterNetwork *network);
void gh_list_adapter_set_derive(GhListAdapter *self, GhListDeriveFunc derive, gpointer data);
const GNostrListAdapter *gh_list_adapter_get_callbacks(void);
/* Pending first remote hide-list import: no projection was committed yet. */
gboolean gh_list_adapter_get_import(GhListAdapter *self, guint *accounts, guint *items);
const gchar *gh_list_adapter_get_import_id(GhListAdapter *self);
gboolean gh_list_adapter_confirm_import(GhListAdapter *self, GNostrListService *service,
                                         gboolean accept, GError **error);
gboolean gh_list_adapter_hide_unsynced(GhListAdapter *self);
void gh_list_adapter_revoke(GhListAdapter *self);
void gh_list_adapter_free(GhListAdapter *self);
G_END_DECLS
#endif
