/**
 * gnostr-testkit.h — Shared test infrastructure for GNostr projects
 *
 * Provides:
 *   - Temporary nostrdb instance management
 *   - Bulk event fixture generation
 *   - GLib main-loop test helpers
 *   - RSS memory measurement (Linux)
 *   - GLib log capture helpers
 *
 * Link against: gnostr-testkit (static library)
 * Dependencies: nostr-gobject, glib-2.0, gio-2.0
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GNOSTR_TESTKIT_H
#define GNOSTR_TESTKIT_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ════════════════════════════════════════════════════════════════════
 * Temporary NDB Instance
 * ════════════════════════════════════════════════════════════════════ */

/**
 * GnTestNdb:
 *
 * Manages a temporary nostrdb instance in a unique temp directory.
 * The database is initialized on creation and shut down on free.
 * Use this to get an isolated nostrdb for each test case.
 */
typedef struct _GnTestNdb GnTestNdb;

/**
 * gn_test_ndb_new:
 * @opts_json: (nullable): JSON options string for storage_ndb_init(),
 *   or %NULL for defaults (small mapsize suitable for testing).
 *
 * Creates a new temporary nostrdb instance.
 * The database directory is created under g_get_tmp_dir().
 *
 * Returns: (transfer full): a new #GnTestNdb, free with gn_test_ndb_free()
 */
GnTestNdb *gn_test_ndb_new(const char *opts_json);

/**
 * gn_test_ndb_get_dir:
 * @ndb: a #GnTestNdb
 *
 * Returns: (transfer none): the temporary directory path containing the database
 */
const char *gn_test_ndb_get_dir(GnTestNdb *ndb);

/**
 * gn_test_ndb_ingest_json:
 * @ndb: a #GnTestNdb
 * @json: a valid nostr event JSON string (must include "tags":[] field)
 *
 * Ingests a single event JSON string into the test database.
 *
 * Returns: %TRUE on success
 */
gboolean gn_test_ndb_ingest_json(GnTestNdb *ndb, const char *json);

/**
 * gn_test_ndb_free:
 * @ndb: (transfer full): a #GnTestNdb to free
 *
 * Shuts down the database and removes the temporary directory.
 */
void gn_test_ndb_free(GnTestNdb *ndb);

/* ════════════════════════════════════════════════════════════════════
 * Event Fixture Generation
 * ════════════════════════════════════════════════════════════════════ */

/**
 * gn_test_make_event_json:
 * @kind: event kind (e.g., 1 for text note, 0 for profile)
 * @content: event content string
 * @created_at: Unix timestamp for the event
 *
 * Generates a minimal valid nostr event JSON string with a random
 * pubkey and id. Suitable for ingestion testing.
 *
 * Returns: (transfer full): a JSON string, free with g_free()
 */
char *gn_test_make_event_json(int kind, const char *content, gint64 created_at);

/**
 * gn_test_make_events_bulk:
 * @n: number of events to generate
 * @kind: event kind for all events
 * @start_ts: starting Unix timestamp; each subsequent event is start_ts + i
 *
 * Generates an array of valid nostr event JSON strings.
 *
 * Returns: (transfer full) (element-type utf8): a #GPtrArray of JSON strings.
 *   Free with g_ptr_array_unref() (strings are owned by the array).
 */
GPtrArray *gn_test_make_events_bulk(guint n, int kind, gint64 start_ts);

/**
 * gn_test_make_event_json_with_pubkey:
 * @kind: event kind
 * @content: event content string
 * @created_at: Unix timestamp
 * @pubkey_hex: 64-character hex pubkey to use
 *
 * Like gn_test_make_event_json() but with a specific pubkey.
 *
 * Returns: (transfer full): a JSON string, free with g_free()
 */
char *gn_test_make_event_json_with_pubkey(int kind,
                                           const char *content,
                                           gint64 created_at,
                                           const char *pubkey_hex);

/* ════════════════════════════════════════════════════════════════════
 * Main Loop Helpers
 * ════════════════════════════════════════════════════════════════════ */

/**
 * GnTestPredicate:
 * @user_data: user data passed to gn_test_run_loop_until()
 *
 * Callback that returns %TRUE when the condition is met.
 */
typedef gboolean (*GnTestPredicate)(gpointer user_data);

/**
 * gn_test_run_loop_until:
 * @pred: predicate to check after each main loop iteration
 * @user_data: data to pass to @pred
 * @timeout_ms: maximum time to wait in milliseconds
 *
 * Runs the default GLib main loop, iterating until @pred returns %TRUE
 * or @timeout_ms elapses. Useful for async test assertions.
 *
 * Returns: %TRUE if @pred returned %TRUE before timeout, %FALSE on timeout
 */
gboolean gn_test_run_loop_until(GnTestPredicate pred,
                                 gpointer user_data,
                                 guint timeout_ms);

/**
 * gn_test_drain_main_loop:
 *
 * Iterates the default GLib main context until no pending dispatches remain.
 * Useful after triggering async operations to ensure all idle handlers,
 * timeouts, and signal emissions have been processed.
 */
void gn_test_drain_main_loop(void);

/* ════════════════════════════════════════════════════════════════════
 * Memory / Resource Measurement
 * ════════════════════════════════════════════════════════════════════ */

/**
 * gn_test_get_rss_bytes:
 *
 * Returns the current Resident Set Size (RSS) of this process in bytes.
 * On Linux, reads from /proc/self/status (VmRSS).
 * On other platforms, returns 0 (measurement not available).
 *
 * Returns: RSS in bytes, or 0 if not available
 */
gsize gn_test_get_rss_bytes(void);

/**
 * gn_test_get_rss_mb:
 *
 * Convenience wrapper returning RSS in megabytes.
 *
 * Returns: RSS in MB, or 0 if not available
 */
double gn_test_get_rss_mb(void);

/* ════════════════════════════════════════════════════════════════════
 * Object Lifecycle Helpers
 * ════════════════════════════════════════════════════════════════════ */

/**
 * GnTestPointerWatch:
 *
 * Tracks whether a GObject has been finalized using g_object_weak_ref().
 */
typedef struct {
    gboolean finalized;
    const char *label;
} GnTestPointerWatch;

/**
 * gn_test_watch_object:
 * @obj: a GObject to watch
 * @label: descriptive label for error messages
 *
 * Installs a weak-ref callback on @obj that sets watch->finalized = TRUE
 * when the object is finalized. Use to verify objects are properly freed.
 *
 * Returns: (transfer full): a #GnTestPointerWatch, free with g_free()
 */
GnTestPointerWatch *gn_test_watch_object(GObject *obj, const char *label);

/**
 * gn_test_assert_finalized:
 * @watch: a #GnTestPointerWatch from gn_test_watch_object()
 *
 * Asserts that the watched object has been finalized. If not, prints
 * the label and triggers a test failure.
 */
void gn_test_assert_finalized(GnTestPointerWatch *watch);

/**
 * gn_test_assert_not_finalized:
 * @watch: a #GnTestPointerWatch
 *
 * Asserts that the watched object has NOT been finalized.
 */
void gn_test_assert_not_finalized(GnTestPointerWatch *watch);

G_END_DECLS

#endif /* GNOSTR_TESTKIT_H */
