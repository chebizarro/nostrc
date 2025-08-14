#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libnostr_store.h"
#include "libnostr_errors.h"

static void free_json_array(char **arr, int n) {
  if (!arr) return;
  for (int i = 0; i < n; i++) free(arr[i]);
  free(arr);
}

int main(int argc, char **argv) {
  const char *dbdir = argc > 1 ? argv[1] : ".ndb-demo";
  const char *opts = (argc > 2 && argv[2] && argv[2][0]) ? argv[2] : "{\"mapsize\":1073741824,\"ingester_threads\":1}"; /* 1 GiB */

  ln_store *store = NULL;
  printf("using dbdir=%s opts=%s\n", dbdir, opts);
  int rc = ln_store_open("nostrdb", dbdir, opts, &store);
  if (rc != LN_OK) { fprintf(stderr, "open failed: %d\n", rc); return 1; }

  /* Print initial stats */
  char *stats = NULL;
  if (ln_store_stat_json(store, &stats) == LN_OK && stats) {
    printf("initial stats: %s\n", stats);
    free(stats);
  }

  /* Ingest a couple of sample events via client-events NDJSON (writer path) */
  const char *ev1 =
    "{"
    "\"id\":\"0000000000000000000000000000000000000000000000000000000000000001\"," 
    "\"pubkey\":\"0000000000000000000000000000000000000000000000000000000000000000\"," 
    "\"created_at\": 1731540000,"
    "\"kind\": 1,"
    "\"tags\": [],"
    "\"content\": \"hello from demo\","
    "\"sig\": \"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\""
    "}";
  const char *ev2 =
    "{"
    "\"id\":\"0000000000000000000000000000000000000000000000000000000000000002\"," 
    "\"pubkey\":\"0000000000000000000000000000000000000000000000000000000000000000\"," 
    "\"created_at\": 1731540001,"
    "\"kind\": 1,"
    "\"tags\": [],"
    "\"content\": \"world from demo\","
    "\"sig\": \"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\""
    "}";

  char *ldjson = NULL;
  size_t ldlen = asprintf(&ldjson, "[\"EVENT\",%s]\n[\"EVENT\",%s]\n", ev1, ev2);
  if (!ldjson || ldlen == (size_t)-1) { fprintf(stderr, "oom building ldjson\n"); ln_store_close(store); return 1; }
  int irc = ln_store_ingest_ldjson(store, ldjson, ldlen, NULL);
  printf("ingest ldjson rc=%d\n", irc);
  free(ldjson);

  /* Print stats after ingestion */
  stats = NULL;
  if (ln_store_stat_json(store, &stats) == LN_OK && stats) {
    printf("post-ingest stats: %s\n", stats);
    free(stats);
  }

  /* Give writer thread time to flush ingested events */
  #ifdef _WIN32
    Sleep(500);
  #else
    usleep(500000);
  #endif

  void *txn = NULL;
  rc = ln_store_begin_query(store, &txn);
  if (rc != LN_OK) { fprintf(stderr, "begin_query failed: %d\n", rc); ln_store_close(store); return 1; }

  /* Single filter */
  const char *filter1 = "{\"kinds\":[1],\"limit\":10}";
  void *results = NULL; int count = 0;
  rc = ln_store_query(store, txn, filter1, &results, &count);
  if (rc == LN_OK) {
    printf("single filter results: %d\n", count);
    free_json_array((char**)results, count);
  } else {
    printf("single filter query rc=%d\n", rc);
  }

  /* Multiple filters */
  const char *filters = "[{\"kinds\":[1],\"limit\":5},{\"kinds\":[6],\"limit\":5}]";
  results = NULL; count = 0;
  rc = ln_store_query(store, txn, filters, &results, &count);
  if (rc == LN_OK) {
    printf("multi filter results: %d\n", count);
    free_json_array((char**)results, count);
  } else {
    printf("multi filter query rc=%d\n", rc);
  }

  /* Text search */
  const char *q = "hello";
  const char *cfg = "{\"limit\":16,\"order\":\"desc\"}";
  results = NULL; count = 0;
  rc = ln_store_text_search(store, txn, q, cfg, &results, &count);
  if (rc == LN_OK) {
    printf("text search results: %d\n", count);
    free_json_array((char**)results, count);
  } else {
    printf("text search rc=%d\n", rc);
  }

  ln_store_end_query(store, txn);
  ln_store_close(store);
  return 0;
}
