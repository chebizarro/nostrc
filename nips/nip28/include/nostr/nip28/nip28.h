#ifndef NIPS_NIP28_NOSTR_NIP28_NIP28_H
#define NIPS_NIP28_NOSTR_NIP28_NIP28_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-28: Public Chat
 *
 * Event kinds for public chat channels:
 *   - Kind 40: Channel Creation (content = JSON metadata)
 *   - Kind 41: Channel Metadata Update (e tag + JSON metadata)
 *   - Kind 42: Channel Message (e tags for root/reply threading)
 *   - Kind 43: Hide Message (e tag + reason)
 *   - Kind 44: Mute User (p tag + reason)
 *
 * Channel metadata JSON: {"name":"...","about":"...","picture":"..."}
 */

#define NOSTR_NIP28_KIND_CHANNEL_CREATE   40
#define NOSTR_NIP28_KIND_CHANNEL_METADATA 41
#define NOSTR_NIP28_KIND_CHANNEL_MESSAGE  42
#define NOSTR_NIP28_KIND_HIDE_MESSAGE     43
#define NOSTR_NIP28_KIND_MUTE_USER        44

/* ============== Channel Reference (kind 41) ============== */

/**
 * Parsed channel reference from a metadata update event.
 * The "e" tag links to the channel creation event.
 */
typedef struct {
    const char *channel_id; /**< "e" tag value — creation event ID (borrowed) */
    const char *relay;      /**< "e" tag relay hint (borrowed, nullable) */
} NostrNip28ChannelRef;

/**
 * nostr_nip28_parse_channel_ref:
 * @ev: (in): kind 41 event to inspect
 * @out: (out): parsed channel reference
 *
 * Extracts the channel creation event reference from a metadata update.
 *
 * Returns: 0 on success, -EINVAL on bad input, -ENOENT if no "e" tag
 */
int nostr_nip28_parse_channel_ref(const NostrEvent *ev,
                                   NostrNip28ChannelRef *out);

/* ============== Message Reference (kind 42) ============== */

/**
 * Parsed message references from a channel message event.
 * Contains the root channel reference and optional reply reference.
 */
typedef struct {
    const char *channel_id;    /**< Root "e" tag value (borrowed) */
    const char *channel_relay; /**< Root "e" tag relay (borrowed, nullable) */
    const char *reply_to;      /**< Reply "e" tag value (borrowed, nullable) */
    const char *reply_relay;   /**< Reply "e" tag relay (borrowed, nullable) */
} NostrNip28MessageRef;

/**
 * nostr_nip28_parse_message_ref:
 * @ev: (in): kind 42 event to inspect
 * @out: (out): parsed message references
 *
 * Extracts channel root and optional reply references.
 * Root is identified by marker "root", reply by "reply".
 *
 * Returns: 0 on success, -EINVAL on bad input, -ENOENT if no root e tag
 */
int nostr_nip28_parse_message_ref(const NostrEvent *ev,
                                   NostrNip28MessageRef *out);

/* ============== Hide/Mute References (kinds 43/44) ============== */

/**
 * nostr_nip28_get_hidden_message_id:
 * @ev: (in): kind 43 event
 *
 * Returns: (transfer none) (nullable): borrowed pointer to the hidden
 *          message event ID from the "e" tag, or NULL
 */
const char *nostr_nip28_get_hidden_message_id(const NostrEvent *ev);

/**
 * nostr_nip28_get_muted_pubkey:
 * @ev: (in): kind 44 event
 *
 * Returns: (transfer none) (nullable): borrowed pointer to the muted
 *          user's pubkey from the "p" tag, or NULL
 */
const char *nostr_nip28_get_muted_pubkey(const NostrEvent *ev);

/* ============== Event Creation ============== */

/**
 * nostr_nip28_create_channel:
 * @ev: (inout): event to populate
 * @metadata_json: (in): JSON string with name/about/picture
 *
 * Sets kind to 40, content to metadata JSON.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip28_create_channel(NostrEvent *ev, const char *metadata_json);

/**
 * nostr_nip28_create_channel_metadata:
 * @ev: (inout): event to populate
 * @channel_id: (in): channel creation event ID
 * @relay: (in) (nullable): recommended relay URL
 * @metadata_json: (in): JSON string with updated metadata
 *
 * Sets kind to 41, adds "e" tag referencing channel, content to metadata.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip28_create_channel_metadata(NostrEvent *ev,
                                         const char *channel_id,
                                         const char *relay,
                                         const char *metadata_json);

/**
 * nostr_nip28_create_message:
 * @ev: (inout): event to populate
 * @channel_id: (in): channel creation event ID
 * @relay: (in): recommended relay URL
 * @content: (in): message content
 * @reply_to: (in) (nullable): event ID being replied to, or NULL
 *
 * Sets kind to 42, adds root "e" tag (and reply "e" tag if replying).
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip28_create_message(NostrEvent *ev, const char *channel_id,
                                const char *relay, const char *content,
                                const char *reply_to);

/**
 * nostr_nip28_create_hide_message:
 * @ev: (inout): event to populate
 * @message_id: (in): event ID of message to hide
 * @reason: (in) (nullable): reason string (set as content JSON)
 *
 * Sets kind to 43, adds "e" tag, content to reason JSON.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip28_create_hide_message(NostrEvent *ev,
                                     const char *message_id,
                                     const char *reason);

/**
 * nostr_nip28_create_mute_user:
 * @ev: (inout): event to populate
 * @pubkey: (in): pubkey of user to mute
 * @reason: (in) (nullable): reason string (set as content JSON)
 *
 * Sets kind to 44, adds "p" tag, content to reason JSON.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip28_create_mute_user(NostrEvent *ev, const char *pubkey,
                                  const char *reason);

/* ============== Validation ============== */

/**
 * nostr_nip28_is_channel_event:
 * @ev: (in): event to check
 *
 * Returns: true if the event kind is one of 40-44
 */
bool nostr_nip28_is_channel_event(const NostrEvent *ev);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP28_NOSTR_NIP28_NIP28_H */
