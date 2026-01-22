#pragma once

#include <glib.h>
#include <gio/gio.h>

/* Return config file path; respects GNOSTR_CONFIG_PATH if set. Caller frees. */
gchar *gnostr_config_path(void);

/* Basic validation for Nostr relay URLs: must be ws:// or wss:// and have a host. */
gboolean gnostr_is_valid_relay_url(const char *url);

/* Normalize relay URL (trim spaces, lowercase scheme/host, remove trailing slash). Returns newly allocated string or NULL if invalid. */
gchar *gnostr_normalize_relay_url(const char *url);

/* Load relay URLs from config into provided array of gchar* (owned by caller). */
void gnostr_load_relays_into(GPtrArray *out);

/* Save relay URLs from provided array to config; replaces the list. */
void gnostr_save_relays_from(GPtrArray *arr);

/* NIP-65 Relay List Metadata (kind 10002) */
typedef enum {
  GNOSTR_RELAY_READWRITE = 0,  /* No marker - read/write */
  GNOSTR_RELAY_READ      = 1,  /* "read" marker - read-only */
  GNOSTR_RELAY_WRITE     = 2,  /* "write" marker - write-only */
} GnostrRelayType;

typedef struct {
  gchar *url;
  GnostrRelayType type;
} GnostrNip65Relay;

/* Free a NIP-65 relay entry */
void gnostr_nip65_relay_free(GnostrNip65Relay *relay);

/* Parse a kind 10002 event JSON and extract relay list.
 * Returns a GPtrArray of GnostrNip65Relay* (caller owns).
 * Pass NULL for the out_created_at to ignore timestamp. */
GPtrArray *gnostr_nip65_parse_event(const gchar *event_json, gint64 *out_created_at);

/* Fetch NIP-65 relay list for a pubkey asynchronously.
 * Queries configured relays for kind 10002 event by the given author.
 * Callback receives GPtrArray* of GnostrNip65Relay* on success. */
typedef void (*GnostrNip65RelayCallback)(GPtrArray *relays, gpointer user_data);
void gnostr_nip65_fetch_relays_async(const gchar *pubkey_hex,
                                      GCancellable *cancellable,
                                      GnostrNip65RelayCallback callback,
                                      gpointer user_data);

/* Get write relays from a NIP-65 relay list (for reading user's posts).
 * Returns a new GPtrArray of gchar* URLs (caller frees). */
GPtrArray *gnostr_nip65_get_write_relays(GPtrArray *nip65_relays);

/* Get read relays from a NIP-65 relay list (for publishing to user).
 * Returns a new GPtrArray of gchar* URLs (caller frees). */
GPtrArray *gnostr_nip65_get_read_relays(GPtrArray *nip65_relays);
