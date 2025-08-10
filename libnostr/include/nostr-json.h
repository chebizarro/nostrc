#ifndef __NOSTR_JSON_H__
#define __NOSTR_JSON_H__

/*
 * Nostr JSON Interface (GLib/GObject friendly)
 *
 * This header provides:
 * - GI/GTK-Doc annotations for the C JSON API in json.h
 * - An optional GObject interface (NOSTR_HAVE_GLIB) to allow language bindings
 *   to implement/override JSON behavior via a GInterface.
 */

#include "json.h"
#include "nostr-filter.h"
#include "nostr-glib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== C API (GI Annotations) ============================================ */

/**
 * nostr_json_init:
 *
 * Initializes the JSON subsystem. Idempotent.
 */

/**
 * nostr_json_cleanup:
 *
 * Cleans up the JSON subsystem. Safe to call multiple times.
 */

/**
 * nostr_event_serialize:
 * @event: (transfer none): event to serialize
 *
 * Serializes a NostrEvent into a newly-allocated JSON string.
 *
 * Returns: (transfer full): newly-allocated NUL-terminated string
 */

/**
 * nostr_event_deserialize:
 * @event: (transfer none) (out caller-allocates): destination event to fill
 * @json: (transfer none): JSON string
 *
 * Parses JSON and populates @event.
 *
 * Returns: 0 on success, non-zero on error
 */

/**
 * nostr_envelope_serialize:
 * @envelope: (transfer none): envelope to serialize
 *
 * Returns: (transfer full): newly-allocated JSON string
 */

/**
 * nostr_envelope_deserialize:
 * @envelope: (transfer none) (out caller-allocates): destination envelope
 * @json: (transfer none): JSON string
 *
 * Returns: 0 on success, non-zero on error
 */

/**
 * nostr_filter_serialize:
 * @filter: (transfer none): filter
 *
 * Returns: (transfer full): newly-allocated JSON string
 */

/**
 * nostr_filter_deserialize:
 * @filter: (transfer none) (out caller-allocates): destination filter
 * @json: (transfer none): JSON string
 *
 * Returns: 0 on success, non-zero on error
 */

/* ==== Optional GObject Interface ======================================== */

#if defined(NOSTR_HAVE_GLIB) && NOSTR_HAVE_GLIB

#include <glib-object.h>

#define NOSTR_TYPE_JSON_PROVIDER (nostr_json_provider_get_type())

typedef struct _NostrJsonProvider NostrJsonProvider; /* dummy instance type */
typedef struct _NostrJsonProviderInterface NostrJsonProviderInterface;

struct _NostrJsonProviderInterface {
    GTypeInterface parent_iface;

    /* vfuncs should return newly allocated strings (transfer full) for
     * serialize methods, and 0 for success for deserialize methods. */

    /* (nullable) */
    char *(*serialize_event)(const NostrEvent *event);
    int   (*deserialize_event)(NostrEvent *event, const char *json);

    /* (nullable) */
    char *(*serialize_envelope)(const NostrEnvelope *envelope);
    int   (*deserialize_envelope)(NostrEnvelope *envelope, const char *json);

    /* (nullable) */
    char *(*serialize_filter)(const NostrFilter *filter);
    int   (*deserialize_filter)(NostrFilter *filter, const char *json);
};

GType nostr_json_provider_get_type(void);

/**
 * nostr_json_provider_install:
 * @provider: (transfer none): provider implementing #NostrJsonProvider
 *
 * Installs @provider as the active JSON backend by bridging to the legacy
 * `NostrJsonInterface` in `json.h`.
 */
void nostr_json_provider_install(gpointer provider);

/**
 * nostr_json_provider_uninstall:
 *
 * Uninstalls any active provider and clears the legacy interface.
 */
void nostr_json_provider_uninstall(void);

#endif /* NOSTR_HAVE_GLIB */

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_JSON_H__ */
