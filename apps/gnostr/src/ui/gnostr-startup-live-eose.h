#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  GNOSTR_STARTUP_LIVE_EOSE_FLAG_NONE  = 0,
  GNOSTR_STARTUP_LIVE_EOSE_FLAG_FIRST = 1u << 0,
  GNOSTR_STARTUP_LIVE_EOSE_FLAG_ALL   = 1u << 1,
} GnostrStartupLiveEoseFlags;

guint gnostr_main_window_track_startup_live_eose_internal(GHashTable  *seen_relays,
                                                           guint        expected_relays,
                                                           const gchar *relay_url);

G_END_DECLS
