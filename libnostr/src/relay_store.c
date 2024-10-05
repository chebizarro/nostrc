#include "relay_store.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

// Function to create a MultiStore
MultiStore *create_multi_store(size_t initial_size) {
    MultiStore *multi = (MultiStore *)malloc(sizeof(MultiStore));
    if (!multi) return NULL;

    multi->stores = (RelayStore **)malloc(initial_size * sizeof(RelayStore *));
    if (!multi->stores) {
        free(multi);
        return NULL;
    }

    multi->stores_count = 0;
    return multi;
}

// Function to free a MultiStore
void free_multi_store(MultiStore *multi) {
    if (multi) {
        free(multi->stores);
        free(multi);
    }
}

// Function to publish an event to multiple stores
int multi_store_publish(MultiStore *multi, void *ctx, NostrEvent *event) {
    int result = 0;
    for (size_t i = 0; i < multi->stores_count; i++) {
        int res = multi->stores[i]->publish(multi->stores[i], ctx, event);
        if (res != 0) {
            result = res;
        }
    }
    return result;
}

// Function to query events from multiple stores
int multi_store_query_sync(MultiStore *multi, void *ctx, Filter *filter, NostrEvent ***events, size_t *events_count) {
    int result = 0;
    size_t total_events_count = 0;

    for (size_t i = 0; i < multi->stores_count; i++) {
        NostrEvent **store_events = NULL;
        size_t store_events_count = 0;
        int res = multi->stores[i]->query_sync(multi->stores[i], ctx, filter, &store_events, &store_events_count);
        if (res != 0) {
            result = res;
        }

        *events = (NostrEvent **)realloc(*events, (total_events_count + store_events_count) * sizeof(NostrEvent *));
        memcpy(*events + total_events_count, store_events, store_events_count * sizeof(NostrEvent *));
        total_events_count += store_events_count;
        free(store_events);
    }

    *events_count = total_events_count;
    return result;
}