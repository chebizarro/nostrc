/* nwa-lnurl.h - LNURL-pay (LUD-06) and Lightning addresses (LUD-16)
 *
 * SPDX-License-Identifier: MIT
 *
 *   lightning:lnurl1…   LUD-01 bech32 -> https://… (http only for .onion)
 *   lightning:user@host LUD-16 -> https://host/.well-known/lnurlp/user
 *
 * Flow (driven by nwa-service.c for scheme-handler links):
 *   GET the target        -> payRequest {callback, minSendable, maxSendable,
 *                            metadata, commentAllowed?}  (tag must be
 *                            "payRequest"; withdrawRequest is refused)
 *   the user picks an amount in [min, max] (and a comment if allowed) in
 *   the agent's dialog
 *   GET callback?amount=<msat>[&comment=…] -> {pr}
 *   check the invoice: amount == the chosen amount, description_hash ==
 *   sha256(metadata) (LUD-06), then pay it through the normal path.
 *
 * HTTP: libsoup, HTTPS only (http:// for .onion hosts; test builds also
 * allow loopback), no redirects, 15 s, JSON bodies capped at 64 KiB;
 * {"status":"ERROR","reason":…} answers become errors carrying the reason.
 */
#ifndef NWA_LNURL_H
#define NWA_LNURL_H

#include "nwa-bolt11.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define NWA_LNURL_MAX_BODY (64 * 1024)

typedef struct {
  gchar   *callback;
  guint64  min_msat, max_msat;
  gchar   *metadata;        /* raw metadata string (what the invoice commits to) */
  gchar   *description;     /* text/plain (required) */
  gchar   *long_description;/* text/long-desc, nullable */
  gchar   *identifier;      /* text/identifier or text/email, nullable */
  guint    comment_allowed; /* LUD-12: max comment length, 0 = none */
  gchar   *domain;          /* host serving the request (shown to the user) */
} NwaLnurlPay;

void nwa_lnurl_pay_clear(NwaLnurlPay *p);

/* lnurl1… or user@host (as the scheme handler classified it) -> the URL to
 * fetch. Checks the scheme rule above. */
gchar   *nwa_lnurl_target_url(const gchar *lnurl_or_address, GError **error);

/* TRUE for https://, http:// on .onion, and (test builds) http:// on
 * 127.0.0.1 / localhost. */
gboolean nwa_lnurl_url_allowed(const gchar *url);

/* Parse a payRequest JSON object fetched from @url; @address (nullable) is
 * the LUD-16 address it was resolved from and must match a text/identifier
 * or text/email entry when the metadata has one. */
gboolean nwa_lnurl_parse_pay(JsonNode *root, const gchar *url, const gchar *address,
                             NwaLnurlPay *out, GError **error);

/* callback with amount (and comment, trimmed to comment_allowed chars when
 * allowed and non-empty) appended. */
gchar   *nwa_lnurl_callback_url(const NwaLnurlPay *p, guint64 amount_msat, const gchar *comment);

/* The callback's invoice must be for @amount_msat and commit to the
 * metadata (description_hash = sha256(metadata)). */
gboolean nwa_lnurl_check_invoice(const NwaLnurlPay *p, guint64 amount_msat, const NwaBolt11 *inv,
                                 GError **error);

/* GET @url (Accept: application/json) and parse the JSON body; an LNURL
 * error object becomes NWA_ERROR_WALLET "[LNURL] reason". */
void      nwa_lnurl_fetch_json_async(SoupSession *session, const gchar *url, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
JsonNode *nwa_lnurl_fetch_json_finish(GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* NWA_LNURL_H */
