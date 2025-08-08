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
 * @npub: (nullable) (transfer none): bech32 profile id or hex
 * @relays: (nullable) (array zero-terminated=1) (transfer none): relay URLs
 *
 * Returns: (transfer full): newly-allocated `NostrProfilePointer*`
 */
#define nostr_profile_pointer_new   create_profile_pointer
/**
 * nostr_profile_pointer_free:
 * @ptr: (transfer full): pointer to free
 */
#define nostr_profile_pointer_free  free_profile_pointer

/**
 * nostr_event_pointer_new:
 * @id: (nullable) (transfer none): event id (hex)
 * @author: (nullable) (transfer none): author pubkey (hex)
 * @kind: kind number
 * @relays: (nullable) (array zero-terminated=1) (transfer none): relay URLs
 *
 * Returns: (transfer full): newly-allocated `NostrEventPointer*`
 */
#define nostr_event_pointer_new     create_event_pointer
/**
 * nostr_event_pointer_free:
 * @ptr: (transfer full): pointer to free
 */
#define nostr_event_pointer_free    free_event_pointer

/**
 * nostr_entity_pointer_new:
 * @kind: entity kind selector
 * @data: (transfer none): implementation-defined payload
 *
 * Returns: (transfer full): newly-allocated `NostrEntityPointer*`
 */
#define nostr_entity_pointer_new    create_entity_pointer
/**
 * nostr_entity_pointer_free:
 * @ptr: (transfer full): pointer to free
 */
#define nostr_entity_pointer_free   free_entity_pointer

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_POINTER_H__ */
