#ifndef __NOSTR_EVENT_EXTRA_H__
#define __NOSTR_EVENT_EXTRA_H__

#include "nostr-json-export.h"

/* GLib-friendly transitional header. These helpers are implemented and
 * exported by libjson, not by the core libnostr DLL. */

#include <stdbool.h>

/* Opaque forward declaration to avoid pulling internal headers into GIR */
typedef struct _NostrEvent NostrEvent;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * nostr_event_set_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @value: (transfer none) (type gpointer): opaque JSON value
 *
 * Sets/overwrites an extra field on @event.
 */
NOSTR_JSON_API void nostr_event_set_extra(NostrEvent *event, const char *key, void *value);

/**
 * nostr_event_remove_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Removes an extra field.
 */
NOSTR_JSON_API void nostr_event_remove_extra(NostrEvent *event, const char *key);

/**
 * nostr_event_get_extra:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Returns: (transfer none) (nullable) (type gpointer): internal pointer to value
 */
NOSTR_JSON_API void *nostr_event_get_extra(NostrEvent *event, const char *key);

/**
 * nostr_event_get_extra_string:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 *
 * Returns: (transfer full) (nullable): newly-allocated copy of string value
 */
NOSTR_JSON_API char *nostr_event_get_extra_string(NostrEvent *event, const char *key);

/**
 * nostr_event_get_extra_number:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @out: (out): number result
 *
 * Returns: %TRUE on success
 */
NOSTR_JSON_API bool nostr_event_get_extra_number(NostrEvent *event, const char *key, double *out);

/**
 * nostr_event_get_extra_bool:
 * @event: (transfer none): event
 * @key: (transfer none): key string
 * @out: (out): boolean result
 *
 * Returns: %TRUE on success
 */
NOSTR_JSON_API bool nostr_event_get_extra_bool(NostrEvent *event, const char *key, bool *out);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_EVENT_EXTRA_H__ */
