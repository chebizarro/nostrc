#ifndef NIPS_SCHEMA_NOSTR_SCHEMA_SCHEMA_H
#define NIPS_SCHEMA_NOSTR_SCHEMA_SCHEMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Event Schema Validation
 *
 * Provides type validators for common Nostr value formats
 * and event-level validation for known kinds.
 *
 * Type validators check individual field values (pubkeys, event IDs,
 * relay URLs, etc.). Event validators check kind-specific rules
 * (required tags, content format, addressable event "d" tag).
 */

/** Maximum length of a validation error message */
#define NOSTR_SCHEMA_MAX_ERROR 256

/**
 * Validation result.
 */
typedef struct {
    bool valid;
    char error[NOSTR_SCHEMA_MAX_ERROR]; /**< Empty string if valid */
    int tag_index;   /**< -1 if not tag-related */
    int item_index;  /**< -1 if not item-related */
} NostrSchemaResult;

/* ---- Type validators ---- */

/** Check if string is a valid 64-char hex event ID. */
bool nostr_schema_is_valid_id(const char *str);

/** Check if string is a valid 64-char hex public key. */
bool nostr_schema_is_valid_pubkey(const char *str);

/** Check if string is a valid relay URL (ws:// or wss://). */
bool nostr_schema_is_valid_relay_url(const char *str);

/** Check if string is a valid kind number (unsigned integer). */
bool nostr_schema_is_valid_kind_str(const char *str);

/** Check if string is a valid Unix timestamp (unsigned integer). */
bool nostr_schema_is_valid_timestamp(const char *str);

/** Check if string is valid hex (even length, all hex chars). */
bool nostr_schema_is_valid_hex(const char *str);

/** Check if string has no leading/trailing whitespace. */
bool nostr_schema_is_trimmed(const char *str);

/** Basic JSON validity check (balanced braces/brackets, quoted strings). */
bool nostr_schema_is_valid_json(const char *str);

/**
 * Check if string is a valid NIP-33 addressable event reference.
 * Format: <kind>:<pubkey>:<d-tag>
 */
bool nostr_schema_is_valid_addr(const char *str);

/* ---- Kind classification ---- */

/** Check if a kind is addressable (parameterized replaceable). */
bool nostr_schema_is_addressable(int kind);

/** Check if a kind is replaceable. */
bool nostr_schema_is_replaceable(int kind);

/** Check if a kind is ephemeral. */
bool nostr_schema_is_ephemeral(int kind);

/* ---- Event validation ---- */

/**
 * Validate an event against schema rules.
 *
 * Checks:
 *   - Content is trimmed (no dangling whitespace)
 *   - No empty tags
 *   - Kind 0: content must be valid JSON
 *   - Addressable kinds: must have a "d" tag
 *   - "e" tag values: must be valid event IDs
 *   - "p" tag values: must be valid pubkeys
 *   - "a" tag values: must be valid addr references
 *   - Relay hints: must be valid relay URLs
 *
 * Returns a result with valid=true or an error description.
 */
NostrSchemaResult nostr_schema_validate_event(const NostrEvent *ev);

/**
 * Validate a single tag value against a type.
 *
 * @type_name: One of "id", "pubkey", "relay", "kind", "timestamp",
 *             "hex", "json", "addr", "free"
 * @value: The string value to validate
 *
 * Returns true if valid for the given type.
 */
bool nostr_schema_validate_type(const char *type_name, const char *value);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_SCHEMA_NOSTR_SCHEMA_SCHEMA_H */
