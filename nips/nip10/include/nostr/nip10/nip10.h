#ifndef NIPS_NIP10_NOSTR_NIP10_NIP10_H
#define NIPS_NIP10_NOSTR_NIP10_NIP10_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * NIP-10: Threaded conversations
 *
 * This header provides canonical helpers for working with NIP-10 thread tags.
 * All functions are stable C APIs intended to be GObject-Introspection (GI)
 * friendly. Ownership/transfer and nullability are documented using GTK-Doc
 * style annotations.
 */

/* Marked e-tags */
typedef enum { NOSTR_E_MARK_NONE=0, NOSTR_E_MARK_ROOT, NOSTR_E_MARK_REPLY } NostrEMarker;

/**
 * nostr_nip10_add_marked_e_tag:
 * @ev: (inout) (transfer none) (not nullable): target event to modify
 * @event_id: (in) (array fixed-size=32) (not nullable): 32-byte event id (binary)
 * @relay_opt: (in) (nullable): optional relay URL (e.g. "wss://...")
 * @marker: (in): marker kind for the tag (root, reply, or none)
 * @author_pk_opt: (in) (array fixed-size=32) (nullable): optional author pubkey (binary)
 *
 * Adds an 'e' tag with an optional relay and a NIP-10 marker at the canonical
 * positions (index 0: "e", index 1: hex id, index 2: relay, index 3: marker).
 * Ensures uniqueness with existing tags of identical tuple (id, relay, marker).
 *
 * Returns: 0 on success, negative errno-style value on failure.
 */
int nostr_nip10_add_marked_e_tag(NostrEvent *ev,
                                 const unsigned char event_id[32],
                                 const char *relay_opt,
                                 NostrEMarker marker,
                                 const unsigned char *author_pk_opt);

/**
 * NostrThreadContext:
 * @has_root: whether a root id was found
 * @has_reply: whether an immediate reply id was found
 * @root_id: (array fixed-size=32): binary id for the root event
 * @reply_id: (array fixed-size=32): binary id for the immediate reply parent
 *
 * Output structure populated by nostr_nip10_get_thread().
 */
typedef struct {
  bool has_root, has_reply;
  unsigned char root_id[32];
  unsigned char reply_id[32];
} NostrThreadContext;

/**
 * nostr_nip10_get_thread:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @out: (out caller-allocates) (not nullable): thread context to populate
 *
 * Parses the event's 'e' tags to derive the thread context according to NIP-10.
 * Recognizes explicit markers (root/reply). If no explicit root exists, may
 * fall back to the first 'e' tag as root to maintain compatibility with legacy
 * events.
 *
 * Returns: 0 on success, negative errno-style value on failure.
 */
int  nostr_nip10_get_thread(const NostrEvent *ev, NostrThreadContext *out);

/**
 * nostr_nip10_ensure_p_participants:
 * @reply_ev: (inout) (transfer none) (not nullable): reply event to augment
 * @parent_ev: (in) (transfer none) (not nullable): parent event providing context
 *
 * Ensures the reply event contains 'p' tags for the parent author and any
 * participant 'p' tags found on the parent, preserving relay data. Duplicate
 * entries are avoided.
 *
 * Returns: 0 on success, negative errno-style value on failure.
 */
int  nostr_nip10_ensure_p_participants(NostrEvent *reply_ev, const NostrEvent *parent_ev);

/**
 * NostrNip10ThreadInfo:
 * @root_id: (nullable): hex string of root event ID, or NULL if not found. Caller must free.
 * @reply_id: (nullable): hex string of immediate reply parent ID, or NULL if not found. Caller must free.
 * @root_relay_hint: (nullable): relay URL hint for root event, or NULL if not specified. Caller must free.
 * @reply_relay_hint: (nullable): relay URL hint for reply parent event, or NULL if not specified. Caller must free.
 *
 * Thread info parsed from NIP-10 e-tags. Contains heap-allocated strings.
 * Use nostr_nip10_thread_info_clear() to free the strings.
 */
typedef struct {
  char *root_id;           /* hex string, caller-owned, may be NULL */
  char *reply_id;          /* hex string, caller-owned, may be NULL */
  char *root_relay_hint;   /* relay URL, caller-owned, may be NULL */
  char *reply_relay_hint;  /* relay URL, caller-owned, may be NULL */
} NostrNip10ThreadInfo;

/**
 * nostr_nip10_thread_info_clear:
 * @info: (inout) (not nullable): thread info to clear
 *
 * Frees the strings within a NostrNip10ThreadInfo and sets them to NULL.
 */
void nostr_nip10_thread_info_clear(NostrNip10ThreadInfo *info);

/**
 * nostr_nip10_parse_thread_from_event:
 * @ev: (in) (transfer none) (not nullable): event to parse
 * @info: (out caller-allocates) (not nullable): thread info to populate
 *
 * Parses NIP-10 e-tags from an event to extract root and reply IDs.
 * Uses explicit markers (root/reply) when present, falls back to positional
 * interpretation for legacy events.
 *
 * IMPORTANT: This is the canonical NIP-10 parsing function. All code should
 * use this instead of implementing custom parsing logic.
 *
 * Returns: 0 on success, negative errno-style value on failure.
 */
int nostr_nip10_parse_thread_from_event(const NostrEvent *ev, NostrNip10ThreadInfo *info);

/**
 * nostr_nip10_parse_thread_from_json:
 * @json_str: (in) (not nullable): JSON string of an event
 * @info: (out caller-allocates) (not nullable): thread info to populate
 *
 * Parses NIP-10 e-tags from a JSON event string to extract root and reply IDs.
 * This is a convenience wrapper that deserializes the JSON and calls
 * nostr_nip10_parse_thread_from_event().
 *
 * IMPORTANT: This is the canonical NIP-10 JSON parsing function. All code should
 * use this instead of implementing custom parsing logic.
 *
 * Returns: 0 on success, negative errno-style value on failure.
 */
int nostr_nip10_parse_thread_from_json(const char *json_str, NostrNip10ThreadInfo *info);

/**
 * nostr_nip10_parse_thread_from_tags:
 * @tags: (in) (transfer none) (not nullable): NostrTags to parse
 * @info: (out caller-allocates) (not nullable): thread info to populate
 *
 * Parses NIP-10 e-tags directly from a tags collection.
 * This is the lowest-level parsing function used by the other variants.
 *
 * Returns: 0 on success, negative errno-style value on failure.
 */
int nostr_nip10_parse_thread_from_tags(const NostrTags *tags, NostrNip10ThreadInfo *info);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP10_NOSTR_NIP10_NIP10_H */
