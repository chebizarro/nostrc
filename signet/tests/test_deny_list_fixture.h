/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_TEST_DENY_LIST_FIXTURE_H
#define SIGNET_TEST_DENY_LIST_FIXTURE_H

#include "signet/revocation.h"
#include "signet/store.h"
#include "test_check.h"
#include <sqlite3.h>

/* Load the same persisted state as a daemon starting after a revocation,
 * without revoking the key, leases, or bindings under test. Callers must
 * update any borrowed pointer to the old list after this fixture change. */
static inline SignetDenyList *test_set_denied(SignetStore *store,
                                            SignetDenyList *old_list,
                                            const char *pubkey,
                                            const char *agent_id,
                                            int64_t now, bool denied) {
  sqlite3_stmt *stmt = NULL;
  sqlite3 *db = signet_store_get_db(store);
  const char *sql = denied
      ? "INSERT INTO deny_list(pubkey_hex, agent_id, reason, denied_at) "
        "VALUES (?1, ?2, 'test fixture', ?3)"
      : "DELETE FROM deny_list WHERE pubkey_hex = ?1";
  CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, 1, pubkey, -1, SQLITE_TRANSIENT) == SQLITE_OK);
  if (denied) {
    CHECK(sqlite3_bind_text(stmt, 2, agent_id ? agent_id : pubkey,
                             -1, SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(stmt, 3, now) == SQLITE_OK);
  }
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_changes(db) == 1);
  CHECK(sqlite3_finalize(stmt) == SQLITE_OK);
  SignetDenyList *new_list = signet_deny_list_new(store);
  CHECK(new_list != NULL);
  signet_deny_list_free(old_list);
  return new_list;
}

#endif
