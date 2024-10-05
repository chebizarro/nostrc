#include "nostr_pointer.h"
#include <glib.h>

/* NostrPointer GObject implementation */
G_DEFINE_TYPE(NostrPointer, nostr_pointer, G_TYPE_OBJECT)

static void nostr_pointer_finalize(GObject *object) {
    NostrPointer *self = NOSTR_POINTER(object);
    if (self->pointer) {
        pointer_free(self->pointer);
    }
    G_OBJECT_CLASS(nostr_pointer_parent_class)->finalize(object);
}

static void nostr_pointer_class_init(NostrPointerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_pointer_finalize;
}

static void nostr_pointer_init(NostrPointer *self) {
    self->pointer = NULL;
}

NostrPointer *nostr_pointer_new(const gchar *public_key, gint kind, const gchar *identifier, const gchar **relays) {
    NostrPointer *self = g_object_new(NOSTR_TYPE_POINTER, NULL);
    self->pointer = pointer_new(public_key, kind, identifier, relays);
    return self;
}