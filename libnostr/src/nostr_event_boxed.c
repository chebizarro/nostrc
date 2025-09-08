#include "nostr-event.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>

/* Provide the GBoxed registration for NostrEvent using GLib-typed wrappers
 * with exact typedef signatures to match GBoxedCopyFunc/GBoxedFreeFunc. */
static gpointer nostr_event_copy_boxed(gconstpointer e) { return nostr_event_copy((const NostrEvent*)e); }
static void nostr_event_free_boxed(gpointer e) { nostr_event_free((NostrEvent*)e); }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
G_DEFINE_BOXED_TYPE(NostrEvent, nostr_event, nostr_event_copy_boxed, nostr_event_free_boxed)
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif /* NOSTR_HAVE_GLIB */
