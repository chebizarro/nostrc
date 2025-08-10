#include "nostr-simple-pool.h"
#include "nostr-relay.h"
#include "go.h"
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

    return pool;
}

// Function to free a SimplePool
void nostr_simple_pool_free(NostrSimplePool *pool) {
    if (pool) {
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
                Error **err = NULL;
                nostr_relay_connect(pool->relays[i], err);
                pthread_mutex_unlock(&pool->pool_mutex);
                return;
            }
        }
    }

    // If relay not found, create and connect a new one
    GoContext *ctx = NULL;
    go_context_init(ctx, 7);
    Error **err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, url, err);
    nostr_relay_connect(relay, err);

    pool->relays = (NostrRelay **)realloc(pool->relays, (pool->relay_count + 1) * sizeof(NostrRelay *));
    pool->relays[pool->relay_count++] = relay;

    pthread_mutex_unlock(&pool->pool_mutex);
}

// Thread function for SimplePool
void *simple_pool_thread_func(void *arg) {
    NostrSimplePool *pool = (NostrSimplePool *)arg;

    while (pool->running) {
        // Implement event handling and relay management here

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
    for (size_t i = 0; i < url_count; i++) {
        nostr_simple_pool_ensure_relay(pool, urls[i]);
    }

    // Implement subscription logic here
    (void)filters;
    (void)unique;
}

// Function to query a single event from multiple relays
void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, NostrFilter filter) {
    for (size_t i = 0; i < url_count; i++) {
        nostr_simple_pool_ensure_relay(pool, urls[i]);
    }

    // Implement query logic here
    (void)filter;
}
