/**
 * NIP-A4 (Kind 164) Public Messages Support
 *
 * This module provides data structures and utilities for NIP-A4 public messages:
 * - Kind 164 (0xA4): Public message/announcement
 *
 * Public messages are broadcast messages useful for:
 * - Public service announcements
 * - Broadcast messages to followers
 * - Location-based alerts
 *
 * Event Structure:
 * - kind: 164
 * - content: message content
 * - tags:
 *   - ["subject", "<subject-line>"] - message subject (like email)
 *   - ["t", "<tag>"] - topic tags (repeatable)
 *   - ["expiration", "<timestamp>"] - when message expires
 *   - ["p", "<pubkey>"] - recipients (repeatable, optional)
 *   - ["location", "<geo>"] - location tag
 *   - ["g", "<geohash>"] - geohash for location-based filtering
 */

#ifndef GNOSTR_NIP_A4_PUBLIC_H
#define GNOSTR_NIP_A4_PUBLIC_H

#include <glib.h>

G_BEGIN_DECLS

/* NIP-A4 Event Kind */
#define NIPA4_KIND_PUBLIC_MESSAGE 164

/**
 * GnostrPublicMessage - Represents a NIP-A4 public message
 */
typedef struct {
    gchar *event_id;          /* Event ID (hex) */
    gchar *pubkey;            /* Author's pubkey (hex) */
    gchar *subject;           /* Message subject line */
    gchar *content;           /* Message content */
    gchar **tags;             /* Topic tags (NULL-terminated) */
    gsize tag_count;          /* Number of topic tags */
    gint64 expiration;        /* Expiration timestamp (0 = no expiration) */
    gchar **recipients;       /* Recipient pubkeys (NULL-terminated) */
    gsize recipient_count;    /* Number of recipients */
    gchar *location;          /* Location string */
    gchar *geohash;           /* Geohash for location filtering */
    gint64 created_at;        /* Event creation timestamp */
} GnostrPublicMessage;

/**
 * gnostr_public_message_new:
 *
 * Allocate a new GnostrPublicMessage structure.
 *
 * Returns: (transfer full): New message structure. Free with
 *          gnostr_public_message_free().
 */
GnostrPublicMessage *gnostr_public_message_new(void);

/**
 * gnostr_public_message_free:
 * @msg: Message to free
 *
 * Free a GnostrPublicMessage structure and all its contents.
 */
void gnostr_public_message_free(GnostrPublicMessage *msg);

/**
 * gnostr_public_message_copy:
 * @msg: Message to copy
 *
 * Create a deep copy of a GnostrPublicMessage.
 *
 * Returns: (transfer full) (nullable): Copy of the message, or NULL if
 *          input is NULL. Free with gnostr_public_message_free().
 */
GnostrPublicMessage *gnostr_public_message_copy(const GnostrPublicMessage *msg);

/**
 * gnostr_public_message_parse:
 * @json_str: JSON string of the event
 *
 * Parse a NIP-A4 public message event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed message or NULL on error.
 *          Free with gnostr_public_message_free().
 */
GnostrPublicMessage *gnostr_public_message_parse(const gchar *json_str);

/**
 * gnostr_public_message_is_expired:
 * @msg: Public message
 *
 * Check if the message has passed its expiration time.
 *
 * Returns: TRUE if expired, FALSE otherwise
 */
gboolean gnostr_public_message_is_expired(const GnostrPublicMessage *msg);

/**
 * gnostr_public_message_has_expiration:
 * @msg: Public message
 *
 * Check if the message has an expiration timestamp.
 *
 * Returns: TRUE if message has expiration set
 */
gboolean gnostr_public_message_has_expiration(const GnostrPublicMessage *msg);

/**
 * gnostr_public_message_add_topic:
 * @msg: Public message
 * @topic: Topic tag to add
 *
 * Add a topic tag to the message.
 */
void gnostr_public_message_add_topic(GnostrPublicMessage *msg, const gchar *topic);

/**
 * gnostr_public_message_add_recipient:
 * @msg: Public message
 * @pubkey: Recipient pubkey (hex)
 *
 * Add a recipient to the message.
 */
void gnostr_public_message_add_recipient(GnostrPublicMessage *msg, const gchar *pubkey);

/**
 * gnostr_public_message_build_tags:
 * @msg: Public message with data to serialize
 *
 * Build the tags array JSON for a kind-164 public message event.
 *
 * Returns: (transfer full) (nullable): JSON array string of tags,
 *          or NULL on error. Caller must free.
 */
gchar *gnostr_public_message_build_tags(const GnostrPublicMessage *msg);

/**
 * gnostr_public_message_build_event:
 * @msg: Public message with data to serialize
 *
 * Build an unsigned kind-164 public message event JSON.
 * The event must be signed before publishing.
 *
 * Returns: (transfer full) (nullable): JSON string of the unsigned event,
 *          or NULL on error. Caller must free.
 */
gchar *gnostr_public_message_build_event(const GnostrPublicMessage *msg);

/**
 * gnostr_public_message_is_kind:
 * @kind: Event kind
 *
 * Check if an event kind is a NIP-A4 public message (kind 164).
 *
 * Returns: TRUE if kind is 164
 */
gboolean gnostr_public_message_is_kind(gint kind);

/**
 * gnostr_public_message_format_expiration:
 * @expiration: Expiration timestamp
 *
 * Format the expiration time for display.
 *
 * Returns: (transfer full) (nullable): Formatted string or NULL if no expiration.
 *          Caller must free.
 */
gchar *gnostr_public_message_format_expiration(gint64 expiration);

G_END_DECLS

#endif /* GNOSTR_NIP_A4_PUBLIC_H */
