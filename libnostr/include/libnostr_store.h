#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "libnostr_errors.h"

/* Opaque handle */
typedef struct ln_store ln_store;

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
  int  (*text_search)(ln_store *s, void *txn, const char *query, const char *config_json, void **results, int *count);

  /* helpers */
  int  (*get_note_by_id)(ln_store *s, void *txn, const unsigned char id[32], const char **json, int *json_len);
  int  (*get_profile_by_pubkey)(ln_store *s, void *txn, const unsigned char pk[32], const char **json, int *json_len);

  /* stats/maintenance */
  int  (*stat_json)(ln_store *s, char **json_out);
} ln_store_ops;

/* Factory API */
int ln_store_open(const char *backend, const char *path, const char *opts_json, ln_store **out);
void ln_store_close(ln_store *s);

/* Convenience wrappers */
int ln_store_ingest_event_json(ln_store *s, const char *json, int len, const char *relay);
int ln_store_ingest_ldjson(ln_store *s, const char *ldjson, size_t len, const char *relay);
int ln_store_begin_query(ln_store *s, void **txn);
int ln_store_end_query(ln_store *s, void *txn);
int ln_store_query(ln_store *s, void *txn, const char *filters_json, void **results, int *count);
int ln_store_text_search(ln_store *s, void *txn, const char *query, const char *config_json, void **results, int *count);
int ln_store_get_note_by_id(ln_store *s, void *txn, const unsigned char id[32], const char **json, int *json_len);
int ln_store_get_profile_by_pubkey(ln_store *s, void *txn, const unsigned char pk[32], const char **json, int *json_len);
int ln_store_stat_json(ln_store *s, char **json_out);

#ifdef __cplusplus
}
#endif
