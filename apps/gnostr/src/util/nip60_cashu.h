/**
 * NIP-60: Cashu Wallet Utility
 *
 * NIP-60 defines how to store Cashu (ecash) wallet data on Nostr.
 * Cashu is a Chaumian ecash system that provides privacy for
 * Lightning-based payments.
 *
 * Event Kinds:
 * - 17375: Token event (stores ecash proofs, encrypted with NIP-44)
 * - 7375: Wallet transaction history
 *
 * Token Event (kind 17375) Structure:
 * - content: NIP-44 encrypted JSON with Cashu tokens
 * - tags:
 *   - ["a", "<kind>:<pubkey>:<d-tag>"] - wallet reference
 *   - ["e", "<event-id>"] - related event (optional, e.g., nutzap)
 *   - ["direction", "in"|"out"] - transaction direction
 *   - ["amount", "<msats>"] - amount in millisatoshis
 *   - ["unit", "sat"|"usd"|"eur"] - currency unit
 *   - ["p", "<pubkey>"] - counterparty pubkey
 *
 * Wallet Discovery:
 * - Kind 10002 relay list indicates where wallet events are stored
 * - Tokens are encrypted to user's pubkey using NIP-44
 *
 * Note: Encryption/decryption using NIP-44 is handled elsewhere.
 */

#ifndef GNOSTR_NIP60_CASHU_H
#define GNOSTR_NIP60_CASHU_H

#include <glib.h>

G_BEGIN_DECLS

/* NIP-60 Event Kinds */
#define NIP60_KIND_TOKEN   17375  /* Cashu token event (encrypted) */
#define NIP60_KIND_HISTORY 7375   /* Wallet transaction history */

/* Transaction directions */
#define NIP60_DIRECTION_IN  "in"
#define NIP60_DIRECTION_OUT "out"

/* Common currency units */
#define NIP60_UNIT_SAT "sat"
#define NIP60_UNIT_USD "usd"
#define NIP60_UNIT_EUR "eur"

/**
 * GnostrCashuToken:
 * @proofs_json: JSON string containing Cashu proofs array
 * @mint_url: URL of the Cashu mint
 * @amount_msats: Token amount in millisatoshis
 * @unit: Currency unit (sat, usd, eur, etc.)
 * @event_id: Source event ID (hex, 64 chars)
 * @direction: Transaction direction ("in" or "out")
 * @counterparty: Counterparty pubkey if applicable (hex, 64 chars)
 * @related_event_id: Related event ID (e.g., nutzap source)
 * @wallet_ref: Wallet reference "a" tag value
 * @created_at: Token creation timestamp
 *
 * Represents a Cashu token stored in a kind 17375 event.
 * The proofs_json contains the actual ecash proofs that can be redeemed.
 */
typedef struct {
  gchar *proofs_json;        /* Cashu proofs as JSON array */
  gchar *mint_url;           /* Mint URL */
  gint64 amount_msats;       /* Amount in millisatoshis */
  gchar *unit;               /* Currency unit (sat, usd, eur) */
  gchar *event_id;           /* Token event ID (hex) */
  gchar *direction;          /* "in" or "out" */
  gchar *counterparty;       /* Counterparty pubkey (hex, optional) */
  gchar *related_event_id;   /* Related event ID (optional) */
  gchar *wallet_ref;         /* Wallet "a" tag reference */
  gint64 created_at;         /* Event creation timestamp */
} GnostrCashuToken;

/**
 * GnostrCashuTx:
 * @event_id: Transaction history event ID (hex, 64 chars)
 * @direction: Transaction direction ("in" or "out")
 * @amount_msats: Transaction amount in millisatoshis
 * @unit: Currency unit
 * @counterparty: Counterparty pubkey (hex, optional)
 * @timestamp: Transaction timestamp
 * @wallet_ref: Wallet reference "a" tag value
 * @related_event_id: Related event ID (optional)
 *
 * Represents a wallet transaction from the history (kind 7375).
 */
typedef struct {
  gchar *event_id;           /* History event ID (hex) */
  gchar *direction;          /* "in" or "out" */
  gint64 amount_msats;       /* Amount in millisatoshis */
  gchar *unit;               /* Currency unit */
  gchar *counterparty;       /* Counterparty pubkey (hex, optional) */
  gint64 timestamp;          /* Transaction timestamp */
  gchar *wallet_ref;         /* Wallet "a" tag reference */
  gchar *related_event_id;   /* Related event ID (optional) */
} GnostrCashuTx;

/* ============== Kind Checking ============== */

/**
 * gnostr_nip60_is_token_kind:
 * @kind: Event kind number
 *
 * Check if an event kind is a Cashu token event (kind 17375).
 *
 * Returns: TRUE if kind is 17375
 */
gboolean gnostr_nip60_is_token_kind(gint kind);

/**
 * gnostr_nip60_is_history_kind:
 * @kind: Event kind number
 *
 * Check if an event kind is a wallet history event (kind 7375).
 *
 * Returns: TRUE if kind is 7375
 */
gboolean gnostr_nip60_is_history_kind(gint kind);

/* ============== Token API ============== */

/**
 * gnostr_cashu_token_new:
 *
 * Create a new empty Cashu token structure.
 *
 * Returns: (transfer full): New token structure. Free with gnostr_cashu_token_free().
 */
GnostrCashuToken *gnostr_cashu_token_new(void);

/**
 * gnostr_cashu_token_free:
 * @token: Token to free
 *
 * Free a Cashu token and all its allocated memory.
 */
void gnostr_cashu_token_free(GnostrCashuToken *token);

/**
 * gnostr_cashu_token_parse:
 * @event_json: JSON string of a kind 17375 event
 * @decrypted_content: The decrypted content (NIP-44 decryption done externally)
 *
 * Parse a Cashu token from event JSON.
 * The content field should already be decrypted using NIP-44.
 *
 * Returns: (transfer full) (nullable): Parsed token or NULL on error.
 *          Free with gnostr_cashu_token_free().
 */
GnostrCashuToken *gnostr_cashu_token_parse(const gchar *event_json,
                                            const gchar *decrypted_content);

/**
 * gnostr_cashu_token_parse_tags:
 * @event_json: JSON string of a kind 17375 event
 *
 * Parse only the tags from a token event (without decrypted content).
 * Useful for displaying transaction metadata before decryption.
 *
 * Returns: (transfer full) (nullable): Partially parsed token with tags only.
 */
GnostrCashuToken *gnostr_cashu_token_parse_tags(const gchar *event_json);

/* ============== Transaction API ============== */

/**
 * gnostr_cashu_tx_new:
 *
 * Create a new empty transaction structure.
 *
 * Returns: (transfer full): New transaction structure. Free with gnostr_cashu_tx_free().
 */
GnostrCashuTx *gnostr_cashu_tx_new(void);

/**
 * gnostr_cashu_tx_free:
 * @tx: Transaction to free
 *
 * Free a transaction and all its allocated memory.
 */
void gnostr_cashu_tx_free(GnostrCashuTx *tx);

/**
 * gnostr_cashu_tx_parse:
 * @event_json: JSON string of a kind 7375 event
 *
 * Parse a wallet transaction from event JSON.
 *
 * Returns: (transfer full) (nullable): Parsed transaction or NULL on error.
 *          Free with gnostr_cashu_tx_free().
 */
GnostrCashuTx *gnostr_cashu_tx_parse(const gchar *event_json);

/* ============== Tag Building ============== */

/**
 * gnostr_cashu_build_token_tags:
 * @wallet_ref: Wallet "a" tag reference (e.g., "37375:<pubkey>:<d-tag>")
 * @direction: Transaction direction ("in" or "out")
 * @amount_msats: Amount in millisatoshis
 * @unit: Currency unit (sat, usd, eur)
 * @counterparty: (nullable): Counterparty pubkey (hex)
 * @related_event_id: (nullable): Related event ID
 *
 * Build the tags array for a kind 17375 token event.
 * The returned array contains tag arrays (each tag is a GPtrArray of strings).
 *
 * Returns: (transfer full) (element-type GPtrArray): Array of tag arrays.
 *          Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_cashu_build_token_tags(const gchar *wallet_ref,
                                          const gchar *direction,
                                          gint64 amount_msats,
                                          const gchar *unit,
                                          const gchar *counterparty,
                                          const gchar *related_event_id);

/**
 * gnostr_cashu_build_history_tags:
 * @wallet_ref: Wallet "a" tag reference
 * @direction: Transaction direction ("in" or "out")
 * @amount_msats: Amount in millisatoshis
 * @unit: Currency unit
 * @counterparty: (nullable): Counterparty pubkey (hex)
 * @related_event_id: (nullable): Related event ID
 *
 * Build the tags array for a kind 7375 history event.
 *
 * Returns: (transfer full) (element-type GPtrArray): Array of tag arrays.
 *          Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_cashu_build_history_tags(const gchar *wallet_ref,
                                            const gchar *direction,
                                            gint64 amount_msats,
                                            const gchar *unit,
                                            const gchar *counterparty,
                                            const gchar *related_event_id);

/* ============== Utility Functions ============== */

/**
 * gnostr_cashu_format_amount:
 * @amount_msats: Amount in millisatoshis
 * @unit: Currency unit
 *
 * Format a Cashu amount for display (e.g., "1000 sats", "$1.50").
 *
 * Returns: (transfer full): Formatted string
 */
gchar *gnostr_cashu_format_amount(gint64 amount_msats, const gchar *unit);

/**
 * gnostr_cashu_validate_direction:
 * @direction: Direction string to validate
 *
 * Validate that a direction string is either "in" or "out".
 *
 * Returns: TRUE if valid direction
 */
gboolean gnostr_cashu_validate_direction(const gchar *direction);

/**
 * gnostr_cashu_validate_unit:
 * @unit: Unit string to validate
 *
 * Validate that a unit string is a known currency unit.
 *
 * Returns: TRUE if valid unit (sat, usd, eur, etc.)
 */
gboolean gnostr_cashu_validate_unit(const gchar *unit);

/**
 * gnostr_cashu_get_mint_from_proofs:
 * @proofs_json: JSON string containing Cashu proofs
 *
 * Extract the mint URL from Cashu proofs JSON.
 *
 * Returns: (transfer full) (nullable): Mint URL or NULL if not found
 */
gchar *gnostr_cashu_get_mint_from_proofs(const gchar *proofs_json);

/**
 * gnostr_cashu_calculate_proofs_amount:
 * @proofs_json: JSON string containing Cashu proofs
 *
 * Calculate the total amount from Cashu proofs.
 *
 * Returns: Total amount in the mint's base unit, or 0 on error
 */
gint64 gnostr_cashu_calculate_proofs_amount(const gchar *proofs_json);

G_END_DECLS

#endif /* GNOSTR_NIP60_CASHU_H */
