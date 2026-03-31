#define G_LOG_DOMAIN "gnostr-live-state-kinds"

#include "gnostr-live-state-kinds.h"

#include <nostr-gobject-1.0/nostr_json.h>
#include <string.h>

NostrFilters *
gnostr_live_state_build_subscription_filters(const char *user_pubkey_hex)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *timeline = nostr_filter_new();
  int timeline_kinds[] = {0, 1, 5, 6, 7, 16, 1111};

  nostr_filter_set_kinds(timeline, timeline_kinds, G_N_ELEMENTS(timeline_kinds));
  nostr_filters_add(filters, timeline);

  if (user_pubkey_hex && strlen(user_pubkey_hex) == 64) {
    NostrFilter *state = nostr_filter_new();
    int state_kinds[] = {3, 10000, 10002};
    const char *authors[] = { user_pubkey_hex };

    nostr_filter_set_kinds(state, state_kinds, G_N_ELEMENTS(state_kinds));
    nostr_filter_set_authors(state, authors, 1);
    nostr_filters_add(filters, state);
  }

  return filters;
}

GnostrLiveStateRefreshKind
gnostr_live_state_refresh_kind_from_event_json(const char *event_json,
                                               const char *user_pubkey_hex)
{
  if (!event_json || !*event_json || !user_pubkey_hex || strlen(user_pubkey_hex) != 64)
    return GNOSTR_LIVE_STATE_REFRESH_NONE;

  g_autofree char *pubkey_hex = gnostr_json_get_string(event_json, "pubkey", NULL);
  if (!pubkey_hex || g_ascii_strcasecmp(pubkey_hex, user_pubkey_hex) != 0)
    return GNOSTR_LIVE_STATE_REFRESH_NONE;

  g_autofree char *kind_raw = gnostr_json_get_raw(event_json, "kind", NULL);
  if (!kind_raw || !*kind_raw)
    return GNOSTR_LIVE_STATE_REFRESH_NONE;

  int kind = (int)g_ascii_strtoll(kind_raw, NULL, 10);
  switch (kind) {
    case 3:
      return GNOSTR_LIVE_STATE_REFRESH_FOLLOW_LIST;
    case 10000:
      return GNOSTR_LIVE_STATE_REFRESH_MUTE_LIST;
    case 10002:
      return GNOSTR_LIVE_STATE_REFRESH_RELAY_LIST;
    default:
      return GNOSTR_LIVE_STATE_REFRESH_NONE;
  }
}
