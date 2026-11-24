/* SPDX-License-Identifier: MIT */
#include "test_check.h"
#include "signet/store.h"
#include "signet/store_leases.h"

#include <sqlite3.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
  CHECK(argc == 4);
  bool seed = strcmp(argv[1], "seed") == 0;
  SignetStoreConfig cfg = {.db_path = argv[2], .master_key = argv[3],
                           .read_only = !seed};
  SignetStore *store = signet_store_open(&cfg);
  CHECK(store);
  if (seed) {
    int64_t now = time(NULL);
    int64_t issued = now - 3 * 86400;
    CHECK(signet_store_issue_lease(store, "old", "credential", "agent", issued,
                                  now - 2 * 86400, NULL) == 0);
    CHECK(signet_store_issue_lease(store, "recent", "credential", "agent", issued,
                                  now - 3600, NULL) == 0);
    CHECK(signet_store_issue_lease(store, "active", "credential", "agent", issued,
                                  now + 3600, NULL) == 0);
  } else {
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_prepare_v2(signet_store_get_db(store),
          "SELECT lease_id FROM leases ORDER BY lease_id", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(strcmp((const char *)sqlite3_column_text(stmt, 0), "active") == 0);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(strcmp((const char *)sqlite3_column_text(stmt, 0), "recent") == 0);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
  }
  signet_store_close(store);
  return 0;
}
