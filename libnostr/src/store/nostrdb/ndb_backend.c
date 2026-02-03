#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libnostr_store.h"
#include "libnostr_errors.h"
#include "store_int.h"
#include "ndb_backend.h"
/* Internal dependency: vendored nostrdb (suppress pedantic warnings from vendor headers) */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wpedantic"
#  pragma clang diagnostic ignored "-Wzero-length-array"
#  pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "nostrdb.h"
#include "bindings/c/profile_reader.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* very small helpers to parse minimal JSON-like key/values without deps */
static int parse_kv_int(const char *json, const char *key, long long *out)
{
  if (!json || !key || !out) return 0;
  const char *p = strstr(json, key);
  if (!p) return 0;
  p = strchr(p, ':');
  if (!p) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  long long v = 0; int neg = 0;
  if (*p == '-') { neg = 1; p++; }
  if (*p < '0' || *p > '9') return 0;
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  *out = neg ? -v : v;
  return 1;
}

/* Optional ingest filter: skip all validation (ids/sigs) for demo/testing */
static enum ndb_ingest_filter_action ln_ndb_ingest_filter_skip(void *ctx, struct ndb_note *note)
{
  (void)ctx; (void)note;
  return NDB_INGEST_SKIP_VALIDATION;
}

static int parse_kv_str(const char *json, const char *key, const char **out_str, int *out_len)
{
  if (!json || !key || !out_str) return 0;
  const char *p = strstr(json, key);
  if (!p) return 0;
  p = strchr(p, ':');
  if (!p) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return 0;
  p++;
  const char *start = p;
  while (*p && *p != '"') p++;
  if (*p != '"') return 0;
  *out_str = start;
  if (out_len) *out_len = (int)(p - start);
  return 1;
}

static int ln_ndb_open(ln_store **out, const char *path, const char *opts_json)
{
  if (!out || !path) return LN_ERR_DB_OPEN;

  struct ln_store *h = (struct ln_store*)calloc(1, sizeof(*h));
  if (!h) return LN_ERR_OOM;

  struct ln_ndb_impl *impl = (struct ln_ndb_impl*)calloc(1, sizeof(*impl));
  if (!impl) { free(h); return LN_ERR_OOM; }

  /* Initialize nostrdb */
  struct ndb_config cfg;
  ndb_default_config(&cfg);
  /* Reasonable defaults; allow opts_json override */
  cfg.flags = 0; /* full features */
  cfg.ingester_threads = 1;
  cfg.mapsize = (size_t)1ULL << 33; /* 8 GiB default */
  cfg.writer_scratch_buffer_size = 2 * 1024 * 1024;
  cfg.filter_context = NULL;
  cfg.ingest_filter = NULL;

  /* Get subscription callback from storage_ndb layer (if set before init) */
  {
    typedef void (*storage_ndb_notify_fn)(void *ctx, uint64_t subid);
    extern void storage_ndb_get_notify_callback(storage_ndb_notify_fn *fn_out, void **ctx_out);
    storage_ndb_notify_fn sub_fn = NULL;
    void *sub_ctx = NULL;
    storage_ndb_get_notify_callback(&sub_fn, &sub_ctx);
    cfg.sub_cb = (ndb_sub_fn)sub_fn;
    cfg.sub_cb_ctx = sub_ctx;
  }

  if (opts_json && *opts_json) {
    long long v = 0;
    if (parse_kv_int(opts_json, "mapsize", &v) && v > 0) cfg.mapsize = (size_t)v;
    if (parse_kv_int(opts_json, "flags", &v) && v >= 0) cfg.flags = (int)v;
    if (parse_kv_int(opts_json, "ingester_threads", &v) && v > 0) cfg.ingester_threads = (int)v;
    if (parse_kv_int(opts_json, "writer_scratch_buffer_size", &v) && v > 0) cfg.writer_scratch_buffer_size = (int)v;
    if (parse_kv_int(opts_json, "ingest_skip_validation", &v) && v > 0) {
      cfg.ingest_filter = ln_ndb_ingest_filter_skip;
      cfg.filter_context = NULL;
    }
  }

  struct ndb *db = NULL;
  if (!ndb_init(&db, path, &cfg)) {
    free(impl);
    free(h);
    return LN_ERR_DB_OPEN;
  }

  impl->db = db;
  h->impl = impl;
  *out = h;
  return LN_OK;
}

static void ln_ndb_close(ln_store *s)
{
  if (!s) return;
  if (s->impl) {
    struct ln_ndb_impl *impl = (struct ln_ndb_impl *)s->impl;
    if (impl->db) {
      ndb_destroy((struct ndb *)impl->db);
      impl->db = NULL;
    }
    free(s->impl);
  }
  free(s);
}

static int ln_ndb_ingest_event_json(ln_store *s, const char *json, int len, const char *relay)
{
  if (!s || !s->impl || !json) return LN_ERR_INGEST;
  struct ndb *db = (struct ndb *)((struct ln_ndb_impl *)s->impl)->db;
  if (!db) return LN_ERR_INGEST;
  if (len < 0) len = (int)strlen(json);
  struct ndb_ingest_meta meta;
  /* client=1 means raw event JSON (not relay websocket format ["EVENT", "subid", {...}]) */
  ndb_ingest_meta_init(&meta, 1, relay);
  return ndb_process_event_with(db, json, len, &meta) ? LN_OK : LN_ERR_INGEST;
}

static int ln_ndb_ingest_ldjson(ln_store *s, const char *ldjson, size_t len, const char *relay)
{
  (void)relay; /* relay not recorded per-line here */
  if (!s || !s->impl || !ldjson) return LN_ERR_INGEST;
  struct ndb *db = (struct ndb *)((struct ln_ndb_impl *)s->impl)->db;
  if (!db) return LN_ERR_INGEST;
  return ndb_process_client_events(db, ldjson, len) ? LN_OK : LN_ERR_INGEST;
}

static int ln_ndb_begin_query(ln_store *s, void **txn_out)
{
  if (!s || !s->impl || !txn_out) return LN_ERR_DB_TXN;
  struct ndb *db = (struct ndb *)((struct ln_ndb_impl *)s->impl)->db;
  if (!db) return LN_ERR_DB_TXN;

  /* No TLS caching - each caller gets their own transaction.
   * This is required because:
   * 1. Transaction caching causes MDB_MAP_FULL during heavy writes (LMDB
   *    read transactions prevent page reclamation)
   * 2. Signal handlers can trigger nested begin_query calls on the same thread,
   *    causing use-after-free if TLS references are freed/reused
   *
   * The performance cost is minimal - ndb_begin_query() just acquires an
   * LMDB read slot (no I/O). */

  /* Create new transaction - single attempt, no retries.
   * LMDB reader slot acquisition should succeed immediately if slots are
   * available. If it fails, retrying just delays the inevitable.
   * Callers should handle failure gracefully. */
  struct ndb_txn *txn = (struct ndb_txn *)calloc(1, sizeof(*txn));
  if (!txn) return LN_ERR_OOM;

  if (ndb_begin_query(db, txn)) {
    *txn_out = txn;
    return LN_OK;
  }

  free(txn);
  return LN_ERR_DB_TXN;
}

static int ln_ndb_end_query(ln_store *s, void *txn)
{
  (void)s;
  if (!txn) return LN_ERR_DB_TXN;

  /* Each transaction is owned by its caller - just end and free it */
  int ok = ndb_end_query((struct ndb_txn *)txn);
  free(txn);
  return ok ? LN_OK : LN_ERR_DB_TXN;
}

/* No-op: TLS caching removed, no cache to invalidate */
static void ln_ndb_invalidate_txn_cache(ln_store *s)
{
  (void)s;
}

/* External entry point for storage_ndb to call */
void ln_ndb_invalidate_txn_cache_ext(void)
{
  ln_ndb_invalidate_txn_cache(NULL);
}

static int ln_ndb_query(ln_store *s, void *txn, const char *filters_json, void **results, int *count)
{
  if (!s || !s->impl || !txn || !filters_json || !results || !count) return LN_ERR_QUERY;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;

  /* Detect array vs single object */
  const char *p = filters_json;
  while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
  int is_array = (*p == '[');
  #ifdef LN_NDB_DEBUG
  fprintf(stderr, "[ndb] ln_ndb_query: is_array=%d\n", is_array);
  #endif

  /* Prepare up to 16 filters */
  enum { MAXF = 16 };
  struct ndb_filter filters[MAXF];
  int num_filters = 0;
  int init_count = 0;
  memset(filters, 0, sizeof(filters));
  /* Each filter keeps pointers into the provided buffer; use distinct buffers */
  unsigned char *tmpbufs[MAXF] = {0};

  if (!is_array) {
    if (!ndb_filter_init(&filters[0])) { 
      #ifdef LN_NDB_DEBUG
      fprintf(stderr, "[ndb] filter_init failed for single filter\n");
      #endif
      return LN_ERR_QUERY; 
    }
    int fjlen = (int)strlen(filters_json);
    tmpbufs[0] = (unsigned char*)malloc(4096);
    if (!tmpbufs[0]) { ndb_filter_destroy(&filters[0]); return LN_ERR_OOM; }
    if (!ndb_filter_from_json(filters_json, fjlen, &filters[0], tmpbufs[0], 4096)) {
      #ifdef LN_NDB_DEBUG
      fprintf(stderr, "[ndb] filter_from_json failed for single filter, len=%d\n", fjlen);
      #endif
      ndb_filter_destroy(&filters[0]);
      free(tmpbufs[0]); tmpbufs[0] = NULL;
      return LN_ERR_QUERY;
    }
    init_count = num_filters = 1;
  } else {
    /* Minimal array splitter: handles braces/strings/escapes */
    const char *sjson = p + 1; /* after '[' */
    int depth = 0; int in_str = 0; int esc = 0;
    const char *seg_start = NULL;
    for (const char *q = sjson; *q; q++) {
      char c = *q;
      if (in_str) {
        if (esc) { esc = 0; continue; }
        if (c == '\\') { esc = 1; continue; }
        if (c == '"') { in_str = 0; }
        continue;
      }
      if (c == '"') { in_str = 1; continue; }
      if (c == '{') { if (depth == 0) seg_start = q; depth++; continue; }
      if (c == '}') { depth--; if (depth == 0 && seg_start) {
          int seg_len = (int)((q + 1) - seg_start);
          if (num_filters < MAXF) {
            if (!ndb_filter_init(&filters[num_filters])) { /* cleanup below */ 
              #ifdef LN_NDB_DEBUG
              fprintf(stderr, "[ndb] filter_init failed at index %d\n", num_filters);
              #endif
              num_filters = -1; break; }
            init_count++;
            tmpbufs[num_filters] = (unsigned char*)malloc(4096);
            if (!tmpbufs[num_filters]) { 
              #ifdef LN_NDB_DEBUG
              fprintf(stderr, "[ndb] OOM allocating tmpbuf for filter %d\n", num_filters);
              #endif
              num_filters = -1; break; }
            if (!ndb_filter_from_json(seg_start, seg_len, &filters[num_filters], tmpbufs[num_filters], 4096)) { 
              #ifdef LN_NDB_DEBUG
              fprintf(stderr, "[ndb] filter_from_json failed for filter %d, seg_len=%d\n", num_filters, seg_len);
              #endif
              num_filters = -1; break; }
            num_filters++;
          }
          seg_start = NULL;
        }
        continue;
      }
      if (c == ']' && depth == 0) break;
    }
    if (num_filters <= 0) {
      #ifdef LN_NDB_DEBUG
      fprintf(stderr, "[ndb] no valid filters parsed from array\n");
      #endif
      for (int i = 0; i < init_count; i++) { ndb_filter_destroy(&filters[i]); free(tmpbufs[i]); }
      return LN_ERR_QUERY;
    }
  }

  /* Query */
  enum { CAP = 256 };
  struct ndb_query_result qres[CAP];
  int got = 0;
  #ifdef LN_NDB_DEBUG
  fprintf(stderr, "[ndb] calling ndb_query with %d filters\n", num_filters);
  #endif
  if (!ndb_query(ntxn, filters, num_filters, qres, CAP, &got)) {
    #ifdef LN_NDB_DEBUG
    fprintf(stderr, "[ndb] ndb_query failed for %d filters\n", num_filters);
    #endif
    for (int i = 0; i < num_filters; i++) { ndb_filter_destroy(&filters[i]); free(tmpbufs[i]); }
    return LN_ERR_QUERY;
  }
  #ifdef LN_NDB_DEBUG
  fprintf(stderr, "[ndb] ndb_query returned %d results\n", got);
  #endif

  /* Allocate array of JSON strings */
  char **arr = (char **)calloc((size_t)got, sizeof(char *));
  if (!arr) {
    for (int i = 0; i < num_filters; i++) ndb_filter_destroy(&filters[i]);
    return LN_ERR_OOM;
  }

  /* Serialize notes to JSON strings */
  for (int i = 0; i < got; i++) {
    struct ndb_note *note = qres[i].note;
    if (!note) { arr[i] = NULL; continue; }

    int bufsize = 1024;
    char *buf = NULL;
    for (;;) {
      char *nb = (char *)realloc(buf, (size_t)bufsize);
      if (!nb) { free(buf); buf = NULL; break; }
      buf = nb;
      int n = ndb_note_json(note, buf, bufsize);
      if (n > 0 && n < bufsize) {
        /* ensure NUL-termination */
        buf[n] = '\0';
        break;
      }
      bufsize *= 2;
      if (bufsize > (32 * 1024 * 1024)) { /* 32MB cap */
        free(buf); buf = NULL; break;
      }
    }
    arr[i] = buf; /* may be NULL if OOM */
  }

  for (int i = 0; i < num_filters; i++) { ndb_filter_destroy(&filters[i]); free(tmpbufs[i]); }
  *results = arr;
  *count = got;
  return LN_OK;
}

static int ln_ndb_text_search(ln_store *s, void *txn, const char *query, const char *config_json, void **results, int *count)
{
  if (!s || !s->impl || !txn || !query || !results || !count) return LN_ERR_TEXTSEARCH;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;

  struct ndb_text_search_config cfg;
  ndb_default_text_search_config(&cfg);
  ndb_text_search_config_set_limit(&cfg, 128);
  if (config_json && *config_json) {
    long long lim = 0;
    if (parse_kv_int(config_json, "limit", &lim) && lim > 0 && lim <= 1024) {
      ndb_text_search_config_set_limit(&cfg, (int)lim);
    }
    const char *ostr = NULL; int olen = 0;
    if (parse_kv_str(config_json, "order", &ostr, &olen) && ostr && olen > 0) {
      if (olen == 3 && strncmp(ostr, "asc", 3) == 0) {
        ndb_text_search_config_set_order(&cfg, NDB_ORDER_ASCENDING);
      } else if (olen == 4 && strncmp(ostr, "desc", 4) == 0) {
        ndb_text_search_config_set_order(&cfg, NDB_ORDER_DESCENDING);
      }
    }
  }

  struct ndb_text_search_results r;
  if (!ndb_text_search(ntxn, query, &r, &cfg)) return LN_ERR_TEXTSEARCH;

  int got = r.num_results;
  char **arr = (char **)calloc((size_t)got, sizeof(char *));
  if (!arr) return LN_ERR_OOM;

  for (int i = 0; i < got; i++) {
    struct ndb_note *note = r.results[i].note;
    size_t note_size = r.results[i].note_size;
    if (!note) {
      /* fetch by key if note not populated */
      note = ndb_get_note_by_key(ntxn, r.results[i].key.note_id, &note_size);
      if (!note) { arr[i] = NULL; continue; }
    }

    int bufsize = (int)note_size + 256;
    if (bufsize < 1024) bufsize = 1024;
    char *buf = NULL;
    for (;;) {
      char *nb = (char *)realloc(buf, (size_t)bufsize);
      if (!nb) { free(buf); buf = NULL; break; }
      buf = nb;
      int n = ndb_note_json(note, buf, bufsize);
      if (n > 0 && n < bufsize) { buf[n] = '\0'; break; }
      bufsize *= 2;
      if (bufsize > (32 * 1024 * 1024)) { free(buf); buf = NULL; break; }
    }
    arr[i] = buf;
  }

  *results = arr;
  *count = got;
  return LN_OK;
}

static int ln_ndb_get_note_by_id(ln_store *s, void *txn, const unsigned char id[32], const char **json, int *json_len)
{
  if (!s || !s->impl || !txn || !id || !json) return LN_ERR_QUERY;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;
  size_t note_len = 0;
  uint64_t key = 0;
  struct ndb_note *note = ndb_get_note_by_id(ntxn, id, &note_len, &key);
  if (!note) return LN_ERR_NOT_FOUND;

  int bufsize = (int)(note_len + 256);
  if (bufsize < 1024) bufsize = 1024;
  char *buf = NULL;
  for (;;) {
    char *nb = (char *)realloc(buf, (size_t)bufsize);
    if (!nb) { free(buf); return LN_ERR_OOM; }
    buf = nb;
    int n = ndb_note_json(note, buf, bufsize);
    if (n > 0 && n < bufsize) {
      buf[n] = '\0';
#ifdef LN_NDB_DEBUG
      size_t slen = strnlen(buf, (size_t)n + 1);
      int head_len = n < 80 ? n : 80;
      char head[81]; memcpy(head, buf, head_len); head[head_len] = '\0';
      const char *tail_src = buf + (n > 80 ? (n - 80) : 0);
      int tail_len = n < 80 ? n : 80;
      char tail[81]; memcpy(tail, tail_src, tail_len); tail[tail_len] = '\0';
      fprintf(stderr, "[ndb] get_profile_by_pubkey: n=%d slen=%zu nul_at_n=%s head='%s'%s tail='%s'\n",
              n, slen, (buf[n] == '\0' ? "yes" : "no"), head, (n > 80 ? "…" : ""), tail);
#endif
      if (json_len) *json_len = n;
      *json = buf;
      return LN_OK;
    }
    bufsize *= 2;
    if (bufsize > (32 * 1024 * 1024)) { free(buf); return LN_ERR_QUERY; }
  }
}

static int ln_ndb_get_profile_by_pubkey(ln_store *s, void *txn, const unsigned char pk[32], const char **json, int *json_len)
{
  if (!s || !s->impl || !txn || !pk || !json) return LN_ERR_QUERY;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;
  size_t record_len = 0;
  uint64_t prim = 0;
  void *root = ndb_get_profile_by_pubkey(ntxn, pk, &record_len, &prim);
  if (!root || record_len == 0) return LN_ERR_NOT_FOUND;

  /* The profile lookup returns a profile record (FlatBuffer). Extract last profile note key */
  NdbProfileRecord_table_t record = NdbProfileRecord_as_root(root);
  uint64_t note_key = NdbProfileRecord_note_key(record);
  size_t note_size = 0;
  struct ndb_note *note = ndb_get_note_by_key(ntxn, note_key, &note_size);
  if (!note) return LN_ERR_NOT_FOUND;

  /* Serialize the profile note to JSON */
  int bufsize = (int)note_size + 256;
  if (bufsize < 1024) bufsize = 1024;
  char *buf = NULL;
  for (;;) {
    char *nb = (char *)realloc(buf, (size_t)bufsize);
    if (!nb) { free(buf); return LN_ERR_OOM; }
    buf = nb;
    int n = ndb_note_json(note, buf, bufsize);
    if (n > 0 && n < bufsize) {
      buf[n] = '\0';
      if (json_len) *json_len = n;
      *json = buf;
      return LN_OK;
    }
    bufsize *= 2;
    if (bufsize > (32 * 1024 * 1024)) { free(buf); return LN_ERR_QUERY; }
  }
}

static int ln_ndb_stat_json(ln_store *s, char **json_out)
{
  if (!s || !s->impl || !json_out) return LN_ERR_QUERY;
  struct ln_ndb_impl *impl = (struct ln_ndb_impl *)s->impl;
  struct ndb *db = (struct ndb *)impl->db;
  if (!db) return LN_ERR_QUERY;
  struct ndb_stat st;
  if (!ndb_stat(db, &st)) return LN_ERR_QUERY;
  /* Minimal JSON with a few totals; extend later */
  char buf[512];
  size_t total = 0;
  for (int i = 0; i < NDB_DBS; i++) total += st.dbs[i].count;
  int n = snprintf(buf, sizeof(buf),
                   "{\"dbs\":%zu,\"notes\":%zu}",
                   total,
                   st.dbs[NDB_DB_NOTE].count);
  if (n < 0) return LN_ERR_QUERY;
  char *out = (char *)malloc((size_t)n + 1);
  if (!out) return LN_ERR_OOM;
  memcpy(out, buf, (size_t)n);
  out[n] = '\0';
  *json_out = out;
  return LN_OK;
}

static const struct ln_store_ops ndb_ops = {
  .open = ln_ndb_open,
  .close = ln_ndb_close,
  .ingest_event_json = ln_ndb_ingest_event_json,
  .ingest_ldjson = ln_ndb_ingest_ldjson,
  .begin_query = ln_ndb_begin_query,
  .end_query = ln_ndb_end_query,
  .query = ln_ndb_query,
  .text_search = ln_ndb_text_search,
  .get_note_by_id = ln_ndb_get_note_by_id,
  .get_profile_by_pubkey = ln_ndb_get_profile_by_pubkey,
  .stat_json = ln_ndb_stat_json,
};

const struct ln_store_ops *ln_ndb_get_ops(void)
{
  return &ndb_ops;
}
