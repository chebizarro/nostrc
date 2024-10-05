#ifndef RELAY_STORE_H
#define RELAY_STORE_H

#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include "nostr.h"

// Define the RelayStore interface
typedef struct {
    int (*publish)(void *self, void *ctx, NostrEvent *event);
    int (*query_sync)(void *self, void *ctx, Filter *filter, NostrEvent ***events, size_t *events_count);
} RelayStore;

// Define the MultiStore struct
typedef struct {
    RelayStore **stores;
    size_t stores_count;
} MultiStore;

// Function prototypes for MultiStore
MultiStore *create_multi_store(size_t initial_size);
void free_multi_store(MultiStore *multi);
int multi_store_publish(MultiStore *multi, void *ctx, NostrEvent *event);
int multi_store_query_sync(MultiStore *multi, void *ctx, Filter *filter, NostrEvent ***events, size_t *events_count);

#endif // RELAY_STORE_H