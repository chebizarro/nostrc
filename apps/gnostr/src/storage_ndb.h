#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thin facade over libnostr ln_store_* for NostrdB usage in gnostr. */

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

/* Queries */
int storage_ndb_query(void *txn, const char *filters_json, char ***out_arr, int *out_count);
int storage_ndb_text_search(void *txn, const char *q, const char *config_json, char ***out_arr, int *out_count);

/* Getters */
int storage_ndb_get_note_by_id(void *txn, const unsigned char id32[32], char **json_out, int *json_len);
int storage_ndb_get_profile_by_pubkey(void *txn, const unsigned char pk32[32], char **json_out, int *json_len);

/* Stats */
int storage_ndb_stat_json(char **json_out);

/* Free results helpers */
void storage_ndb_free_results(char **arr, int n);

#ifdef __cplusplus
}
#endif
