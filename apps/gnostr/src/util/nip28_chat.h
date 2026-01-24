/**
 * NIP-28 Public Chat Support
 *
 * This module provides data structures and utilities for NIP-28 public chat:
 * - Kind 40: Create channel (content = JSON metadata)
 * - Kind 41: Set channel metadata
 * - Kind 42: Create message (references channel with "e" tag)
 * - Kind 43: Hide message
 * - Kind 44: Mute user
 *
 * Channel metadata JSON format:
 * {
 *   "name": "channel name",
 *   "about": "channel description",
 *   "picture": "https://example.com/avatar.png"
 * }
 */

#ifndef GNOSTR_NIP28_CHAT_H
#define GNOSTR_NIP28_CHAT_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* NIP-28 Event Kinds */
#define NIP28_KIND_CHANNEL_CREATE   40
#define NIP28_KIND_CHANNEL_METADATA 41
#define NIP28_KIND_CHANNEL_MESSAGE  42
#define NIP28_KIND_HIDE_MESSAGE     43
#define NIP28_KIND_MUTE_USER        44

/**
 * GnostrChannel - Represents a NIP-28 public chat channel
 */
typedef struct {
    char *channel_id;       /* Event ID of the kind-40 create event (hex) */
    char *creator_pubkey;   /* Pubkey of channel creator (hex) */
    char *name;             /* Channel name */
    char *about;            /* Channel description */
    char *picture;          /* Channel avatar URL */
    gint64 created_at;      /* Unix timestamp of creation */
    gint64 metadata_at;     /* Timestamp of last metadata update */
    guint message_count;    /* Approximate message count (for display) */
    guint member_count;     /* Approximate member count (unique posters) */
} GnostrChannel;

/**
 * GnostrChatMessage - Represents a message in a public chat channel
 */
typedef struct {
    char *event_id;         /* Event ID of this message (hex) */
    char *channel_id;       /* Channel this message belongs to (hex) */
    char *author_pubkey;    /* Author's pubkey (hex) */
    char *content;          /* Message content (plaintext) */
    gint64 created_at;      /* Unix timestamp */
    char *reply_to;         /* Event ID being replied to, or NULL */
    char *root_id;          /* Root message ID for threading, or NULL */
    gboolean is_hidden;     /* TRUE if hidden by moderator */
} GnostrChatMessage;

/**
 * Allocate a new GnostrChannel structure
 */
GnostrChannel *gnostr_channel_new(void);

/**
 * Free a GnostrChannel structure
 */
void gnostr_channel_free(GnostrChannel *channel);

/**
 * Deep copy a GnostrChannel
 */
GnostrChannel *gnostr_channel_copy(const GnostrChannel *channel);

/**
 * Allocate a new GnostrChatMessage structure
 */
GnostrChatMessage *gnostr_chat_message_new(void);

/**
 * Free a GnostrChatMessage structure
 */
void gnostr_chat_message_free(GnostrChatMessage *msg);

/**
 * Deep copy a GnostrChatMessage
 */
GnostrChatMessage *gnostr_chat_message_copy(const GnostrChatMessage *msg);

/**
 * Parse channel metadata from JSON content
 * @content: JSON string from kind-40 or kind-41 event content
 * @channel: Channel to populate (name, about, picture fields)
 * @return: TRUE on success, FALSE on parse error
 */
gboolean gnostr_channel_parse_metadata(const char *content, GnostrChannel *channel);

/**
 * Create JSON content for channel metadata
 * @channel: Channel with metadata to serialize
 * @return: Newly allocated JSON string, or NULL on error
 */
char *gnostr_channel_create_metadata_json(const GnostrChannel *channel);

/**
 * Extract channel_id from a kind-42 message event's "e" tags
 * @tags_json: JSON array of tags
 * @return: Newly allocated channel_id (hex), or NULL if not found
 */
char *gnostr_chat_message_extract_channel_id(const char *tags_json);

/**
 * Create tags array for a kind-42 channel message
 * @channel_id: The channel to post to
 * @reply_to: Event ID being replied to (nullable)
 * @recommended_relay: Relay URL hint (nullable)
 * @return: JSON array string for tags, caller must free
 */
char *gnostr_chat_message_create_tags(const char *channel_id,
                                       const char *reply_to,
                                       const char *recommended_relay);

/**
 * Create tags array for a kind-40 channel creation event
 * @return: JSON array string for tags (typically empty), caller must free
 */
char *gnostr_channel_create_tags(void);

/**
 * Create tags array for a kind-41 channel metadata update
 * @channel_id: The channel to update
 * @recommended_relay: Relay URL hint (nullable)
 * @return: JSON array string for tags, caller must free
 */
char *gnostr_channel_metadata_create_tags(const char *channel_id,
                                           const char *recommended_relay);

G_END_DECLS

#endif /* GNOSTR_NIP28_CHAT_H */
