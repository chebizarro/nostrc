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

#ifdef __cplusplus
}
#endif
