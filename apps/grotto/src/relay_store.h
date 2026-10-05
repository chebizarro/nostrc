/* relay_store.h - Relay configuration management
 *
 * Manages relay list per NIP-65 (kind:10002 relay list metadata).
 * Each relay has read/write permissions.
 * Supports per-identity relay lists (nostrc-5ju).
 */
#ifndef APPS_GNOSTR_SIGNER_RELAY_STORE_H
#define APPS_GNOSTR_SIGNER_RELAY_STORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _RelayStore RelayStore;

/* Relay entry with permissions */
typedef struct {
  gchar *url;           /* Relay URL (wss://...) */
  gboolean read;        /* Allow reading from this relay */
  gboolean write;       /* Allow writing to this relay */
} RelayEntry;

/* Create a new relay store for a specific identity (npub).
 * If identity is NULL, uses a global relay store.
 */
RelayStore *relay_store_new_for_identity(const gchar *identity);

/* Create a new relay store (global/shared) */
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

/* Connection status */
typedef enum {
  RELAY_STATUS_UNKNOWN,
  RELAY_STATUS_CONNECTING,
  RELAY_STATUS_CONNECTED,
  RELAY_STATUS_DISCONNECTED,
  RELAY_STATUS_ERROR
} RelayConnectionStatus;

/* Get connection status for a relay (async check) */
RelayConnectionStatus relay_store_get_status(RelayStore *rs, const gchar *url);

/* Set connection status (called by connection manager) */
void relay_store_set_status(RelayStore *rs, const gchar *url, RelayConnectionStatus status);

/* Test relay connection (async callback) */
typedef void (*RelayTestCallback)(const gchar *url, RelayConnectionStatus status, gpointer user_data);
void relay_store_test_connection(const gchar *url, RelayTestCallback cb, gpointer user_data);

/* Get read-only relays */
GPtrArray *relay_store_get_read_relays(RelayStore *rs);

/* Get write relays */
GPtrArray *relay_store_get_write_relays(RelayStore *rs);

/* Get the identity associated with this store (NULL for global) */
const gchar *relay_store_get_identity(RelayStore *rs);

/* Check if an identity has a custom relay list configured */
gboolean relay_store_identity_has_config(const gchar *identity);

/* Copy relays from another store (useful for inheriting defaults) */
void relay_store_copy_from(RelayStore *dest, RelayStore *src);

/* Reset to defaults (clear current and populate with bootstrap relays) */
void relay_store_reset_to_defaults(RelayStore *rs);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_RELAY_STORE_H */
