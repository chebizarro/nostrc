#include "nostr-config.h"
#include "nostr-json.h"
#include "json.h"
#include "nostr-filter.h"

#if defined(NOSTR_HAVE_GLIB) && NOSTR_HAVE_GLIB

#include <glib-object.h>

static gpointer s_provider = NULL; /* (owned) strong ref */

/* Minimal interface registration and lookup helpers (no G_DECLARE_INTERFACE). */
GType nostr_json_provider_get_type(void);
static void nostr_json_provider_default_init(NostrJsonProviderInterface *iface) { (void)iface; }
/* Wrapper with exact GClassInitFunc signature to avoid function pointer cast warnings */
static void nostr_json_provider_class_init(gpointer klass, gpointer class_data) {
    (void)class_data;
    nostr_json_provider_default_init((NostrJsonProviderInterface *)klass);
}

GType nostr_json_provider_get_type(void) {
    static gsize gtype_id = 0;
    if (g_once_init_enter(&gtype_id)) {
        GType tid = g_type_register_static_simple(
            G_TYPE_INTERFACE,
            g_intern_static_string("NostrJsonProvider"),
            sizeof(NostrJsonProviderInterface),
            (GClassInitFunc)nostr_json_provider_class_init,
            0,
            NULL,
            0);
        g_type_interface_add_prerequisite(tid, G_TYPE_OBJECT);
        g_once_init_leave(&gtype_id, tid);
    }
    return (GType)gtype_id;
}

static inline NostrJsonProviderInterface *get_iface_from_obj(gpointer obj) {
    if (!obj) return NULL;
    GTypeClass *klass = (GTypeClass *)G_OBJECT_GET_CLASS(obj);
    return (NostrJsonProviderInterface *)g_type_interface_peek(klass, NOSTR_TYPE_JSON_PROVIDER);
}

/* Trampolines mapping to provider vfuncs. */
static char *tr_serialize_event(const NostrEvent *event) {
    if (!s_provider) return NULL;
    NostrJsonProviderInterface *iface = get_iface_from_obj(s_provider);
    if (iface->serialize_event)
        return iface->serialize_event(event);
    return NULL;
}

static int tr_deserialize_event(NostrEvent *event, const char *json) {
    if (!s_provider) return -1;
    NostrJsonProviderInterface *iface = get_iface_from_obj(s_provider);
    if (iface->deserialize_event)
        return iface->deserialize_event(event, json);
    return -1;
}

static char *tr_serialize_envelope(const NostrEnvelope *envelope) {
    if (!s_provider) return NULL;
    NostrJsonProviderInterface *iface = get_iface_from_obj(s_provider);
    if (iface->serialize_envelope)
        return iface->serialize_envelope(envelope);
    return NULL;
}

static int tr_deserialize_envelope(NostrEnvelope *envelope, const char *json) {
    if (!s_provider) return -1;
    NostrJsonProviderInterface *iface = get_iface_from_obj(s_provider);
    if (iface->deserialize_envelope)
        return iface->deserialize_envelope(envelope, json);
    return -1;
}

static char *tr_serialize_filter(const NostrFilter *filter) {
    if (!s_provider) return NULL;
    NostrJsonProviderInterface *iface = get_iface_from_obj(s_provider);
    if (iface->serialize_filter)
        return iface->serialize_filter(filter);
    return NULL;
}

static int tr_deserialize_filter(NostrFilter *filter, const char *json) {
    if (!s_provider) return -1;
    NostrJsonProviderInterface *iface = get_iface_from_obj(s_provider);
    if (iface->deserialize_filter)
        return iface->deserialize_filter(filter, json);
    return -1;
}

static NostrJsonInterface s_iface = {
    .init = NULL,
    .cleanup = NULL,
    .serialize_event = tr_serialize_event,
    .deserialize_event = tr_deserialize_event,
    .serialize_envelope = tr_serialize_envelope,
    .deserialize_envelope = tr_deserialize_envelope,
    .serialize_filter = tr_serialize_filter,
    .deserialize_filter = tr_deserialize_filter,
};

void nostr_json_provider_install(gpointer provider) {
    if (s_provider) {
        /* Replace existing */
        g_object_unref(s_provider);
        s_provider = NULL;
    }
    if (provider) {
        s_provider = g_object_ref(provider);
        nostr_set_json_interface(&s_iface);
    } else {
        nostr_set_json_interface(NULL);
    }
}

void nostr_json_provider_uninstall(void) {
    if (s_provider) {
        g_object_unref(s_provider);
        s_provider = NULL;
    }
    nostr_set_json_interface(NULL);
}

#endif /* NOSTR_HAVE_GLIB */
