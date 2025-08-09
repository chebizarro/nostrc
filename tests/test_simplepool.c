#include "nostr-simple-pool.h"
#include <assert.h>

void auth_handler(NostrEvent *event) {
    // Implement auth handler logic here
}

void event_middleware(IncomingEvent *ie) {
    // Implement event middleware logic here
}

int main() {
    NostrSimplePool *pool = nostr_simple_pool_new();
    assert(pool != NULL);

    pool->auth_handler = auth_handler;
    pool->event_middleware = event_middleware;

    nostr_simple_pool_start(pool);

    const char *urls[] = {"wss://relay1.example.com", "wss://relay2.example.com"};
    Filters filters = *create_filters(1);
    nostr_simple_pool_subscribe(pool, urls, 2, filters, true);

    nostr_simple_pool_stop(pool);
    nostr_simple_pool_free(pool);

    return 0;
}