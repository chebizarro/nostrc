#ifndef __NOSTR_EVENT_H__
#define __NOSTR_EVENT_H__

/* Transitional header: exposes GLib-friendly libnostr naming for events.
 * Avoids including internal headers; wrapper source bridges to internals. */

#include <stdbool.h>
#include <stdint.h>

/* Opaque to GI; implemented in src wrappers */
/**
 * NostrEvent:
 *
 * Opaque event record; registered as a GBoxed type via `nostr_event_get_type()`.
 */
typedef struct _NostrEvent NostrEvent;

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

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_EVENT_H__ */
