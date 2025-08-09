#include <stdio.h>
#include <stdlib.h>
#include <glib-object.h>
#include "nostr-json.h"
#include "nostr-event.h"

/* A minimal GObject implementing the NostrJsonProvider interface.
 * Only implements serialize_event to demonstrate provider install. */

typedef struct _DemoProvider {
    GObject parent_instance;
} DemoProvider;

typedef struct _DemoProviderClass {
    GObjectClass parent_class;
} DemoProviderClass;

G_DEFINE_TYPE_WITH_CODE(DemoProvider, demo_provider, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(NOSTR_TYPE_JSON_PROVIDER, NULL))

static char *demo_serialize_event(const NostrEvent *event) {
    (void)event;
    /* Return a simple JSON string to prove the provider was used. */
    const char *s = "{\"provider\":\"demo\",\"ok\":true}";
    return g_strdup(s);
}

static void demo_provider_init(DemoProvider *self) { (void)self; }

static void demo_provider_class_init(DemoProviderClass *klass) {
    (void)klass;
    /* Install vfuncs into the interface vtable at runtime */
    NostrJsonProviderInterface *iface = g_type_default_interface_peek(NOSTR_TYPE_JSON_PROVIDER);
    if (iface) {
        iface->serialize_event = demo_serialize_event;
    }
}

int main(void) {
    /* Create provider instance and install it */
    DemoProvider *prov = g_object_new(demo_provider_get_type(), NULL);
    nostr_json_provider_install(prov);

    /* Create a minimal event and invoke serialize through C API to ensure our provider is used. */
    NostrEvent *ev = nostr_event_new();
    char *json = nostr_event_serialize(ev);
    printf("Provider JSON: %s\n", json ? json : "(null)");

    g_free(json);
    nostr_event_free(ev);
    nostr_json_provider_uninstall();
    g_object_unref(prov);
    return 0;
}
