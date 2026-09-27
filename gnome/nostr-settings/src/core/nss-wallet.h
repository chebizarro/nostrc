/* nss-wallet.h — Wallet page model over org.nostr.Wallet1
 * (gnome/nostr-wallet-agent, gnome/dbus/org.nostr.Wallet1.xml).
 * SPDX-License-Identifier: MIT
 *
 * The installed Nostr Settings is the agent's "grant admin" (the agent
 * checks its executable, <bindir>/nostr-settings, by path and inode): it
 * lists every app with ListApps and grants/revokes read access
 * (SetReadAccess) without the agent's confirmation dialog. Budget changes
 * (SetBudget) are still confirmed by the agent's dialog. When the
 * agent refuses ListApps — Settings run from a build tree, or an older
 * agent — the page falls back to reading the agent's documented store,
 * $XDG_STATE_HOME/nostr-wallet/budgets.json, and every change is then
 * confirmed by the agent's own dialog.
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

/* ListApps reply body (a{sa{sv}}) → NssBudget* array sorted by app id. */
GPtrArray *nss_wallet_apps_from_variant(GVariant *apps);

/* The identity the agent gives GNOME Shell (and so every Shell extension). */
#define NSS_WALLET_SHELL_APP_ID "exe:/usr/bin/gnome-shell"

/* "1,000 sats" from msat (rounded down to whole sats). */
gchar     *nss_format_sats(guint64 msat);
/* Friendly app label: "exe:/usr/bin/foo" → "foo (unverified)", reverse-DNS
 * kept, GNOME Shell named, web origins → "Website https://…". */
gchar     *nss_wallet_app_label(const gchar *app_id);

G_END_DECLS

#endif /* NSS_WALLET_H */
