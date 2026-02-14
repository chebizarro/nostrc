#ifndef NIPS_NIP18_NOSTR_NIP18_NIP18_H
#define NIPS_NIP18_NOSTR_NIP18_NIP18_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * NIP-18: Reposts
 *
 * This header provides canonical helpers for working with NIP-18 reposts.
 * All functions are stable C APIs intended to be GObject-Introspection (GI)
 * friendly. Ownership/transfer and nullability are documented using GTK-Doc
 * style annotations.
 *
 * Kind 6: Repost of kind 1 notes
 * Kind 16: Generic repost (for any other kind)
 *
 * Repost events contain:
 * - An "e" tag pointing to the reposted event id (with optional relay hint)
 * - A "p" tag pointing to the author of the reposted event
 * - Optionally, a "k" tag with the kind of the reposted event (for kind 16)
 * - Content is either empty or the JSON of the reposted event
 */

/* Kind constants */
#define NOSTR_KIND_REPOST         6
#define NOSTR_KIND_GENERIC_REPOST 16

/**
 * NostrRepostInfo:
 * @repost_event_id: (array fixed-size=32): binary event id of the reposted event
 * @repost_pubkey: (array fixed-size=32): binary pubkey of the original author
 * @repost_kind: kind of the reposted event (1 for kind 6, varies for kind 16)
 * @relay_hint: (nullable): relay URL hint for fetching the reposted event
 * @embedded_json: (nullable): JSON content of the reposted event if included
 * @has_repost_event: whether a valid repost event id was found
 * @has_repost_pubkey: whether a valid repost pubkey was found
 *
 * Output structure populated by nostr_nip18_parse_repost().
 */
typedef struct {
    bool has_repost_event;
    bool has_repost_pubkey;
    unsigned char repost_event_id[32];
    unsigned char repost_pubkey[32];
    int repost_kind;
    char *relay_hint;      /* owned, must be freed */
    char *embedded_json;   /* owned, must be freed */
} NostrRepostInfo;

/**
 * nostr_nip18_repost_info_clear:
 * @info: (inout) (not nullable): repost info to clear
 *
 * Frees any allocated memory in the repost info structure.
 * The structure itself is not freed (caller-allocated).
 */
void nostr_nip18_repost_info_clear(NostrRepostInfo *info);

/**
 * nostr_nip18_create_repost:
 * @reposted_event: (in) (transfer none) (not nullable): the event to repost (must be kind 1)
 * @relay_hint: (in) (nullable): optional relay URL hint where the event can be found
 * @include_json: whether to include the reposted event JSON in content
 *
 * Creates a kind 6 repost event for a kind 1 note.
 * The caller must sign the returned event.
 *
 * Returns: (transfer full) (nullable): new unsigned repost event, or NULL on error
 */
NostrEvent *nostr_nip18_create_repost(const NostrEvent *reposted_event,
                                       const char *relay_hint,
                                       bool include_json);

/**
 * nostr_nip18_create_repost_from_id:
 * @event_id: (in) (array fixed-size=32) (not nullable): binary event id to repost
 * @author_pubkey: (in) (array fixed-size=32) (not nullable): binary pubkey of original author
 * @relay_hint: (in) (nullable): optional relay URL hint
 * @event_json: (in) (nullable): optional JSON of the reposted event to embed in content
 *
 * Creates a kind 6 repost event using raw event id and pubkey.
 * The caller must sign the returned event.
 *
 * Returns: (transfer full) (nullable): new unsigned repost event, or NULL on error
 */
NostrEvent *nostr_nip18_create_repost_from_id(const unsigned char event_id[32],
                                               const unsigned char author_pubkey[32],
                                               const char *relay_hint,
                                               const char *event_json);

/**
 * nostr_nip18_create_generic_repost:
 * @reposted_event: (in) (transfer none) (not nullable): the event to repost (any kind)
 * @relay_hint: (in) (nullable): optional relay URL hint
 * @include_json: whether to include the reposted event JSON in content
 *
 * Creates a kind 16 generic repost event for any kind of event.
 * The caller must sign the returned event.
 *
 * Returns: (transfer full) (nullable): new unsigned repost event, or NULL on error
 */
NostrEvent *nostr_nip18_create_generic_repost(const NostrEvent *reposted_event,
                                               const char *relay_hint,
                                               bool include_json);

/**
 * nostr_nip18_create_generic_repost_from_id:
 * @event_id: (in) (array fixed-size=32) (not nullable): binary event id to repost
 * @author_pubkey: (in) (array fixed-size=32) (not nullable): binary pubkey of original author
 * @reposted_kind: kind of the event being reposted
 * @relay_hint: (in) (nullable): optional relay URL hint
 * @event_json: (in) (nullable): optional JSON of the reposted event to embed in content
 *
 * Creates a kind 16 generic repost event using raw event id, pubkey, and kind.
 * The caller must sign the returned event.
 *
 * Returns: (transfer full) (nullable): new unsigned repost event, or NULL on error
 */
NostrEvent *nostr_nip18_create_generic_repost_from_id(const unsigned char event_id[32],
                                                       const unsigned char author_pubkey[32],
                                                       int reposted_kind,
                                                       const char *relay_hint,
                                                       const char *event_json);

/**
 * nostr_nip18_parse_repost:
 * @ev: (in) (transfer none) (not nullable): repost event to parse (kind 6 or 16)
 * @out: (out caller-allocates) (not nullable): repost info to populate
 *
 * Parses a repost event to extract the reposted event info.
 * Works with both kind 6 (note reposts) and kind 16 (generic reposts).
 *
 * Returns: 0 on success, negative errno-style value on failure
 */
int nostr_nip18_parse_repost(const NostrEvent *ev, NostrRepostInfo *out);

/**
 * nostr_nip18_is_repost:
 * @ev: (in) (transfer none) (nullable): event to check
 *
 * Checks if the event is a repost (kind 6 or 16).
 *
 * Returns: true if the event is a repost
 */
bool nostr_nip18_is_repost(const NostrEvent *ev);

/**
 * nostr_nip18_is_note_repost:
 * @ev: (in) (transfer none) (nullable): event to check
 *
 * Checks if the event is a note repost (kind 6).
 *
 * Returns: true if the event is a kind 6 repost
 */
bool nostr_nip18_is_note_repost(const NostrEvent *ev);

/**
 * nostr_nip18_is_generic_repost:
 * @ev: (in) (transfer none) (nullable): event to check
 *
 * Checks if the event is a generic repost (kind 16).
 *
 * Returns: true if the event is a kind 16 repost
 */
bool nostr_nip18_is_generic_repost(const NostrEvent *ev);

/* Quote reposts: kind 1 events with q-tag referencing another event */

/**
 * nostr_nip18_add_q_tag:
 * @ev: (inout) (transfer none) (not nullable): event to add quote tag to
 * @quoted_event_id: (in) (array fixed-size=32) (not nullable): binary event id being quoted
 * @relay_hint: (in) (nullable): optional relay URL hint
 * @author_pubkey: (in) (array fixed-size=32) (nullable): optional pubkey of quoted author
 *
 * Adds a "q" tag to an event for quoting another event.
 * The q-tag format is: ["q", <event-id-hex>, <relay?>, <pubkey?>]
 *
 * Returns: 0 on success, negative errno-style value on failure
 */
int nostr_nip18_add_q_tag(NostrEvent *ev,
                          const unsigned char quoted_event_id[32],
                          const char *relay_hint,
                          const unsigned char *author_pubkey);

/**
 * NostrQuoteInfo:
 * @quoted_event_id: (array fixed-size=32): binary event id of the quoted event
 * @quoted_pubkey: (array fixed-size=32): binary pubkey of the quoted author (if present)
 * @relay_hint: (nullable): relay URL hint for fetching the quoted event
 * @has_quoted_event: whether a valid quoted event id was found
 * @has_quoted_pubkey: whether a valid quoted pubkey was found
 *
 * Output structure populated by nostr_nip18_get_quote().
 */
typedef struct {
    bool has_quoted_event;
    bool has_quoted_pubkey;
    unsigned char quoted_event_id[32];
    unsigned char quoted_pubkey[32];
    char *relay_hint;  /* owned, must be freed */
} NostrQuoteInfo;

/**
 * nostr_nip18_quote_info_clear:
 * @info: (inout) (not nullable): quote info to clear
 *
 * Frees any allocated memory in the quote info structure.
 */
void nostr_nip18_quote_info_clear(NostrQuoteInfo *info);

/**
 * nostr_nip18_get_quote:
 * @ev: (in) (transfer none) (not nullable): event to check for quotes
 * @out: (out caller-allocates) (not nullable): quote info to populate
 *
 * Extracts quote information from an event's q-tag if present.
 *
 * Returns: 0 if quote found, -ENOENT if no quote, negative errno on error
 */
int nostr_nip18_get_quote(const NostrEvent *ev, NostrQuoteInfo *out);

/**
 * nostr_nip18_has_quote:
 * @ev: (in) (transfer none) (nullable): event to check
 *
 * Checks if the event has a q-tag (is a quote post).
 *
 * Returns: true if the event quotes another event
 */
bool nostr_nip18_has_quote(const NostrEvent *ev);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP18_NOSTR_NIP18_NIP18_H */
