/* nss-wallet.h — Wallet page model over org.nostr.Wallet1
 * (gnome/nostr-wallet-agent, gnome/dbus/org.nostr.Wallet1.xml).
 * SPDX-License-Identifier: MIT
 *
 * Budgets are enumerated READ-ONLY from the agent's documented store,
 * $XDG_STATE_HOME/nostr-wallet/budgets.json (see the agent's README,
 * "Budgets"), because GetBudget for another app is trusted-only and
 * "trusted" requires a Flatpak-verified caller: an unsandboxed Settings
 * app would be refused. Every change still goes through SetBudget, so the
 * agent's own confirmation dialog decides. No ListBudgets method was added
 * to the agent.
 */
#ifndef NSS_WALLET_H
#define NSS_WALLET_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NSS_WALLET_BUS_NAME "org.nostr.Wallet1"
#define NSS_WALLET_OBJ_PATH "/org/nostr/Wallet1"
#define NSS_WALLET_IFACE    "org.nostr.Wallet1"
/* Pair/Unpair/SetBudget wait for the user in the agent's dialog
 * (approval-timeout defaults to 120 s). */
#define NSS_WALLET_PROMPT_TIMEOUT_MS (180 * 1000)

typedef struct {
  gchar   *app_id;
  guint64  limit_msat_per_day;
  guint64  spent_today_msat;   /* 0 unless the record's day is today */
  gboolean allow_read;
} NssBudget;

void       nss_budget_free(NssBudget *b);
gchar     *nss_wallet_budgets_path(void);
/* Missing file → empty array. @today is "YYYY-MM-DD" (local day, as the
 * agent uses) or NULL for the current local date. Sorted by app id. */
GPtrArray *nss_wallet_budgets_load(const gchar *path, const gchar *today, GError **error);

/* "1,000 sats" from msat (rounded down to whole sats). */
gchar     *nss_format_sats(guint64 msat);
/* Friendly app label: "exe:/usr/bin/foo" → "foo (unverified)", reverse-DNS kept. */
gchar     *nss_wallet_app_label(const gchar *app_id);

G_END_DECLS

#endif /* NSS_WALLET_H */
