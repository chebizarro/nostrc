#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libnostr_store.h"
#include "storage_ndb.h"

static ln_store *g_store = NULL;

int storage_ndb_init(const char *dbdir, const char *opts_json)
{
  fprintf(stderr, "[storage_ndb_init] ENTER: dbdir=%s opts=%s\n", dbdir, opts_json);
  fflush(stderr);
  if (g_store) {
    fprintf(stderr, "[storage_ndb_init] Already initialized, returning success\n");
    fflush(stderr);
    return 1; /* already initialized */
  }
  ln_store *s = NULL;
  fprintf(stderr, "[storage_ndb_init] Calling ln_store_open...\n");
  fflush(stderr);
  int rc = ln_store_open("nostrdb", dbdir ? dbdir : ".ndb-demo",
                         opts_json ? opts_json : "{\"mapsize\":1073741824,\"ingester_threads\":1,\"ingest_skip_validation\":1}",
                         &s);
  fprintf(stderr, "[storage_ndb_init] ln_store_open returned rc=%d (LN_OK=%d)\n", rc, LN_OK);
  fflush(stderr);
  if (rc != LN_OK) return 0;
  g_store = s;
  fprintf(stderr, "[storage_ndb_init] SUCCESS, g_store=%p\n", (void*)g_store);
  fflush(stderr);
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
  if (!g_store) {
    fprintf(stderr, "[storage_ndb_ingest_ldjson] ERROR: g_store is NULL!\n");
    fflush(stderr);
    return LN_ERR_INGEST;
  }
  if (!buf) {
    fprintf(stderr, "[storage_ndb_ingest_ldjson] ERROR: buf is NULL!\n");
    fflush(stderr);
    return LN_ERR_INGEST;
  }
  int rc = ln_store_ingest_ldjson(g_store, buf, len, NULL);
  if (rc != 0) {
    fprintf(stderr, "[storage_ndb_ingest_ldjson] ln_store_ingest_ldjson returned rc=%d for %.80s\n", rc, buf);
    fflush(stderr);
  }
  return rc;
}

/* Helper: Add "tags":[] if missing from JSON.
 * Many relays omit the tags field, but nostrdb requires it.
 * Returns newly allocated string that caller must free, or NULL on error. */
static char *ensure_tags_field(const char *json)
{
  if (!json) return NULL;
  
  /* If tags field already exists, just duplicate */
  if (strstr(json, "\"tags\"")) {
    return strdup(json);
  }
  
  /* Find insertion point after "kind" field */
  const char *kind_pos = strstr(json, "\"kind\"");
  if (!kind_pos) {
    /* No kind field? Just duplicate as-is */
    return strdup(json);
  }
  
  /* Find the comma after the kind value */
  const char *comma_after_kind = strchr(kind_pos, ',');
  if (!comma_after_kind) {
    /* No comma after kind? Just duplicate as-is */
    return strdup(json);
  }
  
  /* Build new JSON with tags inserted */
  size_t prefix_len = comma_after_kind - json + 1;
  size_t suffix_len = strlen(comma_after_kind + 1);
  const char *tags_field = "\"tags\":[],";
  size_t tags_len = strlen(tags_field);
  
  char *result = malloc(prefix_len + tags_len + suffix_len + 1);
  if (!result) return NULL;
  
  memcpy(result, json, prefix_len);
  memcpy(result + prefix_len, tags_field, tags_len);
  memcpy(result + prefix_len + tags_len, comma_after_kind + 1, suffix_len);
  result[prefix_len + tags_len + suffix_len] = '\0';
  
  return result;
}

int storage_ndb_ingest_event_json(const char *json, const char *relay_opt)
{
  if (!g_store || !json) return LN_ERR_INGEST;
  
  /* CRITICAL FIX: Add tags field if missing */
  char *fixed_json = ensure_tags_field(json);
  if (!fixed_json) return LN_ERR_INGEST;
  
  /* Ensure explicit length; some backends may not accept -1 */
  size_t len = strlen(fixed_json);
  int rc = ln_store_ingest_event_json(g_store, fixed_json, (int)len, relay_opt);
  
  free(fixed_json);
  return rc;
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
