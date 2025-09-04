#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <stdio.h>
#include <stdlib.h>

NostrJsonInterface *json_interface = NULL;
static int g_json_force_fallback = -1; /* -1 = uninitialized, 0/1 set */

void nostr_set_json_interface(NostrJsonInterface *interface) {
    json_interface = interface;
}

void nostr_json_init(void) {
    if (json_interface && json_interface->init) {
        json_interface->init();
    }
}

void nostr_json_cleanup(void) {
    if (json_interface && json_interface->cleanup) {
        json_interface->cleanup();
    }
}

void nostr_json_force_fallback(bool enable) {
    g_json_force_fallback = enable ? 1 : 0;
}

static inline int json_force_fallback(void) {
    if (g_json_force_fallback == -1) {
        const char *e = getenv("NOSTR_JSON_FORCE_FALLBACK");
        g_json_force_fallback = (e && (*e == '1' || *e == 't' || *e == 'T' || *e == 'y' || *e == 'Y')) ? 1 : 0;
    }
    return g_json_force_fallback;
}

char *nostr_event_serialize(const NostrEvent *event) {
    if (!json_force_fallback()) {
        // Prefer compact fast path
        char *s = nostr_event_serialize_compact(event);
        if (s) return s;
    }
    // Fallback to configured backend, if any
    if (json_interface && json_interface->serialize_event) {
        return json_interface->serialize_event(event);
    }
    return NULL;
}

/* Internal: clear owned fields without freeing the struct itself */
static void nostr_event_clear_fields(NostrEvent *e) {
    if (!e) return;
    if (e->id) { free(e->id); e->id = NULL; }
    if (e->pubkey) { free(e->pubkey); e->pubkey = NULL; }
    if (e->tags) { nostr_tags_free(e->tags); e->tags = NULL; }
    if (e->content) { free(e->content); e->content = NULL; }
    if (e->sig) { free(e->sig); e->sig = NULL; }
}

int nostr_event_deserialize(NostrEvent *event, const char *json_str) {
    if (!json_force_fallback()) {
        // Try compact fast path first using a stack-allocated shadow to avoid partial mutation
        NostrEvent shadow = {0};
        if (nostr_event_deserialize_compact(&shadow, json_str)) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "nostr_event_deserialize: compact path\n");
            // Replace fields in destination
            nostr_event_clear_fields(event);
            event->id = shadow.id; shadow.id = NULL;
            event->pubkey = shadow.pubkey; shadow.pubkey = NULL;
            event->created_at = shadow.created_at;
            event->kind = shadow.kind;
            event->tags = shadow.tags; shadow.tags = NULL;
            event->content = shadow.content; shadow.content = NULL;
            event->sig = shadow.sig; shadow.sig = NULL;
            return 0;
        } else {
            // Clean any partial allocations in shadow before falling back
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "nostr_event_deserialize: compact failed, falling back\n");
            nostr_event_clear_fields(&shadow);
        }
    }
    // Fallback to configured backend
    if (json_interface && json_interface->deserialize_event) {
        if (getenv("NOSTR_DEBUG")) fprintf(stderr, "nostr_event_deserialize: backend provider\n");
        return json_interface->deserialize_event(event, json_str);
    }
    return -1;
}

char *nostr_envelope_serialize(const NostrEnvelope *envelope) {
    if (!json_force_fallback()) {
        // Prefer compact fast path
        char *s = nostr_envelope_serialize_compact(envelope);
        if (s) return s;
    }
    // Fallback to configured backend, if any
    if (json_interface && json_interface->serialize_envelope) {
        return json_interface->serialize_envelope(envelope);
    }
    return NULL;
}

int nostr_envelope_deserialize(NostrEnvelope *envelope, const char *json) {
    if (!json_force_fallback()) {
        // Try compact fast path first
        if (nostr_envelope_deserialize_compact(envelope, json)) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "nostr_envelope_deserialize: compact path\n");
            return 0;
        }
    }
    // Fallback to configured backend
    if (json_interface && json_interface->deserialize_envelope) {
        if (getenv("NOSTR_DEBUG")) fprintf(stderr, "nostr_envelope_deserialize: backend provider\n");
        return json_interface->deserialize_envelope(envelope, json);
    }
    return -1;
}

char *nostr_filter_serialize(const NostrFilter *filter) {
    if (!json_force_fallback()) {
        // Prefer compact fast path
        char *s = nostr_filter_serialize_compact(filter);
        if (s) return s;
    }
    // Fallback to configured backend, if any
    if (json_interface && json_interface->serialize_filter) {
        return json_interface->serialize_filter(filter);
    }
    return NULL;
}

int nostr_filter_deserialize(NostrFilter *filter, const char *json) {
    if (!json_force_fallback()) {
        // Try compact fast path first
        if (nostr_filter_deserialize_compact(filter, json)) {
            if (getenv("NOSTR_DEBUG")) fprintf(stderr, "nostr_filter_deserialize: compact path\n");
            return 0;
        }
    }
    // Fallback to configured backend
    if (json_interface && json_interface->deserialize_filter) {
        if (getenv("NOSTR_DEBUG")) fprintf(stderr, "nostr_filter_deserialize: backend provider\n");
        return json_interface->deserialize_filter(filter, json);
    }
    return -1;
}

