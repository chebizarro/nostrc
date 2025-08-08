#ifndef __NOSTR_RELAY_STORE_H__
#define __NOSTR_RELAY_STORE_H__

/* Transitional header exposing GLib-friendly names for RelayStore/MultiStore. */

#include <stdbool.h>
#include <stddef.h>
#include "relay_store.h" /* legacy RelayStore and MultiStore */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical typedefs for GLib-style naming */
typedef RelayStore NostrRelayStore;
typedef MultiStore NostrMultiStore;

/* New API names mapped to legacy implementations */
#define nostr_multi_store_new          create_multi_store
#define nostr_multi_store_free         free_multi_store
#define nostr_multi_store_publish      multi_store_publish
#define nostr_multi_store_query_sync   multi_store_query_sync

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

#endif /* __NOSTR_RELAY_STORE_H__ */
