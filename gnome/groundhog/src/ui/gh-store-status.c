#include "gh-store-status.h"

#define ADAPTER_DATA "groundhog-store-status"

GhStatusStore
gh_store_status_map(GhAccountStoreState state)
{
  switch (state) {
  case GH_ACCOUNT_STORE_OPENING: return GH_STATUS_STORE_OPENING;
  case GH_ACCOUNT_STORE_OPEN: return GH_STATUS_STORE_OPEN;
  case GH_ACCOUNT_STORE_EPHEMERAL: return GH_STATUS_STORE_EPHEMERAL;
  case GH_ACCOUNT_STORE_LOCKED: return GH_STATUS_STORE_LOCKED;
  case GH_ACCOUNT_STORE_UNAVAILABLE: return GH_STATUS_STORE_UNAVAILABLE;
  case GH_ACCOUNT_STORE_KEY_MISSING: return GH_STATUS_STORE_KEY_MISSING;
  case GH_ACCOUNT_STORE_CORRUPT: return GH_STATUS_STORE_CORRUPT;
  case GH_ACCOUNT_STORE_ERROR: return GH_STATUS_STORE_ERROR;
  case GH_ACCOUNT_STORE_INACTIVE:
  default:
    return GH_STATUS_STORE_NONE;
  }
}

static void
weak_ref_free(gpointer data)
{
  g_weak_ref_clear(data);
  g_free(data);
}

static void
sync_status(GhStatus *status)
{
  GWeakRef *ref = g_object_get_data(G_OBJECT(status), ADAPTER_DATA);
  g_autoptr(GhAccountStore) store = g_weak_ref_get(ref);
  if (!store) {
    gh_status_set_store(status, GH_STATUS_STORE_NONE, NULL);
    return;
  }
  gh_status_set_store(status, gh_store_status_map(gh_account_store_get_state(store)),
                      gh_account_store_get_error(store));
}

void
gh_store_status_attach(GhStatus *status, GhAccountStore *store)
{
  g_return_if_fail(GH_IS_STATUS(status));
  g_return_if_fail(GH_IS_ACCOUNT_STORE(store));
  g_return_if_fail(g_object_get_data(G_OBJECT(status), ADAPTER_DATA) == NULL);
  GWeakRef *ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, store);
  g_object_set_data_full(G_OBJECT(status), ADAPTER_DATA, ref, weak_ref_free);
  g_signal_connect_object(store, "changed", G_CALLBACK(sync_status), status,
                          G_CONNECT_SWAPPED);
  sync_status(status);
}
