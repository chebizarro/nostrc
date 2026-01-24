/**
 * NIP-72 Moderated Communities Support
 *
 * This module provides data structures and utilities for NIP-72 moderated communities:
 * - Kind 34550: Community definition (replaceable, addressable)
 *   - "d" tag: community identifier
 *   - "description" tag: community description
 *   - "image" tag: community image URL
 *   - "rules" tag: community rules
 *   - "p" tags with "moderator" role: moderator pubkeys
 *
 * - Kind 4550: Post approval event (published by moderators)
 *   - "a" tag: reference to the community
 *   - "e" tag: reference to the approved post
 *   - "p" tag: author of the approved post
 *
 * - Kind 1: Regular note (submitted to community)
 *   - "a" tag: reference to the community (34550:pubkey:d-tag)
 */

#ifndef GNOSTR_NIP72_COMMUNITIES_H
#define GNOSTR_NIP72_COMMUNITIES_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* NIP-72 Event Kinds */
#define NIP72_KIND_COMMUNITY_DEFINITION 34550
#define NIP72_KIND_POST_APPROVAL        4550

/**
 * GnostrCommunityModerator - A moderator entry for a community
 */
typedef struct {
    char *pubkey;           /* Moderator's pubkey (hex) */
    char *relay_hint;       /* Optional relay hint */
    char *petname;          /* Optional petname for display */
} GnostrCommunityModerator;

/**
 * GnostrCommunity - Represents a NIP-72 moderated community
 */
typedef struct {
    char *event_id;         /* Event ID of the kind-34550 event (hex) */
    char *creator_pubkey;   /* Pubkey of community creator (hex) */
    char *d_tag;            /* Community identifier (d tag) */
    char *name;             /* Community name (from d tag or explicit name tag) */
    char *description;      /* Community description */
    char *image;            /* Community image URL */
    char *rules;            /* Community rules */
    gint64 created_at;      /* Unix timestamp of creation */
    GPtrArray *moderators;  /* Array of GnostrCommunityModerator* */
    guint post_count;       /* Approximate approved post count (for display) */
    guint member_count;     /* Approximate member count (for display) */
} GnostrCommunity;

/**
 * GnostrApprovedPost - Represents a kind-4550 post approval event
 */
typedef struct {
    char *approval_id;      /* Event ID of the approval event (hex) */
    char *moderator_pubkey; /* Pubkey of the moderator who approved (hex) */
    char *post_event_id;    /* Event ID of the approved post (hex) */
    char *post_author;      /* Pubkey of the post author (hex) */
    char *community_a_tag;  /* The "a" tag referencing the community */
    gint64 approved_at;     /* Unix timestamp of approval */
} GnostrApprovedPost;

/**
 * GnostrCommunityPost - A post submitted to a community (kind 1 with "a" tag)
 */
typedef struct {
    char *event_id;         /* Event ID of the post (hex) */
    char *author_pubkey;    /* Author's pubkey (hex) */
    char *content;          /* Post content (plaintext) */
    char *community_a_tag;  /* The "a" tag referencing the community */
    gint64 created_at;      /* Unix timestamp */
    gboolean is_approved;   /* TRUE if post has been approved by a moderator */
    char *approval_id;      /* Event ID of approval event, or NULL */
} GnostrCommunityPost;

/* ===== GnostrCommunityModerator functions ===== */

/**
 * Allocate a new GnostrCommunityModerator structure
 */
GnostrCommunityModerator *gnostr_community_moderator_new(void);

/**
 * Free a GnostrCommunityModerator structure
 */
void gnostr_community_moderator_free(GnostrCommunityModerator *mod);

/**
 * Deep copy a GnostrCommunityModerator
 */
GnostrCommunityModerator *gnostr_community_moderator_copy(const GnostrCommunityModerator *mod);

/* ===== GnostrCommunity functions ===== */

/**
 * Allocate a new GnostrCommunity structure
 */
GnostrCommunity *gnostr_community_new(void);

/**
 * Free a GnostrCommunity structure
 */
void gnostr_community_free(GnostrCommunity *community);

/**
 * Deep copy a GnostrCommunity
 */
GnostrCommunity *gnostr_community_copy(const GnostrCommunity *community);

/**
 * Parse community definition from event tags
 * @tags_json: JSON array of tags from kind-34550 event
 * @community: Community to populate
 * @return: TRUE on success, FALSE on parse error
 */
gboolean gnostr_community_parse_tags(const char *tags_json, GnostrCommunity *community);

/**
 * Create tags array for a kind-34550 community definition event
 * @community: The community data to serialize
 * @return: JSON array string for tags, caller must free
 */
char *gnostr_community_create_tags(const GnostrCommunity *community);

/**
 * Get the NIP-33 "a" tag reference for a community (34550:pubkey:d-tag)
 * @community: The community
 * @return: Newly allocated string, caller must free
 */
char *gnostr_community_get_a_tag(const GnostrCommunity *community);

/**
 * Check if a pubkey is a moderator of the community
 * @community: The community
 * @pubkey: The pubkey to check (hex)
 * @return: TRUE if the pubkey is a moderator or creator
 */
gboolean gnostr_community_is_moderator(const GnostrCommunity *community,
                                        const char *pubkey);

/* ===== GnostrApprovedPost functions ===== */

/**
 * Allocate a new GnostrApprovedPost structure
 */
GnostrApprovedPost *gnostr_approved_post_new(void);

/**
 * Free a GnostrApprovedPost structure
 */
void gnostr_approved_post_free(GnostrApprovedPost *post);

/**
 * Deep copy a GnostrApprovedPost
 */
GnostrApprovedPost *gnostr_approved_post_copy(const GnostrApprovedPost *post);

/**
 * Parse post approval from event tags
 * @tags_json: JSON array of tags from kind-4550 event
 * @approval: Approval to populate
 * @return: TRUE on success, FALSE on parse error
 */
gboolean gnostr_approved_post_parse_tags(const char *tags_json, GnostrApprovedPost *approval);

/**
 * Create tags array for a kind-4550 post approval event
 * @community_a_tag: The community "a" tag (34550:pubkey:d-tag)
 * @post_event_id: The event ID of the post being approved
 * @post_author: The pubkey of the post author
 * @recommended_relay: Optional relay hint
 * @return: JSON array string for tags, caller must free
 */
char *gnostr_approved_post_create_tags(const char *community_a_tag,
                                        const char *post_event_id,
                                        const char *post_author,
                                        const char *recommended_relay);

/* ===== GnostrCommunityPost functions ===== */

/**
 * Allocate a new GnostrCommunityPost structure
 */
GnostrCommunityPost *gnostr_community_post_new(void);

/**
 * Free a GnostrCommunityPost structure
 */
void gnostr_community_post_free(GnostrCommunityPost *post);

/**
 * Deep copy a GnostrCommunityPost
 */
GnostrCommunityPost *gnostr_community_post_copy(const GnostrCommunityPost *post);

/**
 * Extract community "a" tag from a post's tags
 * @tags_json: JSON array of tags
 * @return: Newly allocated "a" tag string, or NULL if not found
 */
char *gnostr_community_post_extract_a_tag(const char *tags_json);

/**
 * Create tags array for a kind-1 post submitted to a community
 * @community_a_tag: The community "a" tag (34550:pubkey:d-tag)
 * @recommended_relay: Optional relay hint
 * @return: JSON array string for tags, caller must free
 */
char *gnostr_community_post_create_tags(const char *community_a_tag,
                                         const char *recommended_relay);

/**
 * Parse an "a" tag into its components
 * @a_tag: The "a" tag string (kind:pubkey:d-tag)
 * @out_kind: Output for kind number
 * @out_pubkey: Output for pubkey (caller must free)
 * @out_d_tag: Output for d-tag (caller must free)
 * @return: TRUE on success, FALSE on parse error
 */
gboolean gnostr_parse_a_tag(const char *a_tag,
                             guint *out_kind,
                             char **out_pubkey,
                             char **out_d_tag);

G_END_DECLS

#endif /* GNOSTR_NIP72_COMMUNITIES_H */
