#include "nostr-event.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>

/* Provide the GBoxed registration for NostrEvent using exact TN* signatures.
 * Some compilers warn under -Wpedantic due to macro internals, so we suppress
 * pedantic diagnostics around the macro usage. */
static NostrEvent *nostr_event_copy_boxed(const NostrEvent *e) { return nostr_event_copy(e); }
static void nostr_event_free_boxed(NostrEvent *e) { nostr_event_free(e); }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
G_DEFINE_BOXED_TYPE(NostrEvent, nostr_event, nostr_event_copy_boxed, nostr_event_free_boxed)
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif /* NOSTR_HAVE_GLIB */
