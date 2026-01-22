/**
 * gnostr NIP-57 Zaps Utility
 *
 * Lightning zaps implementation per NIP-57 specification.
 * Provides LNURL fetching, zap request creation, and zap receipt handling.
 */

#ifndef GNOSTR_ZAP_H
#define GNOSTR_ZAP_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * Zap error codes
 */
typedef enum {
  GNOSTR_ZAP_ERROR_INVALID_LNURL,
  GNOSTR_ZAP_ERROR_NO_ZAP_SUPPORT,
  GNOSTR_ZAP_ERROR_HTTP_FAILED,
  GNOSTR_ZAP_ERROR_PARSE_FAILED,
  GNOSTR_ZAP_ERROR_AMOUNT_OUT_OF_RANGE,
  GNOSTR_ZAP_ERROR_INVOICE_FAILED,
  GNOSTR_ZAP_ERROR_PAYMENT_FAILED
} GnostrZapError;

#define GNOSTR_ZAP_ERROR (gnostr_zap_error_quark())
GQuark gnostr_zap_error_quark(void);

/**
 * LNURL pay endpoint info (from /.well-known/lnurlp/ or decoded lud06)
 */
typedef struct {
  gchar *callback;         /* URL to request invoice from */
  gint64 min_sendable;     /* Minimum amount in millisatoshis */
  gint64 max_sendable;     /* Maximum amount in millisatoshis */
  gboolean allows_nostr;   /* Whether NIP-57 zaps are supported */
  gchar *nostr_pubkey;     /* Pubkey that will sign zap receipts (hex) */
  gchar *metadata;         /* LNURL metadata JSON */
  gchar *comment_allowed;  /* Max comment length if allowed */
} GnostrLnurlPayInfo;

/**
 * Zap request context - holds all data needed to create a zap
 */
typedef struct {
  gchar *recipient_pubkey;  /* Recipient's nostr pubkey (hex) */
  gchar *event_id;          /* Event ID being zapped (hex), NULL for profile zap */
  gchar *lnurl;             /* Recipient's lnurl (bech32 encoded) */
  gchar *lud16;             /* Recipient's lightning address (user@domain) */
  gint64 amount_msat;       /* Amount in millisatoshis */
  gchar *comment;           /* Optional zap comment */
  gchar **relays;           /* Relays for zap receipt (NULL-terminated) */
  gint event_kind;          /* Kind of event being zapped (1, 30023, etc.) */
} GnostrZapRequest;

/**
 * Zap receipt info (kind:9735 event)
 */
typedef struct {
  gchar *id;                /* Receipt event ID */
  gchar *bolt11;            /* The paid invoice */
  gchar *preimage;          /* Payment preimage (optional) */
  gchar *description;       /* JSON-encoded zap request */
  gchar *sender_pubkey;     /* Zap sender pubkey (from P tag) */
  gchar *recipient_pubkey;  /* Zap recipient pubkey (from p tag) */
  gchar *event_id;          /* Zapped event ID (from e tag, optional) */
  gint64 amount_msat;       /* Amount from bolt11 invoice */
  gint64 created_at;        /* Receipt creation timestamp */
} GnostrZapReceipt;

/**
 * Callback for LNURL info fetch
 */
typedef void (*GnostrLnurlInfoCallback)(GnostrLnurlPayInfo *info,
                                        GError *error,
                                        gpointer user_data);

/**
 * Callback for zap invoice request
 */
typedef void (*GnostrZapInvoiceCallback)(const gchar *bolt11_invoice,
                                         GError *error,
                                         gpointer user_data);

/* ============== Memory Management ============== */

/**
 * gnostr_lnurl_pay_info_free:
 * @info: LNURL pay info to free
 *
 * Free an LNURL pay info structure.
 */
void gnostr_lnurl_pay_info_free(GnostrLnurlPayInfo *info);

/**
 * gnostr_zap_request_free:
 * @req: Zap request to free
 *
 * Free a zap request structure.
 */
void gnostr_zap_request_free(GnostrZapRequest *req);

/**
 * gnostr_zap_receipt_free:
 * @receipt: Zap receipt to free
 *
 * Free a zap receipt structure.
 */
void gnostr_zap_receipt_free(GnostrZapReceipt *receipt);

/* ============== LNURL Operations ============== */

/**
 * gnostr_zap_lud16_to_lnurl:
 * @lud16: Lightning address (user@domain)
 *
 * Convert a lightning address (LUD-16) to an LNURL endpoint.
 *
 * Returns: (transfer full) (nullable): The LNURL endpoint URL, or NULL on error
 */
gchar *gnostr_zap_lud16_to_lnurl(const gchar *lud16);

/**
 * gnostr_zap_fetch_lnurl_info_async:
 * @lud16: Lightning address (user@domain)
 * @callback: Callback when info is fetched
 * @user_data: User data for callback
 * @cancellable: (nullable): Optional cancellable
 *
 * Fetch LNURL pay endpoint info for a lightning address.
 */
void gnostr_zap_fetch_lnurl_info_async(const gchar *lud16,
                                       GnostrLnurlInfoCallback callback,
                                       gpointer user_data,
                                       GCancellable *cancellable);

/* ============== Zap Request Creation ============== */

/**
 * gnostr_zap_create_request_event:
 * @req: Zap request parameters
 * @sender_pubkey: Sender's pubkey (hex)
 *
 * Create a kind:9734 zap request event JSON (unsigned).
 * The event must be signed before sending to the LNURL callback.
 *
 * Returns: (transfer full) (nullable): JSON string of the zap request event
 */
gchar *gnostr_zap_create_request_event(const GnostrZapRequest *req,
                                       const gchar *sender_pubkey);

/**
 * gnostr_zap_request_invoice_async:
 * @lnurl_info: LNURL pay info for recipient
 * @signed_zap_request_json: Signed kind:9734 event JSON
 * @amount_msat: Amount in millisatoshis
 * @callback: Callback with invoice result
 * @user_data: User data for callback
 * @cancellable: (nullable): Optional cancellable
 *
 * Request a lightning invoice from the LNURL callback with the zap request.
 */
void gnostr_zap_request_invoice_async(const GnostrLnurlPayInfo *lnurl_info,
                                      const gchar *signed_zap_request_json,
                                      gint64 amount_msat,
                                      GnostrZapInvoiceCallback callback,
                                      gpointer user_data,
                                      GCancellable *cancellable);

/* ============== Zap Receipt Parsing ============== */

/**
 * gnostr_zap_parse_receipt:
 * @event_json: Kind:9735 event JSON
 *
 * Parse a zap receipt event.
 *
 * Returns: (transfer full) (nullable): Parsed zap receipt or NULL on error
 */
GnostrZapReceipt *gnostr_zap_parse_receipt(const gchar *event_json);

/**
 * gnostr_zap_validate_receipt:
 * @receipt: Zap receipt to validate
 * @expected_nostr_pubkey: Expected pubkey from LNURL info (hex)
 * @error: (out) (optional): Return location for error
 *
 * Validate a zap receipt per NIP-57 spec.
 *
 * Returns: %TRUE if valid
 */
gboolean gnostr_zap_validate_receipt(const GnostrZapReceipt *receipt,
                                     const gchar *expected_nostr_pubkey,
                                     GError **error);

/* ============== Utility Functions ============== */

/**
 * gnostr_zap_format_amount:
 * @amount_msat: Amount in millisatoshis
 *
 * Format a zap amount for display (e.g., "21 sats", "1.5K sats").
 *
 * Returns: (transfer full): Formatted string
 */
gchar *gnostr_zap_format_amount(gint64 amount_msat);

/**
 * gnostr_zap_sats_to_msat:
 * @sats: Amount in satoshis
 *
 * Convert satoshis to millisatoshis.
 *
 * Returns: Amount in millisatoshis
 */
static inline gint64 gnostr_zap_sats_to_msat(gint64 sats) {
  return sats * 1000;
}

/**
 * gnostr_zap_msat_to_sats:
 * @msat: Amount in millisatoshis
 *
 * Convert millisatoshis to satoshis.
 *
 * Returns: Amount in satoshis
 */
static inline gint64 gnostr_zap_msat_to_sats(gint64 msat) {
  return msat / 1000;
}

/**
 * Preset zap amounts in satoshis
 */
#define GNOSTR_ZAP_PRESET_21     21
#define GNOSTR_ZAP_PRESET_100    100
#define GNOSTR_ZAP_PRESET_500    500
#define GNOSTR_ZAP_PRESET_1000   1000
#define GNOSTR_ZAP_PRESET_5000   5000
#define GNOSTR_ZAP_PRESET_10000  10000
#define GNOSTR_ZAP_PRESET_21000  21000

G_END_DECLS

#endif /* GNOSTR_ZAP_H */
