/* SPDX-License-Identifier: MIT
 *
 * test_git_credential.c - Git credential helper canary.
 *
 * Exercises the credential helper protocol engine end-to-end against a real
 * encrypted store through the unified credential-access path (the same
 * enforcement signetd applies for the installed D-Bus-backed binary):
 * an agent-bound GitHub PAT stored as api_token is retrieved for `get`,
 * the token appears ONLY on the credential protocol stream, wrong-agent
 * and revoked paths fail closed with audit entries, rotation is
 * transparent, and the audit chain never contains the token.
 */

#include "signet/capability.h"
#include "signet/credential_access.h"
#include "signet/git_credential.h"
#include "signet/revocation.h"
#include "signet/store.h"
#include "signet/store_audit.h"
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
#define PUBKEY_GIT "1111111111111111111111111111111111111111111111111111111111111111"
#define PUBKEY_OTHER "2222222222222222222222222222222222222222222222222222222222222222"

#define PAT_V1 "ghp_canaryTokenV1_0123456789abcdef"
#define PAT_V2 "ghp_canaryTokenV2_fedcba9876543210"

static char *temp_db_path(void) {
  char tmpl[] = "/tmp/signet-git-cred-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

/* Test lookup: same enforcement chain the daemon applies for the installed
 * D-Bus helper — unified credential access with the explicit
 * credential.get_token capability. */
typedef struct {
  SignetCredentialAccessContext acc;
  const char *agent_id;
  int64_t now;
} LookupCtx;

static int test_lookup(const SignetGitCredentialQuery *query,
                       void *user_data,
                       char **out_username,
                       char **out_password) {
  LookupCtx *lc = (LookupCtx *)user_data;
  if (!query->host || !query->host[0]) return 1;

  char cred_id[70];
  if (signet_git_credential_derive_id(lc->agent_id, query->host,
                                      cred_id) != 0)
    return 1;

  SignetCredentialAccessRequest req = {
    .agent_id = lc->agent_id,
    .credential_id = cred_id,
    .capability = SIGNET_CAP_CREDENTIAL_GET_TOKEN,
    .transport = "git_helper",
    .issue_lease = true,
    .lease_ttl_seconds = 300,
  };
  SignetCredentialAccessGrant grant;
  if (signet_credential_access_acquire(&lc->acc, &req, lc->now, &grant) !=
      SIGNET_CRED_ACCESS_OK)
    return 1;

  *out_password = g_strndup((const char *)grant.record.payload,
                            grant.record.payload_len);
  *out_username = g_strdup("x-access-token");
  signet_credential_access_grant_clear(&grant);
  return 0;
}

/* Run one serve() exchange; returns captured stdout (caller g_free). */
static char *run_serve(const char *action, const char *input,
                       LookupCtx *lc, int *out_rc) {
  FILE *in = fmemopen((void *)input, strlen(input), "r");
  assert(in);
  char *buf = NULL;
  size_t buf_len = 0;
  FILE *out = open_memstream(&buf, &buf_len);
  assert(out);
  *out_rc = signet_git_credential_serve(action, in, out, test_lookup, lc);
  fclose(in);
  fclose(out);
  return buf; /* NUL-terminated by open_memstream */
}

static int count_occurrences(const char *haystack, const char *needle) {
  int count = 0;
  const char *p = haystack;
  while ((p = strstr(p, needle)) != NULL) {
    count++;
    p += strlen(needle);
  }
  return count;
}

static int audit_rows_containing(SignetStore *store, const char *needle) {
  sqlite3 *db = signet_store_get_db(store);
  assert(db);
  const char *sql =
      "SELECT COUNT(*) FROM audit_log WHERE "
      "instr(COALESCE(agent_id,''), ?1) > 0 OR "
      "instr(COALESCE(operation,''), ?1) > 0 OR "
      "instr(COALESCE(secret_id,''), ?1) > 0 OR "
      "instr(COALESCE(detail,''), ?1) > 0;";
  sqlite3_stmt *stmt = NULL;
  assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, needle, -1, SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

int main(void) {
  assert(sodium_init() >= 0);

  char *db_path = temp_db_path();
  SignetStoreConfig scfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&scfg);
  assert(store);

  int64_t now = 2000000000;

  /* Agents. */
  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  assert(signet_store_put_agent_ex(store, "agent-git", sk, sizeof(sk),
                                   "conn-git", PUBKEY_GIT, "provisioned",
                                   now) == 0);
  randombytes_buf(sk, sizeof(sk));
  assert(signet_store_put_agent_ex(store, "agent-other", sk, sizeof(sk),
                                   "conn-other", PUBKEY_OTHER, "provisioned",
                                   now) == 0);
  sodium_memzero(sk, sizeof(sk));

  /* Least-privilege policy: agent-git may retrieve tokens; agent-other has a
   * policy without that capability. */
  SignetPolicyRegistry *policy = signet_policy_registry_new();
  assert(policy);
  const char *token_caps[] = { SIGNET_CAP_CREDENTIAL_GET_TOKEN };
  const char *ssh_caps[] = { SIGNET_CAP_SSH_SIGN };
  SignetAgentPolicy pol;
  memset(&pol, 0, sizeof(pol));
  pol.name = (char *)"git-token";
  pol.capabilities = (char **)token_caps;
  pol.n_capabilities = 1;
  assert(signet_policy_registry_add(policy, &pol) == 0);
  memset(&pol, 0, sizeof(pol));
  pol.name = (char *)"no-token";
  pol.capabilities = (char **)ssh_caps;
  pol.n_capabilities = 1;
  assert(signet_policy_registry_add(policy, &pol) == 0);
  assert(signet_policy_registry_assign(policy, "agent-git", "git-token") == 0);
  assert(signet_policy_registry_assign(policy, "agent-other", "no-token") == 0);

  /* Store the PAT as an api_token bound to agent-git with the derived
   * deterministic id for host github.com. */
  SignetSecretMetadata meta;
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_create_secret(store, "agent-git",
                                    SIGNET_SECRET_API_TOKEN,
                                    "git:github.com",
                                    (const uint8_t *)PAT_V1, strlen(PAT_V1),
                                    NULL, 0, "created", NULL, now,
                                    &meta) == SIGNET_SECRET_OK);
  char derived[70];
  assert(signet_git_credential_derive_id("agent-git", "github.com",
                                         derived) == 0);
  assert(strcmp(derived, meta.id) == 0); /* deterministic id == stored id */
  signet_secret_metadata_clear(&meta);

  LookupCtx lc = {
    .acc = { .store = store, .policy = policy, .deny = NULL, .logger = NULL },
    .agent_id = "agent-git",
    .now = now,
  };

  static const char GET_INPUT[] = "protocol=https\nhost=github.com\n\n";
  int rc = -1;

  /* 1. Canary: authenticated get releases the PAT on the protocol stream
   * (exactly once) and nowhere else. */
  char *out = run_serve("get", GET_INPUT, &lc, &rc);
  assert(rc == 0);
  assert(count_occurrences(out, "password=" PAT_V1 "\n") == 1);
  assert(count_occurrences(out, PAT_V1) == 1);
  assert(count_occurrences(out, "username=x-access-token\n") == 1);
  g_free(out);

  /* 2. store/erase are no-ops: git can never push a secret INTO signet or
   * delete one; nothing is echoed. */
  out = run_serve("store",
                  "protocol=https\nhost=github.com\n"
                  "username=x\npassword=attacker-supplied\n\n",
                  &lc, &rc);
  assert(rc == 0);
  assert(out[0] == '\0');
  g_free(out);
  out = run_serve("erase", GET_INPUT, &lc, &rc);
  assert(rc == 0);
  assert(out[0] == '\0');
  g_free(out);

  /* 3. Unknown host: quiet miss, no output. */
  out = run_serve("get", "protocol=https\nhost=gitlab.example\n\n", &lc, &rc);
  assert(rc == 0);
  assert(out[0] == '\0');
  g_free(out);

  /* 4. Wrong agent (no capability, not owner): quiet refusal, no token. */
  LookupCtx other = lc;
  other.agent_id = "agent-other";
  out = run_serve("get", GET_INPUT, &other, &rc);
  assert(rc == 0);
  assert(out[0] == '\0');
  assert(strstr(out, PAT_V1) == NULL);
  g_free(out);

  /* 5. Rotation-safe: same derived id serves the ACTIVE version. */
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_rotate_secret_ex(store, derived,
                                       (const uint8_t *)PAT_V2,
                                       strlen(PAT_V2), false, 0, now + 10,
                                       &meta) == SIGNET_SECRET_OK);
  signet_secret_metadata_clear(&meta);
  lc.now = now + 20;
  out = run_serve("get", GET_INPUT, &lc, &rc);
  assert(rc == 0);
  assert(count_occurrences(out, "password=" PAT_V2 "\n") == 1);
  assert(strstr(out, PAT_V1) == NULL);
  g_free(out);

  /* 6. Revoked PAT: helper goes quiet, fails closed. */
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_revoke_secret(store, derived, now + 30, &meta) ==
         SIGNET_SECRET_OK);
  signet_secret_metadata_clear(&meta);
  lc.now = now + 40;
  out = run_serve("get", GET_INPUT, &lc, &rc);
  assert(rc == 0);
  assert(out[0] == '\0');
  g_free(out);

  /* 7. Audit: every retrieval outcome is chained; the PAT never appears in
   * the audit trail; deny paths are recorded. */
  int64_t broken_id = 0;
  assert(signet_audit_verify_chain(store, 0, 0, &broken_id) == 0);
  assert(signet_audit_log_count(store) >= 4); /* allow, deny, allow, revoked */
  assert(audit_rows_containing(store, PAT_V1) == 0);
  assert(audit_rows_containing(store, PAT_V2) == 0);
  assert(audit_rows_containing(store, "ghp_") == 0);
  assert(audit_rows_containing(store, "\"decision\":\"allow\"") > 0);
  assert(audit_rows_containing(store, "\"reason\":\"no_capability\"") > 0);
  assert(audit_rows_containing(store, "\"reason\":\"revoked\"") > 0);

  signet_policy_registry_free(policy);
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);

  printf("test_git_credential: OK\n");
  return 0;
}
