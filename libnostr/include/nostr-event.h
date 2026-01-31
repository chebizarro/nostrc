#ifndef __NOSTR_EVENT_H__
#define __NOSTR_EVENT_H__

/* Transitional header: exposes GLib-friendly libnostr naming for events.
 * Avoids including internal headers; wrapper source bridges to internals. */

#include <stdbool.h>
#include <stdint.h>
#include "nostr-tag.h"
#include "secure_buf.h"

/* Opaque to GI; implemented in src wrappers */
/**
 * NostrEvent:
 *
 * Opaque event record; registered as a GBoxed type via `nostr_event_get_type()`.
 */
// Define the NostrEvent structure
typedef struct _NostrEvent {
    char *id;
    char *pubkey;
    int64_t created_at;
    int kind;
    NostrTags *tags;
    char *content;
    char *sig;
    void *extra; // Extra fields
} NostrEvent;

#ifdef __cplusplus
extern "C" {
#endif

/* Types: opaque NostrEvent handle */

/* Constructors and core ops */
/**
 * nostr_event_new:
 *
 * Returns: (transfer full) (nullable): new event
 */
NostrEvent *nostr_event_new(void);

/**
 * nostr_event_free:
 * @event: (transfer full) (nullable): event to free
 */
void nostr_event_free(NostrEvent *event);

/**
 * nostr_event_copy:
 * @event: (nullable): event to copy
 *
 * Returns: (transfer full) (nullable): deep copy of @event
 */
NostrEvent *nostr_event_copy(const NostrEvent *event);

#ifdef __GI_SCANNER__
#include <glib-object.h>
GType nostr_event_get_type(void);
#define NOSTR_TYPE_EVENT (nostr_event_get_type())
#endif

/**
 * nostr_event_get_id:
 * @event: (nullable): event
 *
 * Returns: (transfer full) (nullable): newly allocated hex id
 */
char *nostr_event_get_id(NostrEvent *event);

/**
 * nostr_event_check_signature:
 * @event: (nullable): event
 *
 * Returns: whether signature verifies
 */
bool nostr_event_check_signature(NostrEvent *event);

/**
 * nostr_event_sign:
 * @event: (nullable): event
 * @private_key: (nullable): hex private key
 *
 * Returns: 0 on success
 */
int nostr_event_sign(NostrEvent *event, const char *private_key);

/**
 * nostr_event_sign_secure:
 * @event: (nullable): event
 * @sk: (not nullable): pointer to a 32-byte private key stored in nostr_secure_buf
 *
 * Signs the event using the provided secret key stored in secure memory.
 * Returns: 0 on success
 */
int nostr_event_sign_secure(NostrEvent *event, const nostr_secure_buf *sk);

/**
 * nostr_event_is_regular:
 * @event: (nullable): event
 *
 * Returns: whether event is regular
 */
bool nostr_event_is_regular(NostrEvent *event);

/* Accessors for public struct members (for GObject properties later) */
/**
 * nostr_event_get_pubkey:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable): internal pubkey string
 */
const char *nostr_event_get_pubkey(const NostrEvent *event);
/**
 * nostr_event_set_pubkey:
 * @event: (nullable): event (no-op if NULL)
 * @pubkey: (nullable): hex pubkey; duplicated internally
 */
void nostr_event_set_pubkey(NostrEvent *event, const char *pubkey);

/**
 * nostr_event_get_created_at:
 * @event: (nullable): event
 *
 * Returns: created_at timestamp
 */
int64_t nostr_event_get_created_at(const NostrEvent *event);
/**
 * nostr_event_set_created_at:
 * @event: (nullable): event
 * @created_at: timestamp
 */
void nostr_event_set_created_at(NostrEvent *event, int64_t created_at);

/**
 * nostr_event_get_kind:
 * @event: (nullable): event
 *
 * Returns: kind integer
 */
int nostr_event_get_kind(const NostrEvent *event);
/**
 * nostr_event_set_kind:
 * @event: (nullable): event
 * @kind: kind integer
 */
void nostr_event_set_kind(NostrEvent *event, int kind);

/**
 * nostr_event_get_tags:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable) (type gpointer): owned tags pointer
 */
void *nostr_event_get_tags(const NostrEvent *event);
/**
 * nostr_event_set_tags:
 * @event: (nullable): event (no-op if NULL)
 * @tags: (transfer full) (nullable) (type gpointer): new tags; previous freed if different
 *
 * Ownership: takes full ownership of @tags.
 */
void nostr_event_set_tags(NostrEvent *event, void *tags);

/**
 * nostr_event_get_content:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable)
 */
const char *nostr_event_get_content(const NostrEvent *event);
/**
 * nostr_event_set_content:
 * @event: (nullable): event (no-op if NULL)
 * @content: (nullable): utf8; duplicated internally
 */
void nostr_event_set_content(NostrEvent *event, const char *content);

/**
 * nostr_event_get_sig:
 * @event: (nullable): event
 *
 * Returns: (transfer none) (nullable)
 */
const char *nostr_event_get_sig(const NostrEvent *event);
/**
 * nostr_event_set_sig:
 * @event: (nullable): event (no-op if NULL)
 * @sig: (nullable): hex signature; duplicated internally
 */
void nostr_event_set_sig(NostrEvent *event, const char *sig);

/* No further remapping here to prevent recursive macro definitions. */

/* Fast-path JSON serialization for hot paths (avoids backend/jansson).
 * Returns a newly-allocated compact JSON object string representing the event.
 * Only includes fields that are set (id, pubkey, created_at, kind, tags, content, sig).
 */
char *nostr_event_serialize_compact(const NostrEvent *event);

/* Fast-path JSON deserialization from a compact object string.
 * Returns 1 on success, 0 on parse error. Populates provided @event.
 */
int nostr_event_deserialize_compact(NostrEvent *event, const char *json);

/* ========================================================================
 * Event Priority Classification (nostrc-7u2)
 * ======================================================================== */

/**
 * NostrEventPriority:
 * Priority levels for backpressure decisions.
 */
typedef enum {
    NOSTR_EVENT_PRIORITY_CRITICAL = 0,  /**< DMs, zaps, mentions - never dropped */
    NOSTR_EVENT_PRIORITY_HIGH     = 1,  /**< Replies to own posts */
    NOSTR_EVENT_PRIORITY_NORMAL   = 2,  /**< Timeline events */
    NOSTR_EVENT_PRIORITY_LOW      = 3   /**< Reactions, reposts - dropped first */
} NostrEventPriority;

/**
 * nostr_event_get_priority:
 * @event: event to classify
 * @user_pubkey: (nullable): current user's pubkey for mention detection
 *
 * Classifies an event's priority for backpressure decisions.
 * Classification rules:
 * - CRITICAL: DMs (kind 4, 1059), zaps (kind 9735), mentions of user
 * - HIGH: Replies (kind 1 with "e" tag)
 * - LOW: Reactions (kind 7), reposts (kind 6)
 * - NORMAL: Everything else
 *
 * Returns: priority level
 */
NostrEventPriority nostr_event_get_priority(const NostrEvent *event, const char *user_pubkey);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_EVENT_H__ */
