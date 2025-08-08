#ifndef __NOSTR_EVENT_EXTRA_H__
#define __NOSTR_EVENT_EXTRA_H__

/* GLib-friendly transitional header for event extra fields helpers */

#include "event_extra.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * nostr_event_set_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @value: (transfer none): value string
 *
 * Sets/overwrites an extra field on @event.
 */
#define nostr_event_set_extra        set_extra
/**
 * nostr_event_remove_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Removes an extra field.
 */
#define nostr_event_remove_extra     remove_extra
/**
 * nostr_event_get_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Returns: (transfer none) (nullable): internal pointer to value
 */
#define nostr_event_get_extra        get_extra
/**
 * nostr_event_get_extra_string:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Returns: (transfer full) (nullable): newly-allocated copy of string value
 */
#define nostr_event_get_extra_string get_extra_string
/**
 * nostr_event_get_extra_number:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @out: (out): number result
 *
 * Returns: %TRUE on success
 */
#define nostr_event_get_extra_number get_extra_number
/**
 * nostr_event_get_extra_bool:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @out: (out): boolean result
 *
 * Returns: %TRUE on success
 */
#define nostr_event_get_extra_bool   get_extra_boolean

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_EVENT_EXTRA_H__ */
