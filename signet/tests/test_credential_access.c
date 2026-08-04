/* SPDX-License-Identifier: MIT
 *
 * test_credential_access.c - Item 2 authorization/audit/lease acceptance.
 *
 * Covers the unified credential-access path: explicit capability, deny-list
 * precedence, owner-before-decrypt, type deny rules, expiry/revocation,
 * one-use lease burn, rate limiting, and the mandatory hash-chained audit
 * entry on every outcome (with redaction: payload bytes never reach the
 * audit trail), plus concurrent chain appends.
 */

#include "signet/capability.h"
#include "signet/credential_access.h"
#include "signet/revocation.h"
#include "signet/store.h"
#include "signet/store_audit.h"
#include "signet/store_leases.h"
#include "signet/store_secrets.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>
#include <sqlite3.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

#define PUBKEY_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PUBKEY_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PUBKEY_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define PUBKEY_E "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

#define PAYLOAD_CANARY "tok-A-superSecretValue-9000"

static char *temp_db_path(void) {
  char tmpl[] = "/tmp/signet-cred-access-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static void add_agent(SignetStore *store, const char *agent_id,
                      const char *pubkey, const char *connect_secret,
                      int64_t now) {
  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  assert(signet_store_put_agent_ex(store, agent_id, sk, sizeof(sk),
                                   connect_secret, pubkey, "provisioned",
                                   now) == 0);
  sodium_memzero(sk, sizeof(sk));
}

static void add_policy(SignetPolicyRegistry *pr, const char *name,
                       const char **caps, size_t n_caps,
                       const char **denied_types, size_t n_denied,
                       uint32_t rate_limit) {
  SignetAgentPolicy pol;
  memset(&pol, 0, sizeof(pol));
  pol.name = (char *)name;
  pol.capabilities = (char **)caps;
  pol.n_capabilities = n_caps;
  pol.disallowed_credential_types = (char **)denied_types;
  pol.n_disallowed_types = n_denied;
  pol.rate_limit_per_hour = rate_limit;
  assert(signet_policy_registry_add(pr, &pol) == 0);
}

/* Count audit rows whose any TEXT column contains needle. */
static int audit_rows_containing(SignetStore *store, const char *needle) {
  sqlite3 *db = signet_store_get_db(store);
  assert(db);
  const char *sql =
      "SELECT COUNT(*) FROM audit_log WHERE "
      "instr(COALESCE(agent_id,''), ?1) > 0 OR "
      "instr(COALESCE(operation,''), ?1) > 0 OR "
      "instr(COALESCE(secret_id,''), ?1) > 0 OR "
      "instr(COALESCE(transport,''), ?1) > 0 OR "
      "instr(COALESCE(detail,''), ?1) > 0;";
  sqlite3_stmt *stmt = NULL;
  assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, needle, -1, SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static SignetCredAccessStatus acquire(const SignetCredentialAccessContext *ctx,
                                      const char *agent_id,
                                      const char *cred_id,
                                      const char *capability,
                                      const char *lease_id,
                                      bool issue_lease,
                                      int64_t now,
                                      SignetCredentialAccessGrant *grant) {
  SignetCredentialAccessRequest req = {
    .agent_id = agent_id,
    .credential_id = cred_id,
    .capability = capability,
    .transport = "test",
    .lease_id = lease_id,
    .issue_lease = issue_lease,
    .lease_ttl_seconds = 600,
  };
  return signet_credential_access_acquire(ctx, &req, now, grant);
}

typedef struct {
  SignetStore *store;
  int rounds;
  int index;
} AppendArg;

static gpointer append_thread(gpointer data) {
  AppendArg *arg = (AppendArg *)data;
  for (int i = 0; i < arg->rounds; i++) {
    char op[64];
    snprintf(op, sizeof(op), "concurrent_append_%d_%d", arg->index, i);
    assert(signet_audit_log_append(arg->store, 2000000000 + i,
                                   "thread-agent", op, NULL, "test",
                                   "{\"decision\":\"allow\"}") == 0);
  }
  return NULL;
}

int main(void) {
  assert(sodium_init() >= 0);

  char *db_path = temp_db_path();
  SignetStoreConfig scfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&scfg);
  assert(store);

  int64_t now = 2000000000;
  add_agent(store, "agent-a", PUBKEY_A, "conn-a", now);
  add_agent(store, "agent-b", PUBKEY_B, "conn-b", now);
  add_agent(store, "agent-c", PUBKEY_C, "conn-c", now);
  add_agent(store, "agent-e", PUBKEY_E, "conn-e", now);

  SignetPolicyRegistry *policy = signet_policy_registry_new();
  assert(policy);
  const char *token_caps[] = { SIGNET_CAP_CREDENTIAL_GET_TOKEN };
  const char *no_caps[] = { SIGNET_CAP_SSH_SIGN };
  const char *denied_types[] = { "api_token" };
  add_policy(policy, "token-access", token_caps, 1, NULL, 0, 0);
  add_policy(policy, "no-token-cap", no_caps, 1, NULL, 0, 0);
  add_policy(policy, "deny-api-token", token_caps, 1, denied_types, 1, 0);
  add_policy(policy, "limited", token_caps, 1, NULL, 0, 1);
  assert(signet_policy_registry_assign(policy, "agent-a", "token-access") == 0);
  assert(signet_policy_registry_assign(policy, "agent-b", "token-access") == 0);
  assert(signet_policy_registry_assign(policy, "agent-c", "no-token-cap") == 0);
  assert(signet_policy_registry_assign(policy, "agent-e", "limited") == 0);

  SignetDenyList *deny = signet_deny_list_new(store);
  assert(deny);

  SignetCredentialAccessContext ctx = {
    .store = store, .policy = policy, .deny = deny, .logger = NULL,
  };

  /* Credential owned by agent-a. */
  SignetSecretMetadata meta;
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_create_secret(store, "agent-a",
                                    SIGNET_SECRET_API_TOKEN, "primary",
                                    (const uint8_t *)PAYLOAD_CANARY,
                                    strlen(PAYLOAD_CANARY), NULL,
                                    now + 3600, "created", NULL, now,
                                    &meta) == SIGNET_SECRET_OK);
  char *cred_a = g_strdup(meta.id);
  signet_secret_metadata_clear(&meta);

  /* Credential owned by agent-e (rate-limit case). */
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_create_secret(store, "agent-e",
                                    SIGNET_SECRET_API_TOKEN, "limited",
                                    (const uint8_t *)"tok-E", 5, NULL, 0,
                                    "created", NULL, now,
                                    &meta) == SIGNET_SECRET_OK);
  char *cred_e = g_strdup(meta.id);
  signet_secret_metadata_clear(&meta);

  int64_t audit_before = signet_audit_log_count(store);
  assert(audit_before >= 0);
  int64_t expected_entries = audit_before;
  SignetCredentialAccessGrant grant;

  /* 1. Owner with capability: allow; payload released; lease issued. */
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, true, now, &grant) == SIGNET_CRED_ACCESS_OK);
  expected_entries++;
  assert(grant.record.payload_len == strlen(PAYLOAD_CANARY));
  assert(memcmp(grant.record.payload, PAYLOAD_CANARY,
                grant.record.payload_len) == 0);
  assert(grant.lease_id && strlen(grant.lease_id) == 32);
  assert(grant.lease_expires_at == now + 600);
  char *lease1 = g_strdup(grant.lease_id);
  signet_credential_access_grant_clear(&grant);

  /* 2. One-use lease burn: first presentation consumes it... */
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 lease1, false, now + 1, &grant) == SIGNET_CRED_ACCESS_OK);
  expected_entries++;
  signet_credential_access_grant_clear(&grant);
  /* ...second presentation of the SAME lease fails closed. */
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 lease1, false, now + 2, &grant) ==
         SIGNET_CRED_ACCESS_LEASE_INVALID);
  expected_entries++;

  /* 3. Wrong agent: fails closed BEFORE decrypt; no payload. */
  assert(acquire(&ctx, "agent-b", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now, &grant) == SIGNET_CRED_ACCESS_NOT_OWNER);
  expected_entries++;
  assert(grant.record.payload == NULL);

  /* 4. A foreign lease cannot be burned by another agent. */
  assert(signet_store_issue_lease(store, "foreignlease00000000000000000001",
                                  cred_a, "agent-a", now, now + 600,
                                  NULL) == 0);
  assert(acquire(&ctx, "agent-b", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 "foreignlease00000000000000000001", false, now, &grant) ==
         SIGNET_CRED_ACCESS_NOT_OWNER);
  expected_entries++;
  /* The owner's lease survives the stranger's attempt. */
  assert(signet_store_consume_lease(store,
                                    "foreignlease00000000000000000001",
                                    cred_a, "agent-a", now) == 0);

  /* 5. Missing capability grant: deny. */
  assert(acquire(&ctx, "agent-c", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now, &grant) ==
         SIGNET_CRED_ACCESS_NO_CAPABILITY);
  expected_entries++;

  /* 6. Type deny rules outrank a granted capability. */
  assert(signet_policy_registry_assign(policy, "agent-a",
                                       "deny-api-token") == 0);
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now, &grant) == SIGNET_CRED_ACCESS_TYPE_DENIED);
  expected_entries++;
  assert(signet_policy_registry_assign(policy, "agent-a",
                                       "token-access") == 0);

  /* 7. Deny-list precedence: capability held, still refused. */
  assert(signet_deny_list_add(deny, PUBKEY_A, "agent-a", "test", now) == 0);
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now, &grant) == SIGNET_CRED_ACCESS_DENY_LISTED);
  expected_entries++;
  assert(signet_deny_list_remove(deny, PUBKEY_A) == 0);

  /* 8. Expiry enforced at retrieval. */
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now + 7200, &grant) ==
         SIGNET_CRED_ACCESS_EXPIRED);
  expected_entries++;

  /* 9. Revocation, and revocation outranks expiry. */
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_revoke_secret(store, cred_a, now, &meta) ==
         SIGNET_SECRET_OK);
  signet_secret_metadata_clear(&meta);
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now, &grant) == SIGNET_CRED_ACCESS_REVOKED);
  expected_entries++;
  assert(acquire(&ctx, "agent-a", cred_a, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now + 7200, &grant) ==
         SIGNET_CRED_ACCESS_REVOKED);
  expected_entries++;

  /* 10. Unknown credential. */
  assert(acquire(&ctx, "agent-a", "cred_does_not_exist",
                 SIGNET_CAP_CREDENTIAL_GET_TOKEN, NULL, false, now,
                 &grant) == SIGNET_CRED_ACCESS_NOT_FOUND);
  expected_entries++;

  /* 11. Unknown caller identity: fails closed. */
  assert(acquire(&ctx, "agent-ghost", cred_a,
                 SIGNET_CAP_CREDENTIAL_GET_TOKEN, NULL, false, now,
                 &grant) == SIGNET_CRED_ACCESS_NOT_OWNER);
  expected_entries++;

  /* 12. No policy registry at all: fail closed. */
  SignetCredentialAccessContext no_policy_ctx = {
    .store = store, .policy = NULL, .deny = deny, .logger = NULL,
  };
  assert(acquire(&no_policy_ctx, "agent-a", cred_a,
                 SIGNET_CAP_CREDENTIAL_GET_TOKEN, NULL, false, now,
                 &grant) == SIGNET_CRED_ACCESS_NO_CAPABILITY);
  expected_entries++;

  /* 13. Missing explicit capability in the request: error, still audited. */
  assert(acquire(&ctx, "agent-a", cred_a, NULL, NULL, false, now, &grant) ==
         SIGNET_CRED_ACCESS_ERROR);
  expected_entries++;

  /* 14. Rate limiting (limit 1/hour => burst 1). */
  assert(acquire(&ctx, "agent-e", cred_e, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now, &grant) == SIGNET_CRED_ACCESS_OK);
  expected_entries++;
  signet_credential_access_grant_clear(&grant);
  assert(acquire(&ctx, "agent-e", cred_e, SIGNET_CAP_CREDENTIAL_GET_TOKEN,
                 NULL, false, now, &grant) ==
         SIGNET_CRED_ACCESS_RATE_LIMITED);
  expected_entries++;

  /* Every outcome above appended exactly one hash-chain entry. */
  assert(signet_audit_log_count(store) == expected_entries);

  /* Chain is intact and redacted: the payload canary never reached it. */
  int64_t broken_id = 0;
  assert(signet_audit_verify_chain(store, 0, 0, &broken_id) == 0);
  assert(audit_rows_containing(store, PAYLOAD_CANARY) == 0);
  assert(audit_rows_containing(store, "superSecret") == 0);
  /* Sanity: the probe finds strings that ARE present. */
  assert(audit_rows_containing(store, "credential_access") > 0);
  assert(audit_rows_containing(store, "\"decision\":\"deny\"") > 0);
  assert(audit_rows_containing(store, "\"reason\":\"deny_listed\"") > 0);
  assert(audit_rows_containing(store, "\"reason\":\"not_owner\"") > 0);

  /* 15. Concurrent chain appends stay linear and verifiable. */
  enum { N_THREADS = 8, ROUNDS = 40 };
  GThread *threads[N_THREADS];
  AppendArg args[N_THREADS];
  for (int i = 0; i < N_THREADS; i++) {
    args[i].store = store;
    args[i].rounds = ROUNDS;
    args[i].index = i;
    threads[i] = g_thread_new("append", append_thread, &args[i]);
  }
  for (int i = 0; i < N_THREADS; i++)
    g_thread_join(threads[i]);
  assert(signet_audit_log_count(store) ==
         expected_entries + N_THREADS * ROUNDS);
  broken_id = 0;
  assert(signet_audit_verify_chain(store, 0, 0, &broken_id) == 0);

  g_free(lease1);
  g_free(cred_a);
  g_free(cred_e);
  signet_deny_list_free(deny);
  signet_policy_registry_free(policy);
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);

  printf("test_credential_access: OK\n");
  return 0;
}
