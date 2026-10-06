#include "nostr-config.h"
#include "nostr-json.h"
#include "json.h"
#include "nostr-filter.h"

#if defined(NOSTR_HAVE_GLIB) && NOSTR_HAVE_GLIB

#include <glib-object.h>
#include <pthread.h>

/* nostrc-val0v: s_provider used to be read by the trampolines while
 * nostr_json_provider_install/uninstall unref'd it on another thread — a
 * use-after-free window. The trampolines now take a strong reference under
 * the mutex, call the vfunc, and release; install/uninstall swap the pointer
 * under the same mutex and drop the old reference outside it. */
static gpointer s_provider = NULL; /* (owned) strong ref; guarded by s_provider_mu */
static pthread_mutex_t s_provider_mu = PTHREAD_MUTEX_INITIALIZER;

static gpointer provider_strong_ref(void) {
    pthread_mutex_lock(&s_provider_mu);
    gpointer p = s_provider ? g_object_ref(s_provider) : NULL;
    pthread_mutex_unlock(&s_provider_mu);
    return p;
}

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

/* Trampolines mapping to provider vfuncs. Each holds a strong ref for the
 * duration of the call (nostrc-val0v). */
static char *tr_serialize_event(const NostrEvent *event) {
    gpointer p = provider_strong_ref();
    if (!p) return NULL;
    NostrJsonProviderInterface *iface = get_iface_from_obj(p);
    char *ret = iface->serialize_event ? iface->serialize_event(event) : NULL;
    g_object_unref(p);
    return ret;
}

static int tr_deserialize_event(NostrEvent *event, const char *json) {
    gpointer p = provider_strong_ref();
    if (!p) return -1;
    NostrJsonProviderInterface *iface = get_iface_from_obj(p);
    int ret = iface->deserialize_event ? iface->deserialize_event(event, json) : -1;
    g_object_unref(p);
    return ret;
}

static char *tr_serialize_envelope(const NostrEnvelope *envelope) {
    gpointer p = provider_strong_ref();
    if (!p) return NULL;
    NostrJsonProviderInterface *iface = get_iface_from_obj(p);
    char *ret = iface->serialize_envelope ? iface->serialize_envelope(envelope) : NULL;
    g_object_unref(p);
    return ret;
}

static int tr_deserialize_envelope(NostrEnvelope *envelope, const char *json) {
    gpointer p = provider_strong_ref();
    if (!p) return -1;
    NostrJsonProviderInterface *iface = get_iface_from_obj(p);
    int ret = iface->deserialize_envelope ? iface->deserialize_envelope(envelope, json) : -1;
    g_object_unref(p);
    return ret;
}

static char *tr_serialize_filter(const NostrFilter *filter) {
    gpointer p = provider_strong_ref();
    if (!p) return NULL;
    NostrJsonProviderInterface *iface = get_iface_from_obj(p);
    char *ret = iface->serialize_filter ? iface->serialize_filter(filter) : NULL;
    g_object_unref(p);
    return ret;
}

static int tr_deserialize_filter(NostrFilter *filter, const char *json) {
    gpointer p = provider_strong_ref();
    if (!p) return -1;
    NostrJsonProviderInterface *iface = get_iface_from_obj(p);
    int ret = iface->deserialize_filter ? iface->deserialize_filter(filter, json) : -1;
    g_object_unref(p);
    return ret;
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
    pthread_mutex_lock(&s_provider_mu);
    gpointer old = s_provider;
    s_provider = provider ? g_object_ref(provider) : NULL;
    pthread_mutex_unlock(&s_provider_mu);
    if (old) g_object_unref(old);
    nostr_set_json_interface(provider ? &s_iface : NULL);
}

void nostr_json_provider_uninstall(void) {
    pthread_mutex_lock(&s_provider_mu);
    gpointer old = s_provider;
    s_provider = NULL;
    pthread_mutex_unlock(&s_provider_mu);
    if (old) g_object_unref(old);
    nostr_set_json_interface(NULL);
}

#endif /* NOSTR_HAVE_GLIB */
