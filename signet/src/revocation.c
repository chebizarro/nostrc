/* SPDX-License-Identifier: MIT
 *
 * revocation.c - Agent revocation for Signet v2.
 *
 * Deny list backed by SQLCipher. Emergency and normal revocation paths
 * that burn leases, revoke keys, and audit log.
 */

#include "signet/revocation.h"
#include "signet/health_server.h"  /* g_signet_metrics */
#include "signet/store.h"
#include "signet/store_leases.h"
#include "signet/store_audit.h"
#include "signet/key_store.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <sqlite3.h>

/* ----------------------------- deny list --------------------------------- */

struct SignetDenyList {
  SignetStore *store;
  GHashTable *cache;  /* pubkey_hex → gboolean (in-memory fast check) */
  GMutex mu;
};

SignetDenyList *signet_deny_list_new(SignetStore *store) {
  if (!store) return NULL;

  SignetDenyList *dl = g_new0(SignetDenyList, 1);
  if (!dl) return NULL;

  dl->store = store;
  g_mutex_init(&dl->mu);
  dl->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  /* Load existing deny list entries from DB into cache. */
  struct sqlite3 *db = signet_store_get_db(store);
  if (db) {
    const char *sql = "SELECT pubkey_hex, agent_id FROM deny_list";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *pubkey = (const char *)sqlite3_column_text(stmt, 0);
        const char *aid = (const char *)sqlite3_column_text(stmt, 1);
        if (pubkey)
          g_hash_table_replace(dl->cache, g_strdup(pubkey),
                               GINT_TO_POINTER(1));
        if (aid)
          g_hash_table_replace(dl->cache, g_strdup(aid),
                               GINT_TO_POINTER(1));
      }
      sqlite3_finalize(stmt);
    }
  }

  return dl;
}

void signet_deny_list_free(SignetDenyList *dl) {
  if (!dl) return;
  g_mutex_lock(&dl->mu);
  g_hash_table_destroy(dl->cache);
  g_mutex_unlock(&dl->mu);
  g_mutex_clear(&dl->mu);
  g_free(dl);
}

int signet_deny_list_add(SignetDenyList *dl,
                          const char *pubkey_hex,
                          const char *agent_id,
                          const char *reason,
                          int64_t now) {
  if (!dl || !pubkey_hex) return -1;
  const char *id = agent_id ? agent_id : pubkey_hex;

  struct sqlite3 *db = signet_store_get_db(dl->store);
  if (!db) return -1;

  const char *sql =
      "INSERT OR REPLACE INTO deny_list "
      "(pubkey_hex, agent_id, reason, denied_at) "
      "VALUES (?1, ?2, ?3, ?4)";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, pubkey_hex, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
  if (reason) {
    sqlite3_bind_text(stmt, 3, reason, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 3);
  }
  sqlite3_bind_int64(stmt, 4, now);

  int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
  sqlite3_finalize(stmt);

  if (rc == 0) {
    g_mutex_lock(&dl->mu);
    g_hash_table_replace(dl->cache, g_strdup(pubkey_hex),
                         GINT_TO_POINTER(1));
    if (agent_id)
      g_hash_table_replace(dl->cache, g_strdup(agent_id),
                           GINT_TO_POINTER(1));
    g_mutex_unlock(&dl->mu);
  }

  return rc;
}

int signet_deny_list_remove(SignetDenyList *dl, const char *pubkey_hex) {
  if (!dl || !pubkey_hex) return -1;

  struct sqlite3 *db = signet_store_get_db(dl->store);
  if (!db) return -1;

  const char *sql =
      "DELETE FROM deny_list WHERE pubkey_hex = ?1";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, pubkey_hex, -1, SQLITE_TRANSIENT);
  int step_rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);

  if (step_rc != SQLITE_DONE) return -1;
  if (changes == 0) return 1; /* not found */

  g_mutex_lock(&dl->mu);
  g_hash_table_remove(dl->cache, pubkey_hex);
  g_mutex_unlock(&dl->mu);

  return 0;
}

bool signet_deny_list_contains(SignetDenyList *dl, const char *pubkey_hex) {
  if (!dl || !pubkey_hex) return false;
  g_mutex_lock(&dl->mu);
  bool found = g_hash_table_contains(dl->cache, pubkey_hex);
  g_mutex_unlock(&dl->mu);
  return found;
}

/* ----------------------------- revocation -------------------------------- */

static int signet_revoke_internal(SignetStore *store,
                                   SignetKeyStore *keys,
                                   SignetDenyList *deny,
                                   SignetAuditLogger *audit,
                                   const char *agent_id,
                                   const char *pubkey_hex,
                                   const char *reason,
                                   int64_t now,
                                   bool emergency) {
  if (!store || !agent_id) return -1;
  int rc = 0;

  /* Increment revocation counter. */
  g_atomic_int_inc(&g_signet_metrics.revoke_total);

  /* 1. Add to deny list. */
  if (deny && pubkey_hex) {
    if (signet_deny_list_add(deny, pubkey_hex, agent_id, reason, now) < 0)
      rc = -1;
  }

  /* 2. Revoke all leases for this agent. */
  int lease_rc = signet_store_revoke_agent_leases(store, agent_id, now);
  if (lease_rc < 0) rc = -1;

  /* 3. Revoke key from key store (removes from hot cache + SQLCipher). */
  if (keys) {
    int key_rc = signet_key_store_revoke_agent(keys, agent_id);
    if (key_rc < 0) rc = -1;
  }

  /* 4. Audit log. */
  if (audit) {
    char *detail = g_strdup_printf(
        "{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"reason\":\"%s\","
        "\"emergency\":%s,\"leases_revoked\":%d}",
        agent_id,
        pubkey_hex ? pubkey_hex : "",
        reason ? reason : "",
        emergency ? "true" : "false",
        lease_rc > 0 ? lease_rc : 0);

    signet_audit_log_append(store, now, agent_id,
                            emergency ? "emergency_revoke" : "normal_revoke",
                            NULL /* secret_id */,
                            NULL /* transport */,
                            detail);
    g_free(detail);
  }

  return rc;
}

int signet_revoke_agent(SignetStore *store,
                         SignetKeyStore *keys,
                         SignetDenyList *deny,
                         SignetAuditLogger *audit,
                         const char *agent_id,
                         const char *pubkey_hex,
                         const char *reason,
                         int64_t now) {
  return signet_revoke_internal(store, keys, deny, audit,
                                 agent_id, pubkey_hex, reason, now,
                                 true /* emergency */);
}

int signet_revoke_agent_normal(SignetStore *store,
                                SignetKeyStore *keys,
                                SignetDenyList *deny,
                                SignetAuditLogger *audit,
                                const char *agent_id,
                                const char *pubkey_hex,
                                int64_t now) {
  return signet_revoke_internal(store, keys, deny, audit,
                                 agent_id, pubkey_hex,
                                 "removed from fleet list", now,
                                 false /* normal */);
}
