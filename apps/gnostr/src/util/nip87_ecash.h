/**
 * NIP-87 Ecash Mint Discovery
 *
 * NIP-87 defines Cashu/ecash mint discovery and recommendation through:
 * - Kind 38000: Mint recommendation (parameterized replaceable event)
 *
 * Tags used:
 * - ["d", "<mint-url>"] - unique identifier (mint URL)
 * - ["u", "<mint-url>"] - mint URL
 * - ["network", "mainnet|signet|testnet"] - optional network type
 * - ["k", "<unit>"] - currency unit (sat, usd, eur, etc.)
 * - ["t", "<tag>"] - tags/categories (e.g., "trusted", "custodial")
 *
 * This module provides:
 * - Parsing of kind 38000 mint recommendation events
 * - Mint URL validation (HTTPS required)
 * - Building tags for publishing mint recommendations
 * - Struct types for representing mint information
 */

#ifndef GNOSTR_NIP87_ECASH_H
#define GNOSTR_NIP87_ECASH_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ============== Event Kind Constants ============== */

#define NIP87_KIND_MINT_RECOMMENDATION  38000

/* ============== Network Types ============== */

/**
 * GnostrEcashNetwork:
 * Bitcoin network type for ecash mint
 */
typedef enum {
  GNOSTR_ECASH_NETWORK_UNKNOWN = 0,
  GNOSTR_ECASH_NETWORK_MAINNET,   /* Bitcoin mainnet */
  GNOSTR_ECASH_NETWORK_TESTNET,   /* Bitcoin testnet */
  GNOSTR_ECASH_NETWORK_SIGNET,    /* Bitcoin signet */
} GnostrEcashNetwork;

/* ============== Mint Recommendation (kind 38000) ============== */

/**
 * GnostrEcashMint:
 * Represents a Cashu/ecash mint recommendation (from kind 38000 events).
 *
 * Published by users to recommend mints they trust or use.
 */
typedef struct {
  gchar *event_id_hex;       /* Event ID of the recommendation */
  gchar *pubkey;             /* Publisher pubkey (hex) */
  gchar *mint_url;           /* Mint URL (from "u" tag or "d" tag) */
  gchar *d_tag;              /* Unique identifier (usually mint URL) */

  /* Network info */
  GnostrEcashNetwork network;  /* Network type (mainnet, testnet, signet) */

  /* Supported currency units */
  gchar **units;             /* Array of unit strings (e.g., "sat", "usd") */
  gsize unit_count;          /* Number of units */

  /* Tags/categories */
  gchar **tags;              /* Array of tag strings (e.g., "trusted", "custodial") */
  gsize tag_count;           /* Number of tags */

  /* Timestamps */
  gint64 created_at;         /* Event created_at */
  gint64 cached_at;          /* Local cache timestamp */
} GnostrEcashMint;

/* ============== Memory Management ============== */

/**
 * gnostr_ecash_mint_new:
 *
 * Creates a new empty ecash mint structure.
 *
 * Returns: (transfer full): A new mint structure. Free with gnostr_ecash_mint_free().
 */
GnostrEcashMint *gnostr_ecash_mint_new(void);

/**
 * gnostr_ecash_mint_free:
 * @mint: Mint to free
 *
 * Frees all memory associated with an ecash mint structure.
 */
void gnostr_ecash_mint_free(GnostrEcashMint *mint);

/**
 * gnostr_ecash_mint_copy:
 * @mint: Mint to copy
 *
 * Creates a deep copy of an ecash mint structure.
 *
 * Returns: (transfer full) (nullable): Copy of the mint or NULL if input is NULL.
 */
GnostrEcashMint *gnostr_ecash_mint_copy(const GnostrEcashMint *mint);

/* ============== Parsing ============== */

/**
 * gnostr_ecash_mint_parse_event:
 * @event_json: JSON string of the kind 38000 event
 *
 * Parses a kind 38000 (mint recommendation) event.
 *
 * Returns: (transfer full) (nullable): Parsed mint or NULL on failure
 */
GnostrEcashMint *gnostr_ecash_mint_parse_event(const gchar *event_json);

/**
 * gnostr_ecash_mint_parse_tags:
 * @mint: Mint structure to populate
 * @tags_json: JSON array of Nostr event tags
 *
 * Parses Nostr tags from a tags JSON array and populates the mint structure.
 * Handles: d, u, network, k, and t tags.
 *
 * Returns: TRUE if parsing succeeded and required tags were found
 */
gboolean gnostr_ecash_mint_parse_tags(GnostrEcashMint *mint, const gchar *tags_json);

/**
 * gnostr_ecash_parse_network:
 * @network_str: Network string (e.g., "mainnet", "testnet", "signet")
 *
 * Converts network string to enum value.
 *
 * Returns: Network enum value
 */
GnostrEcashNetwork gnostr_ecash_parse_network(const gchar *network_str);

/**
 * gnostr_ecash_network_to_string:
 * @network: Network enum value
 *
 * Converts network enum to display string.
 *
 * Returns: (transfer none): Static string for the network type
 */
const gchar *gnostr_ecash_network_to_string(GnostrEcashNetwork network);

/* ============== Validation ============== */

/**
 * gnostr_ecash_validate_mint_url:
 * @url: Mint URL to validate
 *
 * Validates that a mint URL is properly formatted.
 * Requirements:
 * - Must use https:// scheme
 * - Must have a valid host
 *
 * Returns: TRUE if the URL is valid, FALSE otherwise
 */
gboolean gnostr_ecash_validate_mint_url(const gchar *url);

/**
 * gnostr_ecash_normalize_mint_url:
 * @url: Mint URL to normalize
 *
 * Normalizes a mint URL by:
 * - Removing trailing slashes
 * - Converting to lowercase
 * - Validating https:// scheme
 *
 * Returns: (transfer full) (nullable): Normalized URL or NULL if invalid
 */
gchar *gnostr_ecash_normalize_mint_url(const gchar *url);

/* ============== Tag Building ============== */

/**
 * gnostr_ecash_build_recommendation_tags:
 * @mint: Mint information to build tags for
 *
 * Builds a Nostr tags array for a mint recommendation event.
 * Creates tags for: d, u, network (if set), k (for each unit), t (for each tag)
 *
 * Returns: (transfer full) (nullable): JSON string of tags array, or NULL on error.
 *          Caller must g_free().
 */
gchar *gnostr_ecash_build_recommendation_tags(const GnostrEcashMint *mint);

/**
 * gnostr_ecash_build_recommendation_tags_array:
 * @mint: Mint information to build tags for
 *
 * Builds a GPtrArray of tag arrays for a mint recommendation event.
 * Each element is a GPtrArray of gchar* strings representing a single tag.
 *
 * Returns: (transfer full) (element-type GPtrArray) (nullable):
 *          Array of tag arrays, or NULL on error.
 *          Free with g_ptr_array_unref(), elements are freed automatically.
 */
GPtrArray *gnostr_ecash_build_recommendation_tags_array(const GnostrEcashMint *mint);

/* ============== Unit Helpers ============== */

/**
 * gnostr_ecash_is_valid_unit:
 * @unit: Currency unit string
 *
 * Checks if a currency unit string is valid.
 * Valid units: sat, msat, usd, eur, gbp, cad, aud, jpy, chf, cny, etc.
 *
 * Returns: TRUE if valid unit
 */
gboolean gnostr_ecash_is_valid_unit(const gchar *unit);

/**
 * gnostr_ecash_format_unit:
 * @unit: Currency unit code
 *
 * Gets a human-readable name for a currency unit.
 *
 * Returns: (transfer none): Display name (e.g., "Satoshis" for "sat")
 */
const gchar *gnostr_ecash_format_unit(const gchar *unit);

/* ============== Mint List Helpers ============== */

/**
 * gnostr_ecash_mint_has_unit:
 * @mint: Mint to check
 * @unit: Currency unit to search for (e.g., "sat", "usd")
 *
 * Checks if a mint supports a specific currency unit.
 *
 * Returns: TRUE if the mint supports the unit
 */
gboolean gnostr_ecash_mint_has_unit(const GnostrEcashMint *mint, const gchar *unit);

/**
 * gnostr_ecash_mint_has_tag:
 * @mint: Mint to check
 * @tag: Tag to search for (e.g., "trusted", "custodial")
 *
 * Checks if a mint has a specific tag.
 *
 * Returns: TRUE if the mint has the tag
 */
gboolean gnostr_ecash_mint_has_tag(const GnostrEcashMint *mint, const gchar *tag);

/**
 * gnostr_ecash_mint_add_unit:
 * @mint: Mint to modify
 * @unit: Currency unit to add
 *
 * Adds a currency unit to the mint's supported units.
 * Does nothing if the unit is already present.
 */
void gnostr_ecash_mint_add_unit(GnostrEcashMint *mint, const gchar *unit);

/**
 * gnostr_ecash_mint_add_tag:
 * @mint: Mint to modify
 * @tag: Tag to add
 *
 * Adds a tag to the mint.
 * Does nothing if the tag is already present.
 */
void gnostr_ecash_mint_add_tag(GnostrEcashMint *mint, const gchar *tag);

/* ============== Filter Building ============== */

/**
 * gnostr_ecash_build_mint_filter:
 * @pubkeys: (nullable): Specific pubkeys to query, or NULL for all
 * @n_pubkeys: Number of pubkeys
 * @limit: Maximum results (0 for default)
 *
 * Builds a NIP-01 filter JSON for querying kind 38000 events.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
gchar *gnostr_ecash_build_mint_filter(const gchar **pubkeys,
                                       gsize n_pubkeys,
                                       gint limit);

G_END_DECLS

#endif /* GNOSTR_NIP87_ECASH_H */
