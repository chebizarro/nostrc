/* nwa-bolt11.h - minimal BOLT-11 invoice decoder
 *
 * SPDX-License-Identifier: MIT
 *
 * Decodes what the approval policy and dialog need: network, amount,
 * timestamp/expiry, payment hash, description (or its hash) and an explicit
 * payee (`n` field). The bech32 checksum is verified; the node signature is
 * NOT (the payee is not recovered from it) — the wallet validates the
 * invoice cryptographically before paying.
 */
#ifndef NWA_BOLT11_H
#define NWA_BOLT11_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gchar   *network;          /* hrp currency: "bc", "tb", "bcrt", "tbs", "sb" */
  guint64  amount_msat;      /* 0 = amount-less */
  gint64   timestamp;        /* unix seconds */
  gint64   expiry;           /* seconds (default 3600) */
  gchar   *payment_hash;     /* 64 hex */
  gchar   *description;      /* `d`, UTF-8, nullable */
  gchar   *description_hash; /* `h`, 64 hex, nullable */
  gchar   *payee;            /* `n`, 66 hex, nullable */
} NwaBolt11;

gboolean nwa_bolt11_decode(const gchar *invoice, NwaBolt11 *out, GError **error);
void     nwa_bolt11_clear(NwaBolt11 *b);
gboolean nwa_bolt11_is_expired(const NwaBolt11 *b, gint64 now_unix);

/* Parse the amount part of a BOLT-11 human-readable part ("2500u", "20m",
 * "9678785340p", "" = amount-less) into msat. Exposed for tests. */
gboolean nwa_bolt11_parse_hrp_amount(const gchar *amount, guint64 *out_msat);

G_END_DECLS

#endif /* NWA_BOLT11_H */
