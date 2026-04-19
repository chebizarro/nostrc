#ifndef NIPS_NIP70_NOSTR_NIP70_NIP70_H
#define NIPS_NIP70_NOSTR_NIP70_NIP70_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include <stdbool.h>

/*
 * NIP-70: Protected Events
 *
 * Events can include a "-" tag to signal that they should not be broadcast
 * to relays other than the one the author intended. When a relay receives
 * a protected event, it should check that the author has authenticated
 * with the relay (NIP-42) and that the event was not forwarded from
 * another relay.
 *
 * Tag format: ["-"]
 */

/**
 * nostr_nip70_is_protected:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Checks whether an event has the "-" protection tag.
 * A valid protection tag is exactly ["-"] with no additional elements.
 *
 * Returns: true if the event is protected, false otherwise
 */
bool nostr_nip70_is_protected(const NostrEvent *ev);

/**
 * nostr_nip70_has_embedded_protected:
 * @ev: (in) (transfer none) (not nullable): event to inspect
 *
 * Checks whether a repost event (kind 6 or 16) contains an embedded
 * protected event in its content. Uses heuristic string matching to
 * detect ["-"] tags inside the embedded event JSON.
 *
 * Returns: true if the event is a repost containing a protected event
 */
bool nostr_nip70_has_embedded_protected(const NostrEvent *ev);

/**
 * nostr_nip70_add_protection:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 *
 * Adds the "-" protection tag to the event. If the event already has
 * a protection tag, this is a no-op (idempotent).
 *
 * Returns: 0 on success, negative errno-style value on failure
 */
int nostr_nip70_add_protection(NostrEvent *ev);

/**
 * nostr_nip70_remove_protection:
 * @ev: (inout) (transfer none) (not nullable): event to modify
 *
 * Removes all "-" protection tags from the event.
 * If the event has no protection tag, this is a no-op.
 *
 * Returns: 0 on success, negative errno-style value on failure
 */
int nostr_nip70_remove_protection(NostrEvent *ev);

/**
 * nostr_nip70_can_rebroadcast:
 * @ev: (in) (transfer none) (not nullable): event to check
 *
 * Returns whether an event can safely be rebroadcast to other relays.
 * Protected events should NOT be rebroadcast.
 *
 * Returns: true if the event can be rebroadcast, false if protected
 */
bool nostr_nip70_can_rebroadcast(const NostrEvent *ev);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP70_NOSTR_NIP70_NIP70_H */
