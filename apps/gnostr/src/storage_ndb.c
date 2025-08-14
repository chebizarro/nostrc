#include <stdlib.h>
#include <string.h>
#include "libnostr_store.h"
#include "storage_ndb.h"

static ln_store *g_store = NULL;

int storage_ndb_init(const char *dbdir, const char *opts_json)
{
  if (g_store) return 1; /* already initialized */
  ln_store *s = NULL;
  int rc = ln_store_open("nostrdb", dbdir ? dbdir : ".ndb-demo",
                         opts_json ? opts_json : "{\"mapsize\":1073741824,\"ingester_threads\":1}",
                         &s);
  if (rc != LN_OK) return 0;
  g_store = s;
  return 1;
}

void storage_ndb_shutdown(void)
{
  if (g_store) {
    ln_store_close(g_store);
    g_store = NULL;
  }
}

int storage_ndb_ingest_ldjson(const char *buf, size_t len)
{
  if (!g_store || !buf) return LN_ERR_INGEST;
  return ln_store_ingest_ldjson(g_store, buf, len, NULL);
}

int storage_ndb_ingest_event_json(const char *json, const char *relay_opt)
{
  if (!g_store || !json) return LN_ERR_INGEST;
  return ln_store_ingest_event_json(g_store, json, -1, relay_opt);
}

int storage_ndb_begin_query(void **txn_out)
{
  if (!g_store || !txn_out) return LN_ERR_DB_TXN;
  return ln_store_begin_query(g_store, txn_out);
}

int storage_ndb_end_query(void *txn)
{
  if (!g_store || !txn) return LN_ERR_DB_TXN;
  return ln_store_end_query(g_store, txn);
}

int storage_ndb_query(void *txn, const char *filters_json, char ***out_arr, int *out_count)
{
  if (!g_store || !txn || !filters_json || !out_arr || !out_count) return LN_ERR_QUERY;
  void *results = NULL; int count = 0;
  int rc = ln_store_query(g_store, txn, filters_json, &results, &count);
  if (rc != LN_OK) return rc;
  *out_arr = (char**)results; *out_count = count;
  return LN_OK;
}

int storage_ndb_text_search(void *txn, const char *q, const char *config_json, char ***out_arr, int *out_count)
{
  if (!g_store || !txn || !q || !out_arr || !out_count) return LN_ERR_TEXTSEARCH;
  void *results = NULL; int count = 0;
  int rc = ln_store_text_search(g_store, txn, q, config_json, &results, &count);
  if (rc != LN_OK) return rc;
  *out_arr = (char**)results; *out_count = count;
  return LN_OK;
}

int storage_ndb_get_note_by_id(void *txn, const unsigned char id32[32], char **json_out, int *json_len)
{
  if (!g_store || !txn || !id32 || !json_out) return LN_ERR_QUERY;
  const char *json = NULL; int len = 0;
  int rc = ln_store_get_note_by_id(g_store, txn, id32, &json, &len);
  if (rc != LN_OK) return rc;
  *json_out = (char*)json; if (json_len) *json_len = len;
  return LN_OK;
}

int storage_ndb_get_profile_by_pubkey(void *txn, const unsigned char pk32[32], char **json_out, int *json_len)
{
  if (!g_store || !txn || !pk32 || !json_out) return LN_ERR_QUERY;
  const char *json = NULL; int len = 0;
  int rc = ln_store_get_profile_by_pubkey(g_store, txn, pk32, &json, &len);
  if (rc != LN_OK) return rc;
  *json_out = (char*)json; if (json_len) *json_len = len;
  return LN_OK;
}

int storage_ndb_stat_json(char **json_out)
{
  if (!g_store || !json_out) return LN_ERR_QUERY;
  return ln_store_stat_json(g_store, json_out);
}

void storage_ndb_free_results(char **arr, int n)
{
  if (!arr) return; for (int i = 0; i < n; i++) free(arr[i]); free(arr);
}
