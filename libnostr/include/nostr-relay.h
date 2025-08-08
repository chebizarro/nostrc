#ifndef __NOSTR_RELAY_H__
#define __NOSTR_RELAY_H__

/* Transitional header exposing GLib-friendly names for Relay. */

#include <stdbool.h>
#include "relay.h"        /* legacy Relay and APIs */
#include "channel.h"      /* GoChannel */
#include "context.h"      /* GoContext */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef for GLib-style naming */
typedef Relay NostrRelay;

/* New API names mapped to legacy implementations */
#define nostr_relay_new             new_relay
#define nostr_relay_free            free_relay
#define nostr_relay_connect         relay_connect
#define nostr_relay_disconnect      relay_disconnect
#define nostr_relay_close           relay_close
#define nostr_relay_subscribe       relay_subscribe
#define nostr_relay_prepare_subscription relay_prepare_subscription
#define nostr_relay_publish         relay_publish
#define nostr_relay_auth            relay_auth
#define nostr_relay_count           relay_count
#define nostr_relay_is_connected    relay_is_connected
#define nostr_relay_enable_debug_raw relay_enable_debug_raw
#define nostr_relay_get_debug_raw_channel relay_get_debug_raw_channel

/* Accessors (GLib-friendly) */
/**
 * nostr_relay_get_url_const:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): internal URL string
 */
const char *nostr_relay_get_url_const(const NostrRelay *relay);

/**
 * nostr_relay_get_context:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): connection GoContext
 */
GoContext *nostr_relay_get_context(const NostrRelay *relay);

/**
 * nostr_relay_get_write_channel:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): internal write queue channel; owned by relay
 */
GoChannel *nostr_relay_get_write_channel(const NostrRelay *relay);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_RELAY_H__ */
