#ifndef __NOSTR_SUBSCRIPTION_H__
#define __NOSTR_SUBSCRIPTION_H__

/* Transitional header exposing GLib-friendly names for Subscription. */

#include <stdbool.h>
#include "nostr-filter.h"
#include "relay.h"
#include "subscription.h" /* defines Subscription and legacy APIs */
#include "channel.h"  /* GoChannel */
#include "context.h"  /* GoContext */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedef for GLib-style naming */
typedef Subscription NostrSubscription;
typedef Relay       NostrRelay;


/* New API names mapped to legacy implementations */
#define nostr_subscription_new          create_subscription
#define nostr_subscription_free         free_subscription
#define nostr_subscription_get_id       subscription_get_id
#define nostr_subscription_unsubscribe  subscription_unsub
#define nostr_subscription_close        subscription_close
#define nostr_subscription_subscribe    subscription_sub
#define nostr_subscription_fire         subscription_fire

/* Accessors for public fields/state (for future GObject properties) */

/* Identity */
/**
 * nostr_subscription_get_id_const:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): internal ID string
 */
const char *nostr_subscription_get_id_const(const NostrSubscription *sub);

/* Associations */
/**
 * nostr_subscription_get_relay:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): associated relay
 */
NostrRelay   *nostr_subscription_get_relay(const NostrSubscription *sub);
/**
 * nostr_subscription_get_filters:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): associated filters
 */
NostrFilters *nostr_subscription_get_filters(const NostrSubscription *sub);
/**
 * nostr_subscription_set_filters:
 * @sub: (nullable): subscription (no-op if NULL)
 * @filters: (transfer full) (nullable): new filters; previous freed if different
 *
 * Ownership: takes full ownership of @filters.
 */
void          nostr_subscription_set_filters(NostrSubscription *sub, NostrFilters *filters);

/* Channels (transfer none): producer owns them */
/**
 * nostr_subscription_get_events_channel:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): events channel owned by subscription
 */
GoChannel    *nostr_subscription_get_events_channel(const NostrSubscription *sub);
/**
 * nostr_subscription_get_eose_channel:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): EOSE notification channel
 */
GoChannel    *nostr_subscription_get_eose_channel(const NostrSubscription *sub);
/**
 * nostr_subscription_get_closed_channel:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): CLOSED reason channel; carries (const char*)
 */
GoChannel    *nostr_subscription_get_closed_channel(const NostrSubscription *sub);

/* Context */
/**
 * nostr_subscription_get_context:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): cancellation context for lifecycle
 */
GoContext    *nostr_subscription_get_context(const NostrSubscription *sub);

/* Lifecycle flags */
/**
 * nostr_subscription_is_live:
 * @sub: (nullable): subscription
 *
 * Returns: whether subscription is currently live
 */
bool          nostr_subscription_is_live(const NostrSubscription *sub);
/**
 * nostr_subscription_is_eosed:
 * @sub: (nullable): subscription
 *
 * Returns: whether EOSE has been observed
 */
bool          nostr_subscription_is_eosed(const NostrSubscription *sub);
/**
 * nostr_subscription_is_closed:
 * @sub: (nullable): subscription
 *
 * Returns: whether CLOSED was observed and emitted
 */
bool          nostr_subscription_is_closed(const NostrSubscription *sub);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_SUBSCRIPTION_H__ */
