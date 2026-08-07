#define G_LOG_DOMAIN "gnostr-sync-bridge-mute"

#include "gnostr-sync-bridge-mute.h"

#include <nostr-gobject-1.0/gnostr-mute-list.h>

/* F36: report fetch completion instead of discarding the result. */
static void
on_mute_list_fetch_done(GNostrMuteList *self, gboolean success, gpointer user_data)
{
  (void)self;
  (void)user_data;
  if (success)
    g_debug("mute list reload complete");
  else
    g_warning("mute list reload failed");
}

gboolean
gnostr_sync_bridge_reload_mute_list(const char *user_pubkey_hex)
{
  if (!user_pubkey_hex || !*user_pubkey_hex)
    return FALSE;

  GNostrMuteList *mute = gnostr_mute_list_get_default();
  if (!mute)
    return FALSE;

  gnostr_mute_list_fetch_async(mute, user_pubkey_hex, NULL,
                               on_mute_list_fetch_done, NULL);
  return TRUE;
}
