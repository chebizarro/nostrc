#ifndef LIBNOSTR_LIBNOSTR_STORE_H
#define LIBNOSTR_LIBNOSTR_STORE_H

#include "nostr-export.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "libnostr_errors.h"

/* Opaque handle */
typedef struct ln_store ln_store;

/* Compact query projection for consumers that only need stable note identity.
 * The result array is heap-allocated and owned by the caller. */
typedef struct ln_store_note_key_result {
  uint64_t note_key;
  uint64_t created_at;
} ln_store_note_key_result;

/* Operations vtable for storage backends */
typedef struct ln_store_ops {
  int  (*open)(ln_store **out, const char *path, const char *opts_json);
  void (*close)(ln_store *s);

  /* ingest */
  int  (*ingest_event_json)(ln_store *s, const char *json, int len, const char *relay_opt);
  int  (*ingest_ldjson)(ln_store *s, const char *ldjson, size_t len, const char *relay_opt);

  /* query lifecycle */
  int  (*begin_query)(ln_store *s, void **txn_out);
  int  (*end_query)(ln_store *s, void *txn);

  /* queries */
  int  (*query)(ln_store *s, void *txn, const char *filters_json, void **results, int *count);
  int  (*query_note_keys)(ln_store *s, void *txn, const char *filters_json, ln_store_note_key_result **results, int *count);
  int  (*text_search)(ln_store *s, void *txn, const char *query, const char *config_json, void **results, int *count);
  int  (*search_profile)(ln_store *s, void *txn, const char *query, int limit, void **results, int *count);

  /* helpers */
  int  (*get_note_by_id)(ln_store *s, void *txn, const unsigned char id[32], const char **json, int *json_len);
  int  (*get_profile_by_pubkey)(ln_store *s, void *txn, const unsigned char pk[32], const char **json, int *json_len);

  /* stats/maintenance */
  int  (*stat_json)(ln_store *s, char **json_out);
} ln_store_ops;

/* Factory API */
NOSTR_API int ln_store_open(const char *backend, const char *path, const char *opts_json, ln_store **out);
NOSTR_API void ln_store_close(ln_store *s);

/* Convenience wrappers */
NOSTR_API int ln_store_ingest_event_json(ln_store *s, const char *json, int len, const char *relay);
NOSTR_API int ln_store_ingest_ldjson(ln_store *s, const char *ldjson, size_t len, const char *relay);
NOSTR_API int ln_store_begin_query(ln_store *s, void **txn);
NOSTR_API int ln_store_end_query(ln_store *s, void *txn);
NOSTR_API int ln_store_query(ln_store *s, void *txn, const char *filters_json, void **results, int *count);
NOSTR_API int ln_store_query_note_keys(ln_store *s, void *txn, const char *filters_json, ln_store_note_key_result **results, int *count);
NOSTR_API int ln_store_text_search(ln_store *s, void *txn, const char *query, const char *config_json, void **results, int *count);
NOSTR_API int ln_store_search_profile(ln_store *s, void *txn, const char *query, int limit, void **results, int *count);
NOSTR_API int ln_store_get_note_by_id(ln_store *s, void *txn, const unsigned char id[32], const char **json, int *json_len);
NOSTR_API int ln_store_get_profile_by_pubkey(ln_store *s, void *txn, const unsigned char pk[32], const char **json, int *json_len);
NOSTR_API int ln_store_stat_json(ln_store *s, char **json_out);

/* Get raw backend handle for direct access (nostrdb only).
 * Returns NULL for non-nostrdb backends. Caller must cast appropriately. */
NOSTR_API void *ln_store_get_backend_handle(ln_store *s);

/* Register a subscription notification callback for the nostrdb backend.
 *
 * The callback is passed to nostrdb via ndb_config.sub_cb when the next
 * ln_ndb_open() runs. Higher layers (e.g. nostr-gobject storage_ndb)
 * register their notifier here before opening the store; this avoids
 * libnostr making an upward extern reference to the wrapping layer, which
 * breaks shared-library link order.
 *
 * Passing fn=NULL clears the callback. Thread-safety: the setter is a
 * plain store; call it before ln_ndb_open() from a single-threaded
 * initialization path. */
typedef void (*ln_ndb_sub_notify_fn)(void *ctx, uint64_t subid);
NOSTR_API void ln_ndb_set_sub_callback(ln_ndb_sub_notify_fn fn, void *ctx);

#ifdef LIBNOSTR_WITH_NOSTRDB
/* Invalidate idle nostrdb read transactions cached by the calling thread.
 * Active queries remain valid. Used by consumers before a fresh read. */
NOSTR_API void ln_ndb_invalidate_txn_cache_ext(void);
/* End every cached transaction on the calling thread, even active ones.
 * Shutdown only: no query/result on this thread may be used afterwards.
 * Call before destroying the backend; this does not flush other threads. */
NOSTR_API void ln_ndb_force_close_txn_cache(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* LIBNOSTR_LIBNOSTR_STORE_H */
