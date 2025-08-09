#include <stddef.h>
#include <stdbool.h>
#include "nostr-simple-pool.h"

NostrSimplePool *nostr_simple_pool_new(void) {
    return create_simple_pool();
}

void nostr_simple_pool_free(NostrSimplePool *pool) {
    free_simple_pool((SimplePool *)pool);
}

void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url) {
    simple_pool_ensure_relay((SimplePool *)pool, url);
}

void nostr_simple_pool_start(NostrSimplePool *pool) {
    simple_pool_start((SimplePool *)pool);
}

void nostr_simple_pool_stop(NostrSimplePool *pool) {
    simple_pool_stop((SimplePool *)pool);
}

void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, Filters filters, bool unique) {
    simple_pool_subscribe((SimplePool *)pool, urls, url_count, filters, unique);
}

void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, Filter filter) {
    simple_pool_query_single((SimplePool *)pool, urls, url_count, filter);
}
