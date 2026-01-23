#ifndef NOSTR_NIP57_H
#define NOSTR_NIP57_H

/**
 * NIP-57: Lightning Zaps
 *
 * Implements zap requests (kind 9734) and zap receipts (kind 9735) for
 * recording lightning payments between users on the Nostr network.
 *
 * Protocol flow:
 * 1. Client gets recipient's LNURL-pay info (must have allowsNostr=true)
 * 2. Client creates a zap request event (kind 9734)
 * 3. Zap request is sent to recipient's LNURL callback URL (not published)
 * 4. LNURL server returns a BOLT11 invoice
 * 5. After payment, LNURL server creates and publishes zap receipt (kind 9735)
 */

#include <stdbool.h>
#include <stdint.h>
#include "nostr/nip57/nip57_types.h"
#include "nostr-event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Zap Request (Kind 9734) Functions
 * ============================================================================ */

/**
 * nostr_nip57_create_zap_request:
 * @sender_sk_hex: sender's private key (64 hex chars)
 * @recipient_pubkey_hex: recipient's public key (64 hex chars)
 * @relays: NULL-terminated array of relay URLs for receipt publication
 * @amount_msats: amount in millisats (use 0 to omit)
 * @lnurl: bech32-encoded lnurl (nullable, recommended)
 * @content: optional message/comment (nullable)
 * @event_id_hex: event ID being zapped (nullable, for zapping notes)
 * @event_coordinate: event coordinate for addressable events (nullable)
 * @event_kind: kind of target event (-1 to omit)
 *
 * Creates a signed zap request event (kind 9734).
 * This event should NOT be published to relays; instead, it should be
 * sent to the recipient's LNURL pay callback URL.
 *
 * Returns: (transfer full) (nullable): signed zap request event, or NULL on error
 */
NostrEvent *nostr_nip57_create_zap_request(const char *sender_sk_hex,
                                            const char *recipient_pubkey_hex,
                                            const char **relays,
                                            uint64_t amount_msats,
                                            const char *lnurl,
                                            const char *content,
                                            const char *event_id_hex,
                                            const char *event_coordinate,
                                            int event_kind);

/**
 * nostr_nip57_parse_zap_request:
 * @event: (transfer none): event to parse (should be kind 9734)
 *
 * Parses a zap request event and extracts its fields.
 *
 * Returns: (transfer full) (nullable): parsed zap request, or NULL on error
 */
NostrZapRequest *nostr_nip57_parse_zap_request(NostrEvent *event);

/**
 * nostr_nip57_validate_zap_request:
 * @event: (transfer none): zap request event to validate
 *
 * Validates a zap request according to NIP-57 requirements:
 * - Must have valid signature
 * - Must have tags
 * - Must have exactly one p tag
 * - Must have 0 or 1 e tags
 * - Should have relays tag
 *
 * Returns: true if valid, false otherwise
 */
bool nostr_nip57_validate_zap_request(NostrEvent *event);

/**
 * nostr_nip57_zap_request_to_json:
 * @event: (transfer none): zap request event
 *
 * Serializes a zap request event to JSON for use in LNURL callback.
 * Caller must free the returned string.
 *
 * Returns: (transfer full) (nullable): JSON string or NULL on error
 */
char *nostr_nip57_zap_request_to_json(NostrEvent *event);

/**
 * nostr_nip57_build_callback_url:
 * @callback: LNURL pay callback URL
 * @amount_msats: amount in millisats
 * @zap_request_json: JSON-encoded zap request event
 * @lnurl: bech32-encoded lnurl (nullable)
 *
 * Builds the full callback URL with query parameters for requesting
 * a zap invoice. Caller must free the returned string.
 *
 * Returns: (transfer full) (nullable): complete URL or NULL on error
 */
char *nostr_nip57_build_callback_url(const char *callback,
                                      uint64_t amount_msats,
                                      const char *zap_request_json,
                                      const char *lnurl);

/* ============================================================================
 * Zap Receipt (Kind 9735) Functions
 * ============================================================================ */

/**
 * nostr_nip57_parse_zap_receipt:
 * @event: (transfer none): event to parse (should be kind 9735)
 *
 * Parses a zap receipt event and extracts its fields.
 *
 * Returns: (transfer full) (nullable): parsed zap receipt, or NULL on error
 */
NostrZapReceipt *nostr_nip57_parse_zap_receipt(NostrEvent *event);

/**
 * nostr_nip57_validate_zap_receipt:
 * @receipt_event: (transfer none): zap receipt event (kind 9735)
 * @expected_provider_pubkey: expected pubkey of LNURL provider (nullable to skip check)
 *
 * Validates a zap receipt according to NIP-57 requirements:
 * - Must have valid signature
 * - Must have bolt11 tag
 * - Must have description tag with valid zap request JSON
 * - Provider pubkey should match if specified
 * - Invoice amount should match zap request amount (if specified)
 *
 * Returns: true if valid, false otherwise
 */
bool nostr_nip57_validate_zap_receipt(NostrEvent *receipt_event,
                                       const char *expected_provider_pubkey);

/**
 * nostr_nip57_validate_zap_receipt_full:
 * @receipt_event: (transfer none): zap receipt event (kind 9735)
 * @expected_provider_pubkey: expected pubkey of LNURL provider (nullable to skip check)
 * @expected_recipient_lnurl: expected recipient lnurl (nullable to skip check)
 *
 * Full validation of a zap receipt including optional lnurl check.
 *
 * Returns: true if valid, false otherwise
 */
bool nostr_nip57_validate_zap_receipt_full(NostrEvent *receipt_event,
                                            const char *expected_provider_pubkey,
                                            const char *expected_recipient_lnurl);

/**
 * nostr_nip57_get_zap_amount:
 * @receipt: (transfer none): parsed zap receipt
 *
 * Extracts the zap amount in millisats from a zap receipt's BOLT11 invoice.
 *
 * Returns: amount in millisats, or 0 on error
 */
uint64_t nostr_nip57_get_zap_amount(const NostrZapReceipt *receipt);

/**
 * nostr_nip57_get_zap_amount_from_event:
 * @receipt_event: (transfer none): zap receipt event (kind 9735)
 *
 * Extracts the zap amount in millisats directly from a zap receipt event.
 *
 * Returns: amount in millisats, or 0 on error
 */
uint64_t nostr_nip57_get_zap_amount_from_event(NostrEvent *receipt_event);

/**
 * nostr_nip57_extract_zap_request_from_receipt:
 * @receipt_event: (transfer none): zap receipt event (kind 9735)
 *
 * Extracts and parses the embedded zap request from a zap receipt's
 * description tag.
 *
 * Returns: (transfer full) (nullable): parsed zap request, or NULL on error
 */
NostrZapRequest *nostr_nip57_extract_zap_request_from_receipt(NostrEvent *receipt_event);

/* ============================================================================
 * BOLT11 Invoice Parsing Helpers
 * ============================================================================ */

/**
 * nostr_nip57_parse_bolt11_amount:
 * @bolt11: BOLT11 invoice string
 *
 * Parses the amount from a BOLT11 invoice.
 *
 * Returns: amount in millisats, or 0 on error/no amount
 */
uint64_t nostr_nip57_parse_bolt11_amount(const char *bolt11);

/**
 * nostr_nip57_get_bolt11_description_hash:
 * @bolt11: BOLT11 invoice string
 * @hash_out: output buffer for 32-byte hash (must be at least 32 bytes)
 *
 * Extracts the description hash from a BOLT11 invoice.
 * Note: This is a simplified parser and may not handle all BOLT11 variants.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip57_get_bolt11_description_hash(const char *bolt11, uint8_t *hash_out);

/* ============================================================================
 * Zap Split Configuration Functions
 * ============================================================================ */

/**
 * nostr_nip57_parse_zap_splits:
 * @event: (transfer none): event containing zap tags
 *
 * Parses zap split configuration from an event's tags.
 * Used for events that specify multiple zap recipients.
 *
 * Returns: (transfer full) (nullable): split configuration, or NULL if none/error
 */
NostrZapSplitConfig *nostr_nip57_parse_zap_splits(NostrEvent *event);

/**
 * nostr_nip57_calculate_split_amount:
 * @config: (transfer none): split configuration
 * @recipient_index: index of recipient in config
 * @total_msats: total amount to split in millisats
 *
 * Calculates the amount for a specific recipient in a zap split.
 *
 * Returns: amount in millisats for the recipient
 */
uint64_t nostr_nip57_calculate_split_amount(const NostrZapSplitConfig *config,
                                             size_t recipient_index,
                                             uint64_t total_msats);

/* ============================================================================
 * LNURL Helper Functions
 * ============================================================================ */

/**
 * nostr_nip57_parse_lnurl_pay_response:
 * @json: JSON response from LNURL-pay endpoint
 *
 * Parses the response from an LNURL-pay endpoint to extract
 * Nostr-specific fields (allowsNostr, nostrPubkey) and standard
 * LNURL fields (callback, minSendable, maxSendable).
 *
 * Returns: (transfer full) (nullable): parsed info, or NULL on error
 */
NostrLnurlPayInfo *nostr_nip57_parse_lnurl_pay_response(const char *json);

/**
 * nostr_nip57_lud16_to_lnurl_url:
 * @lud16: Lightning address (e.g., "user@domain.com")
 *
 * Converts a LUD-16 lightning address to its LNURL-pay endpoint URL.
 * Caller must free the returned string.
 *
 * Returns: (transfer full) (nullable): URL string or NULL on error
 */
char *nostr_nip57_lud16_to_lnurl_url(const char *lud16);

/**
 * nostr_nip57_encode_lnurl:
 * @url: URL to encode
 *
 * Encodes a URL as a bech32 lnurl string.
 * Caller must free the returned string.
 *
 * Returns: (transfer full) (nullable): bech32 lnurl or NULL on error
 */
char *nostr_nip57_encode_lnurl(const char *url);

/**
 * nostr_nip57_decode_lnurl:
 * @lnurl: bech32-encoded lnurl
 *
 * Decodes a bech32 lnurl to its original URL.
 * Caller must free the returned string.
 *
 * Returns: (transfer full) (nullable): URL string or NULL on error
 */
char *nostr_nip57_decode_lnurl(const char *lnurl);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP57_H */
