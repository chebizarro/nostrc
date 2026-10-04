#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include "nostr-storage.h"
#include "nostrdb_storage.h"
#include "relayd_async_storage.h"
#include "nostr-filter.h"
#include "json.h"
#include "nostr-event.h"
#if __has_include("nostrdb.h")
#include "nostrdb.h"
#define HAVE_NOSTRDB 1
#else
#define HAVE_NOSTRDB 0
#endif

#if !HAVE_NOSTRDB
/* Stub implementation when nostrdb is not available; registers backend but returns NULL */
NostrStorage* nostrdb_storage_new(void) { return NULL; }
const struct RelaydAsyncStorageOps *nostrdb_storage_async_ops(void) { return NULL; }
__attribute__((constructor))
static void _nostrdb_auto_register(void) { nostr_storage_register("nostrdb", nostrdb_storage_new); }
#else

typedef struct {
  char *uri;
  char *opts;
  struct ndb *db;
  pthread_mutex_t commit_mutex;
  pthread_cond_t commit_cond;
  uint64_t async_subid;
  void (*async_notify)(void *);
  void *async_notify_ctx;
} NDBImpl;

typedef struct {
  NDBImpl *impl;
  struct ndb_txn txn;
  struct ndb_query_result *results;
  int count;
  int index;
} NDBIter;

/* nostrdb calls this only after a successful writer transaction commit. The
 * callback runs under its subscription-monitor lock, so only wake waiters here;
 * querying the DB (or unsubscribing) from the callback would deadlock. */
static void ndb_committed(void *ctx, uint64_t subid) {
  NDBImpl *impl = (NDBImpl*)ctx;
  pthread_mutex_lock(&impl->commit_mutex);
  pthread_cond_broadcast(&impl->commit_cond);
  if (subid == impl->async_subid && impl->async_notify)
    impl->async_notify(impl->async_notify_ctx);
  pthread_mutex_unlock(&impl->commit_mutex);
}

static int ndb_note_is_stored(NDBImpl *impl, const unsigned char id[32]) {
  struct ndb_txn txn;
  if (!ndb_begin_query(impl->db, &txn)) return -EIO;
  int found = ndb_get_note_by_id(&txn, id, NULL, NULL) != NULL;
  ndb_end_query(&txn);
  return found;
}

static int ndb_open(NostrStorage *st, const char *uri, const char *opts_json) {
  if (!st) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)calloc(1, sizeof(*impl));
  if (!impl) return -ENOMEM;
  int sync_rc = pthread_mutex_init(&impl->commit_mutex, NULL);
  if (sync_rc != 0) { free(impl); return -sync_rc; }
  sync_rc = pthread_cond_init(&impl->commit_cond, NULL);
  if (sync_rc != 0) {
    pthread_mutex_destroy(&impl->commit_mutex);
    free(impl);
    return -sync_rc;
  }
  if (uri) impl->uri = strdup(uri);
  if (opts_json) impl->opts = strdup(opts_json);
  st->impl = impl;

  /* ndb_init() needs the directory to exist. Create the leaf (callers own
   * the parents); if this fails, ndb_init reports the cause. */
  const char *path = impl->uri ? impl->uri : ".ndb";
#ifndef _WIN32
  if (mkdir(path, 0700) != 0 && errno != EEXIST)
    fprintf(stderr, "[nostrdb_storage] mkdir(%s): %s\n", path, strerror(errno));
#endif

  /* Initialize nostrdb with robust defaults */
  struct ndb_config cfg; ndb_default_config(&cfg);
  /* Sensible defaults similar to gnostr: 1 GiB mapsize, multiple ingester threads */
  const char *mapsize_env = getenv("GRELAY_NDB_MAPSIZE_MB");
  size_t mapsize_mb = mapsize_env && *mapsize_env ? (size_t)strtoull(mapsize_env, NULL, 10) : 1024ULL;
  if (mapsize_mb < 64) mapsize_mb = 64; /* minimum */
  ndb_config_set_mapsize(&cfg, (size_t)mapsize_mb * 1024ULL * 1024ULL);
  
  /* Use multiple ingester threads for better throughput */
  int num_threads = 4; /* Use 4 threads for parallel ingestion */
  ndb_config_set_ingest_threads(&cfg, num_threads);
  ndb_config_set_subscription_callback(&cfg, ndb_committed, impl);
  fprintf(stderr, "[nostrdb_storage] Using %d ingester threads\n", num_threads);
  
  /* nostrc-w0n: Signature verification is enabled (default).
   * The multi-threaded ingester (4 threads) handles the crypto load. */

  int rc = ndb_init(&impl->db, path, &cfg);
  /* ndb_init returns nonzero on success, zero on failure */
  if (rc == 0) {
    fprintf(stderr, "[nostrdb_storage] ndb_init(path=%s) failed rc=%d\n", path, rc);
    pthread_cond_destroy(&impl->commit_cond);
    pthread_mutex_destroy(&impl->commit_mutex);
    free(impl->uri);
    free(impl->opts);
    free(impl);
    st->impl = NULL;
    return -EIO;
  }
  return 0;
}

static void ndb_close(NostrStorage *st) {
  if (!st) return;
  NDBImpl *impl = (NDBImpl*)st->impl;
  if (impl) {
    if (impl->db) { ndb_destroy(impl->db); impl->db = NULL; }
    pthread_cond_destroy(&impl->commit_cond);
    pthread_mutex_destroy(&impl->commit_mutex);
    free(impl->uri);
    free(impl->opts);
    free(impl);
    st->impl = NULL;
  }
}

static int ndb_put_event(NostrStorage *st, const NostrEvent *ev) {
  if (!st || !st->impl || !((NDBImpl*)st->impl)->db || !ev) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  char *id_hex = nostr_event_get_id((NostrEvent*)ev);
  if (!id_hex) return -EINVAL;
  unsigned char id[32];
  int id_ok = strlen(id_hex) == 64;
  for (int i = 0; id_ok && i < 32; i++) {
    unsigned int byte;
    if (sscanf(id_hex + 2*i, "%2x", &byte) != 1) id_ok = 0;
    else id[i] = (unsigned char)byte;
  }
  free(id_hex);
  if (!id_ok) return -EINVAL;

  /* A repeated EVENT may be discarded by nostrdb's ingester rather than
   * generating a notification. It is already durable, so accept it. */
  int present = ndb_note_is_stored(impl, id);
  if (present != 0) return present > 0 ? 0 : present;

  struct ndb_filter filter;
  if (!ndb_filter_init(&filter)) return -ENOMEM;
  int filter_ok = ndb_filter_start_field(&filter, NDB_FILTER_IDS);
  if (filter_ok) filter_ok = ndb_filter_add_id_element(&filter, id);
  if (filter_ok) {
    ndb_filter_end_field(&filter);
    filter_ok = ndb_filter_end(&filter);
  }
  uint64_t subid = filter_ok ? ndb_subscribe(impl->db, &filter, 1) : 0;
  ndb_filter_destroy(&filter);
  if (!subid) return -EIO;

  char *json = nostr_event_serialize(ev);
  int queued = json && ndb_process_event(impl->db, json, (int)strlen(json));
  free(json);
  if (!queued) { ndb_unsubscribe(impl->db, subid); return -EIO; }

  /* Queue admission is not persistence. Subscribe before enqueueing, then
   * return success only once a committed read transaction sees this id.
   * The bound prevents a rejected event or failed write from hanging relayd. */
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    ndb_unsubscribe(impl->db, subid);
    return -EIO;
  }
  deadline.tv_sec += 10;
  pthread_mutex_lock(&impl->commit_mutex);
  int rc;
  while ((rc = ndb_note_is_stored(impl, id)) == 0) {
    int wait_rc = pthread_cond_timedwait(&impl->commit_cond,
                                         &impl->commit_mutex, &deadline);
    if (wait_rc != 0) { rc = wait_rc == ETIMEDOUT ? -ETIMEDOUT : -wait_rc; break; }
  }
  pthread_mutex_unlock(&impl->commit_mutex);
  ndb_unsubscribe(impl->db, subid);
  return rc > 0 ? 0 : rc;
}

/* The relay's LWS service thread uses this path instead of put_event().
 * One catch-all subscription wakes it after each successful writer commit;
 * drain() keeps nostrdb's subscription inbox from filling on long runs. */
static int ndb_async_start(NostrStorage *st, void (*notify)(void *), void *ctx) {
  if (!st || !st->impl || !notify) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  uint64_t subid = ndb_subscribe(impl->db, NULL, 0);
  if (!subid) return -EIO;
  pthread_mutex_lock(&impl->commit_mutex);
  if (impl->async_subid) {
    pthread_mutex_unlock(&impl->commit_mutex);
    ndb_unsubscribe(impl->db, subid);
    return -EBUSY;
  }
  impl->async_notify_ctx = ctx;
  impl->async_notify = notify;
  impl->async_subid = subid;
  pthread_mutex_unlock(&impl->commit_mutex);
  return 0;
}

static void ndb_async_stop(NostrStorage *st) {
  if (!st || !st->impl) return;
  NDBImpl *impl = (NDBImpl*)st->impl;
  pthread_mutex_lock(&impl->commit_mutex);
  uint64_t subid = impl->async_subid;
  impl->async_subid = 0;
  impl->async_notify = NULL;
  impl->async_notify_ctx = NULL;
  pthread_mutex_unlock(&impl->commit_mutex);
  if (subid) ndb_unsubscribe(impl->db, subid);
}

static int ndb_async_enqueue(NostrStorage *st, const NostrEvent *ev) {
  if (!st || !st->impl || !ev) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  char *json = nostr_event_serialize(ev);
  if (!json) return -ENOMEM;
  int queued = ndb_process_event(impl->db, json, (int)strlen(json));
  free(json);
  return queued ? 0 : -EIO;
}

static int ndb_async_visible(NostrStorage *st, const unsigned char id[32]) {
  if (!st || !st->impl || !id) return -EINVAL;
  return ndb_note_is_stored((NDBImpl*)st->impl, id);
}

static void ndb_async_drain(NostrStorage *st) {
  if (!st || !st->impl) return;
  NDBImpl *impl = (NDBImpl*)st->impl;
  pthread_mutex_lock(&impl->commit_mutex);
  uint64_t subid = impl->async_subid;
  pthread_mutex_unlock(&impl->commit_mutex);
  uint64_t notes[256];
  while (subid && ndb_poll_for_notes(impl->db, subid, notes, 256) > 0) {}
}

static const RelaydAsyncStorageOps g_async_ops = {
  .start = ndb_async_start,
  .stop = ndb_async_stop,
  .enqueue = ndb_async_enqueue,
  .visible = ndb_async_visible,
  .drain = ndb_async_drain,
};

const struct RelaydAsyncStorageOps *nostrdb_storage_async_ops(void) {
  return &g_async_ops;
}

static int ndb_ingest_ldjson(NostrStorage *st, const char *ldjson, size_t len) {
  if (!st || !st->impl || !((NDBImpl*)st->impl)->db || !ldjson) return -EINVAL;
  /* Use ndb_process_client_events - we're sending raw event JSON, not relay envelopes */
  int rc = ndb_process_client_events(((NDBImpl*)st->impl)->db, ldjson, len);
  /* nostrdb returns nonzero on success */
  return rc ? 0 : -EIO;
}

/* nostrc-57j: relay-aware ingestion variants */
static int ndb_put_event_with_relay(NostrStorage *st, const NostrEvent *ev, const char *relay) {
  if (!st || !st->impl || !((NDBImpl*)st->impl)->db || !ev) return -EINVAL;
  char *json = nostr_event_serialize(ev);
  if (!json) return -EIO;
  struct ndb_ingest_meta meta;
  ndb_ingest_meta_init(&meta, 1, relay);
  int rc = ndb_process_event_with(((NDBImpl*)st->impl)->db, json, (int)strlen(json), &meta);
  free(json);
  return rc ? 0 : -EIO;
}

static int ndb_ingest_ldjson_with_relay(NostrStorage *st, const char *ldjson, size_t len, const char *relay) {
  if (!st || !st->impl || !((NDBImpl*)st->impl)->db || !ldjson) return -EINVAL;
  struct ndb_ingest_meta meta;
  ndb_ingest_meta_init(&meta, 1, relay);
  int rc = ndb_process_events_with(((NDBImpl*)st->impl)->db, ldjson, len, &meta);
  return rc ? 0 : -EIO;
}

static int ndb_delete_event(NostrStorage *st, const char *id_hex) {
  if (!st || !st->impl || !id_hex) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  
  /* Convert hex ID to binary */
  if (strlen(id_hex) != 64) return -EINVAL;
  unsigned char id[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(id_hex + i*2, "%2x", &byte) != 1) return -EINVAL;
    id[i] = (unsigned char)byte;
  }
  
  /* nostrdb doesn't have a direct delete API, but we can mark it as deleted
   * by processing a kind-5 deletion event */
  /* For now, return not supported as proper deletion requires creating a deletion event */
  (void)impl;
  return -ENOTSUP;
}

/* Convert one libnostr filter into an initialised ndb_filter. nostrdb
 * returns nonzero on success throughout, and ndb_filter_from_json() needs a
 * scratch buffer for its JSON tokens. Returns 0 or a negative errno. */
static int build_ndb_filter(const NostrFilter *src, struct ndb_filter *f) {
  if (!ndb_filter_init(f)) return -ENOMEM;
  char *fjson = nostr_filter_serialize_compact(src);
  if (!fjson) { ndb_filter_destroy(f); return -EIO; }
  size_t flen = strlen(fjson);
  /* One jsmn token per few bytes of JSON; generous and bounded by input. */
  size_t scratch_len = flen * 16 + 4096;
  unsigned char *scratch = (unsigned char*)malloc(scratch_len);
  int ok = scratch && ndb_filter_from_json(fjson, (int)flen, f, scratch,
                                           (int)scratch_len);
  free(scratch);
  free(fjson);
  if (!ok) { ndb_filter_destroy(f); return -EINVAL; }
  return 0;
}

static int build_ndb_filters(const NostrFilter *filters, size_t nfilters,
                             struct ndb_filter **out_filters) {
  if (!out_filters) return -EINVAL;
  *out_filters = NULL;
  if (!filters || nfilters == 0) return 0;
  struct ndb_filter *arr = (struct ndb_filter*)calloc(nfilters, sizeof(struct ndb_filter));
  if (!arr) return -ENOMEM;
  for (size_t i = 0; i < nfilters; i++) {
    int rc = build_ndb_filter(&filters[i], &arr[i]);
    if (rc != 0) {
      for (size_t j = 0; j < i; j++) ndb_filter_destroy(&arr[j]);
      free(arr);
      return rc;
    }
  }
  *out_filters = arr;
  return 0;
}
static void* ndb_query_storage(NostrStorage *st, const NostrFilter *filters, size_t nfilters,
                       size_t limit, uint64_t since, uint64_t until, int *err) {
  (void)since; (void)until;
  if (err) *err = 0;
  if (!st || !st->impl) { if (err) *err = -EINVAL; return NULL; }
  NDBImpl *impl = (NDBImpl*)st->impl;
  NDBIter *it = (NDBIter*)calloc(1, sizeof(NDBIter));
  if (!it) { if (err) *err = -ENOMEM; return NULL; }
  it->impl = impl;
  if (!ndb_begin_query(impl->db, &it->txn)) { if (err) *err = -EIO; free(it); return NULL; }
  struct ndb_filter *arr = NULL;
  int rc = build_ndb_filters(filters, nfilters, &arr);
  if (rc != 0) { if (err) *err = rc; ndb_end_query(&it->txn); free(it); return NULL; }
  /* Single-pass with an initial capacity to avoid first-pass failures */
  int capacity = 256;
  if (limit > 0 && (int)limit < capacity) capacity = (int)limit;
  it->results = (struct ndb_query_result*)calloc(capacity > 0 ? capacity : 1, sizeof(struct ndb_query_result));
  if (!it->results) { if (err) *err = -ENOMEM; goto done; }
  it->count = 0; it->index = 0;
  int got = 0;
  rc = ndb_query(&it->txn, arr, (int)nfilters, it->results, capacity, &got);
  if (!rc) {
    if (err) *err = -EIO;
    /* Debug aid */
    fprintf(stderr, "[nostrdb_storage] ndb_query failed for %zu filters\n", nfilters);
    goto done;
  }
  if (limit > 0 && got > (int)limit) got = (int)limit;
  it->count = got;
done:
  if (arr) {
    for (size_t i = 0; i < nfilters; i++) ndb_filter_destroy(&arr[i]);
    free(arr);
  }
  if (err && *err != 0) { if (it->results) free(it->results); ndb_end_query(&it->txn); free(it); return NULL; }
  return it;
}
static int ndb_query_next(NostrStorage *st, void *itp, NostrEvent *out, size_t *n) {
  (void)st;
  if (!itp || !out || !n || *n == 0) return -EINVAL;
  NDBIter *it = (NDBIter*)itp;
  if (it->index >= it->count) { *n = 0; return 0; }
  struct ndb_query_result *qr = &it->results[it->index++];
  /* Convert ndb_note to JSON then to NostrEvent. Escaping can make the JSON
   * larger than the stored note; grow until it fits (bounded). */
  size_t buflen = (size_t)(qr->note_size ? qr->note_size * 2 : 2048);
  char *buf = NULL;
  int w = 0;
  for (; buflen <= (16u << 20); buflen *= 2) {
    char *nb = (char*)realloc(buf, buflen);
    if (!nb) { free(buf); return -ENOMEM; }
    buf = nb;
    w = ndb_note_json(qr->note, buf, (int)buflen);
    if (w > 0) break;
  }
  if (w <= 0) { free(buf); return -EIO; }
  int ok = nostr_event_deserialize(out, buf);
  free(buf);
  if (ok != 0) { *n = 0; return -EIO; }
  *n = 1; return 0;
}
static void ndb_query_free(NostrStorage *st, void *itp) {
  if (!itp) return;
  NDBIter *it = (NDBIter*)itp;
  if (it->results) free(it->results);
  (void)st;
  ndb_end_query(&it->txn);
  free(it);
}

static int ndb_count(NostrStorage *st, const NostrFilter *filters, size_t nfilters, uint64_t *out) {
  if (!st || !st->impl || !out) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  struct ndb_txn txn; if (!ndb_begin_query(impl->db, &txn)) return -EIO;
  struct ndb_filter *arr = NULL; int rc = build_ndb_filters(filters, nfilters, &arr);
  if (rc != 0) { ndb_end_query(&txn); return rc; }
  /* nostrdb has no count primitive: count by querying into a bounded
   * buffer (ndb_query with no buffer reports 0). At the cap the result is
   * a lower bound. */
  enum { NDB_COUNT_CAP = 10000 };
  struct ndb_query_result *res = (struct ndb_query_result*)calloc(NDB_COUNT_CAP, sizeof(*res));
  int count = 0;
  rc = res ? ndb_query(&txn, arr, (int)nfilters, res, NDB_COUNT_CAP, &count) : 0;
  free(res);
  for (size_t i = 0; i < nfilters; i++) ndb_filter_destroy(&arr[i]);
  free(arr);
  ndb_end_query(&txn);
  if (!rc) return -EIO;
  *out = (uint64_t)count;
  return 0;
}

static int ndb_search(NostrStorage *st, const char *q, const NostrFilter *scope, size_t limit, void **it_out) {
  if (it_out) *it_out = NULL;
  if (!st || !st->impl || !q || !it_out) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  NDBIter *it = (NDBIter*)calloc(1, sizeof(NDBIter));
  if (!it) return -ENOMEM;
  it->impl = impl;
  if (!ndb_begin_query(impl->db, &it->txn)) { free(it); return -EIO; }
  struct ndb_text_search_config cfg; ndb_default_text_search_config(&cfg);
  if (limit > 0 && limit < (size_t)cfg.limit) ndb_text_search_config_set_limit(&cfg, (int)limit);
  struct ndb_text_search_results results; memset(&results, 0, sizeof(results));
  int rc;
  if (scope) {
    struct ndb_filter f;
    rc = build_ndb_filter(scope, &f);
    if (rc != 0) { ndb_end_query(&it->txn); free(it); return rc; }
    rc = ndb_text_search_with(&it->txn, q, &results, &cfg, &f);
    ndb_filter_destroy(&f);
  } else {
    rc = ndb_text_search(&it->txn, q, &results, &cfg);
  }
  if (!rc) { ndb_end_query(&it->txn); free(it); return -EIO; } /* 1 == success */
  int count = results.num_results;
  if ((int)limit > 0 && count > (int)limit) count = (int)limit;
  it->results = (struct ndb_query_result*)calloc(count > 0 ? count : 1, sizeof(struct ndb_query_result));
  if (!it->results) { ndb_end_query(&it->txn); free(it); return -ENOMEM; }
  it->count = count; it->index = 0;
  for (int i = 0; i < count; i++) {
    it->results[i].note = results.results[i].note;
    it->results[i].note_size = results.results[i].note_size;
    it->results[i].note_id = results.results[i].key.note_id;
  }
  *it_out = it;
  return 0;
}

static int ndb_set_digest(NostrStorage *st, const NostrFilter *scope, void **state) {
  (void)st; (void)scope; if (state) *state=NULL; return -ENOSYS;
}
static int ndb_set_reconcile(NostrStorage *st, void *state, const void *peer_msg, size_t len,
                             void **resp, size_t *resp_len) {
  (void)st; (void)state; (void)peer_msg; (void)len; if (resp) *resp=NULL; if (resp_len) *resp_len=0; return -ENOSYS;
}
static void ndb_set_free(NostrStorage *st, void *state) { (void)st; (void)state; }

static NostrStorageVTable g_vt = {
  .open = ndb_open,
  .close = ndb_close,
  .put_event = ndb_put_event,
  .ingest_ldjson = ndb_ingest_ldjson,
  .delete_event = ndb_delete_event,
  .query = ndb_query_storage,
  .query_next = ndb_query_next,
  .query_free = ndb_query_free,
  .count = ndb_count,
  .search = ndb_search,
  .set_digest = ndb_set_digest,
  .set_reconcile = ndb_set_reconcile,
  .set_free = ndb_set_free,
  .put_event_with_relay = ndb_put_event_with_relay,
  .ingest_ldjson_with_relay = ndb_ingest_ldjson_with_relay,
};

NostrStorage* nostrdb_storage_new(void) {
  NostrStorage *st = (NostrStorage*)calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->vt = &g_vt;
  return st;
}

__attribute__((constructor))
static void _nostrdb_auto_register(void) {
  nostr_storage_register("nostrdb", nostrdb_storage_new);
}

#endif /* HAVE_NOSTRDB */
