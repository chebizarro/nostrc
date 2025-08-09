#ifndef __NOSTR_RELAY_H__
#define __NOSTR_RELAY_H__

/* Transitional header exposing GLib-friendly names for Relay. */

#include <stdbool.h>
#include "relay.h"        /* legacy Relay and APIs */
#include "channel.h"      /* GoChannel */
#include "context.h"      /* GoContext */
#ifdef NOSTR_WITH_GLIB
#include <glib-object.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef for GLib-style naming */
typedef Relay NostrRelay;

#ifdef NOSTR_WITH_GLIB
/**
 * nostr_relay_get_type:
 *
 * Boxed type for `NostrRelay`. The boxed copy increases the reference count
 * (no deep-copy); boxed free decreases it.
 *
 * Returns: (type GType): the GType of NostrRelay
 */
GType nostr_relay_get_type(void);
#endif

/* GI-facing API (stable symbol names) */
/**
 * nostr_relay_new:
 * @context: (nullable): optional context
 * @url: (not nullable): relay URL
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: (transfer full) (nullable): new relay
 */
NostrRelay *nostr_relay_new(GoContext *context, const char *url, Error **err);

/**
 * nostr_relay_free:
 * @relay: (nullable): relay
 *
 * Convenience alias for unref.
 */
void        nostr_relay_free(NostrRelay *relay);

/**
 * nostr_relay_ref:
 * @relay: (nullable): relay
 *
 * Increments the reference count.
 *
 * Returns: (transfer none) (nullable): same relay
 */
NostrRelay *nostr_relay_ref(NostrRelay *relay);

/**
 * nostr_relay_unref:
 * @relay: (nullable): relay
 *
 * Decrements the reference count and frees when it reaches zero. Safe on NULL.
 */
void        nostr_relay_unref(NostrRelay *relay);

/**
 * nostr_relay_connect:
 * @relay: (nullable): relay
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
bool        nostr_relay_connect(NostrRelay *relay, Error **err);

/**
 * nostr_relay_disconnect:
 * @relay: (nullable): relay
 */
void        nostr_relay_disconnect(NostrRelay *relay);

/**
 * nostr_relay_close:
 * @relay: (nullable): relay
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
bool        nostr_relay_close(NostrRelay *relay, Error **err);

/**
 * nostr_relay_subscribe:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filters: (nullable): filters
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: success
 */
bool        nostr_relay_subscribe(NostrRelay *relay, GoContext *ctx, Filters *filters, Error **err);

/**
 * nostr_relay_prepare_subscription:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filters: (nullable): filters
 *
 * Returns: (transfer none) (nullable): internal subscription pointer
 */
Subscription *nostr_relay_prepare_subscription(NostrRelay *relay, GoContext *ctx, Filters *filters);

/**
 * nostr_relay_publish:
 * @relay: (nullable): relay
 * @event: (nullable): event
 */
void        nostr_relay_publish(NostrRelay *relay, NostrEvent *event);

/**
 * nostr_relay_auth:
 * @relay: (nullable): relay
 * @sign: (nullable): sign callback
 * @err: (out) (optional) (nullable): error out param
 */
void        nostr_relay_auth(NostrRelay *relay, void (*sign)(NostrEvent *, Error **), Error **err);

/**
 * nostr_relay_count:
 * @relay: (nullable): relay
 * @ctx: (nullable): context
 * @filter: (nullable): filter
 * @err: (out) (optional) (nullable): error out param
 *
 * Returns: count
 */
int64_t     nostr_relay_count(NostrRelay *relay, GoContext *ctx, Filter *filter, Error **err);

/**
 * nostr_relay_is_connected:
 * @relay: (nullable): relay
 *
 * Returns: whether connected
 */
bool        nostr_relay_is_connected(NostrRelay *relay);

/**
 * nostr_relay_enable_debug_raw:
 * @relay: (nullable): relay
 * @enable: enable flag
 */
void        nostr_relay_enable_debug_raw(NostrRelay *relay, int enable);

/**
 * nostr_relay_get_debug_raw_channel:
 * @relay: (nullable): relay
 *
 * Returns: (transfer none) (nullable): internal channel owned by relay
 */
GoChannel  *nostr_relay_get_debug_raw_channel(NostrRelay *relay);

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
 * Returns: (transfer none) (nullable): the write channel used internally
 */
GoChannel *nostr_relay_get_write_channel(const NostrRelay *relay);

/**
 * nostr_relay_write:
 * @relay: (nullable): relay
 * @msg: (transfer full): JSON string to send; will be freed by callee
 *
 * Enqueue a JSON message for sending. Returns a channel that yields an Error* (or NULL) once written.
 *
 * Returns: (transfer full) (nullable): channel of Error* result
 */
GoChannel *nostr_relay_write(NostrRelay *relay, char *msg);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_RELAY_H__ */
