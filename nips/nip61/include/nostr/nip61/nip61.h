#ifndef NIPS_NIP61_NOSTR_NIP61_NIP61_H
#define NIPS_NIP61_NOSTR_NIP61_NIP61_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NIP-61: Nutzaps (Cashu-based Zaps)
 *
 * Event kinds:
 *   10019 — Nutzap preferences (mints, relays, p2pk)
 *   9321  — Nutzap event (proofs, mint, recipient)
 *
 * Tags on a nutzap event (kind 9321):
 *   ["proofs", "<json array of cashu proofs>"]
 *   ["u", "<mint-url>"]
 *   ["p", "<recipient-pubkey>"]
 *   ["e", "<zapped-event-id>", "<relay>"]  (optional)
 *   ["a", "<kind:pubkey:d-tag>"]            (optional)
 *
 * Tags on a nutzap prefs event (kind 10019):
 *   ["mint", "<url>", "<unit>", "<optional-pubkey>"]  (one per mint)
 *   ["relay", "<url>"]                                (one per relay)
 *   ["p2pk"]                                          (if required)
 */

/** Event kinds */
#define NOSTR_NIP61_KIND_NUTZAP       9321
#define NOSTR_NIP61_KIND_NUTZAP_PREFS 10019

/**
 * Accepted mint in nutzap preferences.
 * Borrowed pointers valid while source event is alive.
 */
typedef struct {
    const char *url;    /**< Mint URL (borrowed) */
    const char *unit;   /**< Currency unit, e.g. "sat" (borrowed) */
    const char *pubkey; /**< Optional P2PK pubkey (borrowed, nullable) */
} NostrNip61Mint;

/**
 * Parsed nutzap preferences from kind 10019.
 */
typedef struct {
    NostrNip61Mint *mints;  /**< Array of accepted mints (borrowed ptrs) */
    size_t mint_count;

    const char **relays;    /**< Array of relay URLs (borrowed ptrs) */
    size_t relay_count;

    bool require_p2pk;      /**< Whether p2pk tag is present */
} NostrNip61Prefs;

/**
 * Parsed nutzap event from kind 9321.
 * Borrowed pointers valid while source event is alive.
 */
typedef struct {
    const char *proofs_json;      /**< Raw proofs JSON string (borrowed) */
    const char *mint_url;         /**< Mint URL (borrowed) */
    const char *recipient_pubkey; /**< Recipient pubkey (borrowed) */
    const char *zapped_event_id;  /**< Zapped event ID (borrowed, nullable) */
    const char *zapped_relay;     /**< Zapped event relay (borrowed, nullable) */
    const char *addressable_ref;  /**< Addressable event ref (borrowed, nullable) */
} NostrNip61Nutzap;

/* ---- Preferences (kind 10019) ---- */

/**
 * Parse nutzap preferences from a kind 10019 event.
 *
 * @ev: The event to parse
 * @mints: Caller-provided array for mints
 * @max_mints: Capacity of mints array
 * @relays: Caller-provided array for relay pointers
 * @max_relays: Capacity of relays array
 * @out: Output prefs struct (uses caller's arrays)
 *
 * Returns 0 on success, -EINVAL on bad input.
 */
int nostr_nip61_parse_prefs(const NostrEvent *ev,
                             NostrNip61Mint *mints, size_t max_mints,
                             const char **relays, size_t max_relays,
                             NostrNip61Prefs *out);

/**
 * Create a nutzap preferences event (kind 10019).
 */
int nostr_nip61_create_prefs(NostrEvent *ev,
                              const NostrNip61Mint *mints, size_t mint_count,
                              const char **relays, size_t relay_count,
                              bool require_p2pk);

/**
 * Check if preferences accept a given mint URL.
 */
bool nostr_nip61_prefs_accepts_mint(const NostrNip61Prefs *prefs,
                                     const char *mint_url);

/* ---- Nutzap event (kind 9321) ---- */

/**
 * Parse a nutzap event (kind 9321).
 * Borrowed pointers valid while event is alive.
 *
 * Returns 0 on success, -EINVAL on bad input or missing required fields.
 */
int nostr_nip61_parse_nutzap(const NostrEvent *ev,
                              NostrNip61Nutzap *out);

/**
 * Create a nutzap event (kind 9321).
 */
int nostr_nip61_create_nutzap(NostrEvent *ev,
                               const char *proofs_json,
                               const char *mint_url,
                               const char *recipient_pubkey,
                               const char *zapped_event_id,
                               const char *zapped_relay,
                               const char *addressable_ref);

/* ---- Utilities ---- */

/** Validate a mint URL (must be https:// or localhost). */
bool nostr_nip61_is_valid_mint_url(const char *url);

/**
 * Calculate total amount from a Cashu proofs JSON array.
 * Scans for "amount":<number> patterns in the JSON.
 * Returns total, or 0 on error.
 */
int64_t nostr_nip61_proofs_total_amount(const char *proofs_json);

/**
 * Count the number of proofs in a JSON proofs array.
 */
size_t nostr_nip61_proofs_count(const char *proofs_json);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP61_NOSTR_NIP61_NIP61_H */
