#include "nostr-relay-store.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// Create a NostrMultiStore
NostrMultiStore *nostr_multi_store_new(size_t initial_size) {
    NostrMultiStore *multi = (NostrMultiStore *)malloc(sizeof(NostrMultiStore));
    if (!multi)
        return NULL;

    multi->stores = (NostrRelayStore **)malloc(initial_size * sizeof(NostrRelayStore *));
    if (!multi->stores) {
        free(multi);
        return NULL;
    }

    multi->stores_count = 0;
    return multi;
}

// Free a NostrMultiStore
void nostr_multi_store_free(NostrMultiStore *multi) {
    if (multi) {
        free(multi->stores);
        free(multi);
    }
}

// Publish an event to multiple stores
int nostr_multi_store_publish(NostrMultiStore *multi, void *ctx, NostrEvent *event) {
    if (!multi || !multi->stores) return -1;
    int result = 0;
    for (size_t i = 0; i < multi->stores_count; i++) {
        NostrRelayStore *st = multi->stores[i];
        if (!st || !st->publish) { result = -1; continue; }
        int res = st->publish(st, ctx, event);
        if (res != 0) {
            result = res;
        }
    }
    return result;
}

// Query events from multiple stores
int nostr_multi_store_query_sync(NostrMultiStore *multi, void *ctx, NostrFilter *filter, NostrEvent ***events, size_t *events_count) {
    if (!multi || !multi->stores || !events || !events_count) return -1;
    int result = 0;
    size_t total_events_count = 0;
    NostrEvent **acc = NULL; /* build locally; never clobber caller's ptr on failure */

    for (size_t i = 0; i < multi->stores_count; i++) {
        NostrRelayStore *st = multi->stores[i];
        if (!st || !st->query_sync) { result = -1; continue; }
        NostrEvent **store_events = NULL;
        size_t store_events_count = 0;
        int res = st->query_sync(st, ctx, filter, &store_events, &store_events_count);
        if (res != 0) {
            result = res;
        }

        if (store_events_count > 0 && store_events) {
            /* Use a temp so a realloc failure does not leak the existing buffer. */
            NostrEvent **grown = (NostrEvent **)realloc(
                acc, (total_events_count + store_events_count) * sizeof(NostrEvent *));
            if (!grown) {
                free(store_events);
                free(acc);
                *events = NULL;
                *events_count = 0;
                return -1;
            }
            acc = grown;
            memcpy(acc + total_events_count, store_events, store_events_count * sizeof(NostrEvent *));
            total_events_count += store_events_count;
        }
        free(store_events);
    }

    *events = acc;
    *events_count = total_events_count;
    return result;
}

/* GLib-style accessors (header: nostr-relay-store.h) */
size_t nostr_multi_store_get_count(const NostrMultiStore *multi) {
    if (!multi) return 0;
    return multi->stores_count;
}

NostrRelayStore *nostr_multi_store_get_nth(const NostrMultiStore *multi, size_t index) {
    if (!multi) return NULL;
    if (index >= multi->stores_count) return NULL;
    return multi->stores[index];
}
