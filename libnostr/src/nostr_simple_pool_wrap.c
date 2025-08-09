#include <stddef.h>
#include <stdbool.h>
#include "nostr-simple-pool.h"

NostrSimplePool *nostr_simple_pool_new(void) {
    return nostr_simple_pool_new();
}

void nostr_simple_pool_free(NostrSimplePool *pool) {
    nostr_simple_pool_free((SimplePool *)pool);
}

void nostr_simple_pool_ensure_relay(NostrSimplePool *pool, const char *url) {
    nostr_simple_pool_ensure_relay((SimplePool *)pool, url);
}

void nostr_simple_pool_start(NostrSimplePool *pool) {
    nostr_simple_pool_start((SimplePool *)pool);
}

void nostr_simple_pool_stop(NostrSimplePool *pool) {
    nostr_simple_pool_stop((SimplePool *)pool);
}

void nostr_simple_pool_subscribe(NostrSimplePool *pool, const char **urls, size_t url_count, Filters filters, bool unique) {
    nostr_simple_pool_subscribe((SimplePool *)pool, urls, url_count, filters, unique);
}

void nostr_simple_pool_query_single(NostrSimplePool *pool, const char **urls, size_t url_count, Filter filter) {
    nostr_simple_pool_query_single((SimplePool *)pool, urls, url_count, filter);
}
