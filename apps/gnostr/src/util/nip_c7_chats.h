/*
 * nip_c7_chats.h - NIP-C7 (0xC7 / Kind 199) Chat Rooms Support for GNostr
 *
 * NIP-C7 defines chat room messaging:
 *   - Kind 199 (0xC7): Chat message
 *   - Kind 39001: Chat room metadata (room definition)
 *
 * Chat message tags:
 *   - ["d", "<room-id>"] - chat room identifier
 *   - ["name", "<room-name>"] - room display name
 *   - ["about", "<description>"] - room description
 *   - ["picture", "<url>"] - room avatar
 *   - ["e", "<event-id>", "<relay>"] - reply to message
 *   - ["p", "<pubkey>"] - mention user
 *   - ["t", "<topic>"] - room topic tags
 *
 * Room definition (kind 39001) tags:
 *   - ["d", "<room-id>"]
 *   - ["name", "<room-name>"]
 *   - ["about", "<description>"]
 *   - ["picture", "<url>"]
 *   - ["moderator", "<pubkey>"]
 */

#ifndef GNOSTR_NIP_C7_CHATS_H
#define GNOSTR_NIP_C7_CHATS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* NIP-C7 Event Kinds */
#define NIPC7_KIND_CHAT_MESSAGE  199    /* 0xC7 in hex */
#define NIPC7_KIND_CHAT_ROOM     39001  /* Chat room metadata/definition */

/**
 * GnostrChatRoom:
 *
 * Represents a NIP-C7 chat room (kind 39001).
 * Contains room metadata and moderator list.
 */
typedef struct _GnostrChatRoom {
  gchar *room_id;          /* "d" tag value - unique room identifier */
  gchar *name;             /* "name" tag - room display name */
  gchar *about;            /* "about" tag - room description */
  gchar *picture;          /* "picture" tag - room avatar URL */
  gchar *creator_pubkey;   /* Event author - room creator's pubkey (hex) */
  gchar *event_id;         /* Event ID of the room definition */
  gint64 created_at;       /* Creation timestamp */
  gchar **moderators;      /* Array of moderator pubkeys (hex) */
  guint mod_count;         /* Number of moderators */
  GPtrArray *topics;       /* Array of gchar* topic tags */
} GnostrChatRoom;

/**
 * GnostrChatMessage:
 *
 * Represents a NIP-C7 chat message (kind 199).
 * Contains message content and metadata.
 */
typedef struct _GnostrChatMessage {
  gchar *event_id;         /* Event ID of this message (hex) */
  gchar *room_id;          /* "d" tag - room this message belongs to */
  gchar *author_pubkey;    /* Author's pubkey (hex) */
  gchar *content;          /* Message content (plaintext) */
  gint64 created_at;       /* Unix timestamp */
  gchar *reply_to;         /* Event ID being replied to, or NULL */
  gchar *reply_relay;      /* Relay hint for reply, or NULL */
  gchar **mentions;        /* Array of mentioned pubkeys (hex) */
  guint mention_count;     /* Number of mentions */
  GPtrArray *topics;       /* Array of gchar* topic tags */
} GnostrChatMessage;

/* ============== Chat Room API ============== */

/**
 * gnostr_chat_room_new:
 *
 * Creates a new empty chat room structure.
 *
 * Returns: (transfer full): A new chat room. Free with gnostr_chat_room_free().
 */
GnostrChatRoom *gnostr_chat_room_new(void);

/**
 * gnostr_chat_room_free:
 * @room: Chat room to free.
 *
 * Frees a chat room structure and all its contents.
 */
void gnostr_chat_room_free(GnostrChatRoom *room);

/**
 * gnostr_chat_room_copy:
 * @room: Chat room to copy.
 *
 * Creates a deep copy of a chat room.
 *
 * Returns: (transfer full): A new chat room copy, or NULL if @room is NULL.
 */
GnostrChatRoom *gnostr_chat_room_copy(const GnostrChatRoom *room);

/**
 * gnostr_chat_room_parse:
 * @event_json: JSON string of a kind 39001 event.
 *
 * Parses a chat room from event JSON.
 *
 * Returns: (transfer full): Parsed room or NULL on error.
 */
GnostrChatRoom *gnostr_chat_room_parse(const gchar *event_json);

/**
 * gnostr_chat_room_parse_from_tags:
 * @tags_json: JSON array string of event tags.
 * @room: Room structure to populate.
 *
 * Parses room metadata from tags array.
 * Populates room_id, name, about, picture, moderators, and topics.
 *
 * Returns: TRUE on success, FALSE on parse error.
 */
gboolean gnostr_chat_room_parse_from_tags(const gchar *tags_json, GnostrChatRoom *room);

/**
 * gnostr_chat_room_create_tags:
 * @room: Room with metadata to serialize.
 *
 * Creates JSON tags array for a kind 39001 room definition event.
 *
 * Returns: (transfer full): JSON array string for tags, caller must free.
 */
gchar *gnostr_chat_room_create_tags(const GnostrChatRoom *room);

/**
 * gnostr_chat_room_add_moderator:
 * @room: Chat room to modify.
 * @pubkey_hex: Moderator's pubkey in hex format.
 *
 * Adds a moderator to the room's moderator list.
 */
void gnostr_chat_room_add_moderator(GnostrChatRoom *room, const gchar *pubkey_hex);

/**
 * gnostr_chat_room_is_moderator:
 * @room: Chat room to check.
 * @pubkey_hex: Pubkey to check.
 *
 * Checks if a pubkey is a moderator of the room.
 *
 * Returns: TRUE if the pubkey is a moderator.
 */
gboolean gnostr_chat_room_is_moderator(const GnostrChatRoom *room, const gchar *pubkey_hex);

/* ============== Chat Message API ============== */

/**
 * gnostr_c7_chat_message_new:
 *
 * Creates a new empty chat message structure.
 *
 * Returns: (transfer full): A new chat message. Free with gnostr_c7_chat_message_free().
 */
GnostrChatMessage *gnostr_c7_chat_message_new(void);

/**
 * gnostr_c7_chat_message_free:
 * @msg: Chat message to free.
 *
 * Frees a chat message structure and all its contents.
 */
void gnostr_c7_chat_message_free(GnostrChatMessage *msg);

/**
 * gnostr_c7_chat_message_copy:
 * @msg: Chat message to copy.
 *
 * Creates a deep copy of a chat message.
 *
 * Returns: (transfer full): A new message copy, or NULL if @msg is NULL.
 */
GnostrChatMessage *gnostr_c7_chat_message_copy(const GnostrChatMessage *msg);

/**
 * gnostr_c7_chat_message_parse:
 * @event_json: JSON string of a kind 199 event.
 *
 * Parses a chat message from event JSON.
 *
 * Returns: (transfer full): Parsed message or NULL on error.
 */
GnostrChatMessage *gnostr_c7_chat_message_parse(const gchar *event_json);

/**
 * gnostr_c7_chat_message_parse_from_tags:
 * @tags_json: JSON array string of event tags.
 * @msg: Message structure to populate.
 *
 * Parses message metadata from tags array.
 * Populates room_id, reply_to, mentions, and topics.
 *
 * Returns: TRUE on success, FALSE on parse error.
 */
gboolean gnostr_c7_chat_message_parse_from_tags(const gchar *tags_json, GnostrChatMessage *msg);

/**
 * gnostr_c7_chat_message_create_tags:
 * @msg: Message with metadata to serialize.
 *
 * Creates JSON tags array for a kind 199 chat message event.
 *
 * Returns: (transfer full): JSON array string for tags, caller must free.
 */
gchar *gnostr_c7_chat_message_create_tags(const GnostrChatMessage *msg);

/**
 * gnostr_c7_chat_message_add_mention:
 * @msg: Chat message to modify.
 * @pubkey_hex: Mentioned user's pubkey in hex format.
 *
 * Adds a mention to the message's mention list.
 */
void gnostr_c7_chat_message_add_mention(GnostrChatMessage *msg, const gchar *pubkey_hex);

/**
 * gnostr_c7_chat_message_add_topic:
 * @msg: Chat message to modify.
 * @topic: Topic tag to add.
 *
 * Adds a topic tag to the message.
 */
void gnostr_c7_chat_message_add_topic(GnostrChatMessage *msg, const gchar *topic);

/**
 * gnostr_c7_chat_message_extract_room_id:
 * @tags_json: JSON array of tags.
 *
 * Extracts room_id from a kind 199 message event's "d" tag.
 *
 * Returns: (transfer full): Newly allocated room_id, or NULL if not found.
 */
gchar *gnostr_c7_chat_message_extract_room_id(const gchar *tags_json);

G_END_DECLS

#endif /* GNOSTR_NIP_C7_CHATS_H */
