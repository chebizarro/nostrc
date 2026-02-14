/**
 * Relay Configuration Service
 *
 * Manages relay URLs, NIP-65 relay list metadata, NIP-17 DM relays,
 * and live relay switching. GSettings schema ID is injected via
 * gnostr_relays_init() — the library has no opinion about schema names.
 */
#ifndef NOSTR_GOBJECT_GNOSTR_RELAYS_H
#define NOSTR_GOBJECT_GNOSTR_RELAYS_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * gnostr_relays_init:
 * @schema_id: GSettings schema ID for relay settings (e.g., "org.gnostr.gnostr")
 *
 * Initialize the relay module with the GSettings schema to use.
 * Must be called before any relay load/save functions.
 */
void gnostr_relays_init(const char *schema_id);

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

/* --- NIP-17 DM Relay List (kind 10050) --- */

/* Load DM relay URLs from config into provided array of gchar* (owned by caller). */
void gnostr_load_dm_relays_into(GPtrArray *out);

/* Save DM relay URLs from provided array to config; replaces the list. */
void gnostr_save_dm_relays_from(GPtrArray *arr);

/* Parse a kind 10050 DM relay list event JSON and extract relay URLs.
 * Returns a GPtrArray of gchar* URLs (caller owns).
 * Pass NULL for out_created_at to ignore timestamp. */
GPtrArray *gnostr_nip17_parse_dm_relays_event(const gchar *event_json, gint64 *out_created_at);

/* Fetch NIP-17 DM relay list for a pubkey asynchronously.
 * Queries configured relays for kind 10050 event by the given author.
 * Callback receives GPtrArray* of gchar* relay URLs on success.
 * These are the "inbox" relays where the user receives DMs. */
typedef void (*GnostrNip17DmRelayCallback)(GPtrArray *dm_relays, gpointer user_data);
void gnostr_nip17_fetch_dm_relays_async(const gchar *pubkey_hex,
                                         GCancellable *cancellable,
                                         GnostrNip17DmRelayCallback callback,
                                         gpointer user_data);

/* Get user's own DM relays (from local config or kind 10050).
 * Returns a new GPtrArray of gchar* URLs (caller frees).
 * Falls back to general relays if no DM relays are configured. */
GPtrArray *gnostr_get_dm_relays(void);

/* --- NIP-65 Publishing Functions --- */

/* Build unsigned kind 10002 event JSON from a NIP-65 relay list.
 * Returns newly allocated JSON string (caller frees with g_free).
 * The event contains "r" tags for each relay with optional read/write markers. */
gchar *gnostr_nip65_build_event_json(GPtrArray *nip65_relays);

/* Callback for async NIP-65 publish operation */
typedef void (*GnostrNip65PublishCallback)(gboolean success,
                                            const gchar *error_msg,
                                            gpointer user_data);

/* Publish NIP-65 relay list to relays via signer IPC.
 * Signs the event and publishes to all configured relays.
 * @param nip65_relays  GPtrArray of GnostrNip65Relay* to publish
 * @param callback      Callback when publish completes (may be NULL)
 * @param user_data     User data for callback */
void gnostr_nip65_publish_async(GPtrArray *nip65_relays,
                                 GnostrNip65PublishCallback callback,
                                 gpointer user_data);

/* Callback for async NIP-65 load operation */
typedef void (*GnostrNip65LoadCallback)(GPtrArray *relays,
                                         gpointer user_data);

/* Load NIP-65 relay list for current user on login.
 * Fetches kind 10002 event from configured relays and applies to local config.
 * @param pubkey_hex    User's public key in hex
 * @param callback      Callback with parsed relay list (may be NULL)
 * @param user_data     User data for callback */
void gnostr_nip65_load_on_login_async(const gchar *pubkey_hex,
                                       GnostrNip65LoadCallback callback,
                                       gpointer user_data);

/* Convert local relay config to NIP-65 relay list (all as read+write).
 * Returns a new GPtrArray of GnostrNip65Relay* (caller owns). */
GPtrArray *gnostr_nip65_from_local_config(void);

/* Apply NIP-65 relay list to local config (saves URLs to settings).
 * Only saves unique relay URLs, preserving read+write relays. */
void gnostr_nip65_apply_to_local_config(GPtrArray *nip65_relays);

/* Load relays with their NIP-65 types from config.
 * Returns a GPtrArray of GnostrNip65Relay* (caller owns). */
GPtrArray *gnostr_load_nip65_relays(void);

/* Save relays with their NIP-65 types to config.
 * Takes a GPtrArray of GnostrNip65Relay*. */
void gnostr_save_nip65_relays(GPtrArray *relays);

/* Get only read-capable relays (read-only or read+write) from config.
 * Returns a new GPtrArray of gchar* URLs (caller frees). */
GPtrArray *gnostr_get_read_relay_urls(void);

/* Get only write-capable relays (write-only or read+write) from config.
 * Returns a new GPtrArray of gchar* URLs (caller frees). */
GPtrArray *gnostr_get_write_relay_urls(void);

/* Append read-capable relay URLs to an existing array.
 * Useful when relay_urls may already have some entries. */
void gnostr_get_read_relay_urls_into(GPtrArray *out);

/* Append write-capable relay URLs to an existing array.
 * Useful when relay_urls may already have some entries. */
void gnostr_get_write_relay_urls_into(GPtrArray *out);

/* --- Live Relay Switching --- */

/* Callback for relay configuration changes.
 * @param user_data  User data passed during registration */
typedef void (*GnostrRelayChangeCallback)(gpointer user_data);

/* Register a callback to be notified when relay configuration changes.
 * Uses GSettings "changed" signal internally.
 * @param callback   Function to call when relays change
 * @param user_data  User data to pass to callback
 * @return Handler ID that can be used with gnostr_relay_change_disconnect() */
gulong gnostr_relay_change_connect(GnostrRelayChangeCallback callback, gpointer user_data);

/* Disconnect a previously registered relay change callback.
 * @param handler_id  Handler ID returned by gnostr_relay_change_connect() */
void gnostr_relay_change_disconnect(gulong handler_id);

/* Emit a relay change notification (called when relays are saved).
 * This manually triggers callbacks without waiting for GSettings to propagate.
 * Useful when you need immediate notification after saving. */
void gnostr_relay_change_emit(void);

/* Get the shared GSettings instance for relay configuration.
 * Returns NULL if schema is not available or not initialized.
 * DO NOT unref the returned object - it is managed internally. */
GSettings *gnostr_relay_get_settings(void);

G_END_DECLS
#endif /* NOSTR_GOBJECT_GNOSTR_RELAYS_H */
