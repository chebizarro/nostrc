#ifndef NIPS_NIP60_NOSTR_NIP60_NIP60_H
#define NIPS_NIP60_NOSTR_NIP60_NIP60_H

#include <nostr/nip60/export.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NIP-60: Cashu Wallet
 *
 * Defines how to store Cashu (ecash) wallet data on Nostr.
 *
 * Event kinds:
 *   17375 — Wallet metadata (mints, privkey in NIP-44 encrypted content)
 *   7375  — Token event (Cashu proofs in NIP-44 encrypted content)
 *   7376  — Transaction history (direction/amount in encrypted content)
 *
 * This module provides tag/event structure helpers. Encryption
 * and Cashu proof handling are separate concerns.
 */

/** Event kinds */
#define NOSTR_NIP60_KIND_WALLET  17375
#define NOSTR_NIP60_KIND_TOKEN   7375
#define NOSTR_NIP60_KIND_HISTORY 7376

/** Transaction direction */
typedef enum {
    NOSTR_NIP60_DIR_IN,   /**< Received */
    NOSTR_NIP60_DIR_OUT,  /**< Sent */
} NostrNip60Direction;

/** Common currency units */
typedef enum {
    NOSTR_NIP60_UNIT_SAT,
    NOSTR_NIP60_UNIT_MSAT,
    NOSTR_NIP60_UNIT_USD,
    NOSTR_NIP60_UNIT_EUR,
    NOSTR_NIP60_UNIT_UNKNOWN,
} NostrNip60Unit;

/**
 * Token event metadata (parsed from tags of kind 7375).
 * Proofs are in the encrypted content — not parsed here.
 */
typedef struct {
    const char *mint;      /**< "mint" tag value (borrowed, nullable) */
    int64_t created_at;    /**< Event created_at */
} NostrNip60Token;

/**
 * Transaction history entry (parsed from kind 7376 tags).
 * The direction and amount tags may be in encrypted content.
 */
typedef struct {
    NostrNip60Direction direction;
    uint64_t amount;       /**< Amount in unit's base denomination */
    const char *unit;      /**< "unit" tag value (borrowed, nullable) */
} NostrNip60HistoryEntry;

/* ---- Kind checking ---- */

/** Check if kind is a wallet metadata event */
NOSTR_NIP60_API bool nostr_nip60_is_wallet_kind(int kind);

/** Check if kind is a token event */
NOSTR_NIP60_API bool nostr_nip60_is_token_kind(int kind);

/** Check if kind is a history event */
NOSTR_NIP60_API bool nostr_nip60_is_history_kind(int kind);

/* ---- Direction ---- */

/**
 * Parse direction string ("in"/"out") to enum.
 * Returns -1 on invalid input.
 */
NOSTR_NIP60_API int nostr_nip60_direction_parse(const char *str);

/** Convert direction enum to string. */
NOSTR_NIP60_API const char *nostr_nip60_direction_string(NostrNip60Direction dir);

/* ---- Unit ---- */

/**
 * Parse unit string to enum.
 * Returns NOSTR_NIP60_UNIT_UNKNOWN for unrecognized units.
 */
NOSTR_NIP60_API NostrNip60Unit nostr_nip60_unit_parse(const char *str);

/** Convert unit enum to string. Returns NULL for UNKNOWN. */
NOSTR_NIP60_API const char *nostr_nip60_unit_string(NostrNip60Unit unit);

/** Check if a unit string is valid/known. */
NOSTR_NIP60_API bool nostr_nip60_unit_is_valid(const char *str);

/* ---- Amount formatting ---- */

/**
 * Format amount for display.
 * @amount: Amount value
 * @unit: Unit string (e.g. "sat", "msat", "usd")
 *
 * Examples: "1000 sat", "0.50 usd"
 * Caller must free() the result.
 */
NOSTR_NIP60_API char *nostr_nip60_format_amount(uint64_t amount, const char *unit);

/* ---- Token event helpers ---- */

/**
 * Parse token event tags. Returns 0 on success.
 * The event must be kind 7375.
 */
NOSTR_NIP60_API int nostr_nip60_parse_token(const NostrEvent *ev, NostrNip60Token *out);

/**
 * Create a token event (kind 7375).
 * Content should be the NIP-44 encrypted JSON with proofs.
 * Sets kind and adds mint tag if provided.
 */
NOSTR_NIP60_API int nostr_nip60_create_token(NostrEvent *ev,
                              const char *encrypted_content,
                              const char *mint_url);

/* ---- History event helpers ---- */

/**
 * Parse decrypted history tags.
 * @tags_json: Decrypted content containing JSON array of tags
 * @out: Output history entry
 *
 * Returns 0 on success, -EINVAL on bad input.
 */
NOSTR_NIP60_API int nostr_nip60_parse_history_tags(const char *tags_json,
                                    NostrNip60HistoryEntry *out);

/**
 * Build history content (unencrypted JSON tags array).
 * Caller encrypts the result with NIP-44 before setting as event content.
 * Caller must free() the result.
 */
NOSTR_NIP60_API char *nostr_nip60_build_history_content(NostrNip60Direction direction,
                                         uint64_t amount,
                                         const char *unit);

/**
 * Create a history event shell (kind 7376).
 * Content should be NIP-44 encrypted output from
 * nostr_nip60_build_history_content().
 */
NOSTR_NIP60_API int nostr_nip60_create_history(NostrEvent *ev,
                                const char *encrypted_content);

/* ---- Wallet metadata helpers ---- */

/**
 * Parse mint URLs from decrypted wallet content.
 * The decrypted content is a JSON array of tags like [["mint","url1"],["mint","url2"]].
 *
 * @tags_json: Decrypted content (JSON tag array)
 * @mints: Output array of mint URL pointers (heap-allocated)
 * @max_mints: Maximum entries to fill
 * @out_count: Actual count found
 *
 * Caller must free() each mint string.
 * Returns 0 on success.
 */
NOSTR_NIP60_API int nostr_nip60_parse_wallet_mints(const char *tags_json,
                                    char **mints,
                                    size_t max_mints,
                                    size_t *out_count);

/**
 * Build wallet metadata content (unencrypted JSON tags array).
 * Caller encrypts with NIP-44 before setting as event content.
 * Caller must free().
 */
NOSTR_NIP60_API char *nostr_nip60_build_wallet_content(const char **mint_urls,
                                        size_t n_mints);

/**
 * Create a wallet metadata event (kind 17375).
 * Content should be NIP-44 encrypted wallet metadata.
 */
NOSTR_NIP60_API int nostr_nip60_create_wallet(NostrEvent *ev,
                               const char *encrypted_content);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP60_NOSTR_NIP60_NIP60_H */
