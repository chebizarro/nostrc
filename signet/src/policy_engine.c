/* SPDX-License-Identifier: MIT
 *
 * policy_engine.c - Policy evaluation (Phase 4: file-backed policy + auditing).
 */

#include "signet/policy_engine.h"
#include "signet/policy_store.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>

struct SignetPolicyEngine {
  SignetPolicyStore *store;
  SignetAuditLogger *audit;
  SignetPolicyDecision default_decision;
};

static const char *signet_policy_decision_to_string(SignetPolicyDecision d) {
  return (d == SIGNET_POLICY_DECISION_ALLOW) ? "allow" : "deny";
}

SignetPolicyEngine *signet_policy_engine_new(SignetPolicyStore *store,
                                             SignetAuditLogger *audit,
                                             const SignetPolicyEngineConfig *cfg) {
  SignetPolicyEngine *pe = (SignetPolicyEngine *)calloc(1, sizeof(*pe));
  if (!pe) return NULL;

  pe->store = store;
  pe->audit = audit;
  pe->default_decision = cfg ? cfg->default_decision : SIGNET_POLICY_DECISION_DENY;

  return pe;
}

void signet_policy_engine_free(SignetPolicyEngine *pe) {
  if (!pe) return;
  free(pe);
}

bool signet_policy_engine_eval(SignetPolicyEngine *pe,
                               const char *identity,
                               const char *client_pubkey_hex,
                               const char *method,
                               int event_kind,
                               int64_t now,
                               SignetPolicyResult *out_result) {
  if (!pe || !identity || !client_pubkey_hex || !method || !out_result) return false;

  out_result->decision = SIGNET_POLICY_DECISION_DENY;
  out_result->expires_at = 0;
  out_result->reason_code = "policy.error";

  /* Strong default: if we cannot consult policy, deny. */
  if (!pe->store) {
    out_result->decision = SIGNET_POLICY_DECISION_DENY;
    out_result->reason_code = "policy.no_store";
  } else {
    SignetPolicyKeyView key = {
      .identity = identity,
      .client_pubkey_hex = client_pubkey_hex,
      .method = method,
      .event_kind = event_kind
    };

    SignetPolicyValue val;
    memset(&val, 0, sizeof(val));

    int rc = signet_policy_store_get(pe->store, &key, now, &val);
    if (rc == 0) {
      out_result->decision = (val.decision == SIGNET_POLICY_RULE_ALLOW) ? SIGNET_POLICY_DECISION_ALLOW : SIGNET_POLICY_DECISION_DENY;
      out_result->expires_at = val.expires_at;
      out_result->reason_code = val.reason_code ? val.reason_code : "policy.computed";
    } else if (rc == 1) {
      /* Identity missing or no policy file: required behavior is default-deny. */
      out_result->decision = SIGNET_POLICY_DECISION_DENY;
      out_result->expires_at = 0;
      out_result->reason_code = "policy.identity_missing";
    } else {
      /* Store error: deny. */
      out_result->decision = SIGNET_POLICY_DECISION_DENY;
      out_result->expires_at = 0;
      out_result->reason_code = "policy.store_error";
    }
  }

  /* Audit every decision (never include secrets). */
  if (pe->audit) {
    SignetAuditCommonFields f;
    memset(&f, 0, sizeof(f));
    f.client_pubkey_hex = client_pubkey_hex;
    f.identity = identity;
    f.method = method;
    f.event_kind = event_kind;
    f.decision = signet_policy_decision_to_string(out_result->decision);
    f.reason_code = out_result->reason_code;

    (void)signet_audit_log_common(pe->audit,
                                 SIGNET_AUDIT_EVENT_POLICY_DECISION,
                                 &f,
                                 NULL);
  }

  return true;
}