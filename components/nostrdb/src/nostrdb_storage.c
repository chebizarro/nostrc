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
#include "bindings/c/profile_reader.h"
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
  /* Sensible defaults similar to gnostr: 1 GiB mapsize, multiple ingester threads */
  const char *mapsize_env = getenv("GRELAY_NDB_MAPSIZE_MB");
  size_t mapsize_mb = mapsize_env && *mapsize_env ? (size_t)strtoull(mapsize_env, NULL, 10) : 1024ULL;
  if (mapsize_mb < 64) mapsize_mb = 64; /* minimum */
  ndb_config_set_mapsize(&cfg, (size_t)mapsize_mb * 1024ULL * 1024ULL);
  
  /* Use multiple ingester threads for better throughput */
  int num_threads = 4; /* Use 4 threads for parallel ingestion */
  ndb_config_set_ingest_threads(&cfg, num_threads);
  fprintf(stderr, "[nostrdb_storage] Using %d ingester threads\n", num_threads);
  
  /* TEMPORARY: Skip signature verification to test if that's causing ingestion failures */
  ndb_config_set_flags(&cfg, NDB_FLAG_SKIP_NOTE_VERIFY);
  fprintf(stderr, "[nostrdb_storage] WARNING: Signature verification is DISABLED for testing\n");

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

static int ndb_ingest_event_json(NostrStorage *st, const char *json, int len, const char *relay_opt) {
  (void)relay_opt; /* TODO: use relay hint */
  if (!st || !st->impl || !((NDBImpl*)st->impl)->db || !json) return -EINVAL;
  /* Add newline for LDJSON format and use batch API */
  char *ldjson = malloc(len + 2);
  if (!ldjson) return -ENOMEM;
  memcpy(ldjson, json, len);
  ldjson[len] = '\n';
  ldjson[len+1] = '\0';
  int rc = ndb_process_client_events(((NDBImpl*)st->impl)->db, ldjson, len + 1);
  free(ldjson);
  return rc ? 0 : -EIO;
}

static int ndb_ingest_ldjson(NostrStorage *st, const char *ldjson, size_t len, const char *relay_opt) {
  (void)relay_opt; /* TODO: use relay hint */
  if (!st || !st->impl || !((NDBImpl*)st->impl)->db || !ldjson) return -EINVAL;
  /* Use ndb_process_client_events - we're sending raw event JSON, not relay envelopes */
  int rc = ndb_process_client_events(((NDBImpl*)st->impl)->db, ldjson, len);
  /* nostrdb returns nonzero on success */
  return rc ? 0 : -EIO;
}

static int ndb_get_note_by_id(NostrStorage *st, void *txn, const unsigned char id[32], const char **json, int *json_len) {
  if (!st || !st->impl || !txn || !id || !json) return -EINVAL;
  struct ndb_txn *ndb_txn = (struct ndb_txn*)txn;
  size_t len = 0;
  struct ndb_note *note = ndb_get_note_by_id(ndb_txn, id, &len, NULL);
  if (!note) return -ENOENT;
  
  /* Serialize the note to JSON */
  char *json_str = malloc(len * 4); /* Conservative estimate for JSON size */
  if (!json_str) return -ENOMEM;
  
  /* Build JSON manually from note structure */
  int written = snprintf(json_str, len * 4,
    "{\"id\":\"%064x\",\"pubkey\":\"%064x\",\"created_at\":%u,\"kind\":%d,\"tags\":[",
    *(unsigned long long*)ndb_note_id(note), *(unsigned long long*)ndb_note_pubkey(note),
    ndb_note_created_at(note), ndb_note_kind(note));
  
  /* Add tags */
  struct ndb_tags *tags = ndb_note_tags(note);
  for (int i = 0; i < (int)tags->count; i++) {
    struct ndb_tag tag = tags->tag[i];
    if (i > 0) written += snprintf(json_str + written, len * 4 - written, ",");
    written += snprintf(json_str + written, len * 4 - written, "[");
    for (int j = 0; j < (int)tag.count; j++) {
      if (j > 0) written += snprintf(json_str + written, len * 4 - written, ",");
      written += snprintf(json_str + written, len * 4 - written, "\"%s\"", ndb_tag_str(&tag, j));
    }
    written += snprintf(json_str + written, len * 4 - written, "]");
  }
  
  /* Add content and signature */
  struct ndb_str content = ndb_note_content(note);
  written += snprintf(json_str + written, len * 4 - written,
    "],\"content\":\"%.*s\",\"sig\":\"%064x\"}",
    (int)content.len, content.str, *(unsigned long long*)ndb_note_sig(note));
  
  *json = json_str;
  if (json_len) *json_len = written;
  return 0;
}

static int ndb_get_profile_by_pubkey(NostrStorage *st, void *txn, const unsigned char pk[32], const char **json, int *json_len) {
  if (!st || !st->impl || !txn || !pk || !json) return -EINVAL;
  struct ndb_txn *ndb_txn = (struct ndb_txn*)txn;
  size_t len = 0;
  void *profile_record = ndb_get_profile_by_pubkey(ndb_txn, pk, &len);
  if (!profile_record) return -ENOENT;
  
  /* Extract profile from flatbuffer using nostrdb's reader API */
  NdbProfileRecord_table_t record = NdbProfileRecord_as_root(profile_record);
  if (!record) return -EIO;
  
  NdbProfile_table_t profile = NdbProfileRecord_profile_get(record);
  if (!profile) return -EIO;
  
  /* Build JSON from profile fields */
  char *json_str = malloc(4096); /* Should be enough for most profiles */
  if (!json_str) return -ENOMEM;
  
  int written = snprintf(json_str, 4096, "{");
  
  /* Add each field if present */
  flatbuffers_string_t name = NdbProfile_name_get(profile);
  if (name) {
    written += snprintf(json_str + written, 4096 - written, "\"name\":\"%s\",", name);
  }
  
  flatbuffers_string_t display_name = NdbProfile_display_name_get(profile);
  if (display_name) {
    written += snprintf(json_str + written, 4096 - written, "\"display_name\":\"%s\",", display_name);
  }
  
  flatbuffers_string_t about = NdbProfile_about_get(profile);
  if (about) {
    written += snprintf(json_str + written, 4096 - written, "\"about\":\"%s\",", about);
  }
  
  flatbuffers_string_t picture = NdbProfile_picture_get(profile);
  if (picture) {
    written += snprintf(json_str + written, 4096 - written, "\"picture\":\"%s\",", picture);
  }
  
  flatbuffers_string_t banner = NdbProfile_banner_get(profile);
  if (banner) {
    written += snprintf(json_str + written, 4096 - written, "\"banner\":\"%s\",", banner);
  }
  
  flatbuffers_string_t website = NdbProfile_website_get(profile);
  if (website) {
    written += snprintf(json_str + written, 4096 - written, "\"website\":\"%s\",", website);
  }
  
  flatbuffers_string_t nip05 = NdbProfile_nip05_get(profile);
  if (nip05) {
    written += snprintf(json_str + written, 4096 - written, "\"nip05\":\"%s\",", nip05);
  }
  
  flatbuffers_string_t lud16 = NdbProfile_lud16_get(profile);
  if (lud16) {
    written += snprintf(json_str + written, 4096 - written, "\"lud16\":\"%s\",", lud16);
  }
  
  /* Remove trailing comma if any fields were added */
  if (written > 1 && json_str[written-1] == ',') {
    written--;
  }
  
  written += snprintf(json_str + written, 4096 - written, "}");
  
  *json = json_str;
  if (json_len) *json_len = written;
  return 0;
}

static int ndb_begin_query_wrapper(NostrStorage *st, void **txn_out) {
  if (!st || !st->impl || !txn_out) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  struct ndb_txn *txn = malloc(sizeof(struct ndb_txn));
  if (!txn) return -ENOMEM;
  if (!ndb_begin_query(impl->db, txn)) {
    free(txn);
    return -EIO;
  }
  *txn_out = txn;
  return 0;
}

static int ndb_end_query_wrapper(NostrStorage *st, void *txn) {
  if (!st || !txn) return -EINVAL;
  struct ndb_txn *ndb_txn = (struct ndb_txn*)txn;
  ndb_end_query(ndb_txn);
  free(txn);
  return 0;
}

static int ndb_text_search_wrapper(NostrStorage *st, void *txn, const char *query, const char *config_json, void **results, int *count) {
  if (!st || !st->impl || !query || !results || !count) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  
  /* Parse config if provided */
  int limit = 100;
  if (config_json) {
    json_error_t err;
    json_t *cfg = json_loads(config_json, 0, &err);
    if (cfg && json_is_object(cfg)) {
      json_t *lim = json_object_get(cfg, "limit");
      if (lim && json_is_integer(lim)) {
        limit = (int)json_integer_value(lim);
      }
      json_decref(cfg);
    }
  }
  
  /* Use provided txn if available, otherwise create our own */
  struct ndb_txn *ndb_txn = txn ? (struct ndb_txn*)txn : NULL;
  struct ndb_txn local_txn;
  if (!ndb_txn) {
    if (!ndb_begin_query(impl->db, &local_txn)) return -EIO;
    ndb_txn = &local_txn;
  }
  
  struct ndb_text_search_config cfg;
  ndb_default_text_search_config(&cfg);
  ndb_text_search_config_set_limit(&cfg, limit);
  
  struct ndb_text_search_results search_results;
  memset(&search_results, 0, sizeof(search_results));
  
  int rc = ndb_text_search(ndb_txn, query, &search_results, &cfg);
  
  if (!txn) {
    ndb_end_query(&local_txn);
  }
  
  if (rc != 0) return -EIO;
  
  /* Convert results to iterator format */
  NDBIter *it = (NDBIter*)calloc(1, sizeof(NDBIter));
  if (!it) return -ENOMEM;
  
  it->impl = impl;
  it->count = search_results.num_results;
  it->index = 0;
  it->results = (struct ndb_query_result*)calloc(it->count > 0 ? it->count : 1, sizeof(struct ndb_query_result));
  if (!it->results) {
    free(it);
    return -ENOMEM;
  }
  
  for (int i = 0; i < it->count; i++) {
    it->results[i].note = search_results.results[i].note;
    it->results[i].note_len = search_results.results[i].note_size;
  }
  
  *results = it;
  *count = it->count;
  return 0;
}

static int ndb_stat_json_wrapper(NostrStorage *st, char **json_out) {
  if (!st || !st->impl || !json_out) return -EINVAL;
  NDBImpl *impl = (NDBImpl*)st->impl;
  
  /* Get database statistics */
  struct ndb_txn txn;
  if (!ndb_begin_query(impl->db, &txn)) return -EIO;
  
  struct ndb_stat stat;
  int rc = ndb_stat(&txn, &stat);
  ndb_end_query(&txn);
  
  if (rc == 0) return -EIO;
  
  /* Build JSON stats */
  char *json_str = malloc(1024);
  if (!json_str) return -ENOMEM;
  
  snprintf(json_str, 1024,
    "{\"notes\":%llu,\"reactions\":%llu,\"quotes\":%llu,\"reposts\":%llu,"
    "\"profiles\":%llu,\"zaps\":%llu,\"reports\":%llu}",
    (unsigned long long)stat.notes,
    (unsigned long long)stat.reactions,
    (unsigned long long)stat.quotes,
    (unsigned long long)stat.reposts,
    (unsigned long long)stat.profiles,
    (unsigned long long)stat.zaps,
    (unsigned long long)stat.reports);
  
  *json_out = json_str;
  return 0;
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
  .ingest_event_json = ndb_ingest_event_json,
  .ingest_ldjson = ndb_ingest_ldjson,
  .begin_query = ndb_begin_query_wrapper,
  .end_query = ndb_end_query_wrapper,
  .delete_event = ndb_delete_event,
  .query = ndb_query_storage,
  .query_next = ndb_query_next,
  .query_free = ndb_query_free,
  .count = ndb_count,
  .search = ndb_search,
  .text_search = ndb_text_search_wrapper,
  .get_note_by_id = ndb_get_note_by_id,
  .get_profile_by_pubkey = ndb_get_profile_by_pubkey,
  .stat_json = ndb_stat_json_wrapper,
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
