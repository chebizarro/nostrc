#include "gh-inbox-status.h"

#define ADAPTER_DATA "groundhog-inbox-status"

typedef struct {
  GWeakRef inbox;
  GWeakRef relays;
} Adapter;

static void
adapter_free(gpointer data)
{
  Adapter *adapter = data;
  g_weak_ref_clear(&adapter->inbox);
  g_weak_ref_clear(&adapter->relays);
  g_free(adapter);
}

GhStatusInbox
gh_inbox_status_map(GhDmInboxState inbox, GhAccountRelaysState relays,
                    gboolean inbox_has_relays)
{
  switch (inbox) {
  case GH_DM_INBOX_NO_INBOX_RELAYS:
    switch (relays) {
    case GH_ACCOUNT_RELAYS_NO_SOURCES: return GH_STATUS_INBOX_NO_SOURCES;
    case GH_ACCOUNT_RELAYS_UNREACHABLE: return GH_STATUS_INBOX_LOOKUP_FAILED;
    case GH_ACCOUNT_RELAYS_COMPLETE: return GH_STATUS_INBOX_MISSING;
    /* INACTIVE: the lookup for this account generation has not begun. */
    case GH_ACCOUNT_RELAYS_INACTIVE:
    case GH_ACCOUNT_RELAYS_DISCOVERING:
    default:
      return GH_STATUS_INBOX_LOOKING;
    }
  case GH_DM_INBOX_CONNECTING: return GH_STATUS_INBOX_CONNECTING;
  case GH_DM_INBOX_BACKFILLING: return GH_STATUS_INBOX_BACKFILLING;
  case GH_DM_INBOX_LIVE: return GH_STATUS_INBOX_LIVE;
  case GH_DM_INBOX_ERROR:
    return inbox_has_relays ? GH_STATUS_INBOX_UNREACHABLE : GH_STATUS_INBOX_ERROR;
  /* The account's store is not open: its own status input explains why
   * (gh-store-status.h), and nothing is subscribed. */
  case GH_DM_INBOX_NO_STORAGE:
  case GH_DM_INBOX_INACTIVE:
  default:
    return GH_STATUS_INBOX_INACTIVE;
  }
}

static void
sync_status(GhStatus *status)
{
  Adapter *adapter = g_object_get_data(G_OBJECT(status), ADAPTER_DATA);
  g_autoptr(GhDmInbox) inbox = g_weak_ref_get(&adapter->inbox);
  g_autoptr(GhAccountRelays) relays = g_weak_ref_get(&adapter->relays);
  if (!inbox || !relays) {
    gh_status_set_inbox(status, GH_STATUS_INBOX_INACTIVE, NULL);
    return;
  }
  GhStatusInbox state = gh_inbox_status_map(gh_dm_inbox_get_state(inbox),
                                            gh_account_relays_get_state(relays),
                                            gh_dm_inbox_get_relays(inbox) != NULL);
  gh_status_set_inbox(status, state, gh_dm_inbox_get_error(inbox));
}

void
gh_inbox_status_attach(GhStatus *status, GhDmInbox *inbox, GhAccountRelays *relays)
{
  g_return_if_fail(GH_IS_STATUS(status));
  g_return_if_fail(GH_IS_DM_INBOX(inbox));
  g_return_if_fail(GH_IS_ACCOUNT_RELAYS(relays));
  g_return_if_fail(g_object_get_data(G_OBJECT(status), ADAPTER_DATA) == NULL);
  Adapter *adapter = g_new0(Adapter, 1);
  g_weak_ref_init(&adapter->inbox, inbox);
  g_weak_ref_init(&adapter->relays, relays);
  g_object_set_data_full(G_OBJECT(status), ADAPTER_DATA, adapter, adapter_free);
  /* The inbox reconciles on its own "changed" handler for relays first (it
   * connected earlier), so both are current when this runs. */
  g_signal_connect_object(inbox, "changed", G_CALLBACK(sync_status), status,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(relays, "changed", G_CALLBACK(sync_status), status,
                          G_CONNECT_SWAPPED);
  sync_status(status);
}
