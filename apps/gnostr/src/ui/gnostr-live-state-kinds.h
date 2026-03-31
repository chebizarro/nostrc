#ifndef GNOSTR_LIVE_STATE_KINDS_H
#define GNOSTR_LIVE_STATE_KINDS_H

#include <glib.h>
#include "nostr-filter.h"

G_BEGIN_DECLS

typedef enum {
  GNOSTR_LIVE_STATE_REFRESH_NONE = 0,
  GNOSTR_LIVE_STATE_REFRESH_FOLLOW_LIST,
  GNOSTR_LIVE_STATE_REFRESH_MUTE_LIST,
  GNOSTR_LIVE_STATE_REFRESH_RELAY_LIST
} GnostrLiveStateRefreshKind;

NostrFilters *gnostr_live_state_build_subscription_filters(const char *user_pubkey_hex);

GnostrLiveStateRefreshKind
gnostr_live_state_refresh_kind_from_event_json(const char *event_json,
                                               const char *user_pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_LIVE_STATE_KINDS_H */
