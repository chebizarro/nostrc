#include "nostr-event.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>

/* Provide the GBoxed registration for NostrEvent */
G_DEFINE_BOXED_TYPE(NostrEvent, nostr_event, (GBoxedCopyFunc)nostr_event_copy, (GBoxedFreeFunc)nostr_event_free)

#endif /* NOSTR_HAVE_GLIB */
