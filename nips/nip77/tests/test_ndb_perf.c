#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "../backends/nostrdb/nostr-negentropy-ndb.h"
#include <nostrdb.h>

static double now_seconds(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}

static int ingest(struct ndb *db, size_t n){
  // Create n simple events with created_at increasing and ids with leading 0x8 (to avoid collisions)
  char ev[256];
  for (size_t i=0;i<n;i++){
    char id[65]; memset(id, '0', 64); id[64]='\0';
    id[0] = '8';
    // vary positions 1..3 in hex
    const char hexnib[16] = "0123456789abcdef";
    id[1] = hexnib[(i>>0) & 0xF];
    id[2] = hexnib[(i>>4) & 0xF];
    id[3] = hexnib[(i>>8) & 0xF];
    snprintf(ev, sizeof(ev),
      "{\n  \"id\": \"%s\",\n  \"pubkey\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n  \"created_at\": %zu,\n  \"kind\": 1,\n  \"tags\": [],\n  \"content\": \"perf%zu\",\n  \"sig\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"\n}",
      id, 1+i, i);
    if (ndb_process_event(db, ev, (int)strlen(ev)) != 0) return -1;
  }
  return 0;
}

static int ensure_dir(const char *path){
  if (!path || !*path) return -1;
  if (mkdir(path, 0700) == 0) return 0;
  if (errno == EEXIST) return 0;
  return -1;
}

int main(void){
  const char *run = getenv("NIP77_RUN_PERF");
  if (!run || !*run) { printf("skipped: set NIP77_RUN_PERF=1 to run perf test\n"); return 0; }

  size_t n = 100000; // 100k events by default, tweak via env
  const char *envn = getenv("NIP77_PERF_N");
  if (envn && *envn) {
    char *end=NULL; long long val = strtoll(envn, &end, 10);
    if (end && *end=='\0' && val>0) n = (size_t)val;
  }

  char tmpl[1024];
  const char *base = getenv("NIP77_PERF_TMPDIR");
  if (base && *base) {
    if (ensure_dir(base) != 0) {
      fprintf(stderr, "perf: failed to create base dir '%s': %s\n", base, strerror(errno));
      printf("skipped: cannot create base temp dir\n");
      return 0;
    }
    snprintf(tmpl, sizeof(tmpl), "%s/ndb-perf-XXXXXX", base);
  } else {
    snprintf(tmpl, sizeof(tmpl), "/tmp/ndb-perf-XXXXXX");
  }
  char *dbdir = mkdtemp(tmpl);
  if (!dbdir) { printf("skipped: mkdtemp failed\n"); return 0; }

  struct ndb *db=NULL; struct ndb_config cfg; ndb_default_config(&cfg);
  ndb_config_set_flags(&cfg, NDB_FLAG_NO_FULLTEXT | NDB_FLAG_NO_NOTE_BLOCKS | NDB_FLAG_NO_STATS | NDB_FLAG_SKIP_NOTE_VERIFY);
  // Allow larger mapsize to avoid growth overhead
  unsigned long long mapsize = (unsigned long long)(n * 512ull); if (mapsize < (64ull<<20)) mapsize = (64ull<<20);
  ndb_config_set_mapsize(&cfg, mapsize);
  unsigned long flags = (unsigned long)(NDB_FLAG_NO_FULLTEXT | NDB_FLAG_NO_NOTE_BLOCKS | NDB_FLAG_NO_STATS | NDB_FLAG_SKIP_NOTE_VERIFY);
  if (ndb_init(&db, dbdir, &cfg) != 0) {
    fprintf(stderr, "perf: ndb_init('%s') failed (flags=0x%lx, mapsize=%llu): %s\n", dbdir, flags, mapsize, strerror(errno));
    printf("skipped: ndb_init failed for %s\n", dbdir);
    return 0;
  }

  double t0 = now_seconds();
  if ( ingest(db, n) != 0 ) { printf("skipped: ingestion failed\n"); ndb_destroy(db); return 0; }
  double t1 = now_seconds();
  double ingest_sec = t1 - t0;

  // Build datasource and iterate fully
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
  size_t count = 0; NostrIndexItem it;
  double t2 = now_seconds();
  if (ds.next) while (ds.next(ds.ctx, &it) == 0) { count++; }
  double t3 = now_seconds();
  double iter_sec = t3 - t2;
  if (ds.end_iter) ds.end_iter(ds.ctx);
  ndb_destroy(db);

  // Output simple metrics line for parsing
  printf("perf: n=%zu ingest_sec=%.3f iter_sec=%.3f iter_throughput=%.0f items/s\n",
         n, ingest_sec, iter_sec, (iter_sec>0.0? (double)count/iter_sec : 0.0));
  // Basic sanity check
  assert(count == n);
  return 0;
}
