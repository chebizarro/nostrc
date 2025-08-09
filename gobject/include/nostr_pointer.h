#ifndef NOSTR_POINTER_H
#define NOSTR_POINTER_H

#include <glib-object.h>

/* Define NostrPointer GObject */
#define NOSTR_TYPE_POINTER (nostr_pointer_get_type())
G_DECLARE_FINAL_TYPE(NostrPointer, nostr_pointer, NOSTR, POINTER, GObject)

struct _NostrPointer {
    GObject parent_instance;
    gpointer pointer;
};

NostrPointer *nostr_pointer_new(const gchar *public_key, gint kind, const gchar *identifier, const gchar **relays);

#endif // NOSTR_POINTER_H