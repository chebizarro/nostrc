#include "nostr-config.h"
#if NOSTR_HAVE_GLIB
#include <glib-object.h>
#include "nostr-filter.h"

G_DEFINE_BOXED_TYPE(NostrFilter, nostr_filter, nostr_filter_copy, nostr_filter_free)

#endif /* NOSTR_HAVE_GLIB */
