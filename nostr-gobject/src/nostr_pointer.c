#include "nostr_pointer.h"
#include "nostr-error.h"
#include <nostr/nip19/nip19.h>
#include <glib.h>

/* GNostrPointer GObject implementation.
 * Wraps the core NIP-19 NostrPointer tagged union. */

struct _GNostrPointer {
    GObject parent_instance;
    NostrPointer *ptr; /* owned core pointer, may be NULL */
};

G_DEFINE_TYPE(GNostrPointer, gnostr_pointer, G_TYPE_OBJECT)

static void
gnostr_pointer_finalize(GObject *object)
{
    GNostrPointer *self = GNOSTR_POINTER(object);
    if (self->ptr) {
        nostr_pointer_free(self->ptr);
        self->ptr = NULL;
    }
    G_OBJECT_CLASS(gnostr_pointer_parent_class)->finalize(object);
}

static void
gnostr_pointer_class_init(GNostrPointerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_pointer_finalize;
}

static void
gnostr_pointer_init(GNostrPointer *self)
{
    self->ptr = NULL;
}

GNostrPointer *
gnostr_pointer_new_from_bech32(const gchar *bech32, GError **error)
{
    g_return_val_if_fail(bech32 != NULL, NULL);

    NostrPointer *ptr = NULL;
    int rc = nostr_pointer_parse(bech32, &ptr);
    if (rc != 0 || ptr == NULL) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_EVENT,
                    "Failed to parse NIP-19 bech32: %s", bech32);
        return NULL;
    }

    GNostrPointer *self = g_object_new(GNOSTR_TYPE_POINTER, NULL);
    self->ptr = ptr;
    return self;
}

gchar *
gnostr_pointer_to_bech32(GNostrPointer *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_POINTER(self), NULL);

    if (self->ptr == NULL) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_EVENT,
                            "Pointer is empty");
        return NULL;
    }

    char *bech = NULL;
    int rc = nostr_pointer_to_bech32(self->ptr, &bech);
    if (rc != 0 || bech == NULL) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_EVENT,
                            "Failed to encode pointer to bech32");
        return NULL;
    }

    /* Take ownership into GLib-managed string */
    gchar *result = g_strdup(bech);
    free(bech);
    return result;
}

const gchar *
gnostr_pointer_get_kind_name(GNostrPointer *self)
{
    g_return_val_if_fail(GNOSTR_IS_POINTER(self), "none");
    if (self->ptr == NULL)
        return "none";

    switch (self->ptr->kind) {
    case NOSTR_PTR_NPROFILE: return "nprofile";
    case NOSTR_PTR_NEVENT:   return "nevent";
    case NOSTR_PTR_NADDR:    return "naddr";
    case NOSTR_PTR_NRELAY:   return "nrelay";
    default:                 return "none";
    }
}
