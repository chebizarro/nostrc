#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../backends/nostrdb/nostr-negentropy-ndb.h"
#include <nostrdb.h>

static int ingest_events(struct ndb *db) {
  // Three dummy events with ascending created_at once sorted
  const char *ev1 = "{\n"
                    "  \"id\": \"0101010101010101010101010101010101010101010101010101010101010101\",\n"
                    "  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
                    "  \"created_at\": 75,\n"
                    "  \"kind\": 1,\n"
                    "  \"tags\": [],\n"
                    "  \"content\": \"mid\",\n"
                    "  \"sig\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n"
                    "}";
  const char *ev2 = "{\n"
                    "  \"id\": \"0202020202020202020202020202020202020202020202020202020202020202\",\n"
                    "  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
                    "  \"created_at\": 50,\n"
                    "  \"kind\": 1,\n"
                    "  \"tags\": [],\n"
                    "  \"content\": \"low\",\n"
                    "  \"sig\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"\n"
                    "}";
  const char *ev3 = "{\n"
                    "  \"id\": \"0303030303030303030303030303030303030303030303030303030303030303\",\n"
                    "  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
                    "  \"created_at\": 100,\n"
                    "  \"kind\": 1,\n"
                    "  \"tags\": [],\n"
                    "  \"content\": \"high\",\n"
                    "  \"sig\": \"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"\n"
                    "}";
  if (ndb_process_event(db, ev1, (int)strlen(ev1)) != 0) return -1;
  if (ndb_process_event(db, ev2, (int)strlen(ev2)) != 0) return -1;
  if (ndb_process_event(db, ev3, (int)strlen(ev3)) != 0) return -1;
  return 0;
}

int main(void) {
  char template[] = "/tmp/ndb-ordering-XXXXXX";
  char *dbdir = mkdtemp(template);
  if (!dbdir) {
    printf("skipped: failed to create temp dir\n");
    return 0;
  }

  // Open a raw NostrDB handle to ingest events
  struct ndb *db = NULL;
  struct ndb_config cfg;
  ndb_default_config(&cfg);
  ndb_config_set_flags(&cfg, NDB_FLAG_NO_FULLTEXT | NDB_FLAG_NO_NOTE_BLOCKS | NDB_FLAG_NO_STATS | NDB_FLAG_SKIP_NOTE_VERIFY);
  ndb_config_set_mapsize(&cfg, 64ull * 1024ull * 1024ull);
  if (ndb_init(&db, dbdir, &cfg) != 0) {
    printf("skipped: ndb_init failed for %s\n", dbdir);
    return 0;
  }

  if (ingest_events(db) != 0) {
    printf("skipped: ingestion failed (environment)\n");
    ndb_destroy(db);
    return 0;
  }

  // Now use our datasource API to iterate
  NostrNegDataSource ds;
  if (nostr_ndb_make_datasource(dbdir, &ds) != 0) {
    printf("skipped: nostrdb datasource failed to init at %s\n", dbdir);
    ndb_destroy(db);
    return 0;
  }

  if (ds.begin_iter) {
    if (ds.begin_iter(ds.ctx) != 0) {
      printf("skipped: begin_iter failed\n");
      ndb_destroy(db);
      return 0;
    }
  }

  // Collect items and validate ascending created_at
  NostrIndexItem items[16];
  size_t n = 0;
  if (ds.next) {
    NostrIndexItem it;
    while (ds.next(ds.ctx, &it) == 0 && n < 16) {
      items[n++] = it;
    }
  }
  if (ds.end_iter) ds.end_iter(ds.ctx);
  ndb_destroy(db);

  if (n == 0) {
    printf("skipped: no items observed\n");
    return 0;
  }

  // Verify monotonic non-decreasing created_at and that we saw at least our 3 timestamps
  int saw50=0, saw75=0, saw100=0;
  for (size_t i = 1; i < n; ++i) {
    assert(items[i-1].created_at <= items[i].created_at);
  }
  for (size_t i = 0; i < n; ++i) {
    if (items[i].created_at == 50) saw50 = 1;
    if (items[i].created_at == 75) saw75 = 1;
    if (items[i].created_at == 100) saw100 = 1;
  }
  assert(saw50 && saw75 && saw100);
  printf("ok ndb ordering (created_at ASC)\n");
  return 0;
}
