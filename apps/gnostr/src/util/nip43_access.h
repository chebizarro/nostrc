/**
 * NIP-43 Relay Access Metadata
 *
 * NIP-43 defines how users can specify access requirements for relays.
 * This extends NIP-11 relay information with auth requirements:
 * - The relay's NIP-11 document can indicate auth is required
 * - Users can publish their preferred access methods
 *
 * This module provides:
 * - Parsing of limitation object from NIP-11 info
 * - Parsing of fees structure (admission, subscription, publication)
 * - Helper to check if relay requires payment
 */

#ifndef GNOSTR_NIP43_ACCESS_H
#define GNOSTR_NIP43_ACCESS_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ============== Relay Fee Structures ============== */

/**
 * GnostrRelayFee:
 * @amount: Fee amount in the specified unit
 * @unit: Currency unit (e.g., "msats", "sats")
 * @period: Period in seconds for subscription fees (0 for one-time fees)
 *
 * Represents a single fee entry from the relay's fees structure.
 */
typedef struct _GnostrRelayFee GnostrRelayFee;

struct _GnostrRelayFee {
  gint64 amount;     /* Fee amount */
  gchar *unit;       /* Currency unit (e.g., "msats", "sats") */
  gint64 period;     /* Period in seconds (for subscriptions), 0 for one-time */
};

/**
 * GnostrRelayFees:
 * @admission: Array of one-time admission fees
 * @admission_count: Number of admission fees
 * @subscription: Array of recurring subscription fees
 * @subscription_count: Number of subscription fees
 * @publication: Array of per-publication fees
 * @publication_count: Number of publication fees
 *
 * Contains all fee categories from a relay's NIP-11 fees object.
 */
typedef struct _GnostrRelayFees GnostrRelayFees;

struct _GnostrRelayFees {
  GnostrRelayFee *admission;      /* One-time admission fees */
  gsize admission_count;
  GnostrRelayFee *subscription;   /* Recurring subscription fees */
  gsize subscription_count;
  GnostrRelayFee *publication;    /* Per-publication fees */
  gsize publication_count;
};

/* ============== Relay Access Structure ============== */

/**
 * GnostrRelayAccess:
 * @auth_required: TRUE if relay requires NIP-42 authentication
 * @payment_required: TRUE if relay requires payment
 * @restricted_writes: TRUE if relay restricts write access
 * @payments_url: URL to the relay's payment page
 * @fees: Parsed fee structure (may be NULL if no fees specified)
 *
 * Represents the access requirements for a relay, parsed from NIP-11 info.
 */
typedef struct _GnostrRelayAccess GnostrRelayAccess;

struct _GnostrRelayAccess {
  gboolean auth_required;       /* Requires NIP-42 authentication */
  gboolean payment_required;    /* Requires payment to use */
  gboolean restricted_writes;   /* Has write restrictions */
  gchar *payments_url;          /* URL to payment page */
  GnostrRelayFees *fees;        /* Fee structure (may be NULL) */
};

/* ============== Memory Management ============== */

/**
 * gnostr_relay_fee_free:
 * @fee: Fee to free
 *
 * Frees a single GnostrRelayFee structure.
 */
void gnostr_relay_fee_free(GnostrRelayFee *fee);

/**
 * gnostr_relay_fees_free:
 * @fees: Fees structure to free
 *
 * Frees all memory associated with a GnostrRelayFees structure.
 */
void gnostr_relay_fees_free(GnostrRelayFees *fees);

/**
 * gnostr_relay_access_free:
 * @access: Access structure to free
 *
 * Frees all memory associated with a GnostrRelayAccess structure.
 */
void gnostr_relay_access_free(GnostrRelayAccess *access);

/* ============== Parsing Functions ============== */

/**
 * gnostr_relay_access_parse_info:
 * @info_json: JSON string of NIP-11 relay information document
 *
 * Parses relay access requirements from a NIP-11 info document.
 * Extracts limitation.auth_required, limitation.payment_required,
 * limitation.restricted_writes, payments_url, and fees.
 *
 * Returns: (transfer full) (nullable): Parsed GnostrRelayAccess or NULL on error
 */
GnostrRelayAccess *gnostr_relay_access_parse_info(const gchar *info_json);

/**
 * gnostr_relay_fees_parse:
 * @fees_json: JSON string of the fees object from NIP-11 info
 *
 * Parses the fees object from a NIP-11 relay information document.
 * The fees object contains admission, subscription, and publication arrays.
 *
 * Returns: (transfer full) (nullable): Parsed GnostrRelayFees or NULL on error
 */
GnostrRelayFees *gnostr_relay_fees_parse(const gchar *fees_json);

/**
 * gnostr_relay_access_parse_info_object:
 * @root_object: JsonObject from parsed NIP-11 document
 *
 * Parses relay access requirements from a pre-parsed JSON object.
 * This is useful when you already have a parsed NIP-11 document.
 *
 * Returns: (transfer full) (nullable): Parsed GnostrRelayAccess or NULL on error
 */
GnostrRelayAccess *gnostr_relay_access_parse_info_object(gpointer root_object);

/* ============== Helper Functions ============== */

/**
 * gnostr_relay_access_requires_payment:
 * @access: Relay access structure
 *
 * Checks if the relay requires any form of payment.
 * Returns TRUE if payment_required is set OR if any fees are specified.
 *
 * Returns: TRUE if relay requires payment
 */
gboolean gnostr_relay_access_requires_payment(const GnostrRelayAccess *access);

/**
 * gnostr_relay_access_has_admission_fee:
 * @access: Relay access structure
 *
 * Checks if the relay has any admission (one-time) fees.
 *
 * Returns: TRUE if relay has admission fees
 */
gboolean gnostr_relay_access_has_admission_fee(const GnostrRelayAccess *access);

/**
 * gnostr_relay_access_has_subscription_fee:
 * @access: Relay access structure
 *
 * Checks if the relay has any subscription (recurring) fees.
 *
 * Returns: TRUE if relay has subscription fees
 */
gboolean gnostr_relay_access_has_subscription_fee(const GnostrRelayAccess *access);

/**
 * gnostr_relay_access_has_publication_fee:
 * @access: Relay access structure
 *
 * Checks if the relay charges per-publication fees.
 *
 * Returns: TRUE if relay has publication fees
 */
gboolean gnostr_relay_access_has_publication_fee(const GnostrRelayAccess *access);

/**
 * gnostr_relay_access_get_min_admission_msats:
 * @access: Relay access structure
 *
 * Gets the minimum admission fee in millisatoshis.
 * Converts from other units if necessary.
 *
 * Returns: Minimum admission fee in msats, or 0 if no admission fee
 */
gint64 gnostr_relay_access_get_min_admission_msats(const GnostrRelayAccess *access);

/**
 * gnostr_relay_access_get_min_subscription_msats:
 * @access: Relay access structure
 * @period_out: (out) (optional): Returns the period in seconds for the fee
 *
 * Gets the minimum subscription fee in millisatoshis.
 * Converts from other units if necessary.
 *
 * Returns: Minimum subscription fee in msats, or 0 if no subscription fee
 */
gint64 gnostr_relay_access_get_min_subscription_msats(const GnostrRelayAccess *access,
                                                        gint64 *period_out);

/* ============== Formatting Helpers ============== */

/**
 * gnostr_relay_fee_format:
 * @fee: Fee to format
 *
 * Formats a fee as a human-readable string.
 * Example: "1000 msats" or "50000 msats/month"
 *
 * Returns: (transfer full): Formatted string. Caller must g_free().
 */
gchar *gnostr_relay_fee_format(const GnostrRelayFee *fee);

/**
 * gnostr_relay_fees_format_summary:
 * @fees: Fees structure
 *
 * Formats all fees as a human-readable summary.
 * Example: "Admission: 1000 msats, Subscription: 50000 msats/month"
 *
 * Returns: (transfer full): Formatted string. Caller must g_free().
 */
gchar *gnostr_relay_fees_format_summary(const GnostrRelayFees *fees);

/**
 * gnostr_relay_access_format_requirements:
 * @access: Access structure
 *
 * Formats all access requirements as a human-readable summary.
 *
 * Returns: (transfer full): Formatted string. Caller must g_free().
 */
gchar *gnostr_relay_access_format_requirements(const GnostrRelayAccess *access);

/**
 * gnostr_relay_fee_period_to_string:
 * @period_seconds: Period in seconds
 *
 * Converts a period in seconds to a human-readable string.
 * Examples: "hour", "day", "week", "month", "year"
 *
 * Returns: (transfer none): Static string describing the period
 */
const gchar *gnostr_relay_fee_period_to_string(gint64 period_seconds);

/* ============== Unit Conversion ============== */

/**
 * gnostr_relay_fee_to_msats:
 * @amount: Amount in the specified unit
 * @unit: Currency unit ("msats", "sats", "btc")
 *
 * Converts a fee amount to millisatoshis.
 *
 * Returns: Amount in millisatoshis, or -1 if unit is unknown
 */
gint64 gnostr_relay_fee_to_msats(gint64 amount, const gchar *unit);

G_END_DECLS

#endif /* GNOSTR_NIP43_ACCESS_H */
