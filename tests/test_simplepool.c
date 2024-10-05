#include "simplepool.h"
#include <assert.h>

void auth_handler(NostrEvent *event) {
    // Implement auth handler logic here
}

void event_middleware(IncomingEvent *ie) {
    // Implement event middleware logic here
}

int main() {
    SimplePool *pool = create_simple_pool();
    assert(pool != NULL);

    pool->auth_handler = auth_handler;
    pool->event_middleware = event_middleware;

    simple_pool_start(pool);

    const char *urls[] = {"wss://relay1.example.com", "wss://relay2.example.com"};
    Filters filters = *create_filters(1);
    simple_pool_subscribe(pool, urls, 2, filters, true);

    simple_pool_stop(pool);
    free_simple_pool(pool);

    return 0;
}