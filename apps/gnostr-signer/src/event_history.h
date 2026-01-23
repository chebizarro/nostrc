/* event_history.h - Transaction/event history storage for gnostr-signer
 *
 * Stores history of all signing operations with:
 * - Timestamp of operation
 * - Event kind signed
 * - Client application identifier
 * - Event ID (truncated for display)
 * - Success/failure status
 *
 * Storage: JSON file in user config directory for simplicity and portability.
 * File: ~/.config/gnostr-signer/event_history.json
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GN_TYPE_EVENT_HISTORY_ENTRY (gn_event_history_entry_get_type())
#define GN_TYPE_EVENT_HISTORY (gn_event_history_get_type())

G_DECLARE_FINAL_TYPE(GnEventHistoryEntry, gn_event_history_entry, GN, EVENT_HISTORY_ENTRY, GObject)
G_DECLARE_FINAL_TYPE(GnEventHistory, gn_event_history, GN, EVENT_HISTORY, GObject)

/**
 * GnEventHistoryResult:
 * @GN_EVENT_HISTORY_SUCCESS: Operation completed successfully
 * @GN_EVENT_HISTORY_DENIED: Operation was denied by user
 * @GN_EVENT_HISTORY_ERROR: Operation failed due to error
 * @GN_EVENT_HISTORY_TIMEOUT: Operation timed out
 *
 * Result status of a signing operation.
 */
typedef enum {
  GN_EVENT_HISTORY_SUCCESS,
  GN_EVENT_HISTORY_DENIED,
  GN_EVENT_HISTORY_ERROR,
  GN_EVENT_HISTORY_TIMEOUT
} GnEventHistoryResult;

/* ============================================================================
 * GnEventHistoryEntry - Individual history entry
 * ============================================================================ */

/**
 * gn_event_history_entry_get_id:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the unique entry ID.
 *
 * Returns: (transfer none): Entry ID string
 */
const gchar *gn_event_history_entry_get_id(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_timestamp:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the timestamp when the operation occurred.
 *
 * Returns: Unix timestamp (seconds since epoch)
 */
gint64 gn_event_history_entry_get_timestamp(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_event_id:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the Nostr event ID (hex).
 *
 * Returns: (transfer none) (nullable): Event ID or NULL
 */
const gchar *gn_event_history_entry_get_event_id(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_event_kind:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the Nostr event kind.
 *
 * Returns: Event kind number
 */
gint gn_event_history_entry_get_event_kind(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_client_pubkey:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the client's public key (hex).
 *
 * Returns: (transfer none) (nullable): Client pubkey or NULL
 */
const gchar *gn_event_history_entry_get_client_pubkey(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_client_app:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the client application name.
 *
 * Returns: (transfer none) (nullable): App name or NULL
 */
const gchar *gn_event_history_entry_get_client_app(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_identity:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the identity (npub) that signed the event.
 *
 * Returns: (transfer none) (nullable): Identity npub or NULL
 */
const gchar *gn_event_history_entry_get_identity(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_method:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the NIP-46 method used (e.g., "sign_event", "nip04_encrypt").
 *
 * Returns: (transfer none): Method name
 */
const gchar *gn_event_history_entry_get_method(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_result:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the result status of the operation.
 *
 * Returns: #GnEventHistoryResult
 */
GnEventHistoryResult gn_event_history_entry_get_result(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_content_preview:
 * @self: A #GnEventHistoryEntry
 *
 * Gets a preview of the event content (truncated).
 *
 * Returns: (transfer none) (nullable): Content preview or NULL
 */
const gchar *gn_event_history_entry_get_content_preview(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_get_truncated_event_id:
 * @self: A #GnEventHistoryEntry
 *
 * Gets the event ID truncated for display (first 8 + last 4 chars).
 *
 * Returns: (transfer full) (nullable): Truncated event ID or NULL
 */
gchar *gn_event_history_entry_get_truncated_event_id(GnEventHistoryEntry *self);

/**
 * gn_event_history_entry_format_timestamp:
 * @self: A #GnEventHistoryEntry
 *
 * Formats the timestamp as a human-readable string.
 *
 * Returns: (transfer full): Formatted timestamp string
 */
gchar *gn_event_history_entry_format_timestamp(GnEventHistoryEntry *self);

/* ============================================================================
 * GnEventHistory - History manager
 * ============================================================================ */

/**
 * gn_event_history_new:
 *
 * Creates a new event history manager.
 *
 * Returns: (transfer full): A new #GnEventHistory
 */
GnEventHistory *gn_event_history_new(void);

/**
 * gn_event_history_get_default:
 *
 * Gets the singleton event history instance.
 *
 * Returns: (transfer none): The default #GnEventHistory
 */
GnEventHistory *gn_event_history_get_default(void);

/**
 * gn_event_history_load:
 * @self: A #GnEventHistory
 *
 * Loads history from disk. Safe to call multiple times.
 *
 * Returns: %TRUE if load was successful (or file doesn't exist yet)
 */
gboolean gn_event_history_load(GnEventHistory *self);

/**
 * gn_event_history_save:
 * @self: A #GnEventHistory
 *
 * Saves history to disk.
 *
 * Returns: %TRUE if save was successful
 */
gboolean gn_event_history_save(GnEventHistory *self);

/**
 * gn_event_history_add_entry:
 * @self: A #GnEventHistory
 * @event_id: (nullable): Event ID (hex)
 * @event_kind: Event kind number
 * @client_pubkey: (nullable): Client public key (hex)
 * @client_app: (nullable): Client application name
 * @identity: (nullable): Identity npub that signed
 * @method: NIP-46 method used
 * @result: Operation result
 * @content_preview: (nullable): Preview of content (truncated)
 *
 * Adds a new entry to the history.
 *
 * Returns: (transfer none): The newly created entry
 */
GnEventHistoryEntry *gn_event_history_add_entry(GnEventHistory *self,
                                                  const gchar *event_id,
                                                  gint event_kind,
                                                  const gchar *client_pubkey,
                                                  const gchar *client_app,
                                                  const gchar *identity,
                                                  const gchar *method,
                                                  GnEventHistoryResult result,
                                                  const gchar *content_preview);

/**
 * gn_event_history_list_entries:
 * @self: A #GnEventHistory
 * @offset: Starting offset for pagination
 * @limit: Maximum number of entries (0 for all)
 *
 * Lists history entries with pagination.
 *
 * Returns: (transfer container) (element-type GnEventHistoryEntry):
 *          Array of entries. Free with g_ptr_array_unref().
 */
GPtrArray *gn_event_history_list_entries(GnEventHistory *self,
                                          guint offset,
                                          guint limit);

/**
 * gn_event_history_filter_by_kind:
 * @self: A #GnEventHistory
 * @kind: Event kind to filter by (-1 for all)
 * @offset: Starting offset
 * @limit: Maximum entries
 *
 * Lists entries filtered by event kind.
 *
 * Returns: (transfer container) (element-type GnEventHistoryEntry):
 *          Filtered array. Free with g_ptr_array_unref().
 */
GPtrArray *gn_event_history_filter_by_kind(GnEventHistory *self,
                                            gint kind,
                                            guint offset,
                                            guint limit);

/**
 * gn_event_history_filter_by_client:
 * @self: A #GnEventHistory
 * @client_pubkey: (nullable): Client pubkey to filter by (NULL for all)
 * @offset: Starting offset
 * @limit: Maximum entries
 *
 * Lists entries filtered by client.
 *
 * Returns: (transfer container) (element-type GnEventHistoryEntry):
 *          Filtered array. Free with g_ptr_array_unref().
 */
GPtrArray *gn_event_history_filter_by_client(GnEventHistory *self,
                                              const gchar *client_pubkey,
                                              guint offset,
                                              guint limit);

/**
 * gn_event_history_filter_by_date_range:
 * @self: A #GnEventHistory
 * @start_time: Start timestamp (0 for no start bound)
 * @end_time: End timestamp (0 for no end bound)
 * @offset: Starting offset
 * @limit: Maximum entries
 *
 * Lists entries filtered by date range.
 *
 * Returns: (transfer container) (element-type GnEventHistoryEntry):
 *          Filtered array. Free with g_ptr_array_unref().
 */
GPtrArray *gn_event_history_filter_by_date_range(GnEventHistory *self,
                                                   gint64 start_time,
                                                   gint64 end_time,
                                                   guint offset,
                                                   guint limit);

/**
 * gn_event_history_filter:
 * @self: A #GnEventHistory
 * @kind: Event kind (-1 for all)
 * @client_pubkey: (nullable): Client pubkey (NULL for all)
 * @start_time: Start timestamp (0 for no start bound)
 * @end_time: End timestamp (0 for no end bound)
 * @offset: Starting offset
 * @limit: Maximum entries
 *
 * Combined filter with all criteria.
 *
 * Returns: (transfer container) (element-type GnEventHistoryEntry):
 *          Filtered array. Free with g_ptr_array_unref().
 */
GPtrArray *gn_event_history_filter(GnEventHistory *self,
                                    gint kind,
                                    const gchar *client_pubkey,
                                    gint64 start_time,
                                    gint64 end_time,
                                    guint offset,
                                    guint limit);

/**
 * gn_event_history_get_entry_count:
 * @self: A #GnEventHistory
 *
 * Gets total number of history entries.
 *
 * Returns: Entry count
 */
guint gn_event_history_get_entry_count(GnEventHistory *self);

/**
 * gn_event_history_get_unique_kinds:
 * @self: A #GnEventHistory
 *
 * Gets list of unique event kinds in history.
 *
 * Returns: (transfer full): Array of event kinds (-1 terminated)
 */
gint *gn_event_history_get_unique_kinds(GnEventHistory *self);

/**
 * gn_event_history_get_unique_clients:
 * @self: A #GnEventHistory
 *
 * Gets list of unique client pubkeys in history.
 *
 * Returns: (transfer full): NULL-terminated array of pubkeys
 */
gchar **gn_event_history_get_unique_clients(GnEventHistory *self);

/**
 * gn_event_history_clear:
 * @self: A #GnEventHistory
 *
 * Clears all history entries.
 */
void gn_event_history_clear(GnEventHistory *self);

/**
 * gn_event_history_export_json:
 * @self: A #GnEventHistory
 * @entries: (nullable): Entries to export (NULL for all)
 * @pretty: Whether to pretty-print JSON
 *
 * Exports history to JSON string.
 *
 * Returns: (transfer full): JSON string
 */
gchar *gn_event_history_export_json(GnEventHistory *self,
                                     GPtrArray *entries,
                                     gboolean pretty);

/**
 * gn_event_history_export_csv:
 * @self: A #GnEventHistory
 * @entries: (nullable): Entries to export (NULL for all)
 *
 * Exports history to CSV string.
 *
 * Returns: (transfer full): CSV string with header
 */
gchar *gn_event_history_export_csv(GnEventHistory *self,
                                    GPtrArray *entries);

/**
 * gn_event_history_export_to_file:
 * @self: A #GnEventHistory
 * @path: File path to export to
 * @format: Export format ("json" or "csv")
 * @entries: (nullable): Entries to export (NULL for all)
 * @error: Return location for error
 *
 * Exports history to a file.
 *
 * Returns: %TRUE on success
 */
gboolean gn_event_history_export_to_file(GnEventHistory *self,
                                          const gchar *path,
                                          const gchar *format,
                                          GPtrArray *entries,
                                          GError **error);

G_END_DECLS
