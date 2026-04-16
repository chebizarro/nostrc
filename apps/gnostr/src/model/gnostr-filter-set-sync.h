/* gnostr-filter-set-sync.h — NIP-78 sync layer for custom filter sets.
 *
 * SPDX-License-Identifier: MIT
 *
 * Overview
 * --------
 * Persists the default #GnostrFilterSetManager's custom (user-owned)
 * filter sets to the user's relays using NIP-78 application-specific
 * data events (kind 30078) so a single Gnostr account sees the same
 * tabs across devices. Local JSON storage remains authoritative and is
 * always written first — relays are treated as a best-effort backup
 * that is consulted on login and opportunistically updated afterwards.
 *
 * Design
 * ------
 * - Data key: "filter-sets" (d-tag becomes "gnostr/filter-sets").
 * - Payload: the existing on-disk JSON shape
 *     { "version": 1, "filter_sets": [ <filter-set JSON>, ... ] }
 *   wrapped in a kind-30078 event by gnostr_app_data_manager.
 * - Push: debounced write triggered by the manager's items-changed
 *   signal. Rapid edits during an import coalesce into a single
 *   publish after FLUSH_DELAY_MS idle.
 * - Pull: runs once on enable(). Remote wins on conflict (latest
 *   created_at); local-only sets are preserved by union-on-id so a
 *   brand-new device never clobbers the account's existing tabs.
 * - Deletions: **not** propagated across devices in this revision.
 *   Because remote payloads are full snapshots rather than diffs, a
 *   filter set removed on device A will vanish from A's next push
 *   but continue to exist on device B indefinitely until B receives
 *   a push that touches the same id. A future revision can add a
 *   tombstone list to the payload; current callers must accept that
 *   remove-across-devices is a manual operation for v1.
 * - Fallback: every push also triggers a local save; every pull
 *   apply+save so the JSON file on disk stays current regardless of
 *   relay availability.
 *
 * Threading
 * ---------
 * All public entry points must be called on the GLib main context
 * (same as the filter-set manager itself). The debounce timer and all
 * async callbacks run on the default main context.
 *
 * nostrc-yg8j.9: Filter set persistence via NIP-78.
 */

#ifndef GNOSTR_FILTER_SET_SYNC_H
#define GNOSTR_FILTER_SET_SYNC_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GNOSTR_FILTER_SET_SYNC_DATA_KEY:
 *
 * Data-key component of the NIP-78 d-tag. The full d-tag emitted on
 * the wire is "gnostr/filter-sets".
 */
#define GNOSTR_FILTER_SET_SYNC_DATA_KEY "filter-sets"

/**
 * GNOSTR_FILTER_SET_SYNC_FORMAT_VERSION:
 *
 * Schema version of the JSON payload. Matches the local storage
 * version so the two can share a parser.
 */
#define GNOSTR_FILTER_SET_SYNC_FORMAT_VERSION 1

/**
 * GnostrFilterSetSyncCallback:
 * @success: %TRUE if the operation completed without error
 * @error_msg: (nullable): human-readable error message on failure
 * @user_data: caller-supplied closure data
 *
 * Signature for completion callbacks of the async push/pull
 * entry points.
 */
typedef void (*GnostrFilterSetSyncCallback)(gboolean success,
                                            const gchar *error_msg,
                                            gpointer user_data);

/**
 * gnostr_filter_set_sync_enable:
 * @pubkey_hex: 64-character hex pubkey of the signed-in user
 *
 * Start syncing filter sets for @pubkey_hex. Threads the pubkey into
 * the default #GnostrAppDataManager, triggers an initial pull, and
 * wires a debounced push on the default manager's items-changed
 * signal so subsequent local edits propagate to relays automatically.
 *
 * Does nothing when @pubkey_hex is %NULL or empty. When the same
 * pubkey is already enabled this is a no-op; switching users calls
 * disable() implicitly before re-enabling.
 */
void gnostr_filter_set_sync_enable(const gchar *pubkey_hex);

/**
 * gnostr_filter_set_sync_disable:
 *
 * Stop sync: cancel any pending debounced push, disconnect the
 * items-changed handler, cancel in-flight async operations, and
 * forget the remembered pubkey. Local storage continues to work via
 * gnostr_filter_set_manager_save().
 */
void gnostr_filter_set_sync_disable(void);

/**
 * gnostr_filter_set_sync_is_enabled:
 *
 * Returns: %TRUE if enable() has been called with a non-empty pubkey
 *   and disable() has not yet been called.
 */
gboolean gnostr_filter_set_sync_is_enabled(void);

/**
 * gnostr_filter_set_sync_pull_async:
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (nullable): completion callback
 * @user_data: closure data for @callback
 *
 * Fetch the most recent kind-30078 filter-sets event for the enabled
 * pubkey, apply it to the default #GnostrFilterSetManager, and
 * persist the result to local storage. Fails synchronously (invoking
 * @callback with success=%FALSE) when sync is disabled or no pubkey
 * is set.
 */
void gnostr_filter_set_sync_pull_async(GCancellable *cancellable,
                                       GnostrFilterSetSyncCallback callback,
                                       gpointer user_data);

/**
 * gnostr_filter_set_sync_push_async:
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (nullable): completion callback
 * @user_data: closure data for @callback
 *
 * Serialize the custom filter sets currently in the default manager
 * and publish them to relays. Normally callers do not invoke this
 * directly — enable() watches the manager and debounces a push on
 * every mutation. Exposed for tests and the forthcoming "sync now"
 * UI affordance.
 */
void gnostr_filter_set_sync_push_async(GCancellable *cancellable,
                                       GnostrFilterSetSyncCallback callback,
                                       gpointer user_data);

/**
 * gnostr_filter_set_sync_serialize:
 *
 * Build the JSON payload that push_async() would publish, using the
 * custom filter sets currently held by the default manager. Exposed
 * for testing.
 *
 * Returns: (transfer full) (nullable): JSON string, %NULL on error.
 */
gchar *gnostr_filter_set_sync_serialize(void);

/**
 * gnostr_filter_set_sync_apply:
 * @json: payload previously returned by serialize() or fetched from
 *   a relay (the kind-30078 event's content field)
 * @error: (out) (nullable): error location
 *
 * Merge @json's custom filter sets into the default manager. The
 * merge strategy is union-on-id: sets with ids that already exist
 * locally are replaced with the remote copy, sets that exist only
 * locally are kept untouched (deletions are not propagated — see
 * the header comment). Predefined entries are never modified.
 *
 * On success the default manager is persisted to disk.
 *
 * Returns: %TRUE on success.
 */
gboolean gnostr_filter_set_sync_apply(const gchar *json, GError **error);

G_END_DECLS

#endif /* GNOSTR_FILTER_SET_SYNC_H */
