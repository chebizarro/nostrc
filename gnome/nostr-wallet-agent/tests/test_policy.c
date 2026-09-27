/* test_policy.c - the approval decision matrix, row by row.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-policy.h"

#include <glib.h>

#define A NWA_DECISION_ALLOW
#define P NWA_DECISION_PROMPT
#define D NWA_DECISION_DENY

/* A normal, identified, paired caller on a desktop with a 10k-sat budget. */
static NwaPolicyInput
base(NwaOp op)
{
  NwaPolicyInput in = {
    .op = op,
    .same_uid = TRUE,
    .caller_identified = TRUE,
    .paired = TRUE,
    .ui_available = TRUE,
    .amount_msat = 1000000,
    .limit_msat = 10000000,
    .remaining_msat = 10000000,
  };
  return in;
}

typedef struct {
  const gchar     *name;
  NwaPolicyInput   in;
  NwaDecisionKind  want;
  NwaDenyReason    reason;
  gboolean         over_budget;
} Row;

static void
check(const Row *r)
{
  NwaDecision d = nwa_policy_decide(&r->in);
  if (d.kind != r->want || (r->want == D && d.reason != r->reason) ||
      d.over_budget != r->over_budget)
    g_error("policy row '%s': got kind=%d reason=%d over=%d, want kind=%d reason=%d over=%d",
            r->name, d.kind, d.reason, d.over_budget, r->want, r->reason, r->over_budget);
}

static void
test_pay(void)
{
  Row rows[32];
  guint n = 0;
#define ROW(nm, field_setup, w, rs, ob) do { \
    NwaPolicyInput in = base(NWA_OP_PAY); field_setup; \
    rows[n++] = (Row){ nm, in, w, rs, ob }; } while (0)

  ROW("within budget -> auto", (void)0, A, NWA_DENY_NONE, FALSE);
  ROW("exactly remaining -> auto", in.amount_msat = in.remaining_msat, A, NWA_DENY_NONE, FALSE);
  ROW("over remaining -> prompt+signal", in.remaining_msat = 500000, P, NWA_DENY_NONE, TRUE);
  ROW("over remaining, headless -> deny", (in.remaining_msat = 500000, in.ui_available = FALSE),
      D, NWA_DENY_NO_UI, TRUE);
  ROW("no budget -> prompt", (in.limit_msat = 0, in.remaining_msat = 0), P, NWA_DENY_NONE, FALSE);
  ROW("no budget, headless -> deny", (in.limit_msat = 0, in.remaining_msat = 0, in.ui_available = FALSE),
      D, NWA_DENY_NO_UI, FALSE);
  ROW("unidentified never auto", in.caller_identified = FALSE, P, NWA_DENY_NONE, FALSE);
  ROW("trusted app still bound by budget", (in.caller_trusted = TRUE, in.remaining_msat = 0),
      P, NWA_DENY_NONE, TRUE);
  ROW("always-confirm -> prompt", in.always_confirm = TRUE, P, NWA_DENY_NONE, FALSE);
  ROW("above per-payment cap -> prompt", in.max_auto_pay_msat = 999999, P, NWA_DENY_NONE, FALSE);
  ROW("at per-payment cap -> auto", in.max_auto_pay_msat = 1000000, A, NWA_DENY_NONE, FALSE);
  ROW("scheme link always prompts", in.is_self = TRUE, P, NWA_DENY_NONE, FALSE);
  ROW("foreign uid -> deny", in.same_uid = FALSE, D, NWA_DENY_FOREIGN_UID, FALSE);
  ROW("not paired -> deny", in.paired = FALSE, D, NWA_DENY_NOT_PAIRED, FALSE);
  ROW("zero amount -> deny", in.amount_msat = 0, D, NWA_DENY_INVALID, FALSE);
#undef ROW
  for (guint i = 0; i < n; i++) check(&rows[i]);
}

static void
test_read_receive(void)
{
  const NwaOp ops[] = { NWA_OP_READ, NWA_OP_RECEIVE };
  for (guint i = 0; i < G_N_ELEMENTS(ops); i++) {
    NwaPolicyInput in = base(ops[i]);
    Row r = { "first use -> prompt", in, P, NWA_DENY_NONE, FALSE };
    check(&r);

    in.allow_read = TRUE;
    r = (Row){ "allowed before -> allow", in, A, NWA_DENY_NONE, FALSE };
    check(&r);

    in.caller_identified = FALSE;
    r = (Row){ "unidentified cannot use a stored grant", in, P, NWA_DENY_NONE, FALSE };
    check(&r);

    in = base(ops[i]);
    in.caller_trusted = TRUE;
    r = (Row){ "trusted -> allow", in, A, NWA_DENY_NONE, FALSE };
    check(&r);

    in = base(ops[i]);
    in.ui_available = FALSE;
    r = (Row){ "headless first use -> deny", in, D, NWA_DENY_NO_UI, FALSE };
    check(&r);

    in = base(ops[i]);
    in.paired = FALSE;
    in.allow_read = TRUE;
    r = (Row){ "unpaired -> deny", in, D, NWA_DENY_NOT_PAIRED, FALSE };
    check(&r);

    in = base(ops[i]);
    in.same_uid = FALSE;
    in.caller_trusted = TRUE;
    r = (Row){ "foreign uid beats trust", in, D, NWA_DENY_FOREIGN_UID, FALSE };
    check(&r);
  }
}

static void
test_pairing(void)
{
  NwaPolicyInput in = base(NWA_OP_PAIR);
  Row r = { "pair from app -> prompt", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in.paired = FALSE;
  r = (Row){ "first pairing also prompts", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in.caller_trusted = TRUE;
  r = (Row){ "even a trusted app's pairing is confirmed", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in = base(NWA_OP_PAIR);
  in.is_self = TRUE;
  r = (Row){ "clicked pairing link prompts", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in.ui_available = FALSE;
  r = (Row){ "headless pairing -> deny", in, D, NWA_DENY_NO_UI, FALSE };
  check(&r);

  in = base(NWA_OP_UNPAIR);
  r = (Row){ "unpair -> prompt", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in.paired = FALSE;
  r = (Row){ "unpair when unpaired is a no-op", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
  in = base(NWA_OP_UNPAIR);
  in.caller_trusted = TRUE;
  r = (Row){ "even a trusted app's unpair is confirmed", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in.ui_available = FALSE;
  r = (Row){ "headless unpair -> deny", in, D, NWA_DENY_NO_UI, FALSE };
  check(&r);
}

static void
test_budget_ops(void)
{
  NwaPolicyInput in = base(NWA_OP_BUDGET_QUERY_OTHER);
  Row r = { "query other app -> deny", in, D, NWA_DENY_NOT_TRUSTED, FALSE };
  check(&r);
  in.caller_trusted = TRUE;
  r = (Row){ "settings app may query", in, A, NWA_DENY_NONE, FALSE };
  check(&r);

  in = base(NWA_OP_BUDGET_LOWER_OWN);
  r = (Row){ "lower own budget -> allow", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
  in.ui_available = FALSE;
  r = (Row){ "lower own budget works headless", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
  in.caller_identified = FALSE;
  r = (Row){ "unidentified has no budget", in, D, NWA_DENY_INVALID, FALSE };
  check(&r);

  in = base(NWA_OP_BUDGET_CHANGE);
  r = (Row){ "raise own budget -> prompt", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in.ui_available = FALSE;
  r = (Row){ "raise headless -> deny", in, D, NWA_DENY_NO_UI, FALSE };
  check(&r);
  in = base(NWA_OP_BUDGET_CHANGE);
  in.caller_identified = FALSE;
  r = (Row){ "unidentified cannot raise", in, D, NWA_DENY_INVALID, FALSE };
  check(&r);
  in.caller_identified = TRUE;
  in.caller_trusted = TRUE;
  in.ui_available = FALSE;
  r = (Row){ "settings app sets budgets", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
}

static void
test_read_grants(void)
{
  NwaPolicyInput in = base(NWA_OP_READ_GRANT);
  Row r = { "app asks for a read grant -> prompt", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in.ui_available = FALSE;
  r = (Row){ "grant headless -> deny", in, D, NWA_DENY_NO_UI, FALSE };
  check(&r);
  in.caller_grant_admin = TRUE;
  r = (Row){ "settings app grants without a dialog", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
  in.paired = FALSE;
  r = (Row){ "grants do not need a wallet", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
  in = base(NWA_OP_READ_GRANT);
  in.caller_identified = FALSE;
  r = (Row){ "unidentified cannot grant", in, D, NWA_DENY_INVALID, FALSE };
  check(&r);
  in = base(NWA_OP_READ_GRANT);
  in.caller_grant_admin = TRUE;
  in.same_uid = FALSE;
  r = (Row){ "foreign uid cannot grant", in, D, NWA_DENY_FOREIGN_UID, FALSE };
  check(&r);

  in = base(NWA_OP_READ_REVOKE_OWN);
  in.ui_available = FALSE;
  r = (Row){ "give up own access, no dialog", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
  in.caller_identified = FALSE;
  r = (Row){ "unidentified has nothing to revoke", in, D, NWA_DENY_INVALID, FALSE };
  check(&r);

  in = base(NWA_OP_READ_REVOKE_OTHER);
  r = (Row){ "apps cannot revoke each other", in, D, NWA_DENY_NOT_TRUSTED, FALSE };
  check(&r);
  in.caller_grant_admin = TRUE;
  in.ui_available = FALSE;
  r = (Row){ "settings app revokes without a dialog", in, A, NWA_DENY_NONE, FALSE };
  check(&r);

  /* the exe-identified settings app administers grants, not money */
  in = base(NWA_OP_BUDGET_CHANGE);
  in.caller_grant_admin = TRUE;
  r = (Row){ "settings binary raising a budget is still confirmed", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
  in = base(NWA_OP_BUDGET_QUERY_OTHER);
  in.caller_grant_admin = TRUE;
  r = (Row){ "settings binary may list apps / read budgets", in, A, NWA_DENY_NONE, FALSE };
  check(&r);
  in = base(NWA_OP_READ);
  in.caller_grant_admin = TRUE;
  r = (Row){ "grant admin gets no free read", in, P, NWA_DENY_NONE, FALSE };
  check(&r);
}

/* *NonInteractive reads fail with InteractionRequired exactly when the
 * decision would involve the user (prompt, or deny for lack of a display),
 * and never turn an allow or a hard deny into that error. */
static void
test_needs_user(void)
{
  NwaPolicyInput in = base(NWA_OP_READ);
  g_assert_true(nwa_policy_needs_user(nwa_policy_decide(&in)));      /* no grant: prompt */
  in.ui_available = FALSE;
  g_assert_true(nwa_policy_needs_user(nwa_policy_decide(&in)));      /* no grant, headless */
  in.caller_identified = FALSE;
  in.ui_available = TRUE;
  in.allow_read = TRUE;
  g_assert_true(nwa_policy_needs_user(nwa_policy_decide(&in)));      /* unidentified */
  in = base(NWA_OP_READ);
  in.allow_read = TRUE;
  g_assert_false(nwa_policy_needs_user(nwa_policy_decide(&in)));     /* granted: answer */
  in = base(NWA_OP_READ);
  in.caller_trusted = TRUE;
  g_assert_false(nwa_policy_needs_user(nwa_policy_decide(&in)));
  in = base(NWA_OP_READ);
  in.paired = FALSE;
  g_assert_false(nwa_policy_needs_user(nwa_policy_decide(&in)));     /* NotPaired stays */
  in = base(NWA_OP_READ);
  in.same_uid = FALSE;
  g_assert_false(nwa_policy_needs_user(nwa_policy_decide(&in)));     /* Denied stays */
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/policy/pay", test_pay);
  g_test_add_func("/policy/read-receive", test_read_receive);
  g_test_add_func("/policy/pairing", test_pairing);
  g_test_add_func("/policy/budget-ops", test_budget_ops);
  g_test_add_func("/policy/needs-user", test_needs_user);
  g_test_add_func("/policy/read-grants", test_read_grants);
  return g_test_run();
}
