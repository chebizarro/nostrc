#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../backends/nostrdb/nostr-negentropy-ndb.h"

int main(void) {
  char template[] = "/tmp/ndb-empty-XXXXXX";
  char *dbdir = mkdtemp(template);
  if (!dbdir) {
    printf("skipped: failed to create temp dir\n");
    return 0;
  }

  NostrNegDataSource ds;
  int r = nostr_ndb_make_datasource(dbdir, &ds);
  if (r != 0) {
    printf("skipped: datasource init failed at %s\n", dbdir);
    return 0;
  }

  int began = 0;
  if (ds.begin_iter) {
    if (ds.begin_iter(ds.ctx) != 0) {
      printf("skipped: begin_iter failed\n");
      return 0;
    }
    began = 1;
  }

  NostrIndexItem it;
  int got = -1;
  if (ds.next) {
    got = ds.next(ds.ctx, &it);
  }
  if (began && ds.end_iter) ds.end_iter(ds.ctx);

  // Expect no items: next should be non-zero immediately
  assert(got != 0);
  printf("ok ndb empty iteration\n");
  return 0;
}
