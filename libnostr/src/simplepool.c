#include "simplepool.h"
#include "go.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Function to create a SimplePool
SimplePool *create_simple_pool(void) {
    SimplePool *pool = (SimplePool *)malloc(sizeof(SimplePool));
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
void free_simple_pool(SimplePool *pool) {
    if (pool) {
        for (size_t i = 0; i < pool->relay_count; i++) {
            free_relay(pool->relays[i]);
        }
        free(pool->relays);
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool);
    }
}

// Function to ensure a relay connection
void simple_pool_ensure_relay(SimplePool *pool, const char *url) {
    pthread_mutex_lock(&pool->pool_mutex);

    for (size_t i = 0; i < pool->relay_count; i++) {
        if (strcmp(pool->relays[i]->url, url) == 0) {
            if (relay_is_connected(pool->relays[i])) {
                pthread_mutex_unlock(&pool->pool_mutex);
                return;
            } else {
                // reconnect if not connected
                relay_disconnect(pool->relays[i]);
                Error **err = NULL;
                relay_connect(pool->relays[i], err);
                pthread_mutex_unlock(&pool->pool_mutex);
                return;
            }
        }
    }

    // If relay not found, create and connect a new one
    GoContext *ctx = NULL;
    go_context_init(ctx, 7);
    Error **err = NULL;
    Relay *relay = new_relay(ctx, url, err);
    relay_connect(relay, err);

    pool->relays = (Relay **)realloc(pool->relays, (pool->relay_count + 1) * sizeof(Relay *));
    pool->relays[pool->relay_count++] = relay;

    pthread_mutex_unlock(&pool->pool_mutex);
}

// Thread function for SimplePool
void *simple_pool_thread_func(void *arg) {
    SimplePool *pool = (SimplePool *)arg;

    while (pool->running) {
        // Implement event handling and relay management here

        sleep(SEEN_ALREADY_DROP_TICK);
    }

    return NULL;
}

// Function to start the SimplePool
void simple_pool_start(SimplePool *pool) {
    pool->running = true;
    pthread_create(&pool->thread, NULL, simple_pool_thread_func, (void *)pool);
}

// Function to stop the SimplePool
void simple_pool_stop(SimplePool *pool) {
    pool->running = false;
    pthread_join(pool->thread, NULL);
}

// Function to subscribe to multiple relays
void simple_pool_subscribe(SimplePool *pool, const char **urls, size_t url_count, Filters filters, bool unique) {
    for (size_t i = 0; i < url_count; i++) {
        simple_pool_ensure_relay(pool, urls[i]);
    }

    // Implement subscription logic here
    (void)filters;
    (void)unique;
}

// Function to query a single event from multiple relays
void simple_pool_query_single(SimplePool *pool, const char **urls, size_t url_count, Filter filter) {
    for (size_t i = 0; i < url_count; i++) {
        simple_pool_ensure_relay(pool, urls[i]);
    }

    // Implement query logic here
    (void)filter;
}
