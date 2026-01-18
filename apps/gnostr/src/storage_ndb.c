#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include "libnostr_store.h"
#include "storage_ndb.h"

/* Direct nostrdb access for subscription API */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wpedantic"
#  pragma clang diagnostic ignored "-Wzero-length-array"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "nostrdb.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* Backend impl structure from ndb_backend.h */
struct ln_ndb_impl {
  void *db;
};

static ln_store *g_store = NULL;

/* Get raw nostrdb handle from our store */
static struct ndb *get_ndb(void)
{
  if (!g_store) return NULL;
  struct ln_ndb_impl *impl = (struct ln_ndb_impl *)ln_store_get_backend_handle(g_store);
  return impl ? (struct ndb *)impl->db : NULL;
}

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

  /* Log ingest failures only */
  if (rc != 0) {
    fprintf(stderr, "[storage_ndb_ingest] FAILED rc=%d len=%zu json_head=%.100s\n", rc, len, fixed_json);
    fflush(stderr);
  }

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

int storage_ndb_begin_query_retry(void **txn_out, int attempts, int sleep_ms)
{
  if (!txn_out) return LN_ERR_DB_TXN;
  int rc = 0;
  void *txn = NULL;
  if (attempts <= 0) attempts = 1;
  if (sleep_ms <= 0) sleep_ms = 1;
  for (int i = 0; i < attempts; i++) {
    rc = storage_ndb_begin_query(&txn);
    if (rc == 0 && txn) { *txn_out = txn; return 0; }
    /* Exponential backoff capped at ~512ms between attempts */
    int backoff_ms = sleep_ms << (i / 50); /* increase every 50 attempts */
    if (backoff_ms > 512) backoff_ms = 512;
    usleep((useconds_t)backoff_ms * 1000);
  }
  *txn_out = NULL;
  return rc != 0 ? rc : LN_ERR_DB_TXN;
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

/* Convenience: fetch a note by hex id with internal transaction and retries. */
int storage_ndb_get_note_by_id_nontxn(const char *id_hex, char **json_out, int *json_len)
{
  if (!id_hex || !json_out) return LN_ERR_QUERY;

  /* Convert hex to binary */
  unsigned char id32[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(id_hex + i*2, "%2x", &byte) != 1) return LN_ERR_QUERY;
    id32[i] = (unsigned char)byte;
  }

  /* Try to get note with internal transaction management */
  void *txn = NULL;
  int rc = storage_ndb_begin_query(&txn);

  if (rc != 0 || !txn) {
    return rc != 0 ? rc : LN_ERR_DB_TXN;
  }

  char *json_ptr = NULL;
  rc = storage_ndb_get_note_by_id(txn, id32, &json_ptr, json_len);

  if (rc == 0 && json_ptr && json_len && *json_len > 0) {
    /* Copy the JSON before ending transaction */
    *json_out = malloc(*json_len + 1);
    if (*json_out) {
      memcpy(*json_out, json_ptr, *json_len);
      (*json_out)[*json_len] = '\0';
    }
  }

  storage_ndb_end_query(txn);
  return rc;
}

/* ============== Subscription API Implementation ============== */

void storage_ndb_set_notify_callback(storage_ndb_notify_fn fn, void *ctx)
{
  /* Note: nostrdb subscription callback is set at init time via ndb_config.
   * For now, we don't support changing it after init.
   * The application should poll for notes instead. */
  (void)fn;
  (void)ctx;
}

uint64_t storage_ndb_subscribe(const char *filter_json)
{
  struct ndb *ndb = get_ndb();
  if (!ndb || !filter_json) return 0;

  struct ndb_filter filter;
  if (!ndb_filter_init(&filter)) return 0;

  unsigned char *tmpbuf = (unsigned char *)malloc(4096);
  if (!tmpbuf) {
    ndb_filter_destroy(&filter);
    return 0;
  }

  int len = (int)strlen(filter_json);
  if (!ndb_filter_from_json(filter_json, len, &filter, tmpbuf, 4096)) {
    ndb_filter_destroy(&filter);
    free(tmpbuf);
    return 0;
  }

  uint64_t subid = ndb_subscribe(ndb, &filter, 1);

  ndb_filter_destroy(&filter);
  free(tmpbuf);

  return subid;
}

void storage_ndb_unsubscribe(uint64_t subid)
{
  struct ndb *ndb = get_ndb();
  if (!ndb || subid == 0) return;
  ndb_unsubscribe(ndb, subid);
}

int storage_ndb_poll_notes(uint64_t subid, uint64_t *note_keys, int capacity)
{
  struct ndb *ndb = get_ndb();
  if (!ndb || subid == 0 || !note_keys || capacity <= 0) return 0;
  return ndb_poll_for_notes(ndb, subid, note_keys, capacity);
}

/* ============== Direct Note Access API Implementation ============== */

storage_ndb_note *storage_ndb_get_note_ptr(void *txn, uint64_t note_key)
{
  if (!txn || note_key == 0) return NULL;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;
  size_t note_size = 0;
  return ndb_get_note_by_key(ntxn, note_key, &note_size);
}

uint64_t storage_ndb_get_note_key_by_id(void *txn, const unsigned char id32[32],
                                         storage_ndb_note **note_out)
{
  if (!txn || !id32) return 0;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;
  size_t note_len = 0;
  uint64_t key = 0;
  struct ndb_note *note = ndb_get_note_by_id(ntxn, id32, &note_len, &key);
  if (note_out) *note_out = note;
  return note ? key : 0;
}

const unsigned char *storage_ndb_note_id(storage_ndb_note *note)
{
  if (!note) return NULL;
  return ndb_note_id(note);
}

const unsigned char *storage_ndb_note_pubkey(storage_ndb_note *note)
{
  if (!note) return NULL;
  return ndb_note_pubkey(note);
}

const char *storage_ndb_note_content(storage_ndb_note *note)
{
  if (!note) return NULL;
  return ndb_note_content(note);
}

uint32_t storage_ndb_note_content_length(storage_ndb_note *note)
{
  if (!note) return 0;
  return ndb_note_content_length(note);
}

uint32_t storage_ndb_note_created_at(storage_ndb_note *note)
{
  if (!note) return 0;
  return ndb_note_created_at(note);
}

uint32_t storage_ndb_note_kind(storage_ndb_note *note)
{
  if (!note) return 0;
  return ndb_note_kind(note);
}

void storage_ndb_hex_encode(const unsigned char *bin32, char *hex65)
{
  if (!bin32 || !hex65) return;
  static const char hex_chars[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    hex65[i*2] = hex_chars[(bin32[i] >> 4) & 0x0f];
    hex65[i*2 + 1] = hex_chars[bin32[i] & 0x0f];
  }
  hex65[64] = '\0';
}
