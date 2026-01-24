/**
 * NIP-7D Forum Threads Support
 *
 * This module provides data structures and utilities for NIP-7D threaded discussions:
 * - Kind 11: Thread root event (forum-style discussion starter)
 * - Kind 1111: Thread reply (NIP-22 comment kind)
 *
 * NIP-7D threads are distinct from NIP-10 reply threading:
 * - They represent explicit "forum threads" with a subject line
 * - The root event (kind 11) declares a thread topic
 * - Replies use kind 1111 with ["K", "11"] tag indicating root kind
 *
 * Tag conventions:
 * - ["subject", "Thread title"] - Thread subject/title
 * - ["e", "<root_id>", "<relay>", "root"] - Reference to thread root
 * - ["K", "11"] - In replies, indicates the root event kind
 * - ["p", "<pubkey>"] - Reference to thread author(s)
 * - ["t", "<hashtag>"] - Thread categories/topics
 */

#ifndef GNOSTR_NIP7D_THREADS_H
#define GNOSTR_NIP7D_THREADS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* NIP-7D Event Kinds */
#define NIP7D_KIND_THREAD_ROOT  11
#define NIP7D_KIND_THREAD_REPLY 1111

/**
 * GnostrThread - Represents a NIP-7D forum thread (kind 11 event)
 */
typedef struct {
    char *event_id;         /* Event ID of the kind-11 thread root (hex) */
    char *pubkey;           /* Pubkey of thread creator (hex) */
    char *subject;          /* Thread subject/title from "subject" tag */
    char *content;          /* Thread body content */
    gint64 created_at;      /* Unix timestamp of creation */
    guint replies_count;    /* Number of replies to this thread */
    gint64 last_activity;   /* Timestamp of most recent reply */
    GPtrArray *hashtags;    /* Array of hashtags (char*) from "t" tags */
    GPtrArray *mentions;    /* Array of mentioned pubkeys (char*) from "p" tags */
} GnostrThread;

/**
 * GnostrThreadReply - Represents a reply in a NIP-7D thread (kind 1111 event)
 */
typedef struct {
    char *event_id;         /* Event ID of this reply (hex) */
    char *pubkey;           /* Pubkey of reply author (hex) */
    char *content;          /* Reply content */
    gint64 created_at;      /* Unix timestamp */
    char *thread_root_id;   /* Event ID of the thread root (kind 11) */
    char *parent_id;        /* Event ID of direct parent (for nested replies) */
    guint depth;            /* Nesting depth (0 = direct reply to root) */
} GnostrThreadReply;

/**
 * GnostrThreadTree - Hierarchical tree of thread replies
 */
typedef struct _GnostrThreadTreeNode {
    GnostrThreadReply *reply;               /* The reply at this node (NULL for root) */
    GPtrArray *children;                    /* Array of GnostrThreadTreeNode* children */
    struct _GnostrThreadTreeNode *parent;   /* Parent node (NULL for root) */
} GnostrThreadTreeNode;

/* ============================================================================
 * Thread Structure Management
 * ============================================================================ */

/**
 * gnostr_thread_new:
 *
 * Allocate a new GnostrThread structure.
 *
 * Returns: (transfer full): A newly allocated thread, free with gnostr_thread_free()
 */
GnostrThread *gnostr_thread_new(void);

/**
 * gnostr_thread_free:
 * @thread: (nullable): Thread to free
 *
 * Free a GnostrThread structure and all its members.
 */
void gnostr_thread_free(GnostrThread *thread);

/**
 * gnostr_thread_copy:
 * @thread: (nullable): Thread to copy
 *
 * Deep copy a GnostrThread.
 *
 * Returns: (transfer full) (nullable): A copy of the thread, or NULL
 */
GnostrThread *gnostr_thread_copy(const GnostrThread *thread);

/**
 * gnostr_thread_reply_new:
 *
 * Allocate a new GnostrThreadReply structure.
 *
 * Returns: (transfer full): A newly allocated reply, free with gnostr_thread_reply_free()
 */
GnostrThreadReply *gnostr_thread_reply_new(void);

/**
 * gnostr_thread_reply_free:
 * @reply: (nullable): Reply to free
 *
 * Free a GnostrThreadReply structure and all its members.
 */
void gnostr_thread_reply_free(GnostrThreadReply *reply);

/**
 * gnostr_thread_reply_copy:
 * @reply: (nullable): Reply to copy
 *
 * Deep copy a GnostrThreadReply.
 *
 * Returns: (transfer full) (nullable): A copy of the reply, or NULL
 */
GnostrThreadReply *gnostr_thread_reply_copy(const GnostrThreadReply *reply);

/**
 * gnostr_thread_tree_node_new:
 * @reply: (nullable): Reply for this node, or NULL for virtual root
 *
 * Create a new tree node.
 *
 * Returns: (transfer full): A new tree node, free with gnostr_thread_tree_node_free()
 */
GnostrThreadTreeNode *gnostr_thread_tree_node_new(GnostrThreadReply *reply);

/**
 * gnostr_thread_tree_node_free:
 * @node: (nullable): Tree node to free (recursively frees children)
 *
 * Free a tree node and all its descendants.
 */
void gnostr_thread_tree_node_free(GnostrThreadTreeNode *node);

/* ============================================================================
 * Parsing Functions
 * ============================================================================ */

/**
 * gnostr_thread_parse_from_json:
 * @json_str: JSON string of a kind-11 event
 *
 * Parse a thread root from its JSON representation.
 *
 * Returns: (transfer full) (nullable): Parsed thread, or NULL on error
 */
GnostrThread *gnostr_thread_parse_from_json(const char *json_str);

/**
 * gnostr_thread_reply_parse_from_json:
 * @json_str: JSON string of a kind-1111 event
 *
 * Parse a thread reply from its JSON representation.
 *
 * Returns: (transfer full) (nullable): Parsed reply, or NULL on error
 */
GnostrThreadReply *gnostr_thread_reply_parse_from_json(const char *json_str);

/**
 * gnostr_thread_parse_subject:
 * @tags_json: JSON array string of event tags
 *
 * Extract thread subject from event tags.
 *
 * Returns: (transfer full) (nullable): Thread subject, or NULL if not found
 */
char *gnostr_thread_parse_subject(const char *tags_json);

/**
 * gnostr_thread_parse_hashtags:
 * @tags_json: JSON array string of event tags
 *
 * Extract hashtags from event tags.
 *
 * Returns: (transfer full) (nullable): Array of hashtags, or NULL
 */
GPtrArray *gnostr_thread_parse_hashtags(const char *tags_json);

/**
 * gnostr_thread_reply_extract_root_id:
 * @tags_json: JSON array string of event tags
 *
 * Extract thread root ID from a reply's tags.
 *
 * Returns: (transfer full) (nullable): Root event ID (hex), or NULL
 */
char *gnostr_thread_reply_extract_root_id(const char *tags_json);

/**
 * gnostr_thread_reply_extract_parent_id:
 * @tags_json: JSON array string of event tags
 *
 * Extract direct parent ID from a reply's tags.
 *
 * Returns: (transfer full) (nullable): Parent event ID (hex), or NULL
 */
char *gnostr_thread_reply_extract_parent_id(const char *tags_json);

/* ============================================================================
 * Event Creation Functions
 * ============================================================================ */

/**
 * gnostr_thread_create_tags:
 * @subject: Thread subject/title
 * @hashtags: (nullable): Array of hashtag strings
 *
 * Create tags array for a kind-11 thread root event.
 *
 * Returns: (transfer full): JSON array string for tags, caller must free
 */
char *gnostr_thread_create_tags(const char *subject, const char * const *hashtags);

/**
 * gnostr_thread_reply_create_tags:
 * @thread_root_id: Event ID of the thread root (kind 11)
 * @parent_id: (nullable): Event ID of direct parent for nested replies
 * @author_pubkeys: (nullable): Array of pubkeys to mention
 * @recommended_relay: (nullable): Relay URL hint
 *
 * Create tags array for a kind-1111 thread reply event.
 *
 * Returns: (transfer full): JSON array string for tags, caller must free
 */
char *gnostr_thread_reply_create_tags(const char *thread_root_id,
                                       const char *parent_id,
                                       const char * const *author_pubkeys,
                                       const char *recommended_relay);

/* ============================================================================
 * Reply Tree Building
 * ============================================================================ */

/**
 * gnostr_thread_build_reply_tree:
 * @replies: Array of GnostrThreadReply* to organize
 *
 * Build a hierarchical tree from a flat array of replies.
 * Calculates depths and parent-child relationships.
 *
 * Returns: (transfer full): Root node of the reply tree, free with gnostr_thread_tree_node_free()
 */
GnostrThreadTreeNode *gnostr_thread_build_reply_tree(GPtrArray *replies);

/**
 * gnostr_thread_calculate_depths:
 * @replies: Array of GnostrThreadReply* to update
 * @thread_root_id: Event ID of the thread root
 *
 * Calculate and set the depth field for each reply.
 * Depth 0 = direct reply to thread root.
 */
void gnostr_thread_calculate_depths(GPtrArray *replies, const char *thread_root_id);

/**
 * gnostr_thread_sort_replies_chronological:
 * @replies: Array of GnostrThreadReply* to sort
 *
 * Sort replies by created_at timestamp (oldest first).
 */
void gnostr_thread_sort_replies_chronological(GPtrArray *replies);

/**
 * gnostr_thread_sort_replies_threaded:
 * @replies: Array of GnostrThreadReply* to sort
 * @thread_root_id: Event ID of the thread root
 *
 * Sort replies in threaded display order (parent before children,
 * siblings sorted by timestamp).
 */
void gnostr_thread_sort_replies_threaded(GPtrArray *replies, const char *thread_root_id);

/**
 * gnostr_thread_count_replies:
 * @root: Root node of reply tree
 *
 * Count total replies in a thread tree.
 *
 * Returns: Total number of replies
 */
guint gnostr_thread_count_replies(GnostrThreadTreeNode *root);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * gnostr_thread_is_thread_event:
 * @kind: Event kind number
 *
 * Check if an event kind is a NIP-7D thread event.
 *
 * Returns: TRUE if kind 11 or 1111
 */
gboolean gnostr_thread_is_thread_event(gint kind);

/**
 * gnostr_thread_format_timestamp:
 * @created_at: Unix timestamp
 *
 * Format a timestamp for display (e.g., "2 hours ago").
 *
 * Returns: (transfer full): Formatted string, caller must free
 */
char *gnostr_thread_format_timestamp(gint64 created_at);

/**
 * gnostr_thread_format_reply_count:
 * @count: Number of replies
 *
 * Format reply count for display (e.g., "42 replies").
 *
 * Returns: (transfer full): Formatted string, caller must free
 */
char *gnostr_thread_format_reply_count(guint count);

G_END_DECLS

#endif /* GNOSTR_NIP7D_THREADS_H */
