#ifndef __NOSTR_SIMPLE_POOL_H__
#define __NOSTR_SIMPLE_POOL_H__

#include "nostr-export.h"

/* GLib-friendly transitional header for SimplePool */

#include <stddef.h>
#include <stdbool.h>
#ifndef __cplusplus
#include <stdatomic.h>
#endif

#ifdef __cplusplus
/* _Atomic is unavailable in C++ before C++23; C++ consumers see the plain
 * type with identical size and alignment, so the ABI is unchanged
 * (nostrc-val0v review). */
#define NOSTR_POOL_ATOMIC(T) T
#else
#define NOSTR_POOL_ATOMIC(T) _Atomic T
#endif
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
    /* Protects relay/subscription arrays, the filter/dedup policy and table,
     * middleware configuration, auto-unsubscribe and brown-list enablement.
     * Internal nested order: pool_mutex -> subscription registry mutex.
     * Middleware is snapshotted here but called without pool_mutex held. */
    pthread_mutex_t pool_mutex;
    void (*auth_handler)(NostrEvent *);
    void (*event_middleware)(NostrIncomingEvent *);
    /* Extended middleware with user_data — preferred over event_middleware when set. */
    void (*event_middleware_ex)(NostrIncomingEvent *, void *);
    void *event_middleware_data;
    /* Optional batch middleware: if set, pool may invoke this with a batch for efficiency. */
    void (*batch_middleware)(NostrIncomingEvent *items, size_t count);
    bool (*signature_checker)(NostrEvent);
    /* Worker run flag. _Atomic (in C) has the same size/alignment as bool,
     * so the struct layout is unchanged; atomic access closes the data race
     * between the worker loop and start()/stop()/free() (nostrc-9bygk). */
    NOSTR_POOL_ATOMIC(bool) running;
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
    NOSTR_POOL_ATOMIC(int) redial_stop; /* set by _free() to retire the worker (same size as int: layout unchanged) */
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
NOSTR_API NostrSimplePool *nostr_simple_pool_new(void);
/**
 * nostr_simple_pool_free:
 * @pool: (transfer full): pool to free
 */
NOSTR_API void nostr_simple_pool_free(NostrSimplePool *pool);
/**
 * nostr_simple_pool_ensure_relay:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL
 *
 * Ensures the relay is present in the pool and, if it is not connected, waits
 * for one dial attempt on it: made on the calling thread, or already in flight
 * on the pool's redial worker. Returns without dialling a browned relay, one
 * with auto-reconnect disabled, or one whose own message loop is reconnecting
 * it. Can block for up to NOSTR_CONNECT_RESULT_TIMEOUT_MS; see
 * nostr_simple_pool_ensure_relay_async().
 */
NOSTR_API void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url);
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
NOSTR_API void nostr_simple_pool_ensure_relay_async(NostrSimplePool *pool, const char *url);
/**
 * nostr_simple_pool_add_relay:
 * @pool: (transfer none): pool
 * @relay: (transfer none): relay to add
 *
 * Adds an existing relay to the pool. The pool does not take ownership.
 */
NOSTR_API void nostr_simple_pool_add_relay(NostrSimplePool *pool, NostrRelay *relay);

/**
 * nostr_simple_pool_remove_relay:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL to remove
 *
 * Removes a relay from the pool by URL. Disconnects and frees the relay.
 * Returns: true if the relay was found and removed, false otherwise.
 */
NOSTR_API bool nostr_simple_pool_remove_relay(NostrSimplePool *pool, const char *url);

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
NOSTR_API void nostr_simple_pool_disconnect_all(NostrSimplePool *pool);
/**
 * nostr_simple_pool_start:
 * @pool: (transfer none): pool
 *
 * Starts pool workers. Idempotent: a second call on an already-running pool is
 * a no-op rather than a second worker thread with the first one orphaned.
 */
NOSTR_API void nostr_simple_pool_start(NostrSimplePool *pool);
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
NOSTR_API void nostr_simple_pool_stop(NostrSimplePool *pool);
/**
 * nostr_simple_pool_subscribe:
 * @pool: (transfer none): pool
 * @urls: (array length=url_count) (transfer none): relay URLs
 * @url_count: number of URLs
 * @filters: (transfer none): filters to subscribe
 * @unique: whether to de-duplicate events
 */
/* Subscription lifecycle observer (EOSE/CLOSED). Runs on the pool worker;
 * must not call this setter or free the pool from inside the callback. Setter
 * serializes against in-flight notifications; no public struct/ABI change. */
typedef void (*NostrPoolSubscriptionCallback)(NostrRelay *, const char *state, const char *reason, void *);
NOSTR_API void nostr_simple_pool_set_subscription_callback(NostrSimplePool *, NostrPoolSubscriptionCallback, void *);
/* Configure new subscriptions before subscribe(): reconnect from now-seconds,
 * never from an untrusted received timestamp. The consumer must enforce its
 * admission/freshness window and durable replay guard; use unique=false when
 * retry deliveries need replies. Zero preserves default cursor recovery.
 * Returns false if the pool's private subscription registry is unavailable. */
/* The exponential ladder for CLOSED reasons the pool retries on a timer
 * (NOSTR_POOL_RECOVERY_BACKOFF: "rate-limited:", "error:"); preserves the
 * socket and its NIP-42 authentication. EOSE resets each relay's ladder.
 * Defaults: 1000 ms doubling to 60000 ms. Zero initial_ms restores the
 * defaults (nostrc-7i6w: it used to mean "re-REQ on the next worker pass");
 * max_ms must be >= initial_ms. Private registry state only. */
NOSTR_API bool nostr_simple_pool_set_closed_backoff(NostrSimplePool *, unsigned initial_ms, unsigned max_ms);

/**
 * NostrPoolRecovery:
 * @NOSTR_POOL_RECOVERY_NONE: nothing to recover (EOSE; or an auto-unsubscribe
 *   pool, which never reissues)
 * @NOSTR_POOL_RECOVERY_AWAIT_AUTH: "auth-required:" before this connection's
 *   AUTH was accepted. Reissued exactly once, on the same connection, when
 *   the relay answers an AUTH with OK true after this CLOSED (see
 *   nostr_relay_set_auth_signer_full()); held while there is none, and after
 *   a rejection.
 * @NOSTR_POOL_RECOVERY_AUTH_REFUSED: "auth-required:" although this
 *   connection's AUTH had been accepted when the relay sent it: the identity
 *   is not enough. Held until a later AUTH OK on this connection (a new
 *   challenge, or another identity) or a new connection.
 * @NOSTR_POOL_RECOVERY_BACKOFF: "rate-limited:" or "error:". Reissued after
 *   the relay's ladder delay (nostr_simple_pool_set_closed_backoff()).
 * @NOSTR_POOL_RECOVERY_RECONNECT: any other reason ("blocked:",
 *   "restricted:", "invalid:", unprefixed, ...): the relay's answer for this
 *   connection. Reissued only on its next connection.
 *
 * What the pool does with a CLOSED subscription (nostrc-7i6w). The pool keeps
 * the closed REQ's own filters and reissues them as stated above; every
 * policy also reissues on a new connection. Nothing is re-sent on a timer
 * except BACKOFF. A new nostr_simple_pool_subscribe() supersedes held REQs.
 */
typedef enum NostrPoolRecovery {
    NOSTR_POOL_RECOVERY_NONE = 0,
    NOSTR_POOL_RECOVERY_AWAIT_AUTH,
    NOSTR_POOL_RECOVERY_AUTH_REFUSED,
    NOSTR_POOL_RECOVERY_BACKOFF,
    NOSTR_POOL_RECOVERY_RECONNECT
} NostrPoolRecovery;

NOSTR_API const char *nostr_simple_pool_recovery_name(NostrPoolRecovery recovery);

/**
 * nostr_simple_pool_classify_closed:
 * @reason: (nullable): the CLOSED message
 * @frame: (nullable): its stamp; tells AWAIT_AUTH from AUTH_REFUSED
 *
 * Returns: the recovery a reissuing pool applies to this CLOSED
 */
NOSTR_API NostrPoolRecovery nostr_simple_pool_classify_closed(const char *reason,
                                                            const NostrRelayFrameInfo *frame);

/**
 * NostrPoolCallbackInfo:
 * @frame: the EOSE/CLOSED frame's wire-order stamp (see #NostrRelayFrameInfo):
 *   compare @frame.connection_generation with the relay's current one to
 *   ignore a frame of a lost connection, and @frame.auth_state /
 *   @frame.sequence with an AUTH result to order it against AUTH
 * @recovery: what the pool does with a CLOSED (NONE for EOSE)
 * @retry_after_ms: BACKOFF only: the delay before the reissue
 */
typedef struct NostrPoolCallbackInfo {
    NostrRelayFrameInfo frame;
    NostrPoolRecovery recovery;
    uint64_t retry_after_ms;
} NostrPoolCallbackInfo;

/**
 * nostr_simple_pool_get_callback_info:
 * @pool: pool
 * @out: (out): the boundary being reported
 *
 * Inside this pool's subscription callback, the stamp and recovery of the
 * EOSE/CLOSED it reports. Thread-local; false (and @out zeroed) elsewhere.
 * Since: 2.4
 */
NOSTR_API bool nostr_simple_pool_get_callback_info(NostrSimplePool *pool, NostrPoolCallbackInfo *out);

typedef struct NostrPoolSubscription NostrPoolSubscription;
typedef void (*NostrPoolEventFunc)(NostrPoolSubscription *, NostrIncomingEvent *, void *);
typedef void (*NostrPoolStateFunc)(NostrPoolSubscription *, NostrRelay *, const char *state,
                                   const char *reason, const NostrPoolCallbackInfo *, void *);
/* Open is nonblocking: copies filters/URLs and uses the pool's background dial
 * and recovery-window policy. Call start() on the pool to deliver callbacks.
 * Each handle has independent event-ID deduplication and reconnect ownership.
 * Event callbacks take ownership of incoming->event; other arguments are borrowed.
 * Callbacks run on the pool worker, not under pool_mutex. */
NOSTR_API NostrPoolSubscription *nostr_simple_pool_subscription_open(
    NostrSimplePool *, const char **urls, size_t url_count, const NostrFilters *,
    bool unique, NostrPoolEventFunc, NostrPoolStateFunc, void *user_data);
/* Idempotent; sends CLOSE for this handle's live REQs, drops held recovery and
 * queued events, and waits for its callbacks already executing on another thread.
 * May be called from its own callback. No callback starts after close returns.
 * Replace by closing the old handle and opening a new one; closed handles cannot
 * be reopened. Free releases the caller's ownership and also closes the handle.
 * Do not race free with other uses of the same handle, or pool destruction with
 * any pool/handle API. Pool destruction detaches surviving closed handles. */
NOSTR_API void nostr_pool_subscription_close(NostrPoolSubscription *);
NOSTR_API void nostr_pool_subscription_free(NostrPoolSubscription *);

typedef struct NostrPoolRelaySnapshot {
    char *url;                         /* owned by snapshot */
    NostrRelayConnectionState connection_state;
    NostrRelayAuthStatus auth;
    uint64_t reconnect_after_ms;
    uint64_t closed_backoff_ms;
    bool browned;
} NostrPoolRelaySnapshot;
/* Copies the relay membership under pool_mutex and reads synchronized relay
 * state. Each field is an observation, not a transaction across relay threads.
 * No borrowed relay pointers; safe to retain across reconnect/pool destruction.
 * Returns false on allocation failure; outputs NULL/0 on failure or empty pool. */
NOSTR_API bool nostr_simple_pool_snapshot_relays(NostrSimplePool *,
    NostrPoolRelaySnapshot **out, size_t *count);
NOSTR_API void nostr_simple_pool_snapshot_relays_free(NostrPoolRelaySnapshot *, size_t count);
NOSTR_API bool nostr_simple_pool_set_recovery_window(NostrSimplePool *, unsigned seconds);
NOSTR_API void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique);
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
 * up from the worker loop's reconcile pass, which runs when a relay connects
 * (nostrc-7i6w: no longer on a 200ms cadence). Note that pass is skipped for
 * auto_unsub_on_eose pools -- call nostr_simple_pool_set_auto_unsub_on_eose(pool,
 * false) if you rely on it.
 */
NOSTR_API void nostr_simple_pool_subscribe_async(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique);
/**
 * nostr_simple_pool_query_single:
 * @pool: (transfer none): pool
 * @urls: (array length=url_count) (transfer none): relay URLs
 * @url_count: number of URLs
 * @filter: (transfer none): filter
 */
NOSTR_API void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilter filter);

/**
 * Convenience configuration API
 */
/* Middleware takes ownership of each incoming->event (free it or retain it);
 * the incoming wrapper and relay are borrowed for the call. This also applies
 * to the extended and batch variants below. Configuration is snapshotted per
 * delivered batch. Replacing middleware does not revoke a captured callback;
 * its code and user_data must remain valid until that batch finishes (stop()
 * joins the worker). Do not mutate these fields directly while workers run. */
NOSTR_API void nostr_simple_pool_set_event_middleware(NostrSimplePool *pool,
                                             void (*cb)(NostrIncomingEvent *));
/* Borrowed source subscription during this pool's single-event middleware or
 * subscription-state callback only; NULL outside them (and in batch middleware).
 * The pointer expires on return; do not retain it. NostrIncomingEvent ABI is
 * unchanged. This lets callers scope admission to their current subscription. */
NOSTR_API const struct NostrSubscription *nostr_simple_pool_get_callback_subscription(NostrSimplePool *pool);
/* Producer-observed EOSE time for the current single-event/subscription
 * callback, or zero for history, a retired REQ, cancellation, or no callback.
 * No public channel/payload layout or ownership change. Since: 2.3. */
NOSTR_API int64_t nostr_simple_pool_get_callback_live_since(NostrSimplePool *pool);
NOSTR_API void nostr_simple_pool_set_event_middleware_ex(NostrSimplePool *pool,
                                               void (*cb)(NostrIncomingEvent *, void *),
                                               void *user_data);
NOSTR_API void nostr_simple_pool_set_batch_middleware(NostrSimplePool *pool,
                                            void (*cb)(NostrIncomingEvent *items, size_t count));
NOSTR_API void nostr_simple_pool_set_auto_unsub_on_eose(NostrSimplePool *pool, bool enable);

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
NOSTR_API void nostr_simple_pool_set_brown_list_enabled(NostrSimplePool *pool, bool enabled);

/**
 * nostr_simple_pool_get_brown_list_enabled:
 * @pool: (transfer none): pool
 *
 * Returns: whether brown list is enabled
 */
NOSTR_API bool nostr_simple_pool_get_brown_list_enabled(NostrSimplePool *pool);

/**
 * nostr_simple_pool_get_brown_list:
 * @pool: (transfer none): pool
 *
 * Get direct access to the brown list for advanced configuration.
 *
 * Returns: (transfer none): the pool's brown list, or NULL if not initialized
 */
NOSTR_API NostrBrownList *nostr_simple_pool_get_brown_list(NostrSimplePool *pool);

/**
 * nostr_simple_pool_is_relay_browned:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL to check
 *
 * Check if a relay is currently brown-listed.
 *
 * Returns: true if relay is brown-listed
 */
NOSTR_API bool nostr_simple_pool_is_relay_browned(NostrSimplePool *pool, const char *url);

/**
 * nostr_simple_pool_clear_brown_list:
 * @pool: (transfer none): pool
 *
 * Clear all brown-listed relays, allowing them to be retried immediately.
 */
NOSTR_API void nostr_simple_pool_clear_brown_list(NostrSimplePool *pool);

/**
 * nostr_simple_pool_clear_relay_brown:
 * @pool: (transfer none): pool
 * @url: (transfer none): relay URL to clear
 *
 * Clear a specific relay from the brown list.
 *
 * Returns: true if relay was found and cleared
 */
NOSTR_API bool nostr_simple_pool_clear_relay_brown(NostrSimplePool *pool, const char *url);

/**
 * nostr_simple_pool_get_brown_list_stats:
 * @pool: (transfer none): pool
 * @stats: (out): statistics output
 *
 * Get statistics about the brown list.
 */
NOSTR_API void nostr_simple_pool_get_brown_list_stats(NostrSimplePool *pool, NostrBrownListStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_SIMPLE_POOL_H__ */
