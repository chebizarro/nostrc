/* nwa-policy.h - approval decision matrix (pure; no I/O)
 *
 * SPDX-License-Identifier: MIT
 *
 * Decides, for one org.nostr.Wallet1 request, whether the agent may act
 * without asking (ALLOW), must ask the user (PROMPT), or must refuse (DENY).
 * The table is spelled out in README.md ("Approval matrix") and exercised
 * row by row in tests/test_policy.c.
 */
#ifndef NWA_POLICY_H
#define NWA_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NWA_OP_READ,               /* GetInfo (paired), GetBalance, LookupInvoice, ListTransactions */
  NWA_OP_RECEIVE,            /* MakeInvoice */
  NWA_OP_PAY,                /* PayInvoice */
  NWA_OP_PAIR,               /* Pair */
  NWA_OP_UNPAIR,             /* Unpair */
  NWA_OP_BUDGET_QUERY_OTHER, /* GetBudget(other app) */
  NWA_OP_BUDGET_LOWER_OWN,   /* SetBudget(own, <= current) */
  NWA_OP_BUDGET_CHANGE,      /* SetBudget(own raise | other app) */
} NwaOp;

typedef enum {
  NWA_DECISION_ALLOW,
  NWA_DECISION_PROMPT,
  NWA_DECISION_DENY,
} NwaDecisionKind;

typedef enum {
  NWA_DENY_NONE,
  NWA_DENY_FOREIGN_UID,   /* caller runs as a different Unix user */
  NWA_DENY_NOT_PAIRED,    /* no wallet */
  NWA_DENY_NO_UI,         /* would need to ask, but no display */
  NWA_DENY_NOT_TRUSTED,   /* op reserved for trusted settings apps */
  NWA_DENY_INVALID,       /* e.g. zero amount */
} NwaDenyReason;

typedef struct {
  NwaOp    op;
  gboolean same_uid;          /* caller uid == agent uid */
  gboolean caller_identified; /* an application id was resolved */
  gboolean caller_trusted;    /* listed in trusted-apps AND sandbox-attested */
  gboolean is_self;           /* scheme-handler link flow (always prompts) */
  gboolean paired;
  gboolean ui_available;
  gboolean always_confirm;    /* GSettings always-confirm-payments */
  gboolean allow_read;        /* caller previously allowed to read/receive */
  guint64  amount_msat;       /* PAY: amount + fee reserve */
  guint64  limit_msat;        /* caller's daily limit (0 = none) */
  guint64  remaining_msat;    /* caller's remaining budget today */
  guint64  max_auto_pay_msat; /* GSettings per-payment auto cap (0 = none) */
} NwaPolicyInput;

typedef struct {
  NwaDecisionKind kind;
  NwaDenyReason   reason;
  gboolean        over_budget; /* PAY that does not fit a configured budget */
} NwaDecision;

NwaDecision  nwa_policy_decide(const NwaPolicyInput *in);
/* TRUE when @d means "only the user could allow this": PROMPT, or DENY
 * because there is no display to prompt on. *NonInteractive calls answer
 * these with InteractionRequired instead. */
gboolean     nwa_policy_needs_user(NwaDecision d);
const gchar *nwa_policy_deny_reason_to_string(NwaDenyReason reason);

G_END_DECLS

#endif /* NWA_POLICY_H */
