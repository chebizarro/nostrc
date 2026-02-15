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

#include <gio/gio.h>
#include <glib.h>

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
 * gn_test_ndb_wait_for_ingest:
 *
 * Waits for async NDB ingester threads to commit queued events to the database.
 * Call this after gn_test_ndb_ingest_json() and before querying to ensure
 * events are visible. Typically sleeps 50ms which is sufficient for small batches.
 */
void gn_test_ndb_wait_for_ingest(void);

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

/* ════════════════════════════════════════════════════════════════════
	* Main-Thread NDB Violation Detection Helpers
	*
	* Wraps the storage_ndb_testing_* API (which requires GNOSTR_TESTING
	* to be defined at storage_ndb.c compile time).  These helpers provide
	* a convenient test API plus diagnostic dump on failure.
	* ════════════════════════════════════════════════════════════════════ */

/**
	* gn_test_mark_main_thread:
	*
	* Mark the current thread as the GTK main thread for NDB violation
	* detection. Must be called from the main test thread (the one
	* that runs gtk_test_init / g_test_init). All subsequent NDB
	* transactions opened on this thread will be recorded as violations.
	*/
void gn_test_mark_main_thread(void);

/**
	* gn_test_clear_main_thread:
	*
	* Clear the main-thread marker. Call from test teardown.
	*/
void gn_test_clear_main_thread(void);

/**
	* gn_test_reset_ndb_violations:
	*
	* Reset the violation counter to zero. Call before each test.
	*/
void gn_test_reset_ndb_violations(void);

/**
	* gn_test_get_ndb_violation_count:
	*
	* Returns: the number of NDB transactions opened on the main thread
	* since the last reset.
	*/
unsigned gn_test_get_ndb_violation_count(void);

/**
	* gn_test_assert_no_ndb_violations:
	* @context: descriptive string for error messages (e.g., "during bind")
	*
	* Asserts that zero main-thread NDB violations occurred. On failure,
	* dumps the first N violation function names for diagnosis.
	*/
void gn_test_assert_no_ndb_violations(const char *context);

/* ════════════════════════════════════════════════════════════════════
	* Realistic Event Corpus Generation
	*
	* Generates events with varied, realistic content for integration
	* tests that exercise real code paths (Pango rendering, URL parsing,
	* NDB storage).
	* ════════════════════════════════════════════════════════════════════ */

/**
	* GnTestContentStyle:
	* @GN_TEST_CONTENT_SHORT: Short text (< 100 chars)
	* @GN_TEST_CONTENT_MEDIUM: Medium text with URLs and hashtags
	* @GN_TEST_CONTENT_LONG: Long multi-paragraph text
	* @GN_TEST_CONTENT_UNICODE: Unicode-heavy content (CJK, emoji, ZWS)
	* @GN_TEST_CONTENT_URLS: Multiple URLs and media links
	* @GN_TEST_CONTENT_MENTIONS: Content with nostr mentions (npub/note refs)
	* @GN_TEST_CONTENT_MIXED: Random mix of all styles
	*/
typedef enum {
	GN_TEST_CONTENT_SHORT    = 0,
	GN_TEST_CONTENT_MEDIUM   = 1,
	GN_TEST_CONTENT_LONG     = 2,
	GN_TEST_CONTENT_UNICODE  = 3,
	GN_TEST_CONTENT_URLS     = 4,
	GN_TEST_CONTENT_MENTIONS = 5,
	GN_TEST_CONTENT_MIXED    = 6,
} GnTestContentStyle;

/**
	* gn_test_make_realistic_event_json:
	* @kind: event kind (1 = text note, 0 = profile metadata)
	* @style: content style to generate
	* @created_at: Unix timestamp
	*
	* Generates a realistic nostr event JSON string with content that
	* exercises real parsing paths (URLs, hashtags, unicode, newlines).
	*
	* Returns: (transfer full): a JSON string, free with g_free()
	*/
char *gn_test_make_realistic_event_json(int kind,
											GnTestContentStyle style,
											gint64 created_at);

/**
	* gn_test_make_profile_event_json:
	* @pubkey_hex: 64-character hex pubkey
	* @name: display name
	* @about: (nullable): about text
	* @picture_url: (nullable): avatar URL
	* @created_at: Unix timestamp
	*
	* Generates a kind-0 profile metadata event for a given pubkey.
	*
	* Returns: (transfer full): a JSON string, free with g_free()
	*/
char *gn_test_make_profile_event_json(const char *pubkey_hex,
										const char *name,
										const char *about,
										const char *picture_url,
										gint64 created_at);

/**
	* gn_test_ingest_realistic_corpus:
	* @ndb: a #GnTestNdb
	* @n_events: number of kind-1 events to ingest
	* @n_profiles: number of kind-0 profiles to ingest (one per unique pubkey)
	*
	* Ingests a corpus of realistic events + matching profiles into the
	* test NDB. Events have varied content lengths and styles.
	* Profiles ensure that model readiness filters pass.
	*
	* Returns: (transfer full) (element-type utf8): array of pubkey hex
	* strings used. Free with g_ptr_array_unref().
	*/
GPtrArray *gn_test_ingest_realistic_corpus(GnTestNdb *ndb,
											guint n_events,
											guint n_profiles);

/* ════════════════════════════════════════════════════════════════════
	* Heartbeat (main-loop stall detection)
	* ════════════════════════════════════════════════════════════════════ */

/**
	* GnTestHeartbeat:
	*
	* Lightweight main-loop stall detector. Installs a periodic GSource
	* that tracks the gap between invocations. After the test, check
	* max_gap_us and missed_count.
	*/
typedef struct {
	guint  source_id;
	guint  interval_ms;
	guint  count;
	guint  missed_count;
	gint64 last_us;
	gint64 max_gap_us;
	gint64 max_stall_us;
} GnTestHeartbeat;

/**
	* gn_test_heartbeat_start:
	* @hb: heartbeat struct to initialize
	* @interval_ms: heartbeat interval (e.g. 5 for 5ms)
	* @max_stall_ms: stall threshold; gaps exceeding this count as "missed"
	*
	* Starts a heartbeat timer on the default main context.
	*/
void gn_test_heartbeat_start(GnTestHeartbeat *hb, guint interval_ms, guint max_stall_ms);

/**
	* gn_test_heartbeat_stop:
	* @hb: heartbeat to stop
	*
	* Stops the heartbeat timer and logs summary statistics.
	*/
void gn_test_heartbeat_stop(GnTestHeartbeat *hb);

G_END_DECLS

#endif /* GNOSTR_TESTKIT_H */
