/* nwa-uri.h - scheme-handler URI parsing for nostr-wallet-agent
 *
 * SPDX-License-Identifier: MIT
 *
 *   lightning:<bolt11>                  -> NWA_URI_LIGHTNING_INVOICE
 *   lightning:lnurl1... / user@host     -> NWA_URI_LNURL (LNURL-pay, nwa-lnurl.c)
 *   bitcoin:<addr>?...&lightning=<b11>  -> NWA_URI_LIGHTNING_INVOICE (BIP-21)
 *   bitcoin:<addr>[?amount=...]         -> NWA_URI_BITCOIN_ONCHAIN (unsupported)
 *   nostr+walletconnect://<pk>?relay=...&secret=...
 *                                       -> NWA_URI_NWC_PAIRING
 *
 * Parsing is syntactic; bolt11 invoices are decoded (and checksummed) later
 * by nwa_bolt11_decode() when the payment is actually prepared.
 */
#ifndef NWA_URI_H
#define NWA_URI_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NWA_URI_LIGHTNING_INVOICE,
  NWA_URI_LNURL,
  NWA_URI_BITCOIN_ONCHAIN,
  NWA_URI_NWC_PAIRING,
} NwaUriKind;

typedef struct {
  NwaUriKind kind;
  gchar   *bolt11;       /* LIGHTNING_INVOICE: lower-cased invoice */
  gchar   *lnurl;        /* LNURL: lnurl1... bech32 or user@host address */
  gchar   *address;      /* bitcoin: on-chain address ("" if absent) */
  guint64  amount_msat;  /* bitcoin: BIP-21 amount, 0 if absent */
  gchar   *label;        /* bitcoin: BIP-21 label */
  gchar   *message;      /* bitcoin: BIP-21 message */
  gchar   *nwc_uri;      /* NWC_PAIRING: normalized nostr+walletconnect:// URI */
} NwaUri;

gboolean nwa_uri_parse(const gchar *uri, NwaUri *out, GError **error);
void     nwa_uri_clear(NwaUri *u);

/* BIP-21 decimal BTC amount ("0.00021", "1", "1.") -> msat. Exposed for tests. */
gboolean nwa_uri_parse_btc_amount(const gchar *btc, guint64 *out_msat);

/* Validate a nostr+walletconnect URI (also accepts the "nostr+walletconnect:"
 * form without "//") and return it normalized, or NULL with @error set. A
 * pairing must carry at least one ws(s):// relay and a 64-hex secret. */
gchar *nwa_uri_normalize_nwc(const gchar *uri, GError **error);

G_END_DECLS

#endif /* NWA_URI_H */
