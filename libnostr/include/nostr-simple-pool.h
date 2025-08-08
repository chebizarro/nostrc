#ifndef __NOSTR_SIMPLE_POOL_H__
#define __NOSTR_SIMPLE_POOL_H__

/* GLib-friendly transitional header for SimplePool */

#include "simplepool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NostrSimplePool:
 * NostrIncomingEvent:
 * NostrDirectedFilters:
 *
 * Opaque wrapper typedefs for pool, incoming events, and directed filters.
 */
typedef SimplePool        NostrSimplePool;
typedef IncomingEvent     NostrIncomingEvent;
typedef DirectedFilters   NostrDirectedFilters;

/* Function aliases (stable surface) */
/**
 * nostr_simple_pool_new:
 *
 * Returns: (transfer full): newly-allocated `NostrSimplePool*`
 */
#define nostr_simple_pool_new               create_simple_pool
/**
 * nostr_simple_pool_free:
 * @pool: (transfer full): pool to free
 */
#define nostr_simple_pool_free              free_simple_pool
/**
 * nostr_simple_pool_ensure_relay:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL
 *
 * Ensures the relay is present in the pool.
 * Returns: 0 on success
 */
#define nostr_simple_pool_ensure_relay      simple_pool_ensure_relay
/**
 * nostr_simple_pool_start:
 * @pool: (transfer none): pool
 *
 * Starts pool workers.
 * Returns: 0 on success
 */
#define nostr_simple_pool_start             simple_pool_start
/**
 * nostr_simple_pool_stop:
 * @pool: (transfer none): pool
 *
 * Stops pool workers and drains queues.
 */
#define nostr_simple_pool_stop              simple_pool_stop
/**
 * nostr_simple_pool_subscribe:
 * @pool: (transfer none): pool
 * @filters: (transfer none): filters to subscribe
 * @on_event: (scope call): event callback (nullable)
 * @user_data: (closure): user data for callback
 *
 * Subscribes with directed filters.
 * Returns: 0 on success
 */
#define nostr_simple_pool_subscribe         simple_pool_subscribe
/**
 * nostr_simple_pool_query_single:
 * @pool: (transfer none): pool
 * @relay: (transfer none): relay URL
 * @filter: (transfer none): filter
 * @on_event: (scope call): event callback (nullable)
 * @user_data: (closure): user data for callback
 *
 * Performs a one-shot query to a single relay.
 * Returns: 0 on success
 */
#define nostr_simple_pool_query_single      simple_pool_query_single

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_SIMPLE_POOL_H__ */
