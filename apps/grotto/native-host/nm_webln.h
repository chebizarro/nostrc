/* nm_webln.h - WebLN <-> org.nostr.Wallet1 translation (nostrc-jjyp)
 *
 * Pure functions, no I/O, unit-tested in tests/test_nm_webln.c:
 *   - sats (WebLN) -> msat (Wallet1 "u") conversion with range checks
 *   - makeInvoice RequestInvoiceArgs resolution
 *   - BOLT-11 shape check for sendPayment
 *   - Wallet1.GetInfo a{sv} -> WebLN getInfo result
 *   - org.nostr.Wallet1.Error.* / bus errors -> bridge error codes
 */
#ifndef APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_WEBLN_H
#define APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_WEBLN_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "nm_errors.h"

G_BEGIN_DECLS

/* Wallet1 amounts are "u" msat: at most G_MAXUINT32 msat per call. */
#define NM_WEBLN_MAX_SATS   (G_MAXUINT32 / 1000u) /* 4294967 */
#define NM_WEBLN_MAX_MEMO   639                   /* BOLT-11 description limit, bytes */
#define NM_WEBLN_MAX_BOLT11 7089                  /* QR alphanumeric capacity */

typedef enum {
  NM_SATS_OK,
  NM_SATS_ABSENT,    /* member missing or null */
  NM_SATS_INVALID,   /* not a non-negative integer (number or decimal string) */
  NM_SATS_TOO_LARGE, /* > NM_WEBLN_MAX_SATS */
} NmSatsStatus;

/* A WebLN amount: JSON integer, or a string of decimal digits (WebLN allows
 * string | number). Fractions, negatives, exponents and signs are INVALID. */
NmSatsStatus nm_webln_parse_sats(JsonNode *node, guint64 *out_sats);

/* sats -> msat for a Wallet1 "u" argument; FALSE if it does not fit. */
gboolean nm_webln_sats_to_msat(guint64 sats, guint32 *out_msat);

/* Resolve makeInvoice params ({amount, defaultAmount, minimumAmount,
 * maximumAmount, defaultMemo}, all optional) to what Wallet1.MakeInvoice
 * takes. The amount is `amount`, else `defaultAmount`, else a non-zero
 * `minimumAmount`; it must lie within [minimumAmount, maximumAmount] when
 * those are given. Amount-less invoices are refused (the agent requires an
 * amount). On failure returns FALSE with @code (invalid_request or
 * too_large) and a static @why. @out_memo points into @params. */
gboolean nm_webln_resolve_invoice(JsonObject *params, guint32 *out_msat, const gchar **out_memo,
                                  NmErrorCode *code, const gchar **why);

/* Plausible BOLT-11 invoice: "ln" prefix, bech32 charset in one case, a '1'
 * separator, bounded length. The agent decodes and checksums it. */
gboolean nm_webln_is_bolt11(const gchar *s);

/* Wallet1.GetInfo / GetInfoFor result (a{sv}) -> WebLN getInfo:
 *   {"node": {"alias", "pubkey"?, "color"?}, "methods": [...],
 *    "supports": ["lightning"], "version": "nostr-wallet-agent"}
 * "methods" lists the WebLN calls the paired wallet advertises (all of them
 * when it advertises none). Returns NULL when the dict says paired=false. */
JsonNode *nm_webln_info_from_vardict(GVariant *info);

/* Map a failed Wallet1 call. Sets *@message to a page-safe message (owned by
 * the caller) when the agent's own text is worth passing on, else NULL
 * (use the code's default message). */
NmErrorCode nm_webln_error_from_dbus(const GError *error, gchar **message);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_WEBLN_H */
