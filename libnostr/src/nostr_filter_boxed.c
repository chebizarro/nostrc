#include "nostr-config.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>
#include "nostr-filter.h"

/* Exact-signature wrappers to satisfy pedantic compilers */
static NostrFilter *nostr_filter_copy_boxed(const NostrFilter *f) { return nostr_filter_copy(f); }
static void nostr_filter_free_boxed(NostrFilter *f) { nostr_filter_free(f); }

G_DEFINE_BOXED_TYPE(NostrFilter, nostr_filter, nostr_filter_copy_boxed, nostr_filter_free_boxed)

#endif /* NOSTR_HAVE_GLIB */
