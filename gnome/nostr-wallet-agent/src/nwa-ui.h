/* nwa-ui.h - libadwaita approval dialogs for nostr-wallet-agent
 *
 * SPDX-License-Identifier: MIT
 *
 * The agent is a plain GApplication so it can run (and answer D-Bus) without
 * a display. GTK/libadwaita are initialised lazily the first time a dialog
 * is needed; if that fails (no Wayland/X11 session, or
 * NOSTR_WALLET_AGENT_HEADLESS=1) nwa_ui_available() returns FALSE and the
 * policy fails closed (every request that needs the user is denied).
 *
 * Every dialog answers exactly once: closing the window or letting it time
 * out counts as "deny".
 */
#ifndef NWA_UI_H
#define NWA_UI_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nwa_ui_available(void);

typedef struct {
  const gchar *app_name;
  const gchar *app_id;          /* nullable */
  const gchar *app_kind;        /* "flatpak", "executable", … */
  gboolean     app_attested;
  gboolean     can_remember;    /* identified caller: offer "Always allow" */
  gboolean     is_site;         /* a web origin (browser bridge): say "site", not "app" */
  const gchar *via;             /* is_site: browser the page runs in (nullable) */
  gboolean     via_link;        /* opened from a lightning:/bitcoin: link */
  guint64      amount_msat;
  const gchar *description;     /* nullable */
  const gchar *payee;           /* nullable (hex node id) */
  const gchar *network;         /* "bc", "tb", … */
  guint64      limit_msat;      /* current daily limit, 0 = none */
  guint64      remaining_msat;
  gboolean     over_budget;
} NwaPaymentPrompt;

/* @remember: user ticked "Always allow up to N sats/day"; @remember_limit_msat = N*1000. */
typedef void (*NwaPaymentPromptCallback)(gboolean approved, gboolean remember,
                                         guint64 remember_limit_msat, gpointer user_data);
void nwa_ui_prompt_payment(const NwaPaymentPrompt *prompt, guint timeout_s,
                           NwaPaymentPromptCallback callback, gpointer user_data);

/* LNURL-pay / Lightning address: the user picks the amount (within the
 * recipient's range) and, when allowed, a comment. Approving is the payment
 * confirmation (the invoice fetched afterwards is checked against it). */
typedef struct {
  const gchar *opener;           /* "A link opened by …" */
  const gchar *recipient;        /* address / identifier, or the server's host */
  const gchar *domain;           /* host that served the request */
  const gchar *description;      /* metadata text/plain */
  const gchar *long_description; /* nullable */
  guint64      min_msat, max_msat;
  guint        comment_allowed;  /* 0 = no comment field */
} NwaLnurlPrompt;

typedef void (*NwaLnurlPromptCallback)(gboolean approved, guint64 amount_msat, const gchar *comment,
                                       gpointer user_data);
void nwa_ui_prompt_lnurl(const NwaLnurlPrompt *prompt, guint timeout_s,
                         NwaLnurlPromptCallback callback, gpointer user_data);

/* @remember_label: when non-NULL an "always" switch (default off) is shown and
 * its state is reported as @remember. */
typedef void (*NwaConfirmCallback)(gboolean accepted, gboolean remember, gpointer user_data);
void nwa_ui_confirm(const gchar *title, const gchar *body, const gchar *accept_label,
                    gboolean destructive, const gchar *remember_label, guint timeout_s,
                    NwaConfirmCallback callback, gpointer user_data);

/* Informational window with a toast (e.g. "On-chain payments are not supported"). */
void nwa_ui_show_message(const gchar *title, const gchar *body, const gchar *toast);

/* "21 sats", "1,234 sats", "0.5 sats" (msat precision kept when < 1 sat). */
gchar *nwa_ui_format_msat(guint64 msat);

G_END_DECLS

#endif /* NWA_UI_H */
