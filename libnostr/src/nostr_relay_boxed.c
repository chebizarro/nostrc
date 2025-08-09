#include "nostr-config.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>
#include "relay.h"

/* Relay is ref-counted; boxed copy = nostr_relay_ref, boxed free = nostr_relay_unref */
typedef Relay NostrRelay;

static NostrRelay *nostr_relay_copy(const NostrRelay *src) {
    return (NostrRelay *)nostr_relay_ref((Relay *)src);
}

static void nostr_relay_free_boxed(NostrRelay *relay) {
    if (relay) nostr_relay_unref(relay);
}

G_DEFINE_BOXED_TYPE(NostrRelay, nostr_relay, nostr_relay_copy, nostr_relay_free_boxed)

#endif /* NOSTR_HAVE_GLIB */
