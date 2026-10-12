/* SPDX-License-Identifier: MIT
 *
 * test_policy_engine.c - Policy engine tests
 */

#include "signet/policy_engine.h"
#include "signet/policy_store.h"
#include "signet/audit_logger.h"

#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>

static void test_policy_deny_by_default(void) {
  /* Create a minimal policy store with no policies */
  SignetPolicyStore *ps = signet_policy_store_file_new("/nonexistent/path");
  CHECK(ps != NULL);

  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);

  SignetPolicyEngineConfig cfg = { .default_decision = SIGNET_POLICY_DECISION_DENY };
  SignetPolicyEngine *pe = signet_policy_engine_new(ps, audit, &cfg);
  CHECK(pe != NULL);

  int64_t now = (int64_t)time(NULL);
  SignetPolicyResult result;

  /* Should deny when no policy exists */
  bool ok = signet_policy_engine_eval(pe, "nonexistent_identity", "client_pubkey", "sign_event", 1, now, &result);
  CHECK(ok == true);
  CHECK(result.decision == SIGNET_POLICY_DECISION_DENY);

  signet_policy_engine_free(pe);
  signet_audit_logger_free(audit);
  signet_policy_store_free(ps);
  printf("test_policy_deny_by_default: PASS\n");
}

static void test_policy_null_safety(void) {
  SignetPolicyResult result;

  /* NULL policy engine should fail safely */
  bool ok = signet_policy_engine_eval(NULL, "identity", "client", "method", 1, 0, &result);
  CHECK(ok == false);

  printf("test_policy_null_safety: PASS\n");
}

#define CLIENT_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define CLIENT_D "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

static SignetPolicyRuleDecision decide(SignetPolicyStore *ps,
                                       const char *identity,
                                       const char *client, int kind) {
  SignetPolicyKeyView key = {
    .identity = identity, .client_pubkey_hex = client,
    .method = "sign_event", .event_kind = kind,
  };
  SignetPolicyValue v;
  CHECK(signet_policy_store_get(ps, &key, 2000000000, &v) == 0);
  return v.decision;
}

/* Review (nostrc-v13e0, HIGH): an allow list whose entries cannot be read —
 * a label, a malformed npub, a hex typo, a non-string, unparseable JSON —
 * must not vanish and turn the remaining allow_methods into "anyone may
 * sign". It stays configured, matches only its readable entries, and
 * survives a write-back. */
static void test_unreadable_allow_entries_fail_closed(void) {
  char tmpl[] = "/tmp/signet-policy-lossy-XXXXXX";
  int fd = mkstemp(tmpl);
  CHECK(fd >= 0);
  close(fd);
  const char *file =
      "[identity.label]\ndefault=deny\nallow_clients=[\"bahia-prod\"]\n"
      "allow_methods=[\"sign_event\"]\n"
      "[identity.npub]\ndefault=deny\nallow_clients=[\"npub1qqqqmalformed\"]\n"
      "allow_methods=[\"sign_event\"]\n"
      "[identity.typo]\ndefault=deny\nallow_clients=[\"ccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"]\n"
      "allow_methods=[\"sign_event\"]\n"
      "[identity.number]\ndefault=deny\nallow_clients=[7]\n"
      "allow_methods=[\"sign_event\"]\n"
      "[identity.empty]\ndefault=deny\nallow_clients=[\"\"]\n"
      "allow_methods=[\"sign_event\"]\n"
      "[identity.broken]\ndefault=deny\nallow_clients=[\"cccc\n"
      "allow_methods=[\"sign_event\"]\n"
      "[identity.mixed]\ndefault=deny\nallow_clients=[\"" CLIENT_C "\",\"garbage\"]\n"
      "allow_methods=[\"sign_event\"]\n"
      "[identity.kinds]\ndefault=deny\nallow_clients=[\"" CLIENT_C "\"]\n"
      "allow_methods=[\"sign_event\"]\nallow_kinds=[\"one\"]\n"
      "[identity.somekinds]\ndefault=deny\nallow_clients=[\"" CLIENT_C "\"]\n"
      "allow_methods=[\"sign_event\"]\nallow_kinds=[1,\"one\"]\n"
      "[identity.method]\ndefault=deny\nallow_clients=[\"" CLIENT_C "\"]\n"
      "allow_methods=[5]\n";
  CHECK(g_file_set_contents(tmpl, file, -1, NULL));
  SignetPolicyStore *ps = signet_policy_store_file_new(tmpl);
  CHECK(ps);
  CHECK(signet_policy_store_reload(ps, 2000000000) == 0);

  const char *closed[] = { "label", "npub", "typo", "number", "empty",
                           "broken" };
  for (int round = 0; round < 2; round++) {
    for (size_t i = 0; i < G_N_ELEMENTS(closed); i++) {
      CHECK(decide(ps, closed[i], CLIENT_C, 1) == SIGNET_POLICY_RULE_DENY);
      CHECK(decide(ps, closed[i], CLIENT_D, 1) == SIGNET_POLICY_RULE_DENY);
    }
    CHECK(decide(ps, "mixed", CLIENT_C, 1) == SIGNET_POLICY_RULE_ALLOW);
    CHECK(decide(ps, "mixed", CLIENT_D, 1) == SIGNET_POLICY_RULE_DENY);
    CHECK(decide(ps, "kinds", CLIENT_C, 1) == SIGNET_POLICY_RULE_DENY);
    CHECK(decide(ps, "somekinds", CLIENT_C, 1) == SIGNET_POLICY_RULE_ALLOW);
    CHECK(decide(ps, "somekinds", CLIENT_C, 2) == SIGNET_POLICY_RULE_DENY);
    CHECK(decide(ps, "method", CLIENT_C, 1) == SIGNET_POLICY_RULE_DENY);
    if (round == 1) break;
    /* set-policy for another identity rewrites the whole file; the
     * placeholders must survive it and the reload that follows. */
    char *err = NULL;
    CHECK(signet_policy_store_set_identity_json(
        ps, "other", "{\"default\":\"deny\"}", 2000000000, &err) == 0);
    CHECK(err == NULL);
    CHECK(signet_policy_store_reload(ps, 2000000000) == 0);
  }
  g_autofree char *written = NULL;
  CHECK(g_file_get_contents(tmpl, &written, NULL, NULL));
  CHECK(strstr(written, "bahia-prod") != NULL);
  CHECK(strstr(written, "npub1qqqqmalformed") != NULL);

  /* set-policy refuses kind entries it could not store as written. */
  const char *bad_kinds[] = {
    "{\"default\":\"deny\",\"allow_kinds\":[\"one\"]}",
    "{\"default\":\"deny\",\"allow_kinds\":[1.5]}",
    "{\"default\":\"deny\",\"deny_kinds\":[-3]}",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(bad_kinds); i++) {
    char *err = NULL;
    CHECK(signet_policy_store_set_identity_json(
        ps, "bad", bad_kinds[i], 2000000000, &err) == -1);
    g_free(err);
  }

  signet_policy_store_free(ps);
  unlink(tmpl);
  printf("test_unreadable_allow_entries_fail_closed: PASS\n");
}

int main(void) {
  test_policy_deny_by_default();
  test_policy_null_safety();
  test_unreadable_allow_entries_fail_closed();
  printf("All policy engine tests passed!\n");
  return 0;
}
