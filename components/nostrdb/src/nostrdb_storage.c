#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "nostr-storage.h"
#include "nostrdb_storage.h"
#include "json.h"
#include "nostr-filter.h"
#if __has_include("nostrdb.h")
#include "nostrdb.h"
#define HAVE_NOSTRDB 1
#else
#define HAVE_NOSTRDB 0
#endif

#if !HAVE_NOSTRDB
/* Stub implementation when nostrdb is not available; registers backend but returns NULL */
NostrStorage* nostrdb_storage_new(void) { return NULL; }
__attribute__((constructor))
static void _nostrdb_auto_register(void) { nostr_storage_register("nostrdb", nostrdb_storage_new); }
#else

typedef struct {
  char *uri;
  char *opts;
  struct ndb *db;
} NDBImpl;

typedef struct {
  NDBImpl *impl;
  struct ndb_txn txn;
  struct ndb_query_result *results;
  int count;
  int index;
} NDBIter;

static int ndb_open(NostrStorage *st, const char *uri, const char *opts_json) {
  if (!st) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)calloc(1, sizeof(*impl));
  if (!impl) return -ENOMEM;
  if (uri) impl->uri = strdup(uri);
  if (opts_json) impl->opts = strdup(opts_json);
  st->impl = impl;

  /* Ensure directory exists (simple mkdir -p behavior) */
  const char *path = impl->uri ? impl->uri : ".ndb";
#ifdef _WIN32
  (void)path; /* Caller should ensure path exists on Windows */
#else
  {
    char cmd[4096];
    /* No popen; rely on mkdir(2) via POSIX headers is not included here. Keep minimal: try to create leaf dir. */
    /* Best-effort: if it fails, ndb_init will report the cause */
    (void)cmd;
  }
#endif

  /* Initialize nostrdb with robust defaults */
  struct ndb_config cfg; ndb_default_config(&cfg);
  /* Sensible defaults similar to gnostr: 1 GiB mapsize, 1 ingester thread */
  const char *mapsize_env = getenv("GRELAY_NDB_MAPSIZE_MB");
  size_t mapsize_mb = mapsize_env && *mapsize_env ? (size_t)strtoull(mapsize_env, NULL, 10) : 1024ULL;
  if (mapsize_mb < 64) mapsize_mb = 64; /* minimum */
  ndb_config_set_mapsize(&cfg, (size_t)mapsize_mb * 1024ULL * 1024ULL);
  ndb_config_set_ingest_threads(&cfg, 1);

  int rc = ndb_init(&impl->db, path, &cfg);
  /* ndb_init returns nonzero on success, zero on failure */
  if (rc == 0) {
    fprintf(stderr, "[nostrdb_storage] ndb_init(path=%s) failed rc=%d\n", path, rc);
    return -EIO;
  }
  return 0;
}

static void ndb_close(NostrStorage *st) {
  if (!st) return;
  NDBImpl *impl = (NDBImpl*)st->impl;
  if (impl) {
    if (impl->db) { ndb_destroy(impl->db); impl->db = NULL; }
    free(impl->uri);
    free(impl->opts);
    free(impl);
    st->impl = NULL;
  }
}

static int ndb_put_event(NostrStorage *st, const NostrEvent *ev) {
  if (!st || !st->impl || !((NDBImpl*)st->impl)->db || !ev) return -EINVAL;
  char *json = nostr_event_serialize(ev);
  if (!json) return -EIO;
  int rc = ndb_process_event(((NDBImpl*)st->impl)->db, json, (int)strlen(json));
  free(json);
  /* nostrdb returns nonzero on success */
  return rc ? 0 : -EIO;
}

static int ndb_delete_event(NostrStorage *st, const char *id_hex) {
  (void)st; (void)id_hex; return -ENOTSUP;
}

static int build_ndb_filters(const NostrFilter *filters, size_t nfilters,
                             struct ndb_filter **out_filters) {
  if (!out_filters) return -EINVAL;
  *out_filters = NULL;
  if (!filters || nfilters == 0) return 0;
  struct ndb_filter *arr = (struct ndb_filter*)calloc(nfilters, sizeof(struct ndb_filter));
  if (!arr) return -ENOMEM;
  for (size_t i = 0; i < nfilters; i++) {
    struct ndb_filter *f = &arr[i];
    if (ndb_filter_init_with(f, 1) != 0) { free(arr); return -EIO; }
    char *fjson = nostr_filter_serialize_compact(&filters[i]);
    if (!fjson) { ndb_filter_destroy(f); free(arr); return -EIO; }
    int rc = ndb_filter_from_json(fjson, (int)strlen(fjson), f, NULL, 0);
    free(fjson);
    if (rc != 0) { ndb_filter_destroy(f); free(arr); return -EIO; }
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
  /* Convert ndb_note to JSON then to NostrEvent */
  size_t buflen = (size_t)(qr->note_size ? qr->note_size * 2 : 2048);
  char *buf = (char*)malloc(buflen);
  if (!buf) return -ENOMEM;
  int w = ndb_note_json(qr->note, buf, (int)buflen);
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
  int count = 0;
  rc = ndb_query(&txn, arr, (int)nfilters, NULL, 0, &count);
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
    struct ndb_filter f; if (ndb_filter_init_with(&f, 1) != 0) { ndb_end_query(&it->txn); free(it); return -EIO; }
    char *fjson = nostr_filter_serialize_compact(scope);
    if (!fjson) { ndb_filter_destroy(&f); ndb_end_query(&it->txn); free(it); return -EIO; }
    rc = ndb_filter_from_json(fjson, (int)strlen(fjson), &f, NULL, 0);
    free(fjson);
    if (rc != 0) { ndb_filter_destroy(&f); ndb_end_query(&it->txn); free(it); return -EIO; }
    rc = ndb_text_search_with(&it->txn, q, &results, &cfg, &f);
    ndb_filter_destroy(&f);
  } else {
    rc = ndb_text_search(&it->txn, q, &results, &cfg);
  }
  if (rc != 0) { ndb_end_query(&it->txn); free(it); return -EIO; }
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
  .delete_event = ndb_delete_event,
  .query = ndb_query_storage,
  .query_next = ndb_query_next,
  .query_free = ndb_query_free,
  .count = ndb_count,
  .search = ndb_search,
  .set_digest = ndb_set_digest,
  .set_reconcile = ndb_set_reconcile,
  .set_free = ndb_set_free,
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
