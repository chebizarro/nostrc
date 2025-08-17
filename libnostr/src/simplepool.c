#include "nostr-simple-pool.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "channel.h"
#include "context.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    pool->signature_checker = NULL;
    pool->running = false;
    pool->subs = NULL;
    pool->subs_count = 0;
    pool->dedup_unique = false;
    pool->dedup_cap = 4096; /* lightweight default */
    pool->dedup_ring = NULL;
    pool->dedup_len = 0;
    pool->dedup_head = 0;

    return pool;
}

// Function to free a SimplePool
void nostr_simple_pool_free(NostrSimplePool *pool) {
    if (pool) {
        /* Ensure stopped */
        if (pool->running) {
            pool->running = false;
            pthread_join(pool->thread, NULL);
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
                nostr_relay_disconnect(pool->relays[i]);
                Error *err = NULL;
                (void)nostr_relay_connect(pool->relays[i], &err);
                if (err) free_error(err);
                pthread_mutex_unlock(&pool->pool_mutex);
                return;
            }
        }
    }

    // If relay not found, create and connect a new one
    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, url, &err);
    if (!relay) {
        if (err) free_error(err);
        pthread_mutex_unlock(&pool->pool_mutex);
        return;
    }
    (void)nostr_relay_connect(relay, &err);
    if (err) free_error(err);

    pool->relays = (NostrRelay **)realloc(pool->relays, (pool->relay_count + 1) * sizeof(NostrRelay *));
    pool->relays[pool->relay_count++] = relay;

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

    while (pool->running) {
        // Pull a small batch from each subscription non-blocking
        for (size_t i = 0; i < pool->subs_count; i++) {
            NostrSubscription *sub = pool->subs[i];
            if (!sub) continue;
            GoChannel *ch = nostr_subscription_get_events_channel(sub);
            if (!ch) continue;
            void *msg = NULL;
            int spins = 0;
            while (go_channel_try_receive(ch, &msg) == 0 && spins++ < 64) {
                if (!msg) break;
                NostrEvent *ev = (NostrEvent *)msg;
                const char *eid = nostr_event_get_id(ev);
                if (pool_seen(pool, eid)) {
                    nostr_event_free(ev);
                } else {
                    if (pool->event_middleware) {
                        NostrIncomingEvent incoming = { .event = ev, .relay = nostr_subscription_get_relay(sub) };
                        pool->event_middleware(&incoming);
                    } else {
                        // default: free to avoid leak; real users should set middleware
                        nostr_event_free(ev);
                    }
                }
                msg = NULL;
            }
        }

        sleep(SEEN_ALREADY_DROP_TICK);
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
    pool->running = false;
    pthread_join(pool->thread, NULL);
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
    for (size_t i = 0; i < url_count; i++) {
        nostr_simple_pool_ensure_relay(pool, urls[i]);
    }

    // Implement query logic here
    (void)filter;
}
