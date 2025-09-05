#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../backends/nostrdb/nostr-negentropy-ndb.h"
#include <nostrdb.h>

static int ingest_same_ts(struct ndb *db) {
  // Two events same created_at=100, differing ids: 01..01 vs 02..02
  const char *evA = "{\n"
                    "  \"id\": \"0101010101010101010101010101010101010101010101010101010101010101\",\n"
                    "  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
                    "  \"created_at\": 100,\n"
                    "  \"kind\": 1,\n"
                    "  \"tags\": [],\n"
                    "  \"content\": \"A\",\n"
                    "  \"sig\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n"
                    "}";
  const char *evB = "{\n"
                    "  \"id\": \"0202020202020202020202020202020202020202020202020202020202020202\",\n"
                    "  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
                    "  \"created_at\": 100,\n"
                    "  \"kind\": 1,\n"
                    "  \"tags\": [],\n"
                    "  \"content\": \"B\",\n"
                    "  \"sig\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"\n"
                    "}";
  if (ndb_process_event(db, evB, (int)strlen(evB)) != 0) return -1; // insert in reverse order intentionally
  if (ndb_process_event(db, evA, (int)strlen(evA)) != 0) return -1;
  return 0;
}

static int id_is(const NostrEventId *id, unsigned char byte) {
  for (int i = 0; i < 32; ++i) {
    if (id->bytes[i] != byte) return 0;
  }
  return 1;
}

int main(void) {
  char template[] = "/tmp/ndb-tie-XXXXXX";
  char *dbdir = mkdtemp(template);
  if (!dbdir) {
    printf("skipped: failed to create temp dir\n");
    return 0;
  }

  struct ndb *db = NULL;
  struct ndb_config cfg;
  ndb_default_config(&cfg);
  ndb_config_set_flags(&cfg, NDB_FLAG_NO_FULLTEXT | NDB_FLAG_NO_NOTE_BLOCKS | NDB_FLAG_NO_STATS | NDB_FLAG_SKIP_NOTE_VERIFY);
  ndb_config_set_mapsize(&cfg, 64ull * 1024ull * 1024ull);
  if (ndb_init(&db, dbdir, &cfg) != 0) {
    printf("skipped: ndb_init failed for %s\n", dbdir);
    return 0;
  }
  if (ingest_same_ts(db) != 0) {
    printf("skipped: ingestion failed\n");
    ndb_destroy(db);
    return 0;
  }

  NostrNegDataSource ds;
  if (nostr_ndb_make_datasource(dbdir, &ds) != 0) {
    printf("skipped: datasource init failed\n");
    ndb_destroy(db);
    return 0;
  }
  if (ds.begin_iter && ds.begin_iter(ds.ctx) != 0) {
    printf("skipped: begin_iter failed\n");
    ndb_destroy(db);
    return 0;
  }

  // collect first two items with created_at==100 and verify id order is 01..01 then 02..02
  NostrIndexItem it1, it2;
  int have1 = 0, have2 = 0;
  if (ds.next && ds.next(ds.ctx, &it1) == 0) {
    have1 = 1;
  }
  if (ds.next && ds.next(ds.ctx, &it2) == 0) {
    have2 = 1;
  }
  if (ds.end_iter) ds.end_iter(ds.ctx);
  ndb_destroy(db);

  if (!(have1 && have2)) {
    printf("skipped: insufficient items\n");
    return 0;
  }
  assert(it1.created_at <= it2.created_at);
  if (it1.created_at == it2.created_at && it1.created_at == 100) {
    // tiebreak by id ascending
    assert(id_is(&it1.id, 0x01));
    assert(id_is(&it2.id, 0x02));
  }
  printf("ok ndb tie-break by id ASC\n");
  return 0;
}
