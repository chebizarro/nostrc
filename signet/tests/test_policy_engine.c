/* SPDX-License-Identifier: MIT
 *
 * test_policy_engine.c - Policy engine tests
 */

#include "signet/policy_engine.h"
#include "signet/policy_store.h"
#include "signet/audit_logger.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_policy_deny_by_default(void) {
  /* Create a minimal policy store with no policies */
  SignetPolicyStore *ps = signet_policy_store_file_new("/nonexistent/path");
  assert(ps != NULL);

  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);

  SignetPolicyEngineConfig cfg = { .default_decision = SIGNET_POLICY_DECISION_DENY };
  SignetPolicyEngine *pe = signet_policy_engine_new(ps, audit, &cfg);
  assert(pe != NULL);

  int64_t now = (int64_t)time(NULL);
  SignetPolicyResult result;

  /* Should deny when no policy exists */
  bool ok = signet_policy_engine_eval(pe, "nonexistent_identity", "client_pubkey", "sign_event", 1, now, &result);
  assert(ok == true);
  assert(result.decision == SIGNET_POLICY_DECISION_DENY);

  signet_policy_engine_free(pe);
  signet_audit_logger_free(audit);
  signet_policy_store_free(ps);
  printf("test_policy_deny_by_default: PASS\n");
}

static void test_policy_null_safety(void) {
  SignetPolicyResult result;

  /* NULL policy engine should fail safely */
  bool ok = signet_policy_engine_eval(NULL, "identity", "client", "method", 1, 0, &result);
  assert(ok == false);

  printf("test_policy_null_safety: PASS\n");
}

int main(void) {
  test_policy_deny_by_default();
  test_policy_null_safety();
  printf("All policy engine tests passed!\n");
  return 0;
}
