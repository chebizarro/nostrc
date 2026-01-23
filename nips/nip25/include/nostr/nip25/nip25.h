#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * NIP-25: Reactions
 *
 * This header provides canonical helpers for working with NIP-25 reaction events.
 * Kind 7 events represent reactions to other events (likes, dislikes, emoji reactions).
 *
 * Reaction content meanings:
 *   "+" = like
 *   "-" = dislike
 *   emoji (e.g., ":fire:") = custom reaction
 *   custom shortcode = custom emoji (requires emoji tag)
 *
 * Required tags:
 *   - ["e", "<event-id>"] - the event being reacted to
 *   - ["p", "<pubkey>"] - the author of the event being reacted to
 *   - ["k", "<kind>"] - the kind of the event being reacted to (recommended)
 */

/**
 * NostrReactionType:
 * @NOSTR_REACTION_LIKE: positive reaction ("+")
 * @NOSTR_REACTION_DISLIKE: negative reaction ("-")
 * @NOSTR_REACTION_EMOJI: emoji reaction (unicode or custom)
 * @NOSTR_REACTION_UNKNOWN: unrecognized reaction content
 *
 * Enum representing the type of reaction in a NIP-25 event.
 */
typedef enum {
    NOSTR_REACTION_LIKE = 0,
    NOSTR_REACTION_DISLIKE,
    NOSTR_REACTION_EMOJI,
    NOSTR_REACTION_UNKNOWN
} NostrReactionType;

/**
 * NostrReaction:
 * @type: the reaction type
 * @content: the raw reaction content ("+", "-", or emoji)
 * @event_id: (array fixed-size=32): binary id of the reacted event
 * @author_pubkey: (array fixed-size=32): binary pubkey of the reacted event author
 * @reacted_kind: the kind of the reacted event (-1 if unknown)
 * @has_event_id: whether event_id was found
 * @has_author_pubkey: whether author_pubkey was found
 *
 * Structure representing a parsed NIP-25 reaction event.
 */
typedef struct {
    NostrReactionType type;
    char content[256];  /* reaction content (emoji/shortcode) */
    unsigned char event_id[32];
    unsigned char author_pubkey[32];
    int reacted_kind;
    bool has_event_id;
    bool has_author_pubkey;
} NostrReaction;

/**
 * NostrReactionStats:
 * @like_count: number of "+" reactions
 * @dislike_count: number of "-" reactions
 * @emoji_count: number of emoji reactions
 * @total_count: total number of reactions
 *
 * Structure representing aggregated reaction statistics.
 */
typedef struct {
    uint32_t like_count;
    uint32_t dislike_count;
    uint32_t emoji_count;
    uint32_t total_count;
} NostrReactionStats;

/**
 * nostr_nip25_create_reaction:
 * @reacted_event_id: (in) (array fixed-size=32) (not nullable): 32-byte event id being reacted to
 * @reacted_author_pubkey: (in) (array fixed-size=32) (nullable): 32-byte pubkey of reacted event author
 * @reacted_kind: (in): kind of the reacted event (use -1 if unknown, will omit k-tag)
 * @reaction_content: (in) (nullable): reaction content ("+", "-", emoji). NULL defaults to "+"
 * @relay_url: (in) (nullable): optional relay hint for the e-tag
 *
 * Creates a new NIP-25 kind 7 reaction event (unsigned).
 * The caller must set the pubkey, sign, and publish the event.
 *
 * Returns: (transfer full) (nullable): new reaction event or NULL on error
 */
NostrEvent *nostr_nip25_create_reaction(
    const unsigned char reacted_event_id[32],
    const unsigned char *reacted_author_pubkey,
    int reacted_kind,
    const char *reaction_content,
    const char *relay_url
);

/**
 * nostr_nip25_create_reaction_hex:
 * @reacted_event_id_hex: (in) (not nullable): 64-char hex event id being reacted to
 * @reacted_author_pubkey_hex: (in) (nullable): 64-char hex pubkey of reacted event author
 * @reacted_kind: (in): kind of the reacted event (use -1 if unknown, will omit k-tag)
 * @reaction_content: (in) (nullable): reaction content ("+", "-", emoji). NULL defaults to "+"
 * @relay_url: (in) (nullable): optional relay hint for the e-tag
 *
 * Creates a new NIP-25 kind 7 reaction event from hex strings (unsigned).
 * Convenience wrapper around nostr_nip25_create_reaction().
 *
 * Returns: (transfer full) (nullable): new reaction event or NULL on error
 */
NostrEvent *nostr_nip25_create_reaction_hex(
    const char *reacted_event_id_hex,
    const char *reacted_author_pubkey_hex,
    int reacted_kind,
    const char *reaction_content,
    const char *relay_url
);

/**
 * nostr_nip25_parse_reaction:
 * @ev: (in) (transfer none) (not nullable): reaction event to parse
 * @out: (out caller-allocates) (not nullable): parsed reaction details
 *
 * Parses a kind 7 reaction event to extract reaction details.
 * Extracts the reaction content, target event ID, author pubkey, and reacted kind.
 *
 * Returns: 0 on success, -1 if event is not a valid reaction
 */
int nostr_nip25_parse_reaction(const NostrEvent *ev, NostrReaction *out);

/**
 * nostr_nip25_get_reaction_type:
 * @content: (in) (nullable): reaction content string
 *
 * Determines the reaction type from the content string.
 *
 * Returns: the reaction type
 */
NostrReactionType nostr_nip25_get_reaction_type(const char *content);

/**
 * nostr_nip25_is_like:
 * @ev: (in) (transfer none) (nullable): event to check
 *
 * Checks if an event is a valid NIP-25 like reaction (kind 7 with "+" content).
 *
 * Returns: true if the event is a like reaction
 */
bool nostr_nip25_is_like(const NostrEvent *ev);

/**
 * nostr_nip25_is_dislike:
 * @ev: (in) (transfer none) (nullable): event to check
 *
 * Checks if an event is a valid NIP-25 dislike reaction (kind 7 with "-" content).
 *
 * Returns: true if the event is a dislike reaction
 */
bool nostr_nip25_is_dislike(const NostrEvent *ev);

/**
 * nostr_nip25_is_reaction:
 * @ev: (in) (transfer none) (nullable): event to check
 *
 * Checks if an event is a valid NIP-25 reaction event (kind 7).
 *
 * Returns: true if the event is a reaction event
 */
bool nostr_nip25_is_reaction(const NostrEvent *ev);

/**
 * nostr_nip25_get_reacted_event_id:
 * @ev: (in) (transfer none) (not nullable): reaction event
 * @out_id: (out caller-allocates) (array fixed-size=32) (not nullable): buffer for event id
 *
 * Extracts the reacted event ID from a reaction event.
 *
 * Returns: true if event ID was found and extracted
 */
bool nostr_nip25_get_reacted_event_id(const NostrEvent *ev, unsigned char out_id[32]);

/**
 * nostr_nip25_get_reacted_event_id_hex:
 * @ev: (in) (transfer none) (not nullable): reaction event
 *
 * Extracts the reacted event ID from a reaction event as a hex string.
 *
 * Returns: (transfer full) (nullable): newly allocated hex string or NULL
 */
char *nostr_nip25_get_reacted_event_id_hex(const NostrEvent *ev);

/**
 * nostr_nip25_aggregate_reactions:
 * @reactions: (in) (array length=count) (not nullable): array of reaction events
 * @count: (in): number of reactions in the array
 * @out_stats: (out caller-allocates) (not nullable): aggregated statistics
 *
 * Aggregates multiple reaction events into statistics.
 *
 * Returns: 0 on success
 */
int nostr_nip25_aggregate_reactions(
    const NostrEvent **reactions,
    size_t count,
    NostrReactionStats *out_stats
);

#ifdef __cplusplus
}
#endif
