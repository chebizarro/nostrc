#include "nostr-config.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>
#include "nostr-filter.h"

/* GLib-typed wrappers matching GBoxedCopyFunc/GBoxedFreeFunc. */
static gpointer nostr_filter_copy_boxed(gconstpointer f) { return nostr_filter_copy((const NostrFilter*)f); }
static void nostr_filter_free_boxed(gpointer f) { nostr_filter_free((NostrFilter*)f); }

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
G_DEFINE_BOXED_TYPE(NostrFilter, nostr_filter, nostr_filter_copy_boxed, nostr_filter_free_boxed)
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif /* NOSTR_HAVE_GLIB */
