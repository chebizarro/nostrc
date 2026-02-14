#ifndef __GNOSTR_RELAY_STORE_H__
#define __GNOSTR_RELAY_STORE_H__

/* Public header exposing GI-friendly names for NostrRelayStore/NostrMultiStore (canonical). */

#include <stdbool.h>
#include <stddef.h>
#include "nostr-event.h"
#include "nostr-filter.h"

#ifdef __cplusplus
extern "C" {
#endif

// Define the NostrRelayStore interface (core C type)
typedef struct {
    int (*publish)(void *self, void *ctx, NostrEvent *event);
    int (*query_sync)(void *self, void *ctx, NostrFilter *filter, NostrEvent ***events, size_t *events_count);
} NostrRelayStore;

// Define the NostrMultiStore struct (core C type)
typedef struct {
    NostrRelayStore **stores;
    size_t stores_count;
} NostrMultiStore;

/* Canonical API */
NostrMultiStore *nostr_multi_store_new(size_t initial_size);
void              nostr_multi_store_free(NostrMultiStore *multi);
int               nostr_multi_store_publish(NostrMultiStore *multi, void *ctx, NostrEvent *event);
int               nostr_multi_store_query_sync(NostrMultiStore *multi, void *ctx, NostrFilter *filter, NostrEvent ***events, size_t *events_count);

/* Accessors (GLib-friendly) */
/**
 * nostr_multi_store_get_count:
 * @multi: (nullable): multi store
 *
 * Returns: number of stores (0 if @multi is NULL)
 */
size_t nostr_multi_store_get_count(const NostrMultiStore *multi);

/**
 * nostr_multi_store_get_nth:
 * @multi: (nullable): multi store
 * @index: index into internal stores array
 *
 * Returns: (transfer none) (nullable): pointer to store at @index
 */
NostrRelayStore *nostr_multi_store_get_nth(const NostrMultiStore *multi, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* __GNOSTR_RELAY_STORE_H__ */
