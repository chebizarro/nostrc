#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../backends/nostrdb/nostr-negentropy-ndb.h"

int main(void) {
  char template[] = "/tmp/ndb-test-XXXXXX";
  char *dbdir = mkdtemp(template);
  assert(dbdir != NULL);

  NostrNegDataSource ds; int r = nostr_ndb_make_datasource(dbdir, &ds);
  if (r != 0) {
    printf("skipped: nostrdb backend unavailable or failed to init at %s\n", dbdir);
    return 0;
  }
  if (ds.begin_iter) ds.begin_iter(ds.ctx);
  if (ds.end_iter) ds.end_iter(ds.ctx);
  printf("ok ndb stub\n");
  return 0;
}
