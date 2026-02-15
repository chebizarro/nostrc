#ifndef NOSTR_GOBJECT_STORAGE_NDB_H
#define NOSTR_GOBJECT_STORAGE_NDB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thin facade over libnostr ln_store_* for NostrdB usage in gnostr. */

/* Opaque note pointer - actually struct ndb_note* from nostrdb */
typedef struct ndb_note storage_ndb_note;

/* Subscription callback - called from writer thread when notes match subscription */
typedef void (*storage_ndb_notify_fn)(void *ctx, uint64_t subid);

/* Initialize the store. Returns 1 on success, 0 on failure. */
int storage_ndb_init(const char *dbdir, const char *opts_json);

/* Shutdown and free resources. Safe to call multiple times. */
void storage_ndb_shutdown(void);

/* Ingest APIs */
int storage_ndb_ingest_ldjson(const char *buf, size_t len);
int storage_ndb_ingest_event_json(const char *json, const char *relay_opt);

/* Synchronous event ingestion for tests. Uses ndb_process_event() directly,
 * bypassing async ingester threads. Events are immediately queryable after return.
 * Returns 0 on success, nonzero on failure. */
int storage_ndb_ingest_event_json_sync(const char *json);

/* nostrc-mzab: Async batch ingestion — runs storage_ndb_ingest_event_json for
 * each JSON string in a GLib worker thread. Takes ownership of the GPtrArray
 * (must have g_free as element free func). Use this instead of calling
 * storage_ndb_ingest_event_json in a loop on the main thread. */
void storage_ndb_ingest_events_async(GPtrArray *jsons);

/* Query transaction helpers */
int storage_ndb_begin_query(void **txn_out);
int storage_ndb_end_query(void *txn);

/* Convenience: begin a read query with bounded retries to tolerate transient contention.
 * Returns 0 on success and sets *txn_out, nonzero on failure. Attempts times with sleep_ms between. */
int storage_ndb_begin_query_retry(void **txn_out, int attempts, int sleep_ms);

/* Queries */
int storage_ndb_query(void *txn, const char *filters_json, char ***out_arr, int *out_count);
int storage_ndb_text_search(void *txn, const char *q, const char *config_json, char ***out_arr, int *out_count);
int storage_ndb_search_profile(void *txn, const char *query, int limit, char ***out_arr, int *out_count);

/* Getters */
int storage_ndb_get_note_by_id(void *txn, const unsigned char id32[32], char **json_out, int *json_len);
int storage_ndb_get_profile_by_pubkey(void *txn, const unsigned char pk32[32], char **json_out, int *json_len);

/* ============== Direct Profile FlatBuffer API (hq-cgnhh) ============== */

/* Profile fields extracted directly from NdbProfile FlatBuffer, skipping
 * the JSON round-trip (FlatBuffer -> JSON -> struct).
 * All string fields are g_strdup'd copies; caller must g_free() each. */
typedef struct {
    char *name;		 /* nullable */
    char *display_name;	 /* nullable */
    char *picture;	 /* nullable */
    char *banner;	 /* nullable */
    char *nip05;	 /* nullable */
    char *lud16;	 /* nullable */
    char *about;	 /* nullable */
    char *website;	 /* nullable */
    char *lud06;	 /* nullable */
    uint32_t created_at; /* from the associated kind:0 note, 0 if unavailable */
} StorageNdbProfileMeta;

/* Read profile fields directly from the NdbProfile FlatBuffer record.
 * This avoids the wasteful FlatBuffer -> JSON -> struct round-trip.
 * Returns 0 on success and populates *out. Caller must call
 * storage_ndb_profile_meta_clear() to free string fields.
 * Returns nonzero on failure (not found, txn error, etc.). */
int storage_ndb_get_profile_meta_direct(void *txn,
					const unsigned char pk32[32],
					StorageNdbProfileMeta *out);

/* Free the string fields inside a StorageNdbProfileMeta.
 * Does NOT free the struct itself (it is typically stack-allocated). */
void storage_ndb_profile_meta_clear(StorageNdbProfileMeta *meta);

/* Convenience: fetch a note by hex id with internal begin/end and retries.
 * Returns 0 on success, nonzero on failure. Allocates *json_out owned by store (do not free). */
int storage_ndb_get_note_by_id_nontxn(const char *id_hex, char **json_out, int *json_len);

/* Convenience: fetch a note by key with internal transaction management.
 * Returns 0 on success, nonzero on failure. Caller must free *json_out. */
int storage_ndb_get_note_json_by_key(uint64_t note_key, char **json_out, int *json_len);

/* Stats */
int storage_ndb_stat_json(char **json_out);

/* Structured NDB statistics (nostrc-o6w) */
typedef struct {
    size_t note_count;	  /* NDB_DB_NOTE entries */
    size_t profile_count; /* NDB_DB_PROFILE entries */
    size_t total_bytes;	  /* key_size + value_size across all DBs */
    /* Per-kind note counts */
    size_t kind_text;	  /* kind 1 */
    size_t kind_contacts; /* kind 3 */
    size_t kind_dm;	  /* kind 4 */
    size_t kind_repost;	  /* kind 6 */
    size_t kind_reaction; /* kind 7 */
    size_t kind_zap;	  /* kind 9735 */
} StorageNdbStat;

/* Populate structured statistics from NDB. Returns 0 on success. */
int storage_ndb_get_stat(StorageNdbStat *out);

/* Update metrics gauges from current NDB statistics.
 * Call periodically (e.g. from dashboard refresh timer). */
void storage_ndb_update_metrics(void);

/* Diagnostic counters */
uint64_t storage_ndb_get_ingest_count(void);
uint64_t storage_ndb_get_ingest_bytes(void);

/* Free results helpers */
void storage_ndb_free_results(char **arr, int n);

/* ============== Subscription API ============== */

/* Set global subscription notification callback.
 * Called from nostrdb writer thread when new notes match a subscription.
 * Use g_idle_add() to marshal to GTK main loop.
 * IMPORTANT: Must be called BEFORE storage_ndb_init() for callback to take effect. */
void storage_ndb_set_notify_callback(storage_ndb_notify_fn fn, void *ctx);

/* Get the registered subscription callback (for use by ndb_backend at init time). */
void storage_ndb_get_notify_callback(storage_ndb_notify_fn *fn_out, void **ctx_out);

/* Subscribe to notes matching filter. Returns subscription ID (>0) or 0 on failure.
 * filter_json is a NIP-01 filter object, e.g. {"kinds":[1],"limit":100} */
uint64_t storage_ndb_subscribe(const char *filter_json);

/* Unsubscribe from a subscription. */
void storage_ndb_unsubscribe(uint64_t subid);

/* Poll for new note keys matching subscription. Non-blocking.
 * Returns number of keys written to array (0 if none available). */
int storage_ndb_poll_notes(uint64_t subid, uint64_t *note_keys, int capacity);

/* Invalidate the thread-local transaction cache so next begin_query gets fresh data.
 * Call this before processing subscription callbacks to ensure newly committed
 * notes are visible. */
void storage_ndb_invalidate_txn_cache(void);

/* ============== Direct Note Access API ============== */

/* Get raw note pointer by key. Valid only while txn is open.
 * Returns NULL if not found. DO NOT free the returned pointer. */
storage_ndb_note *storage_ndb_get_note_ptr(void *txn, uint64_t note_key);

/* Get note key from note ID (32-byte binary). Returns key or 0 if not found.
 * Also returns note pointer if note_out is not NULL. */
uint64_t storage_ndb_get_note_key_by_id(void *txn, const unsigned char id32[32],
					storage_ndb_note **note_out);

/* Direct note accessors - work on storage_ndb_note pointer while txn is open */
const unsigned char *storage_ndb_note_id(storage_ndb_note *note);
const unsigned char *storage_ndb_note_pubkey(storage_ndb_note *note);
const char *storage_ndb_note_content(storage_ndb_note *note);
uint32_t storage_ndb_note_content_length(storage_ndb_note *note);
uint32_t storage_ndb_note_created_at(storage_ndb_note *note);
uint32_t storage_ndb_note_kind(storage_ndb_note *note);

/* Convert 32-byte binary to hex string. Caller provides 65-byte buffer. */
void storage_ndb_hex_encode(const unsigned char *bin32, char *hex65);

/* Serialize note tags to JSON array string (for NIP-92 imeta parsing).
 * Returns newly allocated JSON string or NULL if no tags.
 * Caller must g_free() the result. */
char *storage_ndb_note_tags_json(storage_ndb_note *note);

/* Extract hashtags ("t" tags) from note.
 * Returns NULL-terminated array of hashtag strings, or NULL if none.
 * Caller must g_strfreev() the result. */
char **storage_ndb_note_get_hashtags(storage_ndb_note *note);

/* nostrc-57j: Get relay URLs that a note was seen on.
 * Returns NULL-terminated array of relay URL strings, or NULL if none.
 * Caller must g_strfreev() the result. txn must be open. */
char **storage_ndb_note_get_relays(void *txn, uint64_t note_key);

/* ============== Profile Fetch Staleness API (hq-xxnm5) ============== */

/* Default staleness threshold in seconds (1 hour).
 * Profiles fetched more recently than this are considered fresh. */
#define STORAGE_NDB_PROFILE_STALE_SECS 3600

/* Write the timestamp of when a profile was last fetched from relays.
 * pubkey: 32-byte binary pubkey.
 * fetched_at: Unix timestamp (seconds since epoch).
 * Returns 1 on success, 0 on failure. */
int storage_ndb_write_last_profile_fetch(const unsigned char *pubkey, uint64_t fetched_at);

/* Read the timestamp of when a profile was last fetched from relays.
 * txn: open read transaction (struct ndb_txn* cast to void*).
 * pubkey: 32-byte binary pubkey.
 * Returns the Unix timestamp, or 0 if never fetched. */
uint64_t storage_ndb_read_last_profile_fetch(void *txn, const unsigned char *pubkey);

/* Check if a profile is stale (needs re-fetching from relays).
 * pubkey_hex: 64-char hex pubkey string.
 * stale_secs: staleness threshold in seconds (0 = use default).
 * Returns TRUE if the profile has never been fetched or was fetched
 * longer ago than stale_secs. Returns FALSE if recently fetched.
 * Manages its own read transaction internally. */
gboolean storage_ndb_is_profile_stale(const char *pubkey_hex, uint64_t stale_secs);

/* ============== Contact List / Following API ============== */

/* nostrc-f0ll: Get followed pubkeys from a user's contact list (kind 3).
 * @user_pubkey_hex: 64-char hex pubkey of the user whose contact list to fetch
 * Returns NULL-terminated array of pubkey hex strings, or NULL if none/error.
 * Caller must g_strfreev() the result. */
char **storage_ndb_get_followed_pubkeys(const char *user_pubkey_hex);

/* ============== Note Metadata API (hq-vvmzu) ============== */

/* Per-note count structure for reading/writing ndb_note_meta. */
typedef struct {
    guint total_reactions; /* Total reaction count */
    guint direct_replies;  /* Direct reply count */
    guint thread_replies;  /* Thread reply count (includes nested) */
    guint reposts;	   /* Repost count */
    guint quotes;	   /* Quote count */
} StorageNdbNoteCounts;

/* Read all pre-computed counts for a note from ndb_note_meta in one call.
 * id32: 32-byte binary note ID. txn must be open.
 * Populates out struct; zeroes it first. Returns TRUE if metadata was found. */
gboolean storage_ndb_read_note_counts(void *txn, const unsigned char id32[32],
				      StorageNdbNoteCounts *out);

/* Write/update note metadata counts via ndb_set_note_meta.
 * id32: 32-byte binary note ID.
 * counts: the count values to store.
 * Returns 0 on success, nonzero on failure. */
int storage_ndb_write_note_counts(const unsigned char id32[32],
				  const StorageNdbNoteCounts *counts);

/* Increment a specific count field for a note in ndb_note_meta.
 * Reads existing metadata, increments the field, and writes back.
 * field: "reactions", "direct_replies", "thread_replies", "reposts", "quotes"
 * id32: 32-byte binary note ID.
 * Returns 0 on success, nonzero on failure. */
int storage_ndb_increment_note_meta(const unsigned char id32[32], const char *field);

/* Batch count replies for multiple events using ndb_note_meta.
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of event IDs
 * Returns GHashTable mapping event_id_hex (owned) -> GUINT_TO_POINTER(count).
 * Uses direct_replies from ndb_note_meta for O(1) per lookup.
 * Only events with count > 0 appear in the table.
 * Caller must g_hash_table_unref(). */
GHashTable *storage_ndb_count_replies_batch(const char *const *event_ids, guint n_ids);

/* ============== NIP-25 Reaction Count API ============== */

/* Count reactions (kind 7) for a given event.
 * event_id_hex is the 64-char hex ID of the event to count reactions for.
 * Returns the number of reactions found, or 0 if none/error. */
guint storage_ndb_count_reactions(const char *event_id_hex);

/* Check if a specific user has reacted to an event.
 * event_id_hex: 64-char hex ID of the event
 * user_pubkey_hex: 64-char hex pubkey of the user
 * Returns TRUE if the user has reacted, FALSE otherwise. */
gboolean storage_ndb_user_has_reacted(const char *event_id_hex, const char *user_pubkey_hex);

/* NIP-25: Get reaction breakdown for an event (emoji -> count).
 * event_id_hex: 64-char hex ID of the event
 * reactor_pubkeys: (out) (optional): array of reactor pubkeys, caller must free
 * Returns a GHashTable with emoji strings as keys and count as GUINT_TO_POINTER values.
 * Caller must free the returned hash table with g_hash_table_unref(). */
GHashTable *storage_ndb_get_reaction_breakdown(const char *event_id_hex, GPtrArray **reactor_pubkeys);

/* ============== NIP-57 Zap Stats API ============== */

/* Count zap receipts (kind 9735) for a given event.
 * event_id_hex is the 64-char hex ID of the event to count zaps for.
 * Returns the number of zaps found, or 0 if none/error. */
guint storage_ndb_count_zaps(const char *event_id_hex);

/* Get zap statistics for an event.
 * event_id_hex: 64-char hex ID of the event
 * zap_count: (out) number of zaps received
 * total_msat: (out) total amount in millisatoshis
 * Returns TRUE on success, FALSE on error. */
gboolean storage_ndb_get_zap_stats(const char *event_id_hex, guint *zap_count, gint64 *total_msat);

/* ============== Batch Reaction/Zap API (nostrc-qff) ============== */

/* Result struct for batch zap stats */
typedef struct {
    guint zap_count;
    gint64 total_msat;
} StorageNdbZapStats;

/* Batch count reactions (kind 7) for multiple events in a single query.
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of event IDs
 * Returns GHashTable mapping event_id_hex (owned) -> GUINT_TO_POINTER(count).
 * Only events with count > 0 appear in the table.
 * Caller must g_hash_table_unref(). */
GHashTable *storage_ndb_count_reactions_batch(const char *const *event_ids, guint n_ids);

/* Batch count reposts for multiple events using ndb_note_meta.
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of event IDs
 * Returns GHashTable mapping event_id_hex (owned) -> GUINT_TO_POINTER(count).
 * Only events with count > 0 appear in the table.
 * Caller must g_hash_table_unref(). */
GHashTable *storage_ndb_count_reposts_batch(const char *const *event_ids, guint n_ids);

/* Batch check if user has reacted to multiple events in a single query.
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of event IDs
 * user_pubkey_hex: 64-char hex pubkey of the user
 * Returns GHashTable mapping event_id_hex (owned) -> GINT_TO_POINTER(TRUE).
 * Only events the user HAS reacted to appear in the table.
 * Caller must g_hash_table_unref(). */
GHashTable *storage_ndb_user_has_reacted_batch(const char *const *event_ids, guint n_ids,
					       const char *user_pubkey_hex);

/* Batch get zap stats for multiple events in a single query.
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of event IDs
 * Returns GHashTable mapping event_id_hex (owned) -> StorageNdbZapStats* (owned).
 * Only events with zaps appear in the table.
 * Caller must g_hash_table_unref(). */
GHashTable *storage_ndb_get_zap_stats_batch(const char *const *event_ids, guint n_ids);

/* ============== NIP-40 Expiration Timestamp API ============== */

/* Get expiration timestamp from note tags.
 * Returns 0 if no expiration tag is present, otherwise returns the Unix timestamp. */
gint64 storage_ndb_note_get_expiration(storage_ndb_note *note);

/* Check if an event is expired (NIP-40).
 * Returns TRUE if the event has an expiration tag and the timestamp has passed.
 * Returns FALSE if no expiration tag or not yet expired. */
gboolean storage_ndb_note_is_expired(storage_ndb_note *note);

/* Check if an event is expired given its note_key.
 * Convenience function that handles transaction management internally.
 * Returns TRUE if expired, FALSE otherwise. */
gboolean storage_ndb_is_event_expired(uint64_t note_key);

/* ============== NIP-10 Thread Info API ============== */

/* Extract NIP-10 thread context (root_id, reply_id) from note tags.
 * Supports both preferred marker style and positional fallback.
 * Returns allocated strings via out parameters. Caller must g_free().
 * Pass NULL for outputs you don't need. */
void storage_ndb_note_get_nip10_thread(storage_ndb_note *note, char **root_id_out, char **reply_id_out);

/* Extract NIP-10 thread context with relay hints from note tags.
 * nostrc-7r5: Extended version that also extracts relay hints from e-tags.
 * Relay hints indicate which relay to query for the referenced event.
 * Returns allocated strings via out parameters. Caller must g_free().
 * Pass NULL for outputs you don't need. */
void storage_ndb_note_get_nip10_thread_full(storage_ndb_note *note,
					    char **root_id_out,
					    char **reply_id_out,
					    char **root_relay_hint_out,
					    char **reply_relay_hint_out);

/* Get the last "e" tag from a note via direct tag iteration (no NDB query).
 * Useful for reactions (kind 7) and zaps (kind 9735).
 * Returns g_strdup'd hex string, or NULL. Caller must g_free(). */
char *storage_ndb_note_get_last_etag(storage_ndb_note *note);

/* ============== Content Blocks API ============== */

/* Opaque blocks handle - actually struct ndb_blocks* from nostrdb */
typedef struct ndb_blocks storage_ndb_blocks;

/* Get pre-parsed content blocks from NDB_DB_NOTE_BLOCKS.
 * Returns NULL if blocks not available. Caller must call storage_ndb_blocks_free().
 * The blocks reference internal DB memory valid while txn is open. */
storage_ndb_blocks *storage_ndb_get_blocks(void *txn, uint64_t note_key);

/* Parse content on-the-fly into blocks (fallback when no note_key available).
 * Returns NULL on failure. Caller must call storage_ndb_blocks_free(). */
storage_ndb_blocks *storage_ndb_parse_content_blocks(const char *content, int content_len);

/* Free blocks. Safe to call on NULL or DB-owned blocks. */
void storage_ndb_blocks_free(storage_ndb_blocks *blocks);

/* ============== Query Cursor API (nostrc-tbv) ============== */

/* Opaque cursor for streaming NDB query results in pages. */
typedef struct _StorageNdbCursor StorageNdbCursor;

/* Result entry from cursor iteration. */
typedef struct {
    uint64_t note_key;
    uint32_t created_at;
} StorageNdbCursorEntry;

/* Create a new cursor for paginated query iteration.
 * filter_json: NIP-01 filter object (kinds, authors, etc.).
 *   The cursor manages "until" and "limit" internally for pagination.
 * batch_size: Number of results per page (e.g., 50).
 * Returns cursor or NULL on failure. Caller must call storage_ndb_cursor_free(). */
StorageNdbCursor *storage_ndb_cursor_new(const char *filter_json, guint batch_size);

/* Fetch the next batch of results.
 * Manages its own read transaction internally.
 * entries_out: (out) array of results (owned by cursor, valid until next call or free)
 * count_out: (out) number of entries in this batch (0 when exhausted)
 * Returns 0 on success, nonzero on failure. */
int storage_ndb_cursor_next(StorageNdbCursor *cursor,
			    const StorageNdbCursorEntry **entries_out,
			    guint *count_out);

/* Check if cursor has more results.
 * Returns TRUE if there may be more results, FALSE if exhausted. */
gboolean storage_ndb_cursor_has_more(StorageNdbCursor *cursor);

/* Get total items fetched so far across all batches. */
guint storage_ndb_cursor_total_fetched(StorageNdbCursor *cursor);

/* Free cursor and all resources. Safe to call on NULL. */
void storage_ndb_cursor_free(StorageNdbCursor *cursor);

/* ============== Test Instrumentation API (GNOSTR_TESTING) ============== */

#ifdef GNOSTR_TESTING

/**
	* storage_ndb_testing_mark_main_thread:
	*
	* Mark the current thread as the GLib/GTK main thread.
	* After calling this, any NDB transaction opened on this thread
	* will be recorded as a violation. Call from test setUp.
	*/
void storage_ndb_testing_mark_main_thread(void);

/**
	* storage_ndb_testing_clear_main_thread:
	*
	* Clear the main-thread marker. Call from test tearDown.
	*/
void storage_ndb_testing_clear_main_thread(void);

/**
	* storage_ndb_testing_get_violation_count:
	*
	* Returns: the total number of main-thread NDB transaction violations
	* since the last reset. Each call to storage_ndb_begin_query() or
	* storage_ndb_begin_query_retry() on the marked main thread increments
	* this counter.
	*/
unsigned storage_ndb_testing_get_violation_count(void);

/**
	* storage_ndb_testing_reset_violations:
	*
	* Reset the violation counter and log to zero. Call before each test.
	*/
void storage_ndb_testing_reset_violations(void);

/**
	* storage_ndb_testing_get_violation_func:
	* @index: violation index (0-based, wraps at 256)
	*
	* Returns: (nullable): the function name that caused the violation,
	* or NULL if the index is out of range. Useful for diagnostic output.
	*/
const char *storage_ndb_testing_get_violation_func(unsigned index);

#endif /* GNOSTR_TESTING */

#ifdef __cplusplus
}
#endif
#endif /* NOSTR_GOBJECT_STORAGE_NDB_H */
