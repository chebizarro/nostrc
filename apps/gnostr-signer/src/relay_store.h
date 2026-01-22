/* relay_store.h - Relay configuration management
 *
 * Manages relay list per NIP-65 (kind:10002 relay list metadata).
 * Each relay has read/write permissions.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _RelayStore RelayStore;

/* Relay entry with permissions */
typedef struct {
  gchar *url;           /* Relay URL (wss://...) */
  gboolean read;        /* Allow reading from this relay */
  gboolean write;       /* Allow writing to this relay */
} RelayEntry;

/* Create a new relay store */
RelayStore *relay_store_new(void);

/* Free the relay store */
void relay_store_free(RelayStore *rs);

/* Free a relay entry */
void relay_entry_free(RelayEntry *entry);

/* Load relays from local config */
void relay_store_load(RelayStore *rs);

/* Save relays to local config */
void relay_store_save(RelayStore *rs);

/* Add a relay. Returns FALSE if already exists. */
gboolean relay_store_add(RelayStore *rs, const gchar *url,
                         gboolean read, gboolean write);

/* Remove a relay by URL */
gboolean relay_store_remove(RelayStore *rs, const gchar *url);

/* Update relay permissions */
gboolean relay_store_update(RelayStore *rs, const gchar *url,
                            gboolean read, gboolean write);

/* List all relays.
 * Returns: GPtrArray of RelayEntry* (caller owns array)
 */
GPtrArray *relay_store_list(RelayStore *rs);

/* Get relay count */
guint relay_store_count(RelayStore *rs);

/* Build kind:10002 event JSON for relay list */
gchar *relay_store_build_event_json(RelayStore *rs);

/* Parse kind:10002 event and update store */
gboolean relay_store_parse_event(RelayStore *rs, const gchar *event_json);

/* Get default relays (bootstrap list) */
GPtrArray *relay_store_get_defaults(void);

/* Validate relay URL format */
gboolean relay_store_validate_url(const gchar *url);

/* Get read-only relays */
GPtrArray *relay_store_get_read_relays(RelayStore *rs);

/* Get write relays */
GPtrArray *relay_store_get_write_relays(RelayStore *rs);

G_END_DECLS
