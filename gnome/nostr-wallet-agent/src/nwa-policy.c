/* nwa-policy.c - see nwa-policy.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-policy.h"

static NwaDecision
decision(NwaDecisionKind kind, NwaDenyReason reason)
{
  NwaDecision d = { kind, reason, FALSE };
  return d;
}

/* Ask when a UI exists, otherwise fail closed. */
static NwaDecision
prompt_or_deny(const NwaPolicyInput *in)
{
  return in->ui_available ? decision(NWA_DECISION_PROMPT, NWA_DENY_NONE)
                          : decision(NWA_DECISION_DENY, NWA_DENY_NO_UI);
}

NwaDecision
nwa_policy_decide(const NwaPolicyInput *in)
{
  /* 1. Only the session's own user may use the wallet. */
  if (!in->same_uid)
    return decision(NWA_DECISION_DENY, NWA_DENY_FOREIGN_UID);

  switch (in->op) {
    case NWA_OP_READ:
    case NWA_OP_RECEIVE:
      if (!in->paired)
        return decision(NWA_DECISION_DENY, NWA_DENY_NOT_PAIRED);
      if (in->is_self || in->caller_trusted || (in->caller_identified && in->allow_read))
        return decision(NWA_DECISION_ALLOW, NWA_DENY_NONE);
      return prompt_or_deny(in);

    case NWA_OP_PAY: {
      if (!in->paired)
        return decision(NWA_DECISION_DENY, NWA_DENY_NOT_PAIRED);
      if (in->amount_msat == 0)
        return decision(NWA_DECISION_DENY, NWA_DENY_INVALID);
      gboolean over = in->limit_msat > 0 && in->amount_msat > in->remaining_msat;
      /* Links the user clicked are always confirmed: the "caller" is
       * whatever page or document contained the link. */
      gboolean auto_ok = !in->is_self &&
                         in->caller_identified &&
                         !in->always_confirm &&
                         in->limit_msat > 0 &&
                         !over &&
                         (in->max_auto_pay_msat == 0 || in->amount_msat <= in->max_auto_pay_msat);
      if (auto_ok)
        return decision(NWA_DECISION_ALLOW, NWA_DENY_NONE);
      NwaDecision d = prompt_or_deny(in);
      d.over_budget = over;
      return d;
    }

    case NWA_OP_PAIR:
      /* Pairing swaps the wallet every other app pays from/into (a hostile
       * pairing redirects incoming payments), and whoever supplied the URI
       * keeps its secret. Always confirmed — trusted apps included. */
      return prompt_or_deny(in);

    case NWA_OP_UNPAIR:
      if (!in->paired)
        return decision(NWA_DECISION_ALLOW, NWA_DENY_NONE); /* no-op */
      return prompt_or_deny(in);

    case NWA_OP_BUDGET_QUERY_OTHER:
      return in->caller_trusted ? decision(NWA_DECISION_ALLOW, NWA_DENY_NONE)
                                : decision(NWA_DECISION_DENY, NWA_DENY_NOT_TRUSTED);

    case NWA_OP_BUDGET_LOWER_OWN:
      /* Lowering can only reduce what the caller may do. Unidentified
       * callers have no record to lower. */
      return in->caller_identified ? decision(NWA_DECISION_ALLOW, NWA_DENY_NONE)
                                   : decision(NWA_DECISION_DENY, NWA_DENY_INVALID);

    case NWA_OP_BUDGET_CHANGE:
      if (in->caller_trusted)
        return decision(NWA_DECISION_ALLOW, NWA_DENY_NONE);
      if (!in->caller_identified)
        return decision(NWA_DECISION_DENY, NWA_DENY_INVALID);
      return prompt_or_deny(in);
  }
  return decision(NWA_DECISION_DENY, NWA_DENY_INVALID);
}

gboolean
nwa_policy_needs_user(NwaDecision d)
{
  return d.kind == NWA_DECISION_PROMPT ||
         (d.kind == NWA_DECISION_DENY && d.reason == NWA_DENY_NO_UI);
}

const gchar *
nwa_policy_deny_reason_to_string(NwaDenyReason reason)
{
  switch (reason) {
    case NWA_DENY_NONE:        return "allowed";
    case NWA_DENY_FOREIGN_UID: return "caller belongs to another user";
    case NWA_DENY_NOT_PAIRED:  return "no wallet is paired";
    case NWA_DENY_NO_UI:       return "user approval required but no display is available";
    case NWA_DENY_NOT_TRUSTED: return "only the wallet settings application may do this";
    case NWA_DENY_INVALID:     return "invalid request";
  }
  return "denied";
}
