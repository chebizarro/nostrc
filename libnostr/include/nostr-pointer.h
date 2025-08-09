#ifndef __NOSTR_POINTER_H__
#define __NOSTR_POINTER_H__

/* GLib-friendly transitional header for pointer helpers */

#include "pointer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NostrProfilePointer:
 * NostrEventPointer:
 * NostrEntityPointer:
 *
 * Opaque pointer helper types for NIP-19/Nostr pointers.
 */
typedef ProfilePointer NostrProfilePointer;
typedef EventPointer   NostrEventPointer;
typedef EntityPointer  NostrEntityPointer;

/**
 * nostr_profile_pointer_new:
 *
 * Returns: (transfer full): newly-allocated `NostrProfilePointer*`
 */
NostrProfilePointer *nostr_profile_pointer_new(void);
/**
 * nostr_profile_pointer_free:
 * @ptr: (transfer full): pointer to free
 */
void nostr_profile_pointer_free(NostrProfilePointer *ptr);

/**
 * nostr_event_pointer_new:
 *
 * Returns: (transfer full): newly-allocated `NostrEventPointer*`
 */
NostrEventPointer *nostr_event_pointer_new(void);
/**
 * nostr_event_pointer_free:
 * @ptr: (transfer full): pointer to free
 */
void nostr_event_pointer_free(NostrEventPointer *ptr);

/**
 * nostr_entity_pointer_new:
 *
 * Returns: (transfer full): newly-allocated `NostrEntityPointer*`
 */
NostrEntityPointer *nostr_entity_pointer_new(void);
/**
 * nostr_entity_pointer_free:
 * @ptr: (transfer full): pointer to free
 */
void nostr_entity_pointer_free(NostrEntityPointer *ptr);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_POINTER_H__ */
