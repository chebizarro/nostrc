#ifndef __NOSTR_SIMPLE_POOL_H__
#define __NOSTR_SIMPLE_POOL_H__

/* GLib-friendly transitional header for SimplePool */

#include <stddef.h>
#include <stdbool.h>
#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <pthread.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEEN_ALREADY_DROP_TICK 60 // seconds

/**
 * NostrSimplePool:
 * NostrIncomingEvent:
 * NostrDirectedFilters:
 *
 * Opaque wrapper typedefs for pool, incoming events, and directed filters.
 */
typedef struct _NostrIncomingEvent {
    NostrEvent *event;
    NostrRelay *relay;
} NostrIncomingEvent;

typedef struct _NostrSimplePool {
    NostrRelay **relays;
    size_t relay_count;
    pthread_mutex_t pool_mutex;
    void (*auth_handler)(NostrEvent *);
    void (*event_middleware)(NostrIncomingEvent *);
    /* Optional batch middleware: if set, pool may invoke this with a batch for efficiency. */
    void (*batch_middleware)(NostrIncomingEvent *items, size_t count);
    bool (*signature_checker)(NostrEvent);
    bool running;
    pthread_t thread;
    /* Subscriptions and runtime state */
    struct NostrSubscription **subs;
    size_t subs_count;
    NostrFilters *filters_shared; /* shared among current subs; owned */
    /* De-duplication (when enabled via API param) */
    bool dedup_unique;
    size_t dedup_cap;      /* max remembered IDs */
    char **dedup_ring;     /* circular buffer of last IDs */
    size_t dedup_len;
    size_t dedup_head;
    /* Behavior flags */
    bool auto_unsub_on_eose; /* if true, unsubscribe subs upon EOSE (default: false) */
} NostrSimplePool;

typedef struct _NostrDirectedFilters {
    NostrFilters filters;
    char *relay_url;
} NostrDirectedFilters;

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
void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique);
/**
 * nostr_simple_pool_query_single:
 * @pool: (transfer none): pool
 * @urls: (array length=url_count) (transfer none): relay URLs
 * @url_count: number of URLs
 * @filter: (transfer none): filter
 */
void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilter filter);

/**
 * Convenience configuration API
 */
void nostr_simple_pool_set_event_middleware(NostrSimplePool *pool,
                                            void (*cb)(NostrIncomingEvent *));
void nostr_simple_pool_set_batch_middleware(NostrSimplePool *pool,
                                            void (*cb)(NostrIncomingEvent *items, size_t count));
void nostr_simple_pool_set_auto_unsub_on_eose(NostrSimplePool *pool, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_SIMPLE_POOL_H__ */
