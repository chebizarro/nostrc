#ifndef NIPS_NIP22_NOSTR_NIP22_NIP22_H
#define NIPS_NIP22_NOSTR_NIP22_NIP22_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-kinds.h"
#include <stdbool.h>

/*
 * NIP-22: Comments
 *
 * NIP-22 defines kind 1111 as a universal comment event that can reference
 * any event kind. Thread structure uses uppercase tags for roots and
 * lowercase tags for immediate parents:
 *
 *   Uppercase (root):   ["E", id], ["A", addr], ["I", ext-id]
 *   Lowercase (parent): ["e", id], ["a", addr], ["i", ext-id]
 *   Kind tag:           ["K", kind-str] (root), ["k", kind-str] (parent)
 *   Pubkey tags:        ["p", pubkey]
 */

/**
 * Reference type returned by thread root / parent lookups.
 */
typedef enum {
    NOSTR_NIP22_REF_NONE = 0,  /**< No reference found */
    NOSTR_NIP22_REF_EVENT,     /**< E/e tag — event ID reference */
    NOSTR_NIP22_REF_ADDR,      /**< A/a tag — addressable event (kind:pubkey:d-tag) */
    NOSTR_NIP22_REF_EXTERNAL,  /**< I/i tag — external content ID (NIP-73) */
} NostrNip22RefType;

/**
 * A reference to a root or parent event in a NIP-22 comment thread.
 *
 * The value and relay pointers point into the event's tag data and are
 * valid only as long as the source event is alive.
 */
typedef struct {
    NostrNip22RefType type;    /**< Type of reference */
    const char *value;         /**< Tag value (event id, addr, or external id) */
    const char *relay;         /**< Relay hint, or NULL if absent */
} NostrNip22Ref;

/**
 * nostr_nip22_is_comment:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Checks whether an event is a NIP-22 comment (kind 1111).
 *
 * Returns: true if the event is a comment
 */
bool nostr_nip22_is_comment(const NostrEvent *ev);

/**
 * nostr_nip22_get_thread_root:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Extracts the thread root reference from a comment event.
 * Looks for the first uppercase E, A, or I tag in order.
 *
 * The returned pointers are borrowed from the event's tags and are
 * valid only while the event is alive and unmodified.
 *
 * Returns: Reference with type NOSTR_NIP22_REF_NONE if not found
 */
NostrNip22Ref nostr_nip22_get_thread_root(const NostrEvent *ev);

/**
 * nostr_nip22_get_immediate_parent:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Extracts the immediate parent reference from a comment event.
 * Looks for the first lowercase e, a, or i tag in order.
 *
 * The returned pointers are borrowed from the event's tags and are
 * valid only while the event is alive and unmodified.
 *
 * Returns: Reference with type NOSTR_NIP22_REF_NONE if not found
 */
NostrNip22Ref nostr_nip22_get_immediate_parent(const NostrEvent *ev);

/**
 * nostr_nip22_get_root_kind:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @out_kind: (out): kind number of the root event
 *
 * Extracts the root event kind from the "K" (uppercase) tag.
 *
 * Returns: 0 on success, -ENOENT if no K tag, -EINVAL on error
 */
int nostr_nip22_get_root_kind(const NostrEvent *ev, int *out_kind);

/**
 * nostr_nip22_get_parent_kind:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 * @out_kind: (out): kind number of the parent event
 *
 * Extracts the parent event kind from the "k" (lowercase) tag.
 *
 * Returns: 0 on success, -ENOENT if no k tag, -EINVAL on error
 */
int nostr_nip22_get_parent_kind(const NostrEvent *ev, int *out_kind);

/**
 * nostr_nip22_create_comment:
 * @ev: (inout) (transfer none) (not nullable): event to populate
 * @content: comment text content
 * @root_id: event ID of the thread root (NULL to skip E tag)
 * @root_relay: relay hint for root (may be NULL)
 * @root_kind: kind of the root event
 * @parent_id: event ID of the immediate parent (NULL to skip e tag)
 * @parent_relay: relay hint for parent (may be NULL)
 * @parent_kind: kind of the parent event (-1 to skip k tag)
 *
 * Populates an event as a NIP-22 comment (sets kind 1111, content,
 * and the appropriate E/e/K/k tags). Additional p tags or other tags
 * can be added after this call.
 *
 * Returns: 0 on success, negative errno-style value on failure
 */
int nostr_nip22_create_comment(NostrEvent *ev, const char *content,
                                const char *root_id, const char *root_relay,
                                int root_kind,
                                const char *parent_id, const char *parent_relay,
                                int parent_kind);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP22_NOSTR_NIP22_NIP22_H */
