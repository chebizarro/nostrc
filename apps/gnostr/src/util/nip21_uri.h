/**
 * NIP-21: nostr: URI Scheme
 *
 * This module implements NIP-21 which defines the nostr: URI scheme
 * for referencing Nostr entities via URIs.
 *
 * Supported URI formats:
 *   - nostr:npub1...     - Link to a user profile (bare public key)
 *   - nostr:note1...     - Link to an event (bare event ID)
 *   - nostr:nprofile1... - Link to a profile with relay hints (TLV)
 *   - nostr:nevent1...   - Link to an event with relay hints (TLV)
 *   - nostr:naddr1...    - Link to an addressable event (TLV)
 *
 * See: https://github.com/nostr-protocol/nips/blob/master/21.md
 */

#ifndef GNOSTR_NIP21_URI_H
#define GNOSTR_NIP21_URI_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GnostrUriType:
 * @GNOSTR_URI_TYPE_UNKNOWN: Unknown or invalid URI type
 * @GNOSTR_URI_TYPE_NPUB: Bare public key (npub1...)
 * @GNOSTR_URI_TYPE_NOTE: Bare event ID (note1...)
 * @GNOSTR_URI_TYPE_NPROFILE: Profile with relay hints (nprofile1...)
 * @GNOSTR_URI_TYPE_NEVENT: Event with relay hints (nevent1...)
 * @GNOSTR_URI_TYPE_NADDR: Addressable event pointer (naddr1...)
 *
 * Enum representing the type of nostr: URI.
 */
typedef enum {
  GNOSTR_URI_TYPE_UNKNOWN = 0,
  GNOSTR_URI_TYPE_NPUB,
  GNOSTR_URI_TYPE_NOTE,
  GNOSTR_URI_TYPE_NPROFILE,
  GNOSTR_URI_TYPE_NEVENT,
  GNOSTR_URI_TYPE_NADDR
} GnostrUriType;

/**
 * GnostrUri:
 * @type: The type of nostr: URI
 * @raw_bech32: The raw bech32-encoded string (without nostr: prefix)
 * @pubkey_hex: The public key in hex format (64 chars, for npub/nprofile/naddr)
 * @event_id_hex: The event ID in hex format (64 chars, for note/nevent)
 * @relays: Array of relay URLs (for nprofile/nevent/naddr)
 * @relay_count: Number of relays in the array
 * @kind: Event kind (for nevent/naddr, -1 if not specified)
 * @d_tag: The d-tag identifier (for naddr)
 * @author_hex: Author pubkey in hex (for nevent, 64 chars)
 *
 * Structure representing a parsed nostr: URI.
 */
typedef struct {
  GnostrUriType type;
  gchar *raw_bech32;
  gchar *pubkey_hex;
  gchar *event_id_hex;
  gchar **relays;
  gsize relay_count;
  gint kind;
  gchar *d_tag;
  gchar *author_hex;
} GnostrUri;

/**
 * gnostr_uri_parse:
 * @uri: The nostr: URI string to parse
 *
 * Parse a nostr: URI and extract its components.
 * Handles both "nostr:" prefix and URL-encoded URIs.
 *
 * The URI must start with "nostr:" (case-insensitive) followed
 * by a valid NIP-19 bech32 string.
 *
 * Returns: (transfer full) (nullable): A newly allocated #GnostrUri
 *   structure on success, or %NULL if parsing fails. Free with
 *   gnostr_uri_free().
 */
GnostrUri *gnostr_uri_parse(const gchar *uri);

/**
 * gnostr_uri_build_npub:
 * @pubkey_hex: The public key in hex format (64 chars)
 *
 * Build a nostr: URI from a public key.
 *
 * Returns: (transfer full) (nullable): The nostr: URI string,
 *   or %NULL on error. Free with g_free().
 */
gchar *gnostr_uri_build_npub(const gchar *pubkey_hex);

/**
 * gnostr_uri_build_note:
 * @event_id_hex: The event ID in hex format (64 chars)
 *
 * Build a nostr: URI from an event ID.
 *
 * Returns: (transfer full) (nullable): The nostr: URI string,
 *   or %NULL on error. Free with g_free().
 */
gchar *gnostr_uri_build_note(const gchar *event_id_hex);

/**
 * gnostr_uri_build_nprofile:
 * @pubkey_hex: The public key in hex format (64 chars)
 * @relays: (nullable) (array length=relay_count): Array of relay URLs
 * @relay_count: Number of relays in the array
 *
 * Build a nostr: URI for a profile with relay hints.
 *
 * Returns: (transfer full) (nullable): The nostr: URI string,
 *   or %NULL on error. Free with g_free().
 */
gchar *gnostr_uri_build_nprofile(const gchar *pubkey_hex,
                                  const gchar *const *relays,
                                  gsize relay_count);

/**
 * gnostr_uri_build_nevent:
 * @event_id_hex: The event ID in hex format (64 chars)
 * @relays: (nullable) (array length=relay_count): Array of relay URLs
 * @relay_count: Number of relays in the array
 * @author_hex: (nullable): Author pubkey in hex (64 chars)
 * @kind: Event kind (-1 to omit)
 *
 * Build a nostr: URI for an event with relay hints.
 *
 * Returns: (transfer full) (nullable): The nostr: URI string,
 *   or %NULL on error. Free with g_free().
 */
gchar *gnostr_uri_build_nevent(const gchar *event_id_hex,
                                const gchar *const *relays,
                                gsize relay_count,
                                const gchar *author_hex,
                                gint kind);

/**
 * gnostr_uri_build_naddr:
 * @pubkey_hex: The author pubkey in hex format (64 chars)
 * @kind: The event kind (required, must be > 0)
 * @d_tag: The d-tag identifier (required)
 * @relays: (nullable) (array length=relay_count): Array of relay URLs
 * @relay_count: Number of relays in the array
 *
 * Build a nostr: URI for an addressable event.
 *
 * Returns: (transfer full) (nullable): The nostr: URI string,
 *   or %NULL on error. Free with g_free().
 */
gchar *gnostr_uri_build_naddr(const gchar *pubkey_hex,
                               gint kind,
                               const gchar *d_tag,
                               const gchar *const *relays,
                               gsize relay_count);

/**
 * gnostr_uri_free:
 * @uri: (nullable): A #GnostrUri to free
 *
 * Free a #GnostrUri structure and all its members.
 */
void gnostr_uri_free(GnostrUri *uri);

/**
 * gnostr_uri_is_valid:
 * @uri_string: The URI string to validate
 *
 * Check if a string is a valid nostr: URI.
 *
 * This is a quick check that validates the URI format without
 * fully parsing the bech32 content.
 *
 * Returns: %TRUE if the string appears to be a valid nostr: URI,
 *   %FALSE otherwise.
 */
gboolean gnostr_uri_is_valid(const gchar *uri_string);

/**
 * gnostr_uri_type_to_string:
 * @type: The URI type
 *
 * Get a string representation of a URI type.
 *
 * Returns: A static string describing the type (do not free).
 */
const gchar *gnostr_uri_type_to_string(GnostrUriType type);

/**
 * gnostr_uri_get_bech32:
 * @uri: A #GnostrUri
 *
 * Get the raw bech32 string from a parsed URI.
 * This is the string without the "nostr:" prefix.
 *
 * Returns: (transfer none): The bech32 string, or %NULL if not available.
 */
const gchar *gnostr_uri_get_bech32(const GnostrUri *uri);

/**
 * gnostr_uri_to_string:
 * @uri: A #GnostrUri
 *
 * Convert a parsed URI back to a nostr: URI string.
 *
 * Returns: (transfer full) (nullable): The nostr: URI string,
 *   or %NULL on error. Free with g_free().
 */
gchar *gnostr_uri_to_string(const GnostrUri *uri);

G_END_DECLS

#endif /* GNOSTR_NIP21_URI_H */
