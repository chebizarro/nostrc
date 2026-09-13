/* Helper macro for marking intentionally unused static functions */
#if defined(__GNUC__)
#define UNUSED_FUNC __attribute__((unused))
#else
#define UNUSED_FUNC
#endif
#include "nostr-simple-pool.h"
#include "nostr-relay.h"
/* fp-ieg8: the redial worker needs the relay's internal dial-claim primitives.
 * relay-private.h is a libnostr/src-local header, same as this file. */
#include "relay-private.h"
#include "nostr-subscription.h"
#include "channel.h"
#include "context.h"
#include "select.h"
#include "nostr/metrics.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>

static char g_dedup_tombstone;
#define DEDUP_TOMBSTONE ((char *)&g_dedup_tombstone)

/* ========================================================================
 * PHASE 2: SUBSCRIPTION REGISTRY - Pool-level lifecycle management
 * ======================================================================== */

typedef struct PoolSubscriptionEntry {
    NostrSubscription *sub;
    NostrRelay *relay;
    GoContext *ctx;
    CancelFunc cancel;
    uint64_t created_at_ms;
    bool cleanup_requested;
    bool cleanup_in_progress;
    AsyncCleanupHandle *cleanup_handle;
    struct PoolSubscriptionEntry *next;
} PoolSubscriptionEntry;

typedef struct SubscriptionRegistry {
    PoolSubscriptionEntry *head;
    size_t count;
    pthread_mutex_t mutex;
    GoChannel *cleanup_queue;  // Queue of entries to cleanup
    bool shutdown_requested;
} SubscriptionRegistry;

static SubscriptionRegistry *subscription_registry_new(void) {
    SubscriptionRegistry *reg = (SubscriptionRegistry *)malloc(sizeof(SubscriptionRegistry));
    if (!reg) return NULL;
    
    reg->head = NULL;
    reg->count = 0;
    pthread_mutex_init(&reg->mutex, NULL);
    reg->cleanup_queue = go_channel_create(256);  // Buffered queue
    reg->shutdown_requested = false;
    
    return reg;
}

static void subscription_registry_free(SubscriptionRegistry *reg) {
    if (!reg) return;
    
    pthread_mutex_lock(&reg->mutex);
    
    // Free all entries
    PoolSubscriptionEntry *entry = reg->head;
    while (entry) {
        PoolSubscriptionEntry *next = entry->next;
        
        // Cancel context if not already done
        if (entry->cancel && entry->ctx) {
            entry->cancel(entry->ctx);
        }
        
        // Abandon any in-progress cleanup
        if (entry->cleanup_handle) {
            nostr_subscription_cleanup_abandon(entry->cleanup_handle);
        }
        
        free(entry);
        entry = next;
    }
    
    pthread_mutex_unlock(&reg->mutex);
    
    go_channel_free(reg->cleanup_queue);
    pthread_mutex_destroy(&reg->mutex);
    free(reg);
}

static UNUSED_FUNC PoolSubscriptionEntry *subscription_registry_add(SubscriptionRegistry *reg,
                                                         NostrSubscription *sub,
                                                         NostrRelay *relay,
                                                         GoContext *ctx,
                                                         CancelFunc cancel) {
    if (!reg || !sub) return NULL;
    
    PoolSubscriptionEntry *entry = (PoolSubscriptionEntry *)malloc(sizeof(PoolSubscriptionEntry));
    if (!entry) return NULL;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    entry->sub = sub;
    entry->relay = relay;
    entry->ctx = ctx;
    entry->cancel = cancel;
    entry->created_at_ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
    entry->cleanup_requested = false;
    entry->cleanup_in_progress = false;
    entry->cleanup_handle = NULL;
    entry->next = NULL;
    
    pthread_mutex_lock(&reg->mutex);
    
    // Add to head of list
    entry->next = reg->head;
    reg->head = entry;
    reg->count++;
    
    pthread_mutex_unlock(&reg->mutex);
    
    nostr_metric_counter_add("pool_sub_registered", 1);
    
    return entry;
}

static UNUSED_FUNC void subscription_registry_request_cleanup(SubscriptionRegistry *reg, PoolSubscriptionEntry *entry) {
    if (!reg || !entry) return;
    
    pthread_mutex_lock(&reg->mutex);
    
    if (!entry->cleanup_requested) {
        entry->cleanup_requested = true;
        // Queue for cleanup
        go_channel_send(reg->cleanup_queue, entry);
    }
    
    pthread_mutex_unlock(&reg->mutex);
}

static void subscription_registry_remove(SubscriptionRegistry *reg, PoolSubscriptionEntry *entry) {
    if (!reg || !entry) return;
    
    pthread_mutex_lock(&reg->mutex);
    
    // Find and remove from list
    PoolSubscriptionEntry **ptr = &reg->head;
    while (*ptr) {
        if (*ptr == entry) {
            *ptr = entry->next;
            reg->count--;
            
            // Abandon cleanup if in progress
            if (entry->cleanup_handle) {
                nostr_subscription_cleanup_abandon(entry->cleanup_handle);
            }
            
            free(entry);
            pthread_mutex_unlock(&reg->mutex);
            nostr_metric_counter_add("pool_sub_removed", 1);
            return;
        }
        ptr = &(*ptr)->next;
    }
    
    pthread_mutex_unlock(&reg->mutex);
}

/* Background cleanup worker thread */
static void *cleanup_worker_thread(void *arg) {
    NostrSimplePool *pool = (NostrSimplePool *)arg;
    SubscriptionRegistry *reg = pool->sub_registry;
    
    fprintf(stderr, "[pool] cleanup_worker: STARTED\n");
    
    const uint64_t CLEANUP_TIMEOUT_MS = 500;
    
    while (!reg->shutdown_requested) {
        // Wait for cleanup requests with timeout
        PoolSubscriptionEntry *entry = NULL;
        GoSelectCase cases[] = {
            { .op = GO_SELECT_RECEIVE, .chan = reg->cleanup_queue, .recv_buf = (void**)&entry }
        };
        GoSelectResult result = go_select_timeout(cases, 1, 1000); // 1s timeout for periodic checks
        
        if (result.selected_case == -1) {
            // Timeout - check for shutdown
            continue;
        }
        
        if (!result.ok || !entry) {
            // Channel closed or empty
            continue;
        }
        
        // Process cleanup request
        pthread_mutex_lock(&reg->mutex);
        
        if (entry->cleanup_in_progress) {
            // Already being cleaned up
            pthread_mutex_unlock(&reg->mutex);
            continue;
        }
        
        entry->cleanup_in_progress = true;
        NostrSubscription *sub = entry->sub;
        
        pthread_mutex_unlock(&reg->mutex);
        
        // Start async cleanup
        fprintf(stderr, "[pool] cleanup_worker: starting async cleanup for subscription\n");
        AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, CLEANUP_TIMEOUT_MS);
        
        if (handle) {
            entry->cleanup_handle = handle;
            
            // Wait for completion
            bool success = nostr_subscription_cleanup_wait(handle, CLEANUP_TIMEOUT_MS + 500);
            
            if (success) {
                fprintf(stderr, "[pool] cleanup_worker: cleanup SUCCESS\n");
                nostr_metric_counter_add("pool_cleanup_success", 1);
            } else {
                fprintf(stderr, "[pool] cleanup_worker: cleanup TIMEOUT (leaked)\n");
                nostr_metric_counter_add("pool_cleanup_timeout", 1);
            }
            
            nostr_subscription_cleanup_abandon(handle);
            entry->cleanup_handle = NULL;
        } else {
            fprintf(stderr, "[pool] cleanup_worker: failed to start async cleanup\n");
            nostr_metric_counter_add("pool_cleanup_failed", 1);
        }
        
        // Remove entry from registry
        subscription_registry_remove(reg, entry);
    }
    
    fprintf(stderr, "[pool] cleanup_worker: EXITING\n");
    return NULL;
}

/* ========================================================================
 * fp-ieg8: BACKGROUND REDIAL WORKER
 *
 * A relay whose initial connect failed has no message_loop, and therefore no
 * reconnect/backoff loop -- nothing in libnostr ever dialled it again. Callers
 * compensated with periodic ticks of their own (signetd's health tick called
 * ensure_relay every 30s), which meant retry was a property of the CALLER, and
 * a caller that stopped ticking silently stopped retrying. Worse, those ticks
 * ran the blocking dial on whatever thread they lived on.
 *
 * This worker makes retry a property of the pool: one thread, dialling only
 * relays nobody else owns, on the same bounded exponential backoff (1s -> 5min
 * ceiling, jittered) that message_loop uses once a relay has connected at least
 * once.
 * ======================================================================== */

/* Longest idle wait between passes. Only an upper bound: the wait is cut short
 * by a nudge on redial_wake, or by the soonest relay coming due. */
#define POOL_REDIAL_IDLE_MS 1000

static void *pool_redial_thread(void *arg);

/* Test seam (fp-1r0k). Every environment we can test in completes or refuses a
 * local dial in milliseconds -- nostr_connection_new() only blocks when the LWS
 * service thread is stuck, typically in a synchronous DNS lookup for a mistyped
 * hostname -- so no arrangement of sockets reproduces the multi-second dial that
 * the "don't dial on the caller's thread" work is about. Setting
 * NOSTR_TEST_DIAL_DELAY_MS makes a pool dial genuinely slow, so a test can prove
 * the caller stayed responsive THROUGH one instead of asserting a bound that
 * would pass for the wrong reason. Unset (the normal case) this is a getenv. */
static void pool_dial_delay_hook(void) {
    const char *v = getenv("NOSTR_TEST_DIAL_DELAY_MS");
    if (!v || !*v) return;
    long ms = strtol(v, NULL, 10);
    if (ms <= 0) return;
    if (ms > 10000) ms = 10000;   /* capped: a stuck test is a failed test */
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* Nudge the redial worker: something was added, or a backoff was cleared. */
static void pool_redial_wake(NostrSimplePool *pool) {
    if (!pool || !pool->redial_wake) return;
    (void)go_channel_try_send(pool->redial_wake, (void *)(uintptr_t)1);
}

/* Perform one dial of @relay on this thread, with the pool's brown-list
 * bookkeeping and backoff accounting. Must NOT be called with pool_mutex held:
 * a dial can block for up to NOSTR_CONNECT_RESULT_TIMEOUT_MS. */
static bool pool_dial_relay(NostrSimplePool *pool, NostrRelay *relay,
                            const char *url) {
    if (!pool || !relay) return false;

    if (!nostr_relay_dial_try_claim(relay)) {
        /* Someone else is already dialling it, its own loop owns reconnection,
         * auto-reconnect is off, or it is being torn down. All four mean "not
         * ours to dial", and two concurrent nostr_connection_new() calls on one
         * relay would overwrite relay->connection and leak the loser. */
        return false;
    }

    pool_dial_delay_hook();

    Error *err = NULL;
    bool connected = nostr_relay_connect(relay, &err);
    if (err) free_error(err);

    nostr_relay_dial_release(relay, connected);

    if (pool->brown_list_enabled && pool->brown_list && url) {
        if (connected) nostr_brown_list_record_success(pool->brown_list, url);
        else nostr_brown_list_record_failure(pool->brown_list, url);
    }

    return connected;
}

// Function to create a SimplePool
NostrSimplePool *nostr_simple_pool_new(void) {
    NostrSimplePool *pool = (NostrSimplePool *)malloc(sizeof(NostrSimplePool));
    if (!pool)
        return NULL;

    pool->relays = NULL;
    pool->relay_count = 0;
    pthread_mutex_init(&pool->pool_mutex, NULL);
    pool->auth_handler = NULL;
    pool->event_middleware = NULL;
    pool->event_middleware_ex = NULL;
    pool->event_middleware_data = NULL;
    pool->batch_middleware = NULL;
    pool->signature_checker = NULL;
    pool->running = false;
    pool->subs = NULL;
    pool->subs_count = 0;
    pool->filters_shared = NULL;  /* nostrc-ey0f: prevent use-after-free on uninitialized pointer */
    pool->dedup_unique = true;
    pool->dedup_cap = 65536; /* align with GObject reference scale */
    pool->dedup_hash = NULL;
    pool->dedup_hash_sz = 0;
    pool->dedup_count = 0;
    pool->dedup_evict = NULL;
    pool->dedup_evict_head = 0;
    pool->dedup_tombstones = 0;
    // Behavior: auto-unsub on EOSE is off by default; env can enable
    pool->auto_unsub_on_eose = false;
    const char *auto_env = getenv("NOSTR_SIMPLE_POOL_AUTO_UNSUB_EOSE");
    if (auto_env && *auto_env && strcmp(auto_env, "0") != 0) {
        pool->auto_unsub_on_eose = true;
    }
    
    /* Phase 2: Initialize subscription registry and cleanup worker */
    pool->sub_registry = subscription_registry_new();
    pool->cleanup_worker_running = false;

    /* Timeout-audit: wake channel for event-driven worker loop */
    pool->wake_ch = go_channel_create(1);

    if (pool->sub_registry) {
        // Start cleanup worker thread (joinable — NOT detached)
        if (pthread_create(&pool->cleanup_worker_thread, NULL, cleanup_worker_thread, pool) == 0) {
            pool->cleanup_worker_running = true;
            fprintf(stderr, "[pool] cleanup worker thread started\n");
        } else {
            fprintf(stderr, "[pool] WARNING: failed to start cleanup worker thread\n");
        }
    }

    /* nostrc-py1: Initialize brown list for persistently failing relays */
    pool->brown_list = nostr_brown_list_new();
    pool->brown_list_enabled = true;  /* Enabled by default */

    /* Allow environment override */
    const char *brown_env = getenv("NOSTR_BROWN_LIST_ENABLED");
    if (brown_env && strcmp(brown_env, "0") == 0) {
        pool->brown_list_enabled = false;
        fprintf(stderr, "[pool] brown list disabled via environment\n");
    }

    /* nostrc-ey0f: Initialize disposed flag */
    pool->disposed = 0;

    /* fp-ieg8: start the redial worker. Tied to the pool's lifetime, not
     * start()/stop()'s -- see the comment on the struct fields. A pool with no
     * relays registered costs one idle thread; the first ensure_relay_async
     * nudges it awake. */
    pool->redial_thread_running = false;
    pool->redial_stop = 0;
    pool->redial_enabled = true;
    pool->redial_wake = go_channel_create(1);
    if (pthread_create(&pool->redial_thread, NULL, pool_redial_thread, pool) == 0) {
        pool->redial_thread_running = true;
    } else {
        /* Not fatal: relays that connect still self-manage reconnection. What is
         * lost is retry for relays whose first dial fails, so say so rather than
         * failing silently the way the missing mechanism used to. */
        fprintf(stderr, "[pool] WARNING: failed to start redial worker; relays "
                        "whose initial connect fails will not be retried\n");
    }

    return pool;
}

/* Convenience configuration API implementations */
void nostr_simple_pool_set_event_middleware(NostrSimplePool *pool,
                                             void (*cb)(NostrIncomingEvent *)) {
    if (!pool) return;
    pthread_mutex_lock(&pool->pool_mutex);
    pool->event_middleware = cb;
    pool->event_middleware_ex = NULL;
    pool->event_middleware_data = NULL;
    pthread_mutex_unlock(&pool->pool_mutex);
}

void nostr_simple_pool_set_event_middleware_ex(NostrSimplePool *pool,
                                               void (*cb)(NostrIncomingEvent *, void *),
                                               void *user_data) {
    if (!pool) return;
    pthread_mutex_lock(&pool->pool_mutex);
    pool->event_middleware = NULL;  /* clear legacy callback */
    pool->event_middleware_ex = cb;
    pool->event_middleware_data = user_data;
    pthread_mutex_unlock(&pool->pool_mutex);
}

void nostr_simple_pool_set_batch_middleware(NostrSimplePool *pool,
                                            void (*cb)(NostrIncomingEvent *items, size_t count)) {
    if (!pool) return;
    pthread_mutex_lock(&pool->pool_mutex);
    pool->batch_middleware = cb;
    pthread_mutex_unlock(&pool->pool_mutex);
}

void nostr_simple_pool_set_auto_unsub_on_eose(NostrSimplePool *pool, bool enable) {
    if (!pool) return;
    pthread_mutex_lock(&pool->pool_mutex);
    pool->auto_unsub_on_eose = enable;
    pthread_mutex_unlock(&pool->pool_mutex);
}

// Function to free a SimplePool
void nostr_simple_pool_free(NostrSimplePool *pool) {
    if (!pool) return;

    /* nostrc-ey0f: Atomic check to prevent double-free from concurrent paths
     * (e.g., background thread and main thread idle callbacks both unreffing) */
    int expected = 0;
    if (!__atomic_compare_exchange_n(&pool->disposed, &expected, 1,
                                     0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        /* Already disposed by another thread/path */
        return;
    }

    /* fp-ieg8: retire the redial worker before anything it touches is torn
     * down. It holds a ref on any relay it is dialling, so the relay itself
     * cannot be freed underneath it, but it must be gone before pool->relays,
     * the brown list and pool_mutex are destroyed.
     *
     * Cost of this join: at most ONE in-flight connect (the worker re-checks
     * redial_stop between relays), which is the same bound the rest of the
     * shutdown path already accepts. */
    pool->redial_stop = 1;
    pool_redial_wake(pool);
    if (pool->redial_thread_running) {
        pthread_join(pool->redial_thread, NULL);
        pool->redial_thread_running = false;
    }
    if (pool->redial_wake) {
        go_channel_close(pool->redial_wake);
        go_channel_unref(pool->redial_wake);
        pool->redial_wake = NULL;
    }

    /* Phase 2: Shutdown cleanup worker first */
    if (pool->sub_registry) {
        fprintf(stderr, "[pool] shutting down cleanup worker...\n");
        pool->sub_registry->shutdown_requested = true;

        // Close cleanup queue to wake up worker — it checks shutdown_requested
        // on every go_select_timeout cycle and will exit its loop.
        go_channel_close(pool->sub_registry->cleanup_queue);

        // Join the cleanup worker (blocks until it exits — no arbitrary sleep)
        if (pool->cleanup_worker_running) {
            pthread_join(pool->cleanup_worker_thread, NULL);
            pool->cleanup_worker_running = false;
        }

        fprintf(stderr, "[pool] cleanup worker shutdown complete\n");
    }

    /* Ensure stopped */
    if (pool->running) {
        pool->running = false;
        pthread_join(pool->thread, NULL);
    }

    /* Phase 2: Cancel all registered subscriptions */
    if (pool->sub_registry) {
        fprintf(stderr, "[pool] cancelling %zu registered subscriptions...\n",
                pool->sub_registry->count);

        pthread_mutex_lock(&pool->sub_registry->mutex);
        PoolSubscriptionEntry *entry = pool->sub_registry->head;
        while (entry) {
            if (entry->cancel && entry->ctx) {
                entry->cancel(entry->ctx);
            }
            entry = entry->next;
        }
        pthread_mutex_unlock(&pool->sub_registry->mutex);

        /* No arbitrary sleep — the cleanup worker (already joined above)
         * handled in-flight cleanups. Remaining subscriptions in the
         * registry have had their contexts cancelled; they'll tear down
         * when their threads observe the cancellation. */

        subscription_registry_free(pool->sub_registry);
        fprintf(stderr, "[pool] subscription registry freed\n");
    }

    /* Close subscriptions */
    if (pool->subs) {
        for (size_t i = 0; i < pool->subs_count; i++) {
            NostrSubscription *sub = pool->subs[i];
            if (sub) {
                nostr_subscription_close(sub, NULL);
                nostr_subscription_free(sub);
            }
        }
        free(pool->subs);
    }

    /* Free dedup hash and eviction ring */
    if (pool->dedup_hash) {
        for (size_t i = 0; i < pool->dedup_hash_sz; i++) {
            if (pool->dedup_hash[i] && pool->dedup_hash[i] != DEDUP_TOMBSTONE) {
                free(pool->dedup_hash[i]);
            }
        }
        free(pool->dedup_hash);
    }
    if (pool->dedup_evict) {
        free(pool->dedup_evict);
    }

    if (pool->filters_shared) {
        nostr_filters_free(pool->filters_shared);
        pool->filters_shared = NULL;
    }

    for (size_t i = 0; i < pool->relay_count; i++) {
        nostr_relay_free(pool->relays[i]);
    }
    free(pool->relays);

    /* nostrc-py1: Free brown list */
    if (pool->brown_list) {
        nostr_brown_list_free(pool->brown_list);
        pool->brown_list = NULL;
    }

    /* Timeout-audit: Free wake channel */
    if (pool->wake_ch) {
        go_channel_close(pool->wake_ch);
        go_channel_unref(pool->wake_ch);
        pool->wake_ch = NULL;
    }

    pthread_mutex_destroy(&pool->pool_mutex);
    free(pool);
}

/* fp-ieg8: make @url a member of the pool without dialling it.
 *
 * Creating a NostrRelay is cheap and local (nostr_relay_new does no I/O); it is
 * nostr_relay_connect that can block. Splitting the two is what lets a caller on
 * a latency-sensitive thread register its relay set and hand the dialling to the
 * redial worker.
 *
 * Returns a borrowed pointer to the pool's relay for @url, or NULL if it could
 * not be created or registered. Sets @out_is_new when a relay was created.
 */
static NostrRelay *pool_register_relay(NostrSimplePool *pool, const char *url,
                                       bool *out_is_new) {
    if (out_is_new) *out_is_new = false;
    if (!pool || !url || !*url) return NULL;

    pthread_mutex_lock(&pool->pool_mutex);
    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i] && pool->relays[i]->url &&
            strcmp(pool->relays[i]->url, url) == 0) {
            NostrRelay *found = pool->relays[i];
            pthread_mutex_unlock(&pool->pool_mutex);
            return found;
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, url, &err);
    if (err) free_error(err);
    if (!relay) {
        /* nostrc-py1: Record failure even if relay creation fails */
        if (pool->brown_list_enabled && pool->brown_list) {
            nostr_brown_list_record_failure(pool->brown_list, url);
        }
        return NULL;
    }

    /* Skip signature verification - nostrdb handles this during ingestion.
     * This avoids duplicate verification and "Signature verification failed" warnings. */
    relay->assume_valid = true;

    NostrRelay *registered = relay;
    pthread_mutex_lock(&pool->pool_mutex);
    /* Another thread may have registered the same URL while we were allocating. */
    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i] && pool->relays[i]->url &&
            strcmp(pool->relays[i]->url, url) == 0) {
            registered = pool->relays[i];
            break;
        }
    }
    if (registered == relay) {
        // Use a temp so a realloc failure does not leak the existing array.
        NostrRelay **grown = (NostrRelay **)realloc(
            pool->relays, (pool->relay_count + 1) * sizeof(NostrRelay *));
        if (grown) {
            pool->relays = grown;
            pool->relays[pool->relay_count++] = relay;
        } else {
            /* OOM — keep existing pool, drop this relay rather than crash. */
            registered = NULL;
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    if (registered != relay) {
        nostr_relay_free(relay);   /* lost the race, or could not be stored */
    } else if (out_is_new) {
        *out_is_new = true;
    }

    return registered;
}

// Function to ensure a relay connection
void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url) {
    if (!pool || !url || !*url) return;

    /* nostrc-py1: Check brown list before connecting */
    if (pool->brown_list_enabled && pool->brown_list) {
        if (nostr_brown_list_should_skip(pool->brown_list, url)) {
            int remaining = nostr_brown_list_get_time_remaining(pool->brown_list, url);
            fprintf(stderr, "[pool] Skipping browned relay: %s (retry in %ds)\n", url, remaining);
            nostr_metric_counter_add("pool_relay_browned_skip", 1);
            return;
        }
    }

    NostrRelay *relay = pool_register_relay(pool, url, NULL);
    if (!relay) return;

    if (nostr_relay_is_connected(relay)) return;

    /* Dial without pool_mutex held: a connect can block for up to
     * NOSTR_CONNECT_RESULT_TIMEOUT_MS.
     *
     * fp-ieg8: this no longer disconnects an existing-but-unconnected relay
     * first. nostr_relay_disconnect() goes through nostr_relay_close(), which
     * cancels the relay's connection context permanently -- and
     * nostr_relay_connect() reuses that same context, so the reconnected relay
     * came up with a live socket whose reader and writer workers exited
     * immediately. It looked connected and received nothing. A relay that has
     * been explicitly disconnected is finished; register a new one.
     *
     * If the relay's own message_loop is alive it already owns reconnection, so
     * the claim inside pool_dial_relay declines and this returns without
     * fighting it. */
    (void)pool_dial_relay(pool, relay, url);
}

void nostr_simple_pool_ensure_relay_async(NostrSimplePool *pool, const char *url) {
    if (!pool || !url || !*url) return;

    /* No brown-list check here: registration is not a connection attempt, and
     * the redial worker consults the brown list before every dial. Registering
     * a browned relay keeps it visible to get_urls()/reconcile so it rejoins
     * once its brown-list entry expires. */
    if (!pool_register_relay(pool, url, NULL)) return;

    /* Dial promptly rather than on the worker's next idle tick. */
    pool_redial_wake(pool);
}

// Function to add an existing relay to the pool
void nostr_simple_pool_add_relay(NostrSimplePool *pool, NostrRelay *relay) {
    if (!pool || !relay) return;

    pthread_mutex_lock(&pool->pool_mutex);

    // Check if relay already exists (by URL)
    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i] == relay ||
            (pool->relays[i]->url && relay->url && strcmp(pool->relays[i]->url, relay->url) == 0)) {
            pthread_mutex_unlock(&pool->pool_mutex);
            return; // Already in pool
        }
    }

    // Add relay to pool (temp ptr so realloc failure doesn't leak the array).
    {
        NostrRelay **grown = (NostrRelay **)realloc(
            pool->relays, (pool->relay_count + 1) * sizeof(NostrRelay *));
        if (grown) {
            pool->relays = grown;
            pool->relays[pool->relay_count++] = relay;
        }
    }

    pthread_mutex_unlock(&pool->pool_mutex);
}

// Function to remove a relay from the pool by URL (live relay switching)
bool nostr_simple_pool_remove_relay(NostrSimplePool *pool, const char *url) {
    if (!pool || !url || !*url) return false;

    pthread_mutex_lock(&pool->pool_mutex);

    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i] && pool->relays[i]->url &&
            strcmp(pool->relays[i]->url, url) == 0) {
            NostrRelay *relay = pool->relays[i];

            // Shift remaining relays down
            for (size_t j = i + 1; j < pool->relay_count; j++) {
                pool->relays[j - 1] = pool->relays[j];
            }
            pool->relay_count--;

            // Resize array (or set to NULL if empty)
            if (pool->relay_count == 0) {
                free(pool->relays);
                pool->relays = NULL;
            } else {
                pool->relays = (NostrRelay **)realloc(pool->relays,
                    pool->relay_count * sizeof(NostrRelay *));
            }

            pthread_mutex_unlock(&pool->pool_mutex);

            // Disconnect and free relay outside of lock
            fprintf(stderr, "[pool] Removing relay: %s\n", url);
            nostr_relay_disconnect(relay);
            nostr_relay_free(relay);

            return true;
        }
    }

    pthread_mutex_unlock(&pool->pool_mutex);
    return false;
}

// Function to disconnect all relays in the pool (live relay switching)
void nostr_simple_pool_disconnect_all(NostrSimplePool *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->pool_mutex);

    fprintf(stderr, "[pool] Disconnecting all %zu relays\n", pool->relay_count);

    /* fp-ieg8: an explicit disconnect must stick. Without this the redial worker
     * would dial every one of them back on its next pass. Re-armed by start(). */
    pool->redial_enabled = false;

    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i]) {
            nostr_relay_disconnect(pool->relays[i]);
        }
    }

    pthread_mutex_unlock(&pool->pool_mutex);
}

/* fp-ieg8: the pool's retry engine. See the block comment above
 * nostr_simple_pool_new() for why it exists and why its lifetime is the pool's
 * rather than start()/stop()'s. */
static void *pool_redial_thread(void *arg) {
    NostrSimplePool *pool = (NostrSimplePool *)arg;
    if (!pool) return NULL;

    while (!pool->redial_stop) {
        NostrRelay **due = NULL;
        size_t n_due = 0;
        uint64_t wait_ms = POOL_REDIAL_IDLE_MS;

        /* Pick the relays nobody else is responsible for. Refs are taken under
         * pool_mutex so a concurrent remove_relay() or pool free cannot pull a
         * relay out from under the dial below. */
        pthread_mutex_lock(&pool->pool_mutex);
        if (pool->redial_enabled && pool->relay_count > 0) {
            due = (NostrRelay **)calloc(pool->relay_count, sizeof(NostrRelay *));
            for (size_t i = 0; due && i < pool->relay_count; i++) {
                NostrRelay *r = pool->relays[i];
                if (!r) continue;
                if (nostr_relay_is_connected(r)) continue;
                /* The relay connected at least once and its own message_loop is
                 * doing the reconnecting. Touching it here would race that loop
                 * and double-dial the same URL. */
                if (nostr_relay_reconnect_is_self_managed(r)) continue;
                uint64_t remaining = nostr_relay_get_next_reconnect_ms(r);
                if (remaining > 0) {
                    /* Not due yet -- but don't oversleep past it either. */
                    if (remaining < wait_ms) wait_ms = remaining;
                    continue;
                }
                due[n_due++] = nostr_relay_ref(r);
            }
        }
        pthread_mutex_unlock(&pool->pool_mutex);

        for (size_t i = 0; i < n_due; i++) {
            /* Re-check between relays: a pool being freed then waits out at most
             * ONE in-flight connect rather than one per configured relay. */
            if (pool->redial_stop) break;

            NostrRelay *r = due[i];
            const char *url = nostr_relay_get_url_const(r);

            /* Honour the brown list, exactly as ensure_relay does, so a relay
             * that fails persistently while others work is parked instead of
             * being dialled forever. Note the brown list deliberately does not
             * brown anything while NO relay has ever connected, so a
             * single-relay daemon keeps retrying its only relay. */
            if (pool->brown_list_enabled && pool->brown_list && url &&
                nostr_brown_list_should_skip(pool->brown_list, url)) {
                continue;
            }

            nostr_metric_counter_add("pool_redial_attempt", 1);
            if (pool_dial_relay(pool, r, url)) {
                nostr_metric_counter_add("pool_redial_success", 1);
                fprintf(stderr, "[pool] redial connected: %s\n", url ? url : "(null)");
                /* Wake the event worker so its reconcile pass fires the shared
                 * subscription on this relay now rather than up to 200ms later. */
                if (pool->wake_ch)
                    (void)go_channel_try_send(pool->wake_ch, (void *)(uintptr_t)1);
                wait_ms = 0;   /* re-evaluate immediately */
            }
        }

        for (size_t i = 0; i < n_due; i++) nostr_relay_unref(due[i]);
        free(due);

        if (pool->redial_stop) break;

        /* Block rather than spin. Woken by ensure_relay_async (new relay to
         * dial) or by _free() retiring the worker. */
        if (wait_ms > 0 && pool->redial_wake) {
            void *ignored = NULL;
            GoSelectCase wake_case = {
                .op = GO_SELECT_RECEIVE,
                .chan = pool->redial_wake,
                .recv_buf = &ignored,
            };
            (void)go_select_timeout(&wake_case, 1, wait_ms);
        }
    }

    return NULL;
}

// Thread function for SimplePool
static uint64_t dedup_hash_id(const char *id) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)id;
    while (p && *p) {
        h ^= (uint64_t)*p++;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static size_t dedup_next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static int pool_dedup_init(NostrSimplePool *pool) {
    if (!pool || pool->dedup_cap == 0) return 0;
    if (pool->dedup_hash && pool->dedup_evict) return 1;

    size_t hash_sz = dedup_next_pow2(pool->dedup_cap * 2);
    if (hash_sz < 16) hash_sz = 16;
    pool->dedup_hash = (char **)calloc(hash_sz, sizeof(char *));
    pool->dedup_evict = (char **)calloc(pool->dedup_cap, sizeof(char *));
    if (!pool->dedup_hash || !pool->dedup_evict) {
        free(pool->dedup_hash);
        free(pool->dedup_evict);
        pool->dedup_hash = NULL;
        pool->dedup_evict = NULL;
        pool->dedup_hash_sz = 0;
        return 0;
    }
    pool->dedup_hash_sz = hash_sz;
    pool->dedup_count = 0;
    pool->dedup_evict_head = 0;
    pool->dedup_tombstones = 0;
    return 1;
}

static ssize_t dedup_find_slot(NostrSimplePool *pool, const char *id, int *found) {
    if (!pool || !pool->dedup_hash || pool->dedup_hash_sz == 0 || !id) return -1;
    size_t mask = pool->dedup_hash_sz - 1;
    size_t idx = (size_t)dedup_hash_id(id) & mask;
    ssize_t first_tombstone = -1;

    for (size_t probes = 0; probes < pool->dedup_hash_sz; probes++) {
        char *cur = pool->dedup_hash[idx];
        if (!cur) {
            *found = 0;
            return first_tombstone >= 0 ? first_tombstone : (ssize_t)idx;
        }
        if (cur == DEDUP_TOMBSTONE) {
            if (first_tombstone < 0) first_tombstone = (ssize_t)idx;
        } else if (strcmp(cur, id) == 0) {
            *found = 1;
            return (ssize_t)idx;
        }
        idx = (idx + 1) & mask;
    }

    *found = 0;
    return first_tombstone;
}

static int pool_seen(NostrSimplePool *pool, const char *id) {
    if (!pool->dedup_unique || !id || !*id) return 0;
    if (pool->dedup_cap == 0 || !pool_dedup_init(pool)) return 0;

    int found = 0;
    ssize_t slot = dedup_find_slot(pool, id, &found);
    if (found) return 1;
    if (slot < 0) return 0;

    size_t evict_insert_pos = (size_t)-1;
    if (pool->dedup_count == pool->dedup_cap) {
        evict_insert_pos = pool->dedup_evict_head;
        char *evict = pool->dedup_evict[pool->dedup_evict_head];
        if (evict) {
            int evict_found = 0;
            ssize_t evict_slot = dedup_find_slot(pool, evict, &evict_found);
            if (evict_found && evict_slot >= 0) {
                pool->dedup_hash[evict_slot] = DEDUP_TOMBSTONE;
                pool->dedup_tombstones++;
            }
            free(evict);
            pool->dedup_evict[pool->dedup_evict_head] = NULL;
        }
        pool->dedup_count--;
        slot = dedup_find_slot(pool, id, &found);
        if (found) return 1;
        if (slot < 0) return 0;
    }

    char *copy = strdup(id);
    if (!copy) return 0;
    if (pool->dedup_hash[slot] == DEDUP_TOMBSTONE && pool->dedup_tombstones > 0) {
        pool->dedup_tombstones--;
    }
    pool->dedup_hash[slot] = copy;
    size_t pos = (evict_insert_pos != (size_t)-1)
        ? evict_insert_pos
        : (pool->dedup_evict_head + pool->dedup_count) % pool->dedup_cap;
    pool->dedup_evict[pos] = copy;
    pool->dedup_count++;
    if (evict_insert_pos != (size_t)-1) {
        pool->dedup_evict_head = (evict_insert_pos + 1) % pool->dedup_cap;
    }
    return 0;
}

/* Helper: add an event to the batch, growing the buffer as needed.
 * Returns 1 if added, 0 if allocation failed (caller frees ev). */
static int batch_add(NostrIncomingEvent **batch, size_t *len, size_t *cap,
                     NostrEvent *ev, NostrRelay *relay) {
    if (*len == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 64;
        NostrIncomingEvent *nb = realloc(*batch, new_cap * sizeof(NostrIncomingEvent));
        if (!nb) return 0;
        *batch = nb;
        *cap = new_cap;
    }
    (*batch)[(*len)++] = (NostrIncomingEvent){ .event = ev, .relay = relay };
    return 1;
}

/* Helper: remove subscription at index j from pool->subs (caller holds pool_mutex). */
static void pool_remove_sub_locked(NostrSimplePool *pool, size_t j) {
    for (size_t k = j + 1; k < pool->subs_count; k++)
        pool->subs[k - 1] = pool->subs[k];
    pool->subs_count--;
    if (pool->subs_count == 0) { free(pool->subs); pool->subs = NULL; }
}

/* Helper: greedy-drain events from all subscriptions (non-blocking).
 * Also handles CLOSED and EOSE signals. */
static void pool_drain_all(NostrSimplePool *pool,
                           NostrSubscription **subs, size_t count,
                           NostrIncomingEvent **batch, size_t *batch_len,
                           size_t *batch_cap) {
    const int spin_limit = 256;

    for (size_t i = 0; i < count; i++) {
        NostrSubscription *sub = subs[i];
        if (!sub) continue;

        /* --- Drain events channel --- */
        GoChannel *ch = nostr_subscription_get_events_channel(sub);
        if (ch) {
            void *msg = NULL;
            int spins = 0;
            while (go_channel_try_receive(ch, &msg) == 0 && spins++ < spin_limit) {
                if (!msg) break;
                NostrEvent *ev = (NostrEvent *)msg;
                char *eid = nostr_event_get_id(ev);
                int seen = pool_seen(pool, eid);
                free(eid);
                if (seen) {
                    nostr_event_free(ev);
                } else if (pool->event_middleware_ex || pool->event_middleware || pool->batch_middleware) {
                    if (!batch_add(batch, batch_len, batch_cap, ev,
                                   nostr_subscription_get_relay(sub)))
                        nostr_event_free(ev);
                } else {
                    nostr_event_free(ev);
                }
                msg = NULL;
            }
        }

        /* --- CLOSED signal: prune subscription --- */
        GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);
        void *closed_msg = NULL;
        if (ch_closed && go_channel_try_receive(ch_closed, &closed_msg) == 0) {
            pthread_mutex_lock(&pool->pool_mutex);
            for (size_t j = 0; j < pool->subs_count; j++) {
                if (pool->subs[j] == sub) {
                    nostr_subscription_close(sub, NULL);
                    nostr_subscription_free(sub);
                    pool_remove_sub_locked(pool, j);
                    subs[i] = NULL; /* mark stale in local snapshot */
                    break;
                }
            }
            pthread_mutex_unlock(&pool->pool_mutex);
            continue; /* sub freed, skip EOSE check */
        }

        /* --- EOSE signal: optionally auto-unsubscribe --- */
        GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
        if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
            if (pool->auto_unsub_on_eose) {
                pthread_mutex_lock(&pool->pool_mutex);
                for (size_t j = 0; j < pool->subs_count; j++) {
                    if (pool->subs[j] == sub) {
                        nostr_subscription_unsubscribe(sub);
                        nostr_subscription_close(sub, NULL);
                        nostr_subscription_free(sub);
                        pool_remove_sub_locked(pool, j);
                        subs[i] = NULL;
                        break;
                    }
                }
                pthread_mutex_unlock(&pool->pool_mutex);
            }
        }
    }
}

/* nostrc-el92: (Re)fire the shared subscription on connected relays that lack
 * a live sub. nostr_subscription_fire fails when a relay's websocket is not
 * yet up, and previously the error was swallowed with no retry — the pool
 * stayed permanently deaf on any relay that finished connecting after
 * subscribe() was called, or that dropped and reconnected (the CLOSED handler
 * prunes the sub and nothing re-created it). Skipped for auto_unsub_on_eose
 * pools, which drop subs after EOSE on purpose — reconciling those would
 * resubscribe in a loop. Called from the worker loop (≤200ms cadence). */
static void pool_reconcile_subs(NostrSimplePool *pool) {
    if (!pool || pool->auto_unsub_on_eose) return;
    GoContext *bg = go_context_background();
    pthread_mutex_lock(&pool->pool_mutex);
    if (!pool->filters_shared) {
        pthread_mutex_unlock(&pool->pool_mutex);
        return;
    }
    for (size_t i = 0; i < pool->relay_count; i++) {
        NostrRelay *relay = pool->relays[i];
        if (!relay || !nostr_relay_is_connected(relay)) continue;
        bool has_sub = false;
        for (size_t j = 0; j < pool->subs_count; j++) {
            if (pool->subs[j] && nostr_subscription_get_relay(pool->subs[j]) == relay) {
                has_sub = true;
                break;
            }
        }
        if (has_sub) continue;
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, pool->filters_shared);
        if (!sub) continue;
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            fprintf(stderr, "[simplepool] resubscribe fire failed on %s: %s\n",
                    nostr_relay_get_url_const(relay),
                    (err && err->message) ? err->message : "(unknown)");
            if (err) free_error(err);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
            continue;
        }
        NostrSubscription **grown = (NostrSubscription **)realloc(
            pool->subs, (pool->subs_count + 1) * sizeof(NostrSubscription *));
        if (!grown) {
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
            continue;
        }
        pool->subs = grown;
        pool->subs[pool->subs_count++] = sub;
        fprintf(stderr, "[simplepool] (re)subscribed on %s\n",
                nostr_relay_get_url_const(relay));
    }
    pthread_mutex_unlock(&pool->pool_mutex);
}

void *simple_pool_thread_func(void *arg) {
    NostrSimplePool *pool = (NostrSimplePool *)arg;

    /* Timeout-audit: Event-driven worker loop using go_select.
     *
     * Instead of polling all subscription channels with try_receive + usleep,
     * we build a go_select case array covering:
     *   - The pool's wake_ch (signals new subs added or stop requested)
     *   - Every subscription's events channel
     *
     * go_select blocks until at least one channel has data, then we do a
     * greedy non-blocking drain of ALL channels before blocking again.
     * A 200ms timeout ensures we rescan for new/removed subscriptions
     * even if nothing signals the wake channel (safety net). */

    while (pool->running) {
        /* 1. Snapshot subscriptions under lock */
        pthread_mutex_lock(&pool->pool_mutex);
        size_t local_count = pool->subs_count;
        NostrSubscription **local_subs = NULL;
        if (local_count > 0 && pool->subs) {
            local_subs = malloc(local_count * sizeof(NostrSubscription *));
            if (local_subs) {
                memcpy(local_subs, pool->subs,
                       local_count * sizeof(NostrSubscription *));
                /* fp-kxe0: each snapshot owns a reference until the worker has
                 * finished selecting and draining it. Re-subscribe may remove
                 * the pool's reference concurrently, but cannot deallocate a
                 * subscription still present in this snapshot. */
                for (size_t i = 0; i < local_count; i++) {
                    if (local_subs[i]) nostr_subscription_ref(local_subs[i]);
                }
            }
        }
        pthread_mutex_unlock(&pool->pool_mutex);

        /* 2. Build select case array: wake_ch + one events channel per sub.
         * Max cases = 1 (wake) + local_count (events channels). */
        size_t max_cases = 1 + (local_subs ? local_count : 0);
        GoSelectCase *cases = calloc(max_cases, sizeof(GoSelectCase));
        size_t n_cases = 0;

        /* Case 0: wake channel — always present */
        void *wake_val = NULL;
        if (pool->wake_ch) {
            cases[n_cases].op = GO_SELECT_RECEIVE;
            cases[n_cases].chan = pool->wake_ch;
            cases[n_cases].recv_buf = &wake_val;
            n_cases++;
        }

        /* Remaining cases: one events channel per subscription */
        void **recv_bufs = NULL;
        if (local_subs && local_count > 0) {
            recv_bufs = calloc(local_count, sizeof(void *));
            for (size_t i = 0; i < local_count; i++) {
                if (!local_subs[i]) continue;
                GoChannel *ch = nostr_subscription_get_events_channel(local_subs[i]);
                if (!ch) continue;
                cases[n_cases].op = GO_SELECT_RECEIVE;
                cases[n_cases].chan = ch;
                cases[n_cases].recv_buf = &recv_bufs[i];
                n_cases++;
            }
        }

        /* 3. Block until any channel has data (200ms timeout as safety net).
         * n_cases is always >= 1 because wake_ch is always present. */
        if (n_cases > 0) {
            go_select_timeout(cases, n_cases, 200);
        } else {
            /* Defensive: wake_ch should always be present, making n_cases >= 1.
             * If we somehow get here, wait on wake_ch directly instead of usleep. */
            if (pool->wake_ch) {
                GoSelectCase wake_case = { .op = GO_SELECT_RECEIVE, .chan = pool->wake_ch };
                go_select_timeout(&wake_case, 1, 200);
            }
            /* If wake_ch is NULL (shouldn't happen), the loop will spin but
             * pool->running check below will eventually exit. */
        }

        /* 4. Check if we should exit */
        if (!pool->running) {
            free(cases);
            free(recv_bufs);
            if (local_subs) {
                for (size_t i = 0; i < local_count; i++) {
                    if (local_subs[i]) nostr_subscription_unref(local_subs[i]);
                }
            }
            free(local_subs);
            break;
        }

        /* 5. Drain wake channel (consume any pending wake signals) */
        if (pool->wake_ch) {
            void *dummy = NULL;
            while (go_channel_try_receive(pool->wake_ch, &dummy) == 0) { /* drain */ }
        }

        /* 5.5 nostrc-el92: re-fire the shared subscription on relays that
         * (re)connected since the last pass */
        pool_reconcile_subs(pool);

        /* 6. Greedy drain ALL subscription channels (events + closed + eose) */
        NostrIncomingEvent *batch = NULL;
        size_t batch_len = 0, batch_cap = 0;

        if (local_subs && local_count > 0) {
            /* If select returned a specific event, process it first */
            if (recv_bufs) {
                for (size_t i = 0; i < local_count; i++) {
                    if (!recv_bufs[i] || !local_subs[i]) continue;
                    NostrEvent *ev = (NostrEvent *)recv_bufs[i];
                    char *eid = nostr_event_get_id(ev);
                    int seen = pool_seen(pool, eid);
                    free(eid);
                    if (seen) {
                        nostr_event_free(ev);
                    } else if (pool->event_middleware_ex || pool->event_middleware || pool->batch_middleware) {
                        if (!batch_add(&batch, &batch_len, &batch_cap, ev,
                                       nostr_subscription_get_relay(local_subs[i])))
                            nostr_event_free(ev);
                    } else {
                        nostr_event_free(ev);
                    }
                    recv_bufs[i] = NULL;
                }
            }

            /* Now greedy-drain everything else */
            pool_drain_all(pool, local_subs, local_count,
                           &batch, &batch_len, &batch_cap);
        }

        /* 7. Deliver batch to middleware outside of any locks */
        if (batch_len > 0) {
            if (pool->batch_middleware) {
                pool->batch_middleware(batch, batch_len);
            } else if (pool->event_middleware_ex) {
                for (size_t i = 0; i < batch_len; i++) {
                    pool->event_middleware_ex(&batch[i], pool->event_middleware_data);
                }
            } else if (pool->event_middleware) {
                for (size_t i = 0; i < batch_len; i++) {
                    pool->event_middleware(&batch[i]);
                }
            } else {
                for (size_t i = 0; i < batch_len; i++) {
                    if (batch[i].event) nostr_event_free(batch[i].event);
                }
            }
        }

        free(batch);
        free(cases);
        free(recv_bufs);
        if (local_subs) {
            for (size_t i = 0; i < local_count; i++) {
                if (local_subs[i]) nostr_subscription_unref(local_subs[i]);
            }
        }
        free(local_subs);
    }

    return NULL;
}

// Function to start the SimplePool
void nostr_simple_pool_start(NostrSimplePool *pool) {
    if (!pool) return;

    /* fp-ieg8: idempotent. This used to pthread_create unconditionally and
     * overwrite pool->thread, orphaning the previous worker -- never joined,
     * still draining subscriptions. Callers had to serialize start() themselves
     * (signet claims the transition under its own mutex for exactly this
     * reason); a second call is now a no-op instead of a leak. */
    pthread_mutex_lock(&pool->pool_mutex);
    bool already_running = pool->running;
    if (!already_running) {
        pool->running = true;
        /* Re-arm background redial: an explicit disconnect_all()/disconnecting
         * stop() suspended it so it would not immediately dial back what the
         * caller just dropped. */
        pool->redial_enabled = true;
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    if (already_running) return;

    if (pthread_create(&pool->thread, NULL, simple_pool_thread_func, (void *)pool) != 0) {
        pthread_mutex_lock(&pool->pool_mutex);
        pool->running = false;
        pthread_mutex_unlock(&pool->pool_mutex);
        fprintf(stderr, "[pool] ERROR: failed to start pool worker thread\n");
        return;
    }

    /* Anything registered while the pool was down is due now. */
    pool_redial_wake(pool);
}

// Function to stop the SimplePool
void nostr_simple_pool_stop(NostrSimplePool *pool) {
    if (!pool) return;

    /* Mirror of start()'s idempotence: joining pool->thread when no worker was
     * ever created means joining an uninitialized pthread_t. */
    pthread_mutex_lock(&pool->pool_mutex);
    bool was_running = pool->running;
    pool->running = false;
    pthread_mutex_unlock(&pool->pool_mutex);
    if (!was_running) return;

    /* Wake the worker so it sees running=false immediately */
    if (pool->wake_ch) {
        go_channel_try_send(pool->wake_ch, (void *)(uintptr_t)1);
    }
    pthread_join(pool->thread, NULL);
    // On stop: unsubscribe/close/free any active subs and clear list
    pthread_mutex_lock(&pool->pool_mutex);
    if (pool->subs) {
        for (size_t i = 0; i < pool->subs_count; i++) {
            NostrSubscription *sub = pool->subs[i];
            if (!sub) continue;
            nostr_subscription_unsubscribe(sub);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
        }
        free(pool->subs);
        pool->subs = NULL;
        pool->subs_count = 0;
    }
    // Optionally disconnect relays on stop when NOSTR_SIMPLE_POOL_DISCONNECT=1
    const char *disc = getenv("NOSTR_SIMPLE_POOL_DISCONNECT");
    int do_disc = (disc && *disc && strcmp(disc, "0") != 0) ? 1 : 0;
    if (do_disc && pool->relays) {
        /* fp-ieg8: the caller asked for the connections to be dropped, so
         * suspend redial rather than racing it to bring them straight back. */
        pool->redial_enabled = false;
        for (size_t i = 0; i < pool->relay_count; i++) {
            if (pool->relays[i]) nostr_relay_disconnect(pool->relays[i]);
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);
}

/* fp-1r0k: the shared body of subscribe()/subscribe_async(). The only
 * difference is whether relays are dialled on the caller's thread. Everything
 * after that -- the shared filter set, firing on connected relays, waking the
 * worker -- is identical, and must stay identical: the async caller relies on
 * filters_shared being published so the worker's reconcile pass can fire the
 * same subscription on relays that connect later. */
static void pool_subscribe_impl(NostrSimplePool *pool, const char **urls,
                                size_t url_count, NostrFilters filters,
                                bool unique, bool dial_inline) {
    if (!pool || !urls || url_count == 0) return;
    pool->dedup_unique = unique;
    // Ensure relays exist (and, for the blocking variant, are connected)
    for (size_t i = 0; i < url_count; i++) {
        if (!urls[i] || !*urls[i]) continue;
        if (dial_inline) nostr_simple_pool_ensure_relay(pool, urls[i]);
        else nostr_simple_pool_ensure_relay_async(pool, urls[i]);
    }
    // Build deep-copied shared filters object for subscriptions
    NostrFilters *owned = nostr_filters_new();
    if (owned) {
        for (size_t i = 0; i < filters.count; i++) {
            NostrFilter *dup = nostr_filter_copy(&filters.filters[i]);
            if (dup) {
                /* Move contents out of dup into vector, then free shell */
                NostrFilter tmp = *dup;
                free(dup);
                (void)nostr_filters_add(owned, &tmp);
            }
        }
    }
    // Replace pool->filters_shared (must hold mutex — concurrent subscribe calls race)
    GoContext *bg = go_context_background();
    pthread_mutex_lock(&pool->pool_mutex);

    /* fp-kxe0: a subscribe call replaces the pool's subscription intent. Retire
     * every subscription using the previous filters before publishing the new
     * generation, otherwise each reconnect/re-subscribe leaves another live
     * REQ in both the relay map and pool->subs.
     *
     * The pool worker takes a reference while snapshotting pool->subs, so
     * nostr_subscription_free() only drops the pool's reference when a worker
     * is still selecting/draining that subscription. The worker releases its
     * snapshot reference after the iteration, preventing the old pointer from
     * being deallocated underneath it. */
    if (pool->subs) {
        for (size_t i = 0; i < pool->subs_count; i++) {
            NostrSubscription *sub = pool->subs[i];
            if (!sub) continue;
            nostr_subscription_unsubscribe(sub);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
        }
        free(pool->subs);
        pool->subs = NULL;
        pool->subs_count = 0;
    }
    if (pool->filters_shared) {
        nostr_filters_free(pool->filters_shared);
    }
    pool->filters_shared = owned;
    // Create and fire subscriptions per relay
    for (size_t i = 0; i < pool->relay_count; i++) {
        NostrRelay *relay = pool->relays[i];
        if (!relay) continue;
        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, pool->filters_shared);
        if (!sub) continue;
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            /* nostrc-el92: log instead of swallowing; the worker loop's
             * reconcile pass will retry once the relay is connected. */
            fprintf(stderr, "[simplepool] subscribe fire failed on %s (will retry on connect): %s\n",
                    nostr_relay_get_url_const(relay),
                    (err && err->message) ? err->message : "(unknown)");
            if (err) free_error(err);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
            continue;
        }
        pool->subs = (NostrSubscription **)realloc(pool->subs, (pool->subs_count + 1) * sizeof(NostrSubscription *));
        pool->subs[pool->subs_count++] = sub;
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    /* Wake the worker loop so it picks up the new subscriptions immediately */
    if (pool->wake_ch)
        go_channel_try_send(pool->wake_ch, (void *)(uintptr_t)1);
}

// Function to subscribe to multiple relays
void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique) {
    pool_subscribe_impl(pool, urls, url_count, filters, unique, true);
}

void nostr_simple_pool_subscribe_async(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique) {
    pool_subscribe_impl(pool, urls, url_count, filters, unique, false);
}

// Function to query a single event from multiple relays
void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilter filter) {
    if (!pool || !urls || url_count == 0) return;

    // Feature gate: ONESHOT behavior (block until first event or EOSE/CLOSED), else delegate to subscribe
    // nostrc-9o1: Removed arbitrary timeout - use proper signals (EOSE, CLOSED, disconnect)
    const char *oneshot_env = getenv("NOSTR_SIMPLE_POOL_ONESHOT");
    int oneshot = (oneshot_env && *oneshot_env && strcmp(oneshot_env, "0") != 0) ? 1 : 0;

    if (!oneshot) {
        // Ensure relays exist/connected
        for (size_t i = 0; i < url_count; i++) {
            if (urls[i] && *urls[i]) nostr_simple_pool_ensure_relay(pool, urls[i]);
        }
        // Wrap the single filter into a Filters container and delegate to subscribe with de-dup enabled.
        NostrFilters one;
        memset(&one, 0, sizeof(one));
        one.count = 1;
        one.filters = (NostrFilter *)calloc(1, sizeof(NostrFilter));
        if (!one.filters) return;
        NostrFilter *dup = nostr_filter_copy(&filter);
        if (dup) { one.filters[0] = *dup; free(dup); } else { memset(&one.filters[0], 0, sizeof(NostrFilter)); }
        nostr_simple_pool_subscribe(pool, urls, url_count, one, true /* unique/dedup */);
        free(one.filters);
        return;
    }

    // ONESHOT path: create ephemeral subscriptions, deliver first event via middleware, then close.
    GoContext *bg = go_context_background();
    for (size_t i = 0; i < url_count; i++) {
        const char *url = urls[i];
        if (!url || !*url) continue;
        nostr_simple_pool_ensure_relay(pool, url);
        // Find the relay object
        NostrRelay *relay = NULL;
        pthread_mutex_lock(&pool->pool_mutex);
        for (size_t r = 0; r < pool->relay_count; r++) {
            if (pool->relays[r] && strcmp(pool->relays[r]->url, url) == 0) { relay = pool->relays[r]; break; }
        }
        pthread_mutex_unlock(&pool->pool_mutex);
        if (!relay) continue;

        // Build a filters object with the single filter
        NostrFilters *fs = nostr_filters_new();
        if (!fs) continue;
        NostrFilter *dup = nostr_filter_copy(&filter);
        if (dup) { NostrFilter tmp = *dup; free(dup); (void)nostr_filters_add(fs, &tmp); } else { NostrFilter tmp = {0}; (void)nostr_filters_add(fs, &tmp); }

        NostrSubscription *sub = nostr_relay_prepare_subscription(relay, bg, fs);
        if (!sub) { nostr_filters_free(fs); continue; }
        Error *err = NULL;
        if (!nostr_subscription_fire(sub, &err)) {
            if (err) free_error(err);
            nostr_subscription_close(sub, NULL);
            nostr_subscription_free(sub);
            // fs is owned by sub now if fire succeeded; since it failed, free fs
            nostr_filters_free(fs);
            continue;
        }

        // nostrc-9o1: Wait for first event or EOSE/CLOSED using proper signals, not timeouts
        GoChannel *ch_ev = nostr_subscription_get_events_channel(sub);
        GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
        GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);

        GoSelectCase cases[] = {
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ch_ev, .value = NULL, .recv_buf = NULL },
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ch_eose, .value = NULL, .recv_buf = NULL },
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ch_closed, .value = NULL, .recv_buf = NULL },
        };

        while (true) {
            int result = go_select(cases, 3);
            if (result == 0) { // Event received
                void *msg = NULL;
                if (go_channel_try_receive(ch_ev, &msg) == 0 && msg) {
                    NostrEvent *ev = (NostrEvent *)msg;
                    char *eid = nostr_event_get_id(ev);
                    int seen = pool_seen(pool, eid);
                    free(eid);
                    if (!seen) {
                        NostrIncomingEvent incoming = { .event = ev, .relay = relay };
                        if (pool->event_middleware_ex) {
                            pool->event_middleware_ex(&incoming, pool->event_middleware_data);
                        } else if (pool->event_middleware) {
                            pool->event_middleware(&incoming);
                        } else {
                            nostr_event_free(ev);
                        }
                    } else {
                        nostr_event_free(ev);
                    }
                    break; // First event consumed, done with this relay
                }
            } else if (result == 1) { // EOSE
                break; // No events for this filter, move to next relay
            } else if (result == 2) { // CLOSED
                break; // Subscription closed by relay, move to next relay
            } else {
                break; // Unexpected result, move on
            }
        }

        // Close/free ephemeral subscription
        nostr_subscription_close(sub, NULL);
        nostr_subscription_free(sub);
        // fs freed by sub free via nostr_subscription_set_filters ownership
    }
}

/* ========================================================================
 * nostrc-py1: Relay Brown List API
 * ======================================================================== */

void nostr_simple_pool_set_brown_list_enabled(NostrSimplePool *pool, bool enabled) {
    if (!pool) return;
    pthread_mutex_lock(&pool->pool_mutex);
    pool->brown_list_enabled = enabled;
    pthread_mutex_unlock(&pool->pool_mutex);

    fprintf(stderr, "[pool] brown list %s\n", enabled ? "enabled" : "disabled");
}

bool nostr_simple_pool_get_brown_list_enabled(NostrSimplePool *pool) {
    if (!pool) return false;
    pthread_mutex_lock(&pool->pool_mutex);
    bool result = pool->brown_list_enabled;
    pthread_mutex_unlock(&pool->pool_mutex);
    return result;
}

NostrBrownList *nostr_simple_pool_get_brown_list(NostrSimplePool *pool) {
    if (!pool) return NULL;
    return pool->brown_list;
}

bool nostr_simple_pool_is_relay_browned(NostrSimplePool *pool, const char *url) {
    if (!pool || !pool->brown_list || !url) return false;
    return nostr_brown_list_is_browned(pool->brown_list, url);
}

void nostr_simple_pool_clear_brown_list(NostrSimplePool *pool) {
    if (!pool || !pool->brown_list) return;
    nostr_brown_list_clear_all(pool->brown_list);
    fprintf(stderr, "[pool] brown list cleared\n");
}

bool nostr_simple_pool_clear_relay_brown(NostrSimplePool *pool, const char *url) {
    if (!pool || !pool->brown_list || !url) return false;
    return nostr_brown_list_clear_relay(pool->brown_list, url);
}

void nostr_simple_pool_get_brown_list_stats(NostrSimplePool *pool, NostrBrownListStats *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(NostrBrownListStats));
    if (!pool || !pool->brown_list) return;
    nostr_brown_list_get_stats(pool->brown_list, stats);
}
