/* nwa-budget.h - per-application daily spending budgets
 *
 * SPDX-License-Identifier: MIT
 *
 * Persistence: one JSON document at $XDG_STATE_HOME/nostr-wallet/budgets.json
 * (mode 0600, directory 0700, written atomically). GSettings (org.nostr.Wallet)
 * holds only agent preferences, never budgets: budgets carry a mutable
 * spend ledger that is rewritten on every payment, which is state, not a
 * setting.
 *
 *   { "version": 1,
 *     "apps": { "<app-id>": { "limit_msat_per_day": N,
 *                             "allow_read": true,
 *                             "day": "YYYY-MM-DD",
 *                             "spent_msat": M } } }
 *
 * "Per day" is the local calendar day of the injected clock. Spending is
 * charged conservatively: a reservation is added to spent_msat (and saved)
 * *before* the payment is sent, so a crash or an unanswered request counts
 * as spent. release() refunds a reservation only when the wallet reported a
 * definite failure; commit() replaces the reservation by the final amount
 * (amount + fees). If the day rolled over while a payment was in flight the
 * final amount is charged to the new day.
 */
#ifndef NWA_BUDGET_H
#define NWA_BUDGET_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NwaBudgetStore NwaBudgetStore;

/* Returns a new reference to "now" in the zone that defines the budget day. */
typedef GDateTime *(*NwaClockFunc)(gpointer user_data);

typedef struct {
  gboolean known;              /* app has a record */
  guint64  limit_msat_per_day; /* 0 = no automatic payments */
  guint64  spent_today_msat;   /* includes in-flight reservations */
  guint64  remaining_msat;     /* limit - spent (saturating) */
  gboolean allow_read;         /* user allowed balance/history/invoices */
} NwaBudgetInfo;

/* @path: JSON file, or NULL for an in-memory store. @clock: NULL = local time. */
NwaBudgetStore *nwa_budget_store_new(const gchar *path, NwaClockFunc clock, gpointer clock_data);
void            nwa_budget_store_free(NwaBudgetStore *self);

/* Missing file is not an error. A corrupt file is renamed to *.corrupt and
 * the store starts empty (fails closed: no app keeps an automatic budget). */
gboolean nwa_budget_store_load(NwaBudgetStore *self, GError **error);
gboolean nwa_budget_store_save(NwaBudgetStore *self, GError **error);

void     nwa_budget_store_get(NwaBudgetStore *self, const gchar *app_id, NwaBudgetInfo *out);
void     nwa_budget_store_set_limit(NwaBudgetStore *self, const gchar *app_id, guint64 limit_msat_per_day);
void     nwa_budget_store_set_allow_read(NwaBudgetStore *self, const gchar *app_id, gboolean allow);

/* Reserve @amount_msat for @app_id. Without @force, refuses (returns 0) when
 * the amount does not fit the remaining budget. Returns a reservation id. */
guint    nwa_budget_store_reserve(NwaBudgetStore *self, const gchar *app_id,
                                  guint64 amount_msat, gboolean force);
void     nwa_budget_store_commit(NwaBudgetStore *self, guint reservation, guint64 final_msat);
void     nwa_budget_store_release(NwaBudgetStore *self, guint reservation);

/* Routing-fee headroom reserved on top of a payment's amount (1 %, at
 * least 1 sat), so a run of auto-approved payments cannot overshoot the
 * daily limit by their aggregate fees. */
guint64  nwa_budget_fee_reserve(guint64 amount_msat);

/* $XDG_STATE_HOME/nostr-wallet/budgets.json */
gchar   *nwa_budget_store_default_path(void);

G_END_DECLS

#endif /* NWA_BUDGET_H */
