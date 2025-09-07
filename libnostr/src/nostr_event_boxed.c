#include "nostr-event.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>

/* Provide the GBoxed registration for NostrEvent without relying on casts
 * that trigger -Wpedantic on some GCC versions. Use exact-signature wrappers. */
static NostrEvent *nostr_event_copy_boxed(const NostrEvent *e) { return nostr_event_copy(e); }
static void nostr_event_free_boxed(NostrEvent *e) { nostr_event_free(e); }

G_DEFINE_BOXED_TYPE(NostrEvent, nostr_event, nostr_event_copy_boxed, nostr_event_free_boxed)

#endif /* NOSTR_HAVE_GLIB */
