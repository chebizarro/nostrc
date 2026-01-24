/**
 * @file nip62_vanish.h
 * @brief NIP-62 Request to Vanish implementation
 *
 * NIP-62 defines how users can request relays to delete all their events.
 * A kind 62 event signals that the user wants their data removed from relays.
 *
 * When a relay receives a vanish request, it SHOULD:
 * 1. Delete all stored events from this pubkey
 * 2. Optionally block future events from this pubkey
 * 3. Optionally propagate the vanish request to other relays
 *
 * Tag format:
 * - ["relay", "<relay-url>"] - specific relay to vanish from (repeatable)
 *   If omitted, the request applies to the receiving relay.
 *
 * Content: Human-readable reason for vanishing (optional)
 */

#ifndef GNOSTR_NIP62_VANISH_H
#define GNOSTR_NIP62_VANISH_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * NIP-62 event kind for Request to Vanish
 */
#define NIP62_KIND_VANISH 62

/**
 * GnostrVanishRequest:
 *
 * Structure representing a parsed NIP-62 vanish request.
 */
typedef struct {
  gchar *reason;          /* Human-readable reason for vanishing (optional) */
  gchar **relays;         /* Array of relay URLs to vanish from (NULL-terminated) */
  gsize relay_count;      /* Number of relay URLs */
  gint64 created_at;      /* Timestamp of the request */
  gchar *pubkey_hex;      /* Public key of the user requesting to vanish */
  gchar *event_id_hex;    /* Event ID of the vanish request */
} GnostrVanishRequest;

/**
 * gnostr_vanish_request_new:
 *
 * Creates a new empty vanish request structure.
 *
 * Returns: (transfer full): A new GnostrVanishRequest, free with
 *          gnostr_vanish_request_free()
 */
GnostrVanishRequest *gnostr_vanish_request_new(void);

/**
 * gnostr_vanish_request_free:
 * @request: (nullable): The vanish request to free
 *
 * Frees a vanish request structure and all its contents.
 */
void gnostr_vanish_request_free(GnostrVanishRequest *request);

/**
 * gnostr_vanish_request_parse:
 * @event_json: JSON string of a Nostr event
 *
 * Parses a Nostr event JSON to extract vanish request data.
 * The event must be kind 62 (NIP62_KIND_VANISH).
 *
 * Returns: (transfer full) (nullable): A new GnostrVanishRequest if the event
 *          is a valid vanish request, or NULL on error. Free with
 *          gnostr_vanish_request_free().
 */
GnostrVanishRequest *gnostr_vanish_request_parse(const gchar *event_json);

/**
 * gnostr_vanish_build_request_tags:
 * @relays: (nullable) (array zero-terminated=1): Array of relay URLs to
 *          include in the request, or NULL to target only the receiving relay
 * @relay_count: Number of relay URLs in the array
 *
 * Builds a JSON tags array for a vanish request event.
 * Each relay URL is added as a ["relay", "<url>"] tag.
 *
 * If @relays is NULL or @relay_count is 0, returns an empty tags array,
 * indicating the request applies to the receiving relay.
 *
 * Returns: (transfer full): JSON string representing the tags array.
 *          Caller must free with g_free().
 */
gchar *gnostr_vanish_build_request_tags(const gchar **relays, gsize relay_count);

/**
 * gnostr_vanish_build_unsigned_event:
 * @reason: (nullable): Human-readable reason for vanishing
 * @relays: (nullable) (array zero-terminated=1): Array of relay URLs
 * @relay_count: Number of relay URLs
 *
 * Builds an unsigned kind 62 event JSON for a vanish request.
 * The event needs to be signed before publishing.
 *
 * Returns: (transfer full): JSON string of the unsigned event.
 *          Caller must free with g_free().
 */
gchar *gnostr_vanish_build_unsigned_event(const gchar *reason,
                                           const gchar **relays,
                                           gsize relay_count);

/**
 * gnostr_vanish_is_valid_relay_url:
 * @url: URL string to validate
 *
 * Validates that a URL is a proper Nostr relay URL (ws:// or wss://).
 *
 * Returns: TRUE if the URL is a valid relay URL
 */
gboolean gnostr_vanish_is_valid_relay_url(const gchar *url);

/**
 * gnostr_vanish_request_get_relays:
 * @request: The vanish request
 *
 * Gets a GPtrArray of relay URLs from the vanish request.
 * Useful for iterating over target relays.
 *
 * Returns: (transfer full) (nullable): A new GPtrArray of gchar* URLs,
 *          or NULL if no relays specified. Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_vanish_request_get_relays(const GnostrVanishRequest *request);

/**
 * gnostr_vanish_request_has_relay:
 * @request: The vanish request
 * @relay_url: Relay URL to check for
 *
 * Checks if a specific relay URL is targeted by the vanish request.
 *
 * Returns: TRUE if the relay is in the request's relay list
 */
gboolean gnostr_vanish_request_has_relay(const GnostrVanishRequest *request,
                                          const gchar *relay_url);

/**
 * gnostr_vanish_request_is_global:
 * @request: The vanish request
 *
 * Checks if the vanish request applies globally (no specific relays listed).
 * A global request means it applies to whichever relay receives it.
 *
 * Returns: TRUE if no specific relays are targeted
 */
gboolean gnostr_vanish_request_is_global(const GnostrVanishRequest *request);

G_END_DECLS

#endif /* GNOSTR_NIP62_VANISH_H */
