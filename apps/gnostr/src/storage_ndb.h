#pragma once

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

/* Query transaction helpers */
int storage_ndb_begin_query(void **txn_out);
int storage_ndb_end_query(void *txn);

/* Convenience: begin a read query with bounded retries to tolerate transient contention.
 * Returns 0 on success and sets *txn_out, nonzero on failure. Attempts times with sleep_ms between. */
int storage_ndb_begin_query_retry(void **txn_out, int attempts, int sleep_ms);

/* Queries */
int storage_ndb_query(void *txn, const char *filters_json, char ***out_arr, int *out_count);
int storage_ndb_text_search(void *txn, const char *q, const char *config_json, char ***out_arr, int *out_count);

/* Getters */
int storage_ndb_get_note_by_id(void *txn, const unsigned char id32[32], char **json_out, int *json_len);
int storage_ndb_get_profile_by_pubkey(void *txn, const unsigned char pk32[32], char **json_out, int *json_len);

/* Convenience: fetch a note by hex id with internal begin/end and retries.
 * Returns 0 on success, nonzero on failure. Allocates *json_out owned by store (do not free). */
int storage_ndb_get_note_by_id_nontxn(const char *id_hex, char **json_out, int *json_len);

/* Convenience: fetch a note by key with internal transaction management.
 * Returns 0 on success, nonzero on failure. Caller must free *json_out. */
int storage_ndb_get_note_json_by_key(uint64_t note_key, char **json_out, int *json_len);

/* Stats */
int storage_ndb_stat_json(char **json_out);

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

/* ============== Contact List / Following API ============== */

/* nostrc-f0ll: Get followed pubkeys from a user's contact list (kind 3).
 * @user_pubkey_hex: 64-char hex pubkey of the user whose contact list to fetch
 * Returns NULL-terminated array of pubkey hex strings, or NULL if none/error.
 * Caller must g_strfreev() the result. */
char **storage_ndb_get_followed_pubkeys(const char *user_pubkey_hex);

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
GHashTable *storage_ndb_count_reactions_batch(const char * const *event_ids, guint n_ids);

/* Batch check if user has reacted to multiple events in a single query.
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of event IDs
 * user_pubkey_hex: 64-char hex pubkey of the user
 * Returns GHashTable mapping event_id_hex (owned) -> GINT_TO_POINTER(TRUE).
 * Only events the user HAS reacted to appear in the table.
 * Caller must g_hash_table_unref(). */
GHashTable *storage_ndb_user_has_reacted_batch(const char * const *event_ids, guint n_ids,
                                                const char *user_pubkey_hex);

/* Batch get zap stats for multiple events in a single query.
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of event IDs
 * Returns GHashTable mapping event_id_hex (owned) -> StorageNdbZapStats* (owned).
 * Only events with zaps appear in the table.
 * Caller must g_hash_table_unref(). */
GHashTable *storage_ndb_get_zap_stats_batch(const char * const *event_ids, guint n_ids);

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

#ifdef __cplusplus
}
#endif
