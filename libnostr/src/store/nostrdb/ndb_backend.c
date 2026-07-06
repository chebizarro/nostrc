#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
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

/* nostrc-slot: Thread-local storage for transaction reuse with reference counting.
 * This is essential to prevent LMDB reader slot exhaustion. LMDB has a default
 * limit of 126 concurrent readers. Without TLS caching, batch processing with
 * signal handlers can easily open 100+ concurrent transactions.
 *
 * Reference counting fixes the use-after-free issue that occurred with the
 * original TLS caching: when nested begin_query/end_query calls happen (e.g.,
 * signal handler calls begin_query while outer code's transaction is active),
 * end_query now only decrements the refcount instead of freeing. The transaction
 * is only freed when refcount reaches 0. */
typedef struct tls_txn_t {
    struct ndb *db;         /* Owning nostrdb handle for this cached txn */
    struct ndb_txn *txn;
    int refcount;           /* Number of active begin_query calls using this txn */
    time_t last_used;
    struct tls_txn_t *next; /* Per-thread cache entries keyed by db */
} tls_txn_t;

typedef struct live_ndb_handle_t {
    struct ndb *db;
    struct live_ndb_handle_t *next;
} live_ndb_handle_t;

static pthread_key_t tls_txn_key;
static pthread_once_t tls_init_once = PTHREAD_ONCE_INIT;
static void tls_init(void);

/* nostrc-uaf1 / nostrc-mrj: TLS destructors can run after a store is closed.
 * Track live nostrdb handles individually instead of using one process-global
 * alive bit, so closing store B does not make store A's TLS cleanup leak its
 * reader txn, while destructors still skip ndb_end_query for the closed handle. */
static pthread_mutex_t live_ndb_mutex = PTHREAD_MUTEX_INITIALIZER;
static live_ndb_handle_t *live_ndb_handles = NULL;

static int ndb_handle_is_live_locked(struct ndb *db)
{
    for (live_ndb_handle_t *it = live_ndb_handles; it; it = it->next) {
        if (it->db == db) return 1;
    }
    return 0;
}

static int ndb_handle_register(struct ndb *db)
{
    if (!db) return 0;

    live_ndb_handle_t *node = (live_ndb_handle_t *)calloc(1, sizeof(*node));
    if (!node) return 0;
    node->db = db;

    pthread_mutex_lock(&live_ndb_mutex);
    if (ndb_handle_is_live_locked(db)) {
        pthread_mutex_unlock(&live_ndb_mutex);
        free(node);
        return 1;
    }
    node->next = live_ndb_handles;
    live_ndb_handles = node;
    pthread_mutex_unlock(&live_ndb_mutex);
    return 1;
}

static void ndb_handle_unregister(struct ndb *db)
{
    if (!db) return;

    live_ndb_handle_t *removed = NULL;
    pthread_mutex_lock(&live_ndb_mutex);
    live_ndb_handle_t **link = &live_ndb_handles;
    while (*link) {
        if ((*link)->db == db) {
            removed = *link;
            *link = removed->next;
            break;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&live_ndb_mutex);
    free(removed);
}

static int ndb_end_query_if_handle_live(struct ndb *db, struct ndb_txn *txn)
{
    if (!db || !txn) return 0;

    int ok = 0;
    pthread_mutex_lock(&live_ndb_mutex);
    if (ndb_handle_is_live_locked(db)) {
        /* Hold the live-handle mutex while ending the reader txn so close()
         * cannot unregister/destroy this handle between the liveness check and
         * ndb_end_query(). */
        ok = ndb_end_query(txn);
    }
    pthread_mutex_unlock(&live_ndb_mutex);
    return ok;
}

static tls_txn_t *tls_find_for_db(tls_txn_t *head, struct ndb *db)
{
    for (tls_txn_t *it = head; it; it = it->next) {
        if (it->db == db) return it;
    }
    return NULL;
}

static tls_txn_t *tls_find_for_txn(tls_txn_t *head, struct ndb_txn *txn)
{
    for (tls_txn_t *it = head; it; it = it->next) {
        if (it->txn == txn) return it;
    }
    return NULL;
}

static tls_txn_t *tls_ensure_for_db(struct ndb *db)
{
    tls_txn_t *head = (tls_txn_t *)pthread_getspecific(tls_txn_key);
    tls_txn_t *entry = tls_find_for_db(head, db);
    if (entry) return entry;

    entry = (tls_txn_t *)calloc(1, sizeof(*entry));
    if (!entry) return NULL;
    entry->db = db;

    if (!head) {
        if (pthread_setspecific(tls_txn_key, entry) != 0) {
            free(entry);
            return NULL;
        }
    } else {
        entry->next = head->next;
        head->next = entry;
    }
    return entry;
}

static void tls_close_inactive_for_db(struct ndb *db)
{
    if (!db) return;
    pthread_once(&tls_init_once, tls_init);

    tls_txn_t *tls = tls_find_for_db((tls_txn_t *)pthread_getspecific(tls_txn_key), db);
    if (tls && tls->txn && tls->refcount == 0) {
        ndb_end_query_if_handle_live(tls->db, tls->txn);
        free(tls->txn);
        tls->txn = NULL;
        tls->last_used = 0;
    }
}

/* Cleanup function for thread-local transactions */
static void tls_txn_destructor(void *data)
{
    tls_txn_t *tls = (tls_txn_t*)data;
    while (tls) {
        tls_txn_t *next = tls->next;
        if (tls->txn) {
            ndb_end_query_if_handle_live(tls->db, tls->txn);
            free(tls->txn);
        }
        free(tls);
        tls = next;
    }
}

/* Initialize thread-local storage */
static void tls_init(void)
{
    pthread_key_create(&tls_txn_key, tls_txn_destructor);
}

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
      /* SECURITY: skipping ingest validation accepts events with invalid ids
       * and signatures into the durable store. This must NEVER be enabled by a
       * stray production config option alone. Require an explicit, out-of-band
       * environment opt-in (NOSTR_ALLOW_UNSAFE_INGEST=1) so that a leaked/typo'd
       * opts_json key cannot silently disable signature verification. */
      const char *allow = getenv("NOSTR_ALLOW_UNSAFE_INGEST");
      if (allow && allow[0] == '1' && allow[1] == '\0') {
        fprintf(stderr,
          "[ndb] WARNING: ingest validation DISABLED (ingest_skip_validation + "
          "NOSTR_ALLOW_UNSAFE_INGEST=1). Events with invalid ids/signatures will "
          "be accepted. This is UNSAFE and must only be used in tests.\n");
        cfg.ingest_filter = ln_ndb_ingest_filter_skip;
        cfg.filter_context = NULL;
      } else {
        fprintf(stderr,
          "[ndb] REFUSED ingest_skip_validation: set NOSTR_ALLOW_UNSAFE_INGEST=1 "
          "to enable (test-only). Proceeding WITH full id/signature validation.\n");
      }
    }
  }

  struct ndb *db = NULL;
  if (!ndb_init(&db, path, &cfg)) {
    free(impl);
    free(h);
    return LN_ERR_DB_OPEN;
  }

  if (!ndb_handle_register(db)) {
    ndb_destroy(db);
    free(impl);
    free(h);
    return LN_ERR_OOM;
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
      struct ndb *db = (struct ndb *)impl->db;
      /* End this thread's inactive cached reader for this handle before close,
       * then unregister only this handle so other open stores remain live for
       * their TLS destructors. */
      tls_close_inactive_for_db(db);
      ndb_handle_unregister(db);
      ndb_destroy(db);
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

  /* Initialize thread-local storage once */
  pthread_once(&tls_init_once, tls_init);

  /* Get or create the thread-local transaction state for this specific db. */
  tls_txn_t *tls = tls_ensure_for_db(db);
  if (!tls) return LN_ERR_OOM;
  time_t now = time(NULL);

  /* nostrc-slot: Reuse only a transaction owned by this db. This preserves the
   * reader-slot protection for nested/successive queries while preventing a
   * thread that just queried store A from receiving A's txn for store B. */
  if (tls->txn && tls->refcount > 0) {
    /* Existing active transaction - reuse it.
     * Do NOT reset last_used here: the original open time must be preserved
     * so the txn is eventually closed, preventing indefinite LMDB page pinning
     * that causes MDB_MAP_FULL and cascading backpressure (11 GB queue growth). */
    tls->refcount++;
    *txn_out = tls->txn;
    return LN_OK;
  }

  /* Also reuse if this db's transaction is recent (within 2 seconds) even if
   * refcount is 0. Do NOT reset last_used: preserving the original timestamp
   * ensures the txn closes within 2 seconds of creation, not last query. */
  if (tls->txn && tls->refcount == 0 && (now - tls->last_used) < 2) {
    tls->refcount = 1;
    *txn_out = tls->txn;
    return LN_OK;
  }

  /* Clean up this db's old transaction if it exists and is not in use. */
  if (tls->txn && tls->refcount == 0) {
    ndb_end_query_if_handle_live(tls->db, tls->txn);
    free(tls->txn);
    tls->txn = NULL;
  }

  /* Create new transaction with retries for transient contention */
  for (int attempt = 0; attempt < 50; attempt++) {
    struct ndb_txn *txn = (struct ndb_txn *)calloc(1, sizeof(*txn));
    if (!txn) return LN_ERR_OOM;

    if (ndb_begin_query(db, txn)) {
      tls->txn = txn;
      tls->refcount = 1;
      tls->last_used = now;
      *txn_out = txn;
      return LN_OK;
    }

    free(txn);

    /* Exponential backoff for retries */
    int backoff_ms = 10 * (1 << (attempt < 5 ? attempt : 5));
    if (backoff_ms > 200) backoff_ms = 200;
    usleep(backoff_ms * 1000);
  }

  fprintf(stderr, "[ndb] begin_query FAILED after 50 attempts - reader slots exhausted?\n");
  return LN_ERR_DB_TXN;
}

static int ln_ndb_end_query(ln_store *s, void *txn)
{
  if (!s || !s->impl || !txn) return LN_ERR_DB_TXN;
  struct ndb *db = (struct ndb *)((struct ln_ndb_impl *)s->impl)->db;
  if (!db) return LN_ERR_DB_TXN;

  pthread_once(&tls_init_once, tls_init);

  /* nostrc-slot: Decrement reference count instead of immediately freeing.
   * The transaction is only freed when refcount reaches 0 AND it's stale
   * (handled in begin_query when creating a new transaction). */
  tls_txn_t *head = (tls_txn_t*)pthread_getspecific(tls_txn_key);
  tls_txn_t *tls = tls_find_for_db(head, db);
  if (tls && tls->txn == txn && tls->refcount > 0) {
    tls->refcount--;
    /* Don't actually end the transaction - keep it for reuse */
    return LN_OK;
  }

  /* If the txn belongs to another cached db, do not close/free it through this
   * store. That would corrupt the owning TLS cache. */
  tls_txn_t *owner = tls_find_for_txn(head, (struct ndb_txn *)txn);
  if (owner && owner->db != db) return LN_ERR_DB_TXN;

  /* Fallback: if this isn't the TLS transaction, close it directly.
   * This shouldn't normally happen but handles edge cases. */
  int ok = ndb_end_query_if_handle_live(db, (struct ndb_txn *)txn);
  free(txn);
  return ok ? LN_OK : LN_ERR_DB_TXN;
}

/* Invalidate the thread-local transaction cache so next begin_query gets fresh data.
 * This is needed after subscription callbacks since the cached transaction may not
 * see newly committed notes. */
static void ln_ndb_invalidate_txn_cache(ln_store *s)
{
  struct ndb *db = NULL;
  if (s && s->impl) db = (struct ndb *)((struct ln_ndb_impl *)s->impl)->db;

  pthread_once(&tls_init_once, tls_init);
  for (tls_txn_t *tls = (tls_txn_t*)pthread_getspecific(tls_txn_key);
       tls;
       tls = tls->next) {
    if (db && tls->db != db) continue;
    if (tls->txn && tls->refcount == 0) {
      ndb_end_query_if_handle_live(tls->db, tls->txn);
      free(tls->txn);
      tls->txn = NULL;
      tls->last_used = 0;
    }
  }
}

/* External entry point for storage_ndb to call */
void ln_ndb_invalidate_txn_cache_ext(void)
{
  ln_ndb_invalidate_txn_cache(NULL);
}

/* nostrc-i26h: Force-close the TLS transaction regardless of refcount.
 * This is essential for shutdown to prevent database balloon.
 *
 * During normal operation, transactions are held with refcount > 0 and
 * reused for efficiency. But at shutdown, we must forcefully end all
 * transactions so LMDB can reclaim pages. If transactions remain open
 * when ndb_destroy() is called, LMDB cannot free those pages, causing
 * the database file to balloon to gigabytes.
 *
 * Note: This only affects the calling thread's TLS cache. For a clean
 * shutdown, call this from the main thread before storage_ndb_shutdown(). */
void ln_ndb_force_close_txn_cache(void)
{
  pthread_once(&tls_init_once, tls_init);
  for (tls_txn_t *tls = (tls_txn_t*)pthread_getspecific(tls_txn_key);
       tls;
       tls = tls->next) {
    if (tls->txn) {
      ndb_end_query_if_handle_live(tls->db, tls->txn);
      free(tls->txn);
      tls->txn = NULL;
      tls->refcount = 0;
      tls->last_used = 0;
    }
  }
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

  size_t total_entries = 0, total_bytes = 0;
  for (int i = 0; i < NDB_DBS; i++) {
    total_entries += st.dbs[i].count;
    total_bytes += st.dbs[i].key_size + st.dbs[i].value_size;
  }

  char buf[1024];
  int n = snprintf(buf, sizeof(buf),
    "{\"total_entries\":%zu,\"total_bytes\":%zu,"
    "\"notes\":%zu,\"profiles\":%zu,"
    "\"kinds\":{\"text\":%zu,\"contacts\":%zu,\"dm\":%zu,"
    "\"repost\":%zu,\"reaction\":%zu,\"zap\":%zu}}",
    total_entries, total_bytes,
    st.dbs[NDB_DB_NOTE].count, st.dbs[NDB_DB_PROFILE].count,
    st.common_kinds[NDB_CKIND_TEXT].count,
    st.common_kinds[NDB_CKIND_CONTACTS].count,
    st.common_kinds[NDB_CKIND_DM].count,
    st.common_kinds[NDB_CKIND_REPOST].count,
    st.common_kinds[NDB_CKIND_REACTION].count,
    st.common_kinds[NDB_CKIND_ZAP].count);
  if (n < 0 || (size_t)n >= sizeof(buf)) return LN_ERR_QUERY;
  char *out = (char *)malloc((size_t)n + 1);
  if (!out) return LN_ERR_OOM;
  memcpy(out, buf, (size_t)n);
  out[n] = '\0';
  *json_out = out;
  return LN_OK;
}

static int ln_ndb_search_profile(ln_store *s, void *txn, const char *query, int limit, void **results, int *count)
{
  if (!s || !s->impl || !txn || !query || !results || !count) return LN_ERR_QUERY;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;

  if (limit <= 0) limit = 50;
  if (limit > 512) limit = 512;

  struct ndb_search search;
  if (!ndb_search_profile(ntxn, &search, query)) {
    /* No results found */
    *results = NULL;
    *count = 0;
    return LN_OK;
  }

  char **arr = (char **)calloc((size_t)limit, sizeof(char *));
  if (!arr) { ndb_search_profile_end(&search); return LN_ERR_OOM; }

  int got = 0;
  for (int i = 0; i < limit; i++) {
    if (i > 0 && !ndb_search_profile_next(&search))
      break;

    /* profile_key → profile record → note_key → note → JSON */
    size_t profile_len = 0;
    void *profile_root = ndb_get_profile_by_key(ntxn, search.profile_key, &profile_len);
    if (!profile_root) continue;

    NdbProfileRecord_table_t record = NdbProfileRecord_as_root(profile_root);
    uint64_t note_key = NdbProfileRecord_note_key(record);
    size_t note_size = 0;
    struct ndb_note *note = ndb_get_note_by_key(ntxn, note_key, &note_size);
    if (!note) continue;

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
    if (buf) arr[got++] = buf;
  }

  ndb_search_profile_end(&search);
  *results = arr;
  *count = got;
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
  .search_profile = ln_ndb_search_profile,
  .get_note_by_id = ln_ndb_get_note_by_id,
  .get_profile_by_pubkey = ln_ndb_get_profile_by_pubkey,
  .stat_json = ln_ndb_stat_json,
};

const struct ln_store_ops *ln_ndb_get_ops(void)
{
  return &ndb_ops;
}
