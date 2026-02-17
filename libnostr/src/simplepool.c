/* Helper macro for marking intentionally unused static functions */
#if defined(__GNUC__)
#define UNUSED_FUNC __attribute__((unused))
#else
#define UNUSED_FUNC
#endif
#include "nostr-simple-pool.h"
#include "nostr-relay.h"
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
    pool->batch_middleware = NULL;
    pool->signature_checker = NULL;
    pool->running = false;
    pool->subs = NULL;
    pool->subs_count = 0;
    pool->filters_shared = NULL;  /* nostrc-ey0f: prevent use-after-free on uninitialized pointer */
    pool->dedup_unique = false;
    pool->dedup_cap = 65536; /* align with GObject reference scale */
    pool->dedup_ring = NULL;
    pool->dedup_len = 0;
    pool->dedup_head = 0;
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

    return pool;
}

/* Convenience configuration API implementations */
void nostr_simple_pool_set_event_middleware(NostrSimplePool *pool,
                                            void (*cb)(NostrIncomingEvent *)) {
    if (!pool) return;
    pthread_mutex_lock(&pool->pool_mutex);
    pool->event_middleware = cb;
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

    /* Free dedup ring */
    if (pool->dedup_ring) {
        for (size_t i = 0; i < pool->dedup_len; i++) free(pool->dedup_ring[i]);
        free(pool->dedup_ring);
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

// Function to ensure a relay connection
void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url) {
    /* nostrc-py1: Check brown list before connecting */
    if (pool->brown_list_enabled && pool->brown_list) {
        if (nostr_brown_list_should_skip(pool->brown_list, url)) {
            int remaining = nostr_brown_list_get_time_remaining(pool->brown_list, url);
            fprintf(stderr, "[pool] Skipping browned relay: %s (retry in %ds)\n", url, remaining);
            nostr_metric_counter_add("pool_relay_browned_skip", 1);
            return;
        }
    }

    pthread_mutex_lock(&pool->pool_mutex);

    for (size_t i = 0; i < pool->relay_count; i++) {
        if (strcmp(pool->relays[i]->url, url) == 0) {
            if (nostr_relay_is_connected(pool->relays[i])) {
                pthread_mutex_unlock(&pool->pool_mutex);
                return;
            } else {
                // reconnect if not connected
                NostrRelay *relay = pool->relays[i];
                pthread_mutex_unlock(&pool->pool_mutex);  // CRITICAL: Unlock BEFORE blocking operation

                nostr_relay_disconnect(relay);
                Error *err = NULL;
                bool connected = nostr_relay_connect(relay, &err);

                /* nostrc-py1: Record success/failure in brown list */
                if (pool->brown_list_enabled && pool->brown_list) {
                    if (connected && !err) {
                        nostr_brown_list_record_success(pool->brown_list, url);
                    } else {
                        nostr_brown_list_record_failure(pool->brown_list, url);
                    }
                }

                if (err) free_error(err);
                return;
            }
        }
    }

    // If relay not found, create and connect a new one
    // CRITICAL: Unlock mutex before blocking operations (relay_new, relay_connect)
    pthread_mutex_unlock(&pool->pool_mutex);

    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, url, &err);
    if (!relay) {
        /* nostrc-py1: Record failure even if relay creation fails */
        if (pool->brown_list_enabled && pool->brown_list) {
            nostr_brown_list_record_failure(pool->brown_list, url);
        }
        if (err) free_error(err);
        return;
    }

    /* Skip signature verification - nostrdb handles this during ingestion.
     * This avoids duplicate verification and "Signature verification failed" warnings. */
    relay->assume_valid = true;

    bool connected = nostr_relay_connect(relay, &err);

    /* nostrc-py1: Record success/failure in brown list */
    if (pool->brown_list_enabled && pool->brown_list) {
        if (connected && !err) {
            nostr_brown_list_record_success(pool->brown_list, url);
        } else {
            nostr_brown_list_record_failure(pool->brown_list, url);
        }
    }

    if (err) free_error(err);

    // Re-lock to add relay to pool
    pthread_mutex_lock(&pool->pool_mutex);
    pool->relays = (NostrRelay **)realloc(pool->relays, (pool->relay_count + 1) * sizeof(NostrRelay *));
    pool->relays[pool->relay_count++] = relay;
    pthread_mutex_unlock(&pool->pool_mutex);
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

    // Add relay to pool
    pool->relays = (NostrRelay **)realloc(pool->relays, (pool->relay_count + 1) * sizeof(NostrRelay *));
    pool->relays[pool->relay_count++] = relay;

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

    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i]) {
            nostr_relay_disconnect(pool->relays[i]);
        }
    }

    pthread_mutex_unlock(&pool->pool_mutex);
}

// Thread function for SimplePool
static int pool_seen(NostrSimplePool *pool, const char *id) {
    if (!pool->dedup_unique || !id || !*id) return 0;
    /* Linear scan in ring buffer (small cap) */
    for (size_t i = 0; i < pool->dedup_len; i++) {
        size_t idx = (pool->dedup_head + pool->dedup_len - 1 - i) % (pool->dedup_cap ? pool->dedup_cap : 1);
        const char *v = pool->dedup_ring ? pool->dedup_ring[idx] : NULL;
        if (v && strcmp(v, id) == 0) return 1;
    }
    /* Insert */
    if (!pool->dedup_ring && pool->dedup_cap > 0) {
        pool->dedup_ring = (char **)calloc(pool->dedup_cap, sizeof(char *));
        pool->dedup_len = 0;
        pool->dedup_head = 0;
    }
    if (pool->dedup_cap > 0) {
        if (pool->dedup_len < pool->dedup_cap) {
            size_t pos = (pool->dedup_head + pool->dedup_len) % pool->dedup_cap;
            pool->dedup_ring[pos] = strdup(id);
            pool->dedup_len++;
        } else {
            /* evict head */
            if (pool->dedup_ring[pool->dedup_head]) free(pool->dedup_ring[pool->dedup_head]);
            pool->dedup_ring[pool->dedup_head] = strdup(id);
            pool->dedup_head = (pool->dedup_head + 1) % pool->dedup_cap;
        }
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
                } else if (pool->event_middleware || pool->batch_middleware) {
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
            if (local_subs)
                memcpy(local_subs, pool->subs, local_count * sizeof(NostrSubscription *));
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

        /* 3. Block until any channel has data (200ms timeout as safety net) */
        if (n_cases > 0) {
            go_select_timeout(cases, n_cases, 200);
        } else {
            /* No subs and no wake channel — shouldn't happen in practice.
             * go_select_timeout(0) returns immediately, so use usleep
             * to avoid a tight spin while waiting for subs to appear. */
            usleep(50000); /* 50ms */
        }

        /* 4. Check if we should exit */
        if (!pool->running) {
            free(cases);
            free(recv_bufs);
            free(local_subs);
            break;
        }

        /* 5. Drain wake channel (consume any pending wake signals) */
        if (pool->wake_ch) {
            void *dummy = NULL;
            while (go_channel_try_receive(pool->wake_ch, &dummy) == 0) { /* drain */ }
        }

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
                    } else if (pool->event_middleware || pool->batch_middleware) {
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
        free(local_subs);
    }

    return NULL;
}

// Function to start the SimplePool
void nostr_simple_pool_start(NostrSimplePool *pool) {
    pool->running = true;
    pthread_create(&pool->thread, NULL, simple_pool_thread_func, (void *)pool);
}

// Function to stop the SimplePool
void nostr_simple_pool_stop(NostrSimplePool *pool) {
    if (!pool) return;
    pool->running = false;
    /* Wake the worker so it sees running=false immediately */
    if (pool->wake_ch) {
        go_channel_try_send(pool->wake_ch, (void *)(uintptr_t)1);
    }
    if (pool->thread) pthread_join(pool->thread, NULL);
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
        for (size_t i = 0; i < pool->relay_count; i++) {
            if (pool->relays[i]) nostr_relay_disconnect(pool->relays[i]);
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);
}

// Function to subscribe to multiple relays
void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilters filters, bool unique) {
    if (!pool || !urls || url_count == 0) return;
    pool->dedup_unique = unique;
    // Ensure relays exist/connected
    for (size_t i = 0; i < url_count; i++) {
        if (urls[i] && *urls[i]) nostr_simple_pool_ensure_relay(pool, urls[i]);
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
                        if (pool->event_middleware) {
                            NostrIncomingEvent incoming = { .event = ev, .relay = relay };
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
