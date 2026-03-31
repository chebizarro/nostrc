#define G_LOG_DOMAIN "gnostr-sync-bridge-mute"

#include "gnostr-sync-bridge-mute.h"

#include <nostr-gobject-1.0/gnostr-mute-list.h>

gboolean
gnostr_sync_bridge_reload_mute_list(const char *user_pubkey_hex)
{
  if (!user_pubkey_hex || !*user_pubkey_hex)
    return FALSE;

  GNostrMuteList *mute = gnostr_mute_list_get_default();
  if (!mute)
    return FALSE;

  gnostr_mute_list_fetch_async(mute, user_pubkey_hex, NULL, NULL, NULL);
  return TRUE;
}
