#ifndef __NOSTR_SIMPLE_POOL_H__
#define __NOSTR_SIMPLE_POOL_H__

/* GLib-friendly transitional header for SimplePool */

#include <stddef.h>
#include <stdbool.h>
#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-brown-list.h"
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

/* Forward declarations for subscription registry */
typedef struct PoolSubscriptionEntry PoolSubscriptionEntry;
typedef struct SubscriptionRegistry SubscriptionRegistry;

typedef struct _NostrSimplePool {
    NostrRelay **relays;
    size_t relay_count;
    pthread_mutex_t pool_mutex;
    void (*auth_handler)(NostrEvent *);
    void (*event_middleware)(NostrIncomingEvent *);
    /* Extended middleware with user_data — preferred over event_middleware when set. */
    void (*event_middleware_ex)(NostrIncomingEvent *, void *);
    void *event_middleware_data;
    /* Optional batch middleware: if set, pool may invoke this with a batch for efficiency. */
    void (*batch_middleware)(NostrIncomingEvent *items, size_t count);
    bool (*signature_checker)(NostrEvent);
    bool running;
    pthread_t thread;
    /* Subscriptions and runtime state */
    struct NostrSubscription **subs;
    size_t subs_count;
    NostrFilters *filters_shared; /* shared among current subs; owned */
    /* De-duplication (enabled by default; API param can disable) */
    bool dedup_unique;
    size_t dedup_cap;      /* max remembered IDs */
    char **dedup_hash;     /* open-addressed set of remembered IDs */
    size_t dedup_hash_sz;
    size_t dedup_count;
    char **dedup_evict;    /* FIFO eviction ring, pointers owned by hash */
    size_t dedup_evict_head;
    size_t dedup_tombstones;
    /* Behavior flags */
    bool auto_unsub_on_eose; /* if true, unsubscribe subs upon EOSE (default: false) */
    
    /* Phase 2: Subscription registry for lifecycle management */
    SubscriptionRegistry *sub_registry;
    pthread_t cleanup_worker_thread;
    bool cleanup_worker_running;

    /* nostrc-py1: Relay brown list for persistently failing relays */
    NostrBrownList *brown_list;
    bool brown_list_enabled;        /* Whether to use brown list (default: true) */

    /* nostrc-ey0f: Disposed flag to prevent double-free */
    volatile int disposed;          /* 0 = active, 1 = disposed */

    /* fp-ieg8: background redial worker.
     *
     * libnostr's reconnect/backoff loop lives in the relay's message_loop,
     * which nostr_relay_connect() only spawns after a connection succeeds. A
     * relay whose FIRST dial failed had no loop, so nothing ever re-dialled it:
     * "the pool retries in the background" was not a property this pool
     * provided, and callers papered over it with their own periodic ticks.
     * This worker is that retry engine. It takes only relays whose
     * reconnection is not self-managed, and dials them on the same bounded
     * exponential backoff message_loop uses.
     *
     * Its lifetime is the POOL's, not start()/stop()'s: callers stop and
     * restart pools from latency-sensitive threads (signetd does it from its
     * GLib main loop), and joining a thread that may be inside a connect would
     * reintroduce exactly the stall this exists to remove. */
    pthread_t redial_thread;
    bool redial_thread_running;     /* worker was spawned and needs joining */
    volatile int redial_stop;       /* set by _free() to retire the worker */
    bool redial_enabled;            /* cleared by explicit disconnect requests */
    struct GoChannel *redial_wake;  /* nudge: there is something to dial now */

    /* Timeout-audit: Wake channel for event-driven worker loop.
     * Signaled when subscriptions change or pool is stopping.
     * Allows the worker to block in go_select instead of polling. */
    struct GoChannel *wake_ch;
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
 * nostr_simple_pool_ensure_relay_async:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL
 *
 * Registers the relay with the pool and returns immediately. Unlike
 * nostr_simple_pool_ensure_relay(), this never dials on the calling thread:
 * creating the connection is left to the pool's redial worker, which brings the
 * relay up in the background and keeps retrying on bounded backoff if it is
 * unreachable.
 *
 * Use this from any thread that must not stall. nostr_connection_new() can
 * block for up to NOSTR_CONNECT_RESULT_TIMEOUT_MS (30s, not configurable) --
 * a synchronous DNS lookup for a mistyped hostname is enough -- so calling the
 * blocking variant from an event loop freezes the whole process for the
 * duration (fp-1r0k).
 */
void nostr_simple_pool_ensure_relay_async(NostrSimplePool *pool, const char *url);
/**
 * nostr_simple_pool_add_relay:
 * @pool: (transfer none): pool
 * @relay: (transfer none): relay to add
 *
 * Adds an existing relay to the pool. The pool does not take ownership.
 */
void nostr_simple_pool_add_relay(NostrSimplePool *pool, NostrRelay *relay);

/**
 * nostr_simple_pool_remove_relay:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL to remove
 *
 * Removes a relay from the pool by URL. Disconnects and frees the relay.
 * Returns: true if the relay was found and removed, false otherwise.
 */
bool nostr_simple_pool_remove_relay(NostrSimplePool *pool, const char *url);

/**
 * nostr_simple_pool_disconnect_all:
 * @pool: (transfer none): pool
 *
 * Disconnects all relays in the pool without removing them.
 * Useful before reconfiguring the relay list.
 *
 * fp-ieg8: this also suspends the background redial worker, which would
 * otherwise dial straight back everything this call just dropped. The next
 * nostr_simple_pool_start() re-arms it.
 */
void nostr_simple_pool_disconnect_all(NostrSimplePool *pool);
/**
 * nostr_simple_pool_start:
 * @pool: (transfer none): pool
 *
 * Starts pool workers. Idempotent: a second call on an already-running pool is
 * a no-op rather than a second worker thread with the first one orphaned.
 */
void nostr_simple_pool_start(NostrSimplePool *pool);
/**
 * nostr_simple_pool_stop:
 * @pool: (transfer none): pool
 *
 * Stops pool workers and drains queues.
 *
 * fp-ieg8: deliberately does NOT wait for the background redial worker. Callers
 * stop pools from latency-sensitive threads, and blocking on a dial that may
 * already be in flight is the stall this whole mechanism exists to remove. The
 * worker is retired by nostr_simple_pool_free().
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
 * nostr_simple_pool_subscribe_async:
 * @pool: (transfer none): pool
 * @urls: (array length=url_count) (transfer none): relay URLs
 * @url_count: number of URLs
 * @filters: (transfer none): filters to subscribe
 * @unique: whether to de-duplicate events
 *
 * Same as nostr_simple_pool_subscribe(), except relays are registered with
 * nostr_simple_pool_ensure_relay_async() instead of being dialled inline, so
 * this call does not block on the network.
 *
 * The subscription is fired on every relay that is already connected and
 * recorded as the pool's shared filter set; relays that connect later pick it
 * up from the worker loop's reconcile pass (<=200ms cadence). Note that pass is
 * skipped for auto_unsub_on_eose pools -- call
 * nostr_simple_pool_set_auto_unsub_on_eose(pool, false) if you rely on it.
 */
void nostr_simple_pool_subscribe_async(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique);
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
void nostr_simple_pool_set_event_middleware_ex(NostrSimplePool *pool,
                                               void (*cb)(NostrIncomingEvent *, void *),
                                               void *user_data);
void nostr_simple_pool_set_batch_middleware(NostrSimplePool *pool,
                                            void (*cb)(NostrIncomingEvent *items, size_t count));
void nostr_simple_pool_set_auto_unsub_on_eose(NostrSimplePool *pool, bool enable);

/* ========================================================================
 * Brown List API (nostrc-py1)
 * ======================================================================== */

/**
 * nostr_simple_pool_set_brown_list_enabled:
 * @pool: (transfer none): pool
 * @enabled: whether to enable brown list filtering
 *
 * Enable or disable the relay brown list. When enabled, relays that
 * persistently fail to connect will be temporarily excluded from
 * connection attempts.
 *
 * Default: enabled (true)
 */
void nostr_simple_pool_set_brown_list_enabled(NostrSimplePool *pool, bool enabled);

/**
 * nostr_simple_pool_get_brown_list_enabled:
 * @pool: (transfer none): pool
 *
 * Returns: whether brown list is enabled
 */
bool nostr_simple_pool_get_brown_list_enabled(NostrSimplePool *pool);

/**
 * nostr_simple_pool_get_brown_list:
 * @pool: (transfer none): pool
 *
 * Get direct access to the brown list for advanced configuration.
 *
 * Returns: (transfer none): the pool's brown list, or NULL if not initialized
 */
NostrBrownList *nostr_simple_pool_get_brown_list(NostrSimplePool *pool);

/**
 * nostr_simple_pool_is_relay_browned:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL to check
 *
 * Check if a relay is currently brown-listed.
 *
 * Returns: true if relay is brown-listed
 */
bool nostr_simple_pool_is_relay_browned(NostrSimplePool *pool, const char *url);

/**
 * nostr_simple_pool_clear_brown_list:
 * @pool: (transfer none): pool
 *
 * Clear all brown-listed relays, allowing them to be retried immediately.
 */
void nostr_simple_pool_clear_brown_list(NostrSimplePool *pool);

/**
 * nostr_simple_pool_clear_relay_brown:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL to clear
 *
 * Clear a specific relay from the brown list.
 *
 * Returns: true if relay was found and cleared
 */
bool nostr_simple_pool_clear_relay_brown(NostrSimplePool *pool, const char *url);

/**
 * nostr_simple_pool_get_brown_list_stats:
 * @pool: (transfer none): pool
 * @stats: (out): statistics output
 *
 * Get statistics about the brown list.
 */
void nostr_simple_pool_get_brown_list_stats(NostrSimplePool *pool, NostrBrownListStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_SIMPLE_POOL_H__ */
