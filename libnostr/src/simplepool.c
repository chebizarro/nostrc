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
    
    if (pool->sub_registry) {
        // Start cleanup worker thread
        if (pthread_create(&pool->cleanup_worker_thread, NULL, cleanup_worker_thread, pool) == 0) {
            pool->cleanup_worker_running = true;
            pthread_detach(pool->cleanup_worker_thread);
            fprintf(stderr, "[pool] cleanup worker thread started\n");
        } else {
            fprintf(stderr, "[pool] WARNING: failed to start cleanup worker thread\n");
        }
    }

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
    if (pool) {
        /* Phase 2: Shutdown cleanup worker first */
        if (pool->sub_registry) {
            fprintf(stderr, "[pool] shutting down cleanup worker...\n");
            pool->sub_registry->shutdown_requested = true;
            
            // Close cleanup queue to wake up worker
            go_channel_close(pool->sub_registry->cleanup_queue);
            
            // Give worker time to exit gracefully (max 2s)
            if (pool->cleanup_worker_running) {
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 100000000; // 100ms
                for (int i = 0; i < 20; i++) {
                    nanosleep(&ts, NULL);
                    // Worker should exit on its own
                }
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
            
            // Give subscriptions brief time to cleanup (500ms)
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 500000000;
            nanosleep(&ts, NULL);
            
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
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool);
    }
}

// Function to ensure a relay connection
void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url) {
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
                (void)nostr_relay_connect(relay, &err);
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
        if (err) free_error(err);
        return;
    }
    (void)nostr_relay_connect(relay, &err);
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

void *simple_pool_thread_func(void *arg) {
    NostrSimplePool *pool = (NostrSimplePool *)arg;

    // Adaptive backoff between 2ms and 50ms
    unsigned backoff_us = 2000;
    const unsigned backoff_min = 2000;
    const unsigned backoff_max = 50000;
    size_t rr_start = 0; // round-robin start index for fairness

    while (pool->running) {
        // Snapshot subs under lock to avoid races while iterating
        pthread_mutex_lock(&pool->pool_mutex);
        size_t local_count = pool->subs_count;
        NostrSubscription **local_subs = NULL;
        if (local_count > 0 && pool->subs) {
            local_subs = (NostrSubscription **)malloc(local_count * sizeof(NostrSubscription *));
            if (local_subs) memcpy(local_subs, pool->subs, local_count * sizeof(NostrSubscription *));
        }
        pthread_mutex_unlock(&pool->pool_mutex);

        // Batch to deliver to middleware after scan
        NostrIncomingEvent *batch = NULL; size_t batch_len = 0; size_t batch_cap = 0;

        int did_work = 0; // any events processed or subs pruned

        if (local_subs && local_count > 0) {
            // Fairness: start at rotating index
            size_t start = rr_start % local_count;
            for (size_t ofs = 0; ofs < local_count; ofs++) {
                size_t i = (start + ofs) % local_count;
                NostrSubscription *sub = local_subs[i];
                if (!sub) continue;
                GoChannel *ch = nostr_subscription_get_events_channel(sub);
                if (!ch) continue;
                void *msg = NULL;
                int spins = 0;
                while (go_channel_try_receive(ch, &msg) == 0 && spins++ < 32) {
                    if (!msg) break;
                    NostrEvent *ev = (NostrEvent *)msg;
                    const char *eid = nostr_event_get_id(ev);
                    if (pool_seen(pool, eid)) {
                        nostr_event_free(ev);
                    } else {
                        // Add to batch (or free if no middleware)
                        if (pool->event_middleware || pool->batch_middleware) {
                            if (batch_len == batch_cap) {
                                size_t new_cap = batch_cap ? batch_cap * 2 : 64;
                                NostrIncomingEvent *nb = (NostrIncomingEvent *)realloc(batch, new_cap * sizeof(NostrIncomingEvent));
                                if (!nb) { nostr_event_free(ev); break; }
                                batch = nb; batch_cap = new_cap;
                            }
                            batch[batch_len++] = (NostrIncomingEvent){ .event = ev, .relay = nostr_subscription_get_relay(sub) };
                        } else {
                            // default: free to avoid leak; real users should set middleware
                            nostr_event_free(ev);
                        }
                        did_work = 1;
                    }
                    msg = NULL;
                }

                // Opportunistically prune CLOSED subscriptions (non-blocking)
                GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);
                void *closed_msg = NULL;
                if (ch_closed && go_channel_try_receive(ch_closed, &closed_msg) == 0) {
                    pthread_mutex_lock(&pool->pool_mutex);
                    for (size_t j = 0; j < pool->subs_count; j++) {
                        if (pool->subs[j] == sub) {
                            nostr_subscription_close(sub, NULL);
                            nostr_subscription_free(sub);
                            for (size_t k = j + 1; k < pool->subs_count; k++) pool->subs[k - 1] = pool->subs[k];
                            pool->subs_count--;
                            if (pool->subs_count == 0) { free(pool->subs); pool->subs = NULL; }
                            break;
                        }
                    }
                    pthread_mutex_unlock(&pool->pool_mutex);
                    did_work = 1;
                }

                // EOSE handling: optionally auto-unsubscribe
                GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
                if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                    if (pool->auto_unsub_on_eose) {
                        pthread_mutex_lock(&pool->pool_mutex);
                        for (size_t j = 0; j < pool->subs_count; j++) {
                            if (pool->subs[j] == sub) {
                                nostr_subscription_unsubscribe(sub);
                                nostr_subscription_close(sub, NULL);
                                nostr_subscription_free(sub);
                                for (size_t k = j + 1; k < pool->subs_count; k++) pool->subs[k - 1] = pool->subs[k];
                                pool->subs_count--;
                                if (pool->subs_count == 0) { free(pool->subs); pool->subs = NULL; }
                                break;
                            }
                        }
                        pthread_mutex_unlock(&pool->pool_mutex);
                    }
                    did_work = 1;
                }
            }
            rr_start = (start + 1) % local_count;
        }

        // Deliver batch to middleware outside of any locks
        if (batch_len > 0) {
            if (pool->batch_middleware) {
                pool->batch_middleware(batch, batch_len);
            } else if (pool->event_middleware) {
                for (size_t i = 0; i < batch_len; i++) {
                    pool->event_middleware(&batch[i]);
                }
            } else {
                // No middleware: free events to avoid leaks
                for (size_t i = 0; i < batch_len; i++) {
                    if (batch[i].event) nostr_event_free(batch[i].event);
                }
            }
        }
        // If no middleware, events were already freed on receipt
        free(batch);
        free(local_subs);

        // Adaptive sleep/backoff
        if (did_work) {
            backoff_us = backoff_min;
        } else {
            backoff_us = (backoff_us < backoff_max) ? (backoff_us * 2) : backoff_max;
        }
        usleep(backoff_us);
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
    // Replace pool->filters_shared
    if (pool->filters_shared) {
        nostr_filters_free(pool->filters_shared);
    }
    pool->filters_shared = owned;
    // Create and fire subscriptions per relay
    GoContext *bg = go_context_background();
    pthread_mutex_lock(&pool->pool_mutex);
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
}

// Function to query a single event from multiple relays
void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilter filter) {
    if (!pool || !urls || url_count == 0) return;

    // Feature gate: ONESHOT behavior (block briefly per URL until first event or EOSE), else delegate to subscribe
    const char *oneshot_env = getenv("NOSTR_SIMPLE_POOL_ONESHOT");
    int oneshot = (oneshot_env && *oneshot_env && strcmp(oneshot_env, "0") != 0) ? 1 : 0;
    unsigned timeout_ms = 1000; // default 1s per URL
    const char *to_env = getenv("NOSTR_SIMPLE_POOL_Q_TIMEOUT_MS");
    if (to_env && *to_env) { unsigned v = (unsigned)atoi(to_env); if (v > 0) timeout_ms = v; }

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

        // Wait for first event or EOSE/timeout
        GoChannel *ch_ev = nostr_subscription_get_events_channel(sub);
        GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
        const unsigned step_us = 5 * 1000; // 5ms polling
        unsigned waited = 0;
        int done = 0;
        while (!done && waited < timeout_ms) {
            void *msg = NULL;
            if (ch_ev && go_channel_try_receive(ch_ev, &msg) == 0 && msg) {
                NostrEvent *ev = (NostrEvent *)msg;
                if (!pool_seen(pool, nostr_event_get_id(ev))) {
                    if (pool->event_middleware) {
                        NostrIncomingEvent incoming = { .event = ev, .relay = relay };
                        pool->event_middleware(&incoming);
                    } else {
                        nostr_event_free(ev);
                    }
                } else {
                    nostr_event_free(ev);
                }
                done = 1; // first event consumed
                break;
            }
            // Check EOSE: stop if seen
            if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
                break;
            }
            usleep(step_us);
            waited += (step_us / 1000);
        }

        // Close/free ephemeral subscription
        nostr_subscription_close(sub, NULL);
        nostr_subscription_free(sub);
        // fs freed by sub free via nostr_subscription_set_filters ownership
    }
}
