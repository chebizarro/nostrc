#ifndef SIMPLEPOOL_H
#define SIMPLEPOOL_H

#include <stddef.h>
#include "relay.h"
#include "event.h"
#include "filter.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define SEEN_ALREADY_DROP_TICK 60 // seconds

typedef struct _IncomingEvent {
    NostrEvent *event;
    Relay *relay;
} IncomingEvent;

typedef struct _SimplePool {
    Relay **relays;
    size_t relay_count;
    pthread_mutex_t pool_mutex;
    void (*auth_handler)(NostrEvent *);
    void (*event_middleware)(IncomingEvent *);
    bool (*signature_checker)(NostrEvent);
    bool running;
    pthread_t thread;
} SimplePool;

typedef struct _DirectedFilters {
    Filters filters;
    char *relay_url;
} DirectedFilters;

SimplePool *nostr_simple_pool_new(void);
void nostr_simple_pool_free(SimplePool *pool);
void nostr_simple_pool_ensure_relay(SimplePool *pool, const char *url);
void *simple_pool_thread_func(void *arg);
void nostr_simple_pool_start(SimplePool *pool);
void nostr_simple_pool_stop(SimplePool *pool);
void nostr_simple_pool_subscribe(SimplePool *pool, const char **urls, size_t url_count, Filters filters, bool unique);
void nostr_simple_pool_query_single(SimplePool *pool, const char **urls, size_t url_count, Filter filter);

#endif // SIMPLEPOOL_H
