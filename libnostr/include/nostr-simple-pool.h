#ifndef __NOSTR_SIMPLE_POOL_H__
#define __NOSTR_SIMPLE_POOL_H__

/* GLib-friendly transitional header for SimplePool */

#include <stddef.h>
#include <stdbool.h>
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

/* Function prototypes (stable GI-friendly surface) */
/**
 * nostr_simple_pool_new:
 *
 * Returns: (transfer full): newly-allocated `NostrSimplePool*`
 */
NostrSimplePool *nostr_simple_pool_new(void);
/**
 * nostr_simple_pool_free:
 * @pool: (transfer full): pool to free
 */
void nostr_simple_pool_free(NostrSimplePool *pool);
/**
 * nostr_simple_pool_ensure_relay:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL
 *
 * Ensures the relay is present in the pool.
 */
void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url);
/**
 * nostr_simple_pool_start:
 * @pool: (transfer none): pool
 *
 * Starts pool workers.
 */
void nostr_simple_pool_start(NostrSimplePool *pool);
/**
 * nostr_simple_pool_stop:
 * @pool: (transfer none): pool
 *
 * Stops pool workers and drains queues.
 */
void nostr_simple_pool_stop(NostrSimplePool *pool);
/**
 * nostr_simple_pool_subscribe:
 * @pool: (transfer none): pool
 * @urls: (array length=url_count) (transfer none): relay URLs
 * @url_count: number of URLs
 * @filters: (transfer none): filters to subscribe
 * @unique: whether to de-duplicate events
 */
void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, Filters filters, bool unique);
/**
 * nostr_simple_pool_query_single:
 * @pool: (transfer none): pool
 * @urls: (array length=url_count) (transfer none): relay URLs
 * @url_count: number of URLs
 * @filter: (transfer none): filter
 */
void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, Filter filter);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_SIMPLE_POOL_H__ */
