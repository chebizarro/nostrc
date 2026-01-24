/**
 * NIP-22 Comment Support
 *
 * This module provides data structures and utilities for NIP-22 comments:
 * - Kind 1111: Comment
 *
 * NIP-22 defines a standardized way to comment on events across different
 * applications. Unlike kind 1 replies which only work with notes, NIP-22
 * comments can reference any event kind using explicit root/reply markers.
 *
 * Tag structure:
 * - ["e", "<event-id>", "<relay>", "root"] - root event being commented on
 * - ["e", "<event-id>", "<relay>", "reply"] - direct parent comment (for nested threads)
 * - ["p", "<pubkey>"] - authors being replied to
 * - ["k", "<kind>"] - kind of the root event
 * - ["a", "<kind:pubkey:d-tag>", "<relay>"] - for parameterized replaceable events
 */

#ifndef GNOSTR_NIP22_COMMENTS_H
#define GNOSTR_NIP22_COMMENTS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* NIP-22 Event Kind */
#define NIP22_KIND_COMMENT 1111

/* Maximum number of mention pubkeys in a comment */
#define NIP22_MAX_MENTIONS 32

/**
 * GnostrComment - Represents a NIP-22 comment
 *
 * This structure holds parsed comment data from a kind 1111 event,
 * including thread structure (root/reply references) and mentions.
 */
typedef struct {
    gchar *content;           /* Comment text content */
    gchar *root_id;           /* Event ID of the root event (hex, 64 chars) */
    gchar *root_relay;        /* Relay hint for root event (optional) */
    gint root_kind;           /* Kind of the root event (from "k" tag) */
    gchar *reply_id;          /* Event ID of direct parent comment (hex, for nested threads) */
    gchar *reply_relay;       /* Relay hint for reply target (optional) */
    gchar *root_addr;         /* Parameterized replaceable event address ("kind:pubkey:d-tag") */
    gchar *root_addr_relay;   /* Relay hint for addressable event (optional) */
    gchar **mentions;         /* Array of pubkey hex strings being replied to */
    gsize mention_count;      /* Number of mentions */
    gint64 created_at;        /* Unix timestamp of comment creation */
    gchar *event_id;          /* Event ID of this comment (hex) */
    gchar *author_pubkey;     /* Author's pubkey (hex) */
} GnostrComment;

/**
 * gnostr_comment_new:
 *
 * Creates a new empty GnostrComment structure.
 * Use gnostr_comment_free() to free.
 *
 * Returns: (transfer full): New comment structure.
 */
GnostrComment *gnostr_comment_new(void);

/**
 * gnostr_comment_free:
 * @comment: The comment to free, may be NULL.
 *
 * Frees a GnostrComment structure and all its contents.
 */
void gnostr_comment_free(GnostrComment *comment);

/**
 * gnostr_comment_copy:
 * @comment: The comment to copy.
 *
 * Creates a deep copy of a GnostrComment structure.
 *
 * Returns: (transfer full) (nullable): Copied comment or NULL if input was NULL.
 */
GnostrComment *gnostr_comment_copy(const GnostrComment *comment);

/**
 * gnostr_comment_parse:
 * @tags_json: JSON array string containing event tags.
 * @content: The event content (comment text).
 *
 * Parses NIP-22 comment structure from an event's tags array.
 * The tags_json should be the JSON representation of the tags array.
 *
 * This function:
 * - Extracts root event reference (e-tag with "root" marker)
 * - Extracts reply event reference (e-tag with "reply" marker)
 * - Extracts root event kind (k-tag)
 * - Extracts addressable event reference (a-tag)
 * - Collects all mentioned pubkeys (p-tags)
 *
 * Returns: (transfer full) (nullable): Parsed comment or NULL on error.
 */
GnostrComment *gnostr_comment_parse(const gchar *tags_json, const gchar *content);

/**
 * gnostr_comment_build_tags:
 * @comment: Comment with reference information set.
 *
 * Builds a JSON tags array string for creating a NIP-22 comment event.
 *
 * Required fields in @comment:
 * - root_id: Event ID of the root event being commented on
 * - root_kind: Kind number of the root event
 *
 * Optional fields:
 * - root_relay: Relay hint for root event
 * - reply_id: Parent comment ID (for nested replies)
 * - reply_relay: Relay hint for parent comment
 * - root_addr: For parameterized replaceable events ("kind:pubkey:d-tag")
 * - root_addr_relay: Relay hint for addressable event
 * - mentions: Array of pubkeys to notify
 *
 * Returns: (transfer full) (nullable): JSON array string for tags, or NULL on error.
 */
gchar *gnostr_comment_build_tags(const GnostrComment *comment);

/**
 * gnostr_comment_is_comment:
 * @kind: Event kind number.
 *
 * Checks if the given kind is a NIP-22 comment.
 *
 * Returns: TRUE if kind is 1111 (comment), FALSE otherwise.
 */
gboolean gnostr_comment_is_comment(gint kind);

/**
 * gnostr_comment_is_nested_reply:
 * @comment: The comment to check.
 *
 * Checks if this comment is a nested reply (i.e., a reply to another comment
 * rather than a direct comment on the root event).
 *
 * Returns: TRUE if reply_id is set, FALSE otherwise.
 */
gboolean gnostr_comment_is_nested_reply(const GnostrComment *comment);

/**
 * gnostr_comment_is_addressable:
 * @comment: The comment to check.
 *
 * Checks if this comment references a parameterized replaceable event
 * (i.e., uses an "a" tag reference instead of or in addition to an "e" tag).
 *
 * Returns: TRUE if root_addr is set, FALSE otherwise.
 */
gboolean gnostr_comment_is_addressable(const GnostrComment *comment);

/**
 * gnostr_comment_add_mention:
 * @comment: The comment to modify.
 * @pubkey: Pubkey hex string to add (will be copied).
 *
 * Adds a pubkey to the mentions array. Prevents duplicates.
 *
 * Returns: TRUE if added, FALSE if already present or limit reached.
 */
gboolean gnostr_comment_add_mention(GnostrComment *comment, const gchar *pubkey);

/**
 * gnostr_comment_set_root_event:
 * @comment: The comment to modify.
 * @event_id: Event ID hex string of the root event.
 * @kind: Kind number of the root event.
 * @relay: Relay hint (nullable).
 *
 * Sets the root event reference for a comment.
 */
void gnostr_comment_set_root_event(GnostrComment *comment,
                                    const gchar *event_id,
                                    gint kind,
                                    const gchar *relay);

/**
 * gnostr_comment_set_reply_target:
 * @comment: The comment to modify.
 * @event_id: Event ID hex string of the parent comment.
 * @relay: Relay hint (nullable).
 *
 * Sets the reply target for a nested comment thread.
 */
void gnostr_comment_set_reply_target(GnostrComment *comment,
                                      const gchar *event_id,
                                      const gchar *relay);

/**
 * gnostr_comment_set_addressable_root:
 * @comment: The comment to modify.
 * @kind: Kind number of the addressable event.
 * @pubkey: Author pubkey of the addressable event (hex).
 * @d_tag: The "d" tag value.
 * @relay: Relay hint (nullable).
 *
 * Sets the addressable event reference for commenting on parameterized
 * replaceable events (NIP-33 style events like articles, badges, etc.).
 */
void gnostr_comment_set_addressable_root(GnostrComment *comment,
                                          gint kind,
                                          const gchar *pubkey,
                                          const gchar *d_tag,
                                          const gchar *relay);

/**
 * gnostr_comment_parse_addr:
 * @addr: The "a" tag value to parse (format: "kind:pubkey:d-tag").
 * @out_kind: (out) (nullable): Parsed kind number.
 * @out_pubkey: (out) (transfer full) (nullable): Parsed pubkey hex.
 * @out_d_tag: (out) (transfer full) (nullable): Parsed d-tag.
 *
 * Parses an "a" tag value into its components.
 *
 * Returns: TRUE if parsing succeeded.
 */
gboolean gnostr_comment_parse_addr(const gchar *addr,
                                    gint *out_kind,
                                    gchar **out_pubkey,
                                    gchar **out_d_tag);

G_END_DECLS

#endif /* GNOSTR_NIP22_COMMENTS_H */
