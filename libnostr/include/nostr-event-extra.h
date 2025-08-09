#ifndef __NOSTR_EVENT_EXTRA_H__
#define __NOSTR_EVENT_EXTRA_H__

/* GLib-friendly transitional header for event extra fields helpers */

#include <stdbool.h>
#include <jansson.h>
#include "nostr-event.h"
#include "event_extra.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * nostr_event_set_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @value: (transfer none): json value
 *
 * Sets/overwrites an extra field on @event.
 */
void nostr_event_set_extra(NostrEvent *event, const char *key, json_t *value);

/**
 * nostr_event_remove_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Removes an extra field.
 */
void nostr_event_remove_extra(NostrEvent *event, const char *key);

/**
 * nostr_event_get_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Returns: (transfer none) (nullable): internal pointer to value
 */
json_t *nostr_event_get_extra(NostrEvent *event, const char *key);

/**
 * nostr_event_get_extra_string:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Returns: (transfer full) (nullable): newly-allocated copy of string value
 */
char *nostr_event_get_extra_string(NostrEvent *event, const char *key);

/**
 * nostr_event_get_extra_number:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @out: (out): number result
 *
 * Returns: %TRUE on success
 */
bool nostr_event_get_extra_number(NostrEvent *event, const char *key, double *out);

/**
 * nostr_event_get_extra_bool:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @out: (out): boolean result
 *
 * Returns: %TRUE on success
 */
bool nostr_event_get_extra_bool(NostrEvent *event, const char *key, bool *out);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_EVENT_EXTRA_H__ */
