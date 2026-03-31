#include "gnostr-startup-live-eose.h"

guint
gnostr_main_window_track_startup_live_eose_internal(GHashTable  *seen_relays,
                                                    guint        expected_relays,
                                                    const gchar *relay_url)
{
  g_return_val_if_fail(seen_relays != NULL, GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE);
  g_return_val_if_fail(relay_url != NULL && *relay_url != '\0', GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE);

  if (g_hash_table_contains(seen_relays, relay_url))
    return GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE;

  g_hash_table_add(seen_relays, g_strdup(relay_url));

  guint flags = GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE;
  guint seen_count = g_hash_table_size(seen_relays);
  if (seen_count == 1)
    flags |= GNOSTR_STARTUP_LIVE_EOSE_FLAG_FIRST;
  if (expected_relays > 0 && seen_count >= expected_relays)
    flags |= GNOSTR_STARTUP_LIVE_EOSE_FLAG_ALL;

  return flags;
}
