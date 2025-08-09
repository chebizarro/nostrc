#ifndef __NOSTR_EVENT_H__
#define __NOSTR_EVENT_H__

/* Transitional header: exposes GLib-friendly libnostr naming for events.
 * Internally forwards to existing symbols. Will be replaced by direct
 * implementations during the full refactor. */

#include <stdbool.h>
#include <stdint.h>
#include "nostr-tag.h"
#include "event.h" /* defines NostrEvent and old function names */

#ifdef __cplusplus
extern "C" {
#endif

/* Types: already using NostrEvent for the struct. */

/* Mapping policy:
 * - If legacy aliases are ENABLED, map old names -> new names (preferred outward API),
 *   so downstreams using old names still work.
 * - Otherwise (default), map new names -> old implementations.
 * This avoids circular macro expansions.
 */
#if defined(NOSTR_ENABLE_LEGACY_ALIASES) && NOSTR_ENABLE_LEGACY_ALIASES
  /* Legacy ON: old -> new */
#  define create_event                 nostr_event_new
#  define free_event                   nostr_event_free
#  /* Do not remap JSON-layer functions here */
#  define event_get_id                 nostr_event_get_id
#  define event_check_signature        nostr_event_check_signature
#  define event_sign                   nostr_event_sign
#  define event_is_regular             nostr_event_is_regular
#else
  /* Legacy OFF (default): new -> old */
#  define nostr_event_new              create_event
#  define nostr_event_free             free_event
  /* Do not remap JSON-layer functions here */
#  define nostr_event_get_id           event_get_id
#  define nostr_event_check_signature  event_check_signature
#  define nostr_event_sign             event_sign
#  define nostr_event_is_regular       event_is_regular
#endif

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
 * Returns: (transfer none) (nullable): owned tags pointer
 */
NostrTags *nostr_event_get_tags(const NostrEvent *event);
/**
 * nostr_event_set_tags:
 * @event: (nullable): event (no-op if NULL)
 * @tags: (transfer full) (nullable): new tags; previous freed if different
 *
 * Ownership: takes full ownership of @tags.
 */
void nostr_event_set_tags(NostrEvent *event, NostrTags *tags);

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
